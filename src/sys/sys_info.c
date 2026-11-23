#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/sys_info.h>
#include <cwist/core/macros.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

/**
 * @file sys_info.c
 * @brief Cross-platform helpers for estimating available RAM and low-memory pressure.
 */

#if defined(__linux__)
#include <sys/sysinfo.h>

#include <inttypes.h>

/** @brief Read one unsigned integer from a file; false if absent or "max". */
static bool sys_read_u64(const char *path, uint64_t *out) {
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    char buf[64];
    bool ok = fgets(buf, sizeof(buf), fp) != NULL;
    fclose(fp);
    if (!ok || strncmp(buf, "max", 3) == 0) return false;
    char *end = NULL;
    unsigned long long v = strtoull(buf, &end, 10);
    if (end == buf) return false;
    *out = (uint64_t)v;
    return true;
}

/** @brief MemAvailable from /proc/meminfo (free + reclaimable cache), in bytes. */
static bool sys_mem_available(uint64_t *out) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return false;
    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        uint64_t kb = 0;
        if (sscanf(line, "MemAvailable: %" SCNu64 " kB", &kb) == 1) {
            *out = kb * 1024;
            found = true;
            break;
        }
    }
    fclose(fp);
    return found;
}

/**
 * @brief Memory left under this process's cgroup limit (v2, then v1).
 * @return true with @p out set when a finite limit applies.
 */
static bool sys_cgroup_headroom(uint64_t *out) {
    uint64_t limit = 0, usage = 0;
    /* cgroup v2: the process's own group from /proc/self/cgroup ("0::/path"). */
    FILE *fp = fopen("/proc/self/cgroup", "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "0::", 3) != 0) continue;
            char *path = line + 3;
            path[strcspn(path, "\r\n")] = '\0';
            char file[640];
            snprintf(file, sizeof(file), "/sys/fs/cgroup%s/memory.max",
                     strcmp(path, "/") == 0 ? "" : path);
            bool have_limit = sys_read_u64(file, &limit);
            snprintf(file, sizeof(file), "/sys/fs/cgroup%s/memory.current",
                     strcmp(path, "/") == 0 ? "" : path);
            if (have_limit && sys_read_u64(file, &usage)) {
                fclose(fp);
                *out = usage < limit ? limit - usage : 1; /* 0 would mean "unknown" */
                return true;
            }
            break;
        }
        fclose(fp);
    }
    /* cgroup v1 memory controller; "no limit" reads as a huge number. */
    if (sys_read_u64("/sys/fs/cgroup/memory/memory.limit_in_bytes", &limit) &&
        limit < ((uint64_t)1 << 60) &&
        sys_read_u64("/sys/fs/cgroup/memory/memory.usage_in_bytes", &usage)) {
        *out = usage < limit ? limit - usage : 1; /* 0 would mean "unknown" */
        return true;
    }
    return false;
}

/**
 * @brief Estimate available RAM on Linux.
 *
 * Uses MemAvailable (free memory plus page cache the kernel can reclaim),
 * capped by the headroom under the process's cgroup memory limit. The
 * previous estimate, sysinfo().freeram, excludes page cache and is small on
 * any busy host, so callers switched to their low-memory paths with plenty
 * of memory available, and container limits were ignored (#992).
 *
 * @return Available RAM in bytes, or 0 when no estimate is possible.
 */
uint64_t cwist_get_available_ram(void) {
    uint64_t avail = 0;
    bool have = sys_mem_available(&avail);
    if (!have) {
        struct sysinfo si;
        if (sysinfo(&si) == 0) {
            avail = (uint64_t)si.freeram * (uint64_t)si.mem_unit;
            have = true;
        }
    }
    uint64_t headroom = 0;
    if (sys_cgroup_headroom(&headroom) && (!have || headroom < avail)) {
        avail = headroom;
        have = true;
    }
    return have ? avail : 0;
}

#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>

/**
 * @brief Estimate currently available RAM on Apple and BSD platforms.
 * @return Available RAM in bytes, or 0 when the platform cannot provide an estimate.
 */
uint64_t cwist_get_available_ram(void) {
    // macOS/BSD Logic
    // Getting strict "Available" like Linux is harder.
    // We can get page size * free pages.

    int mib[2];
    mib[0] = CTL_HW;

#ifdef __APPLE__
    // Roughly estimate using page size and free count is tricky portably.
    // Let's rely on a simpler 'usermem' or just fail-safe to "High Enough" if we can't detect,
    // assuming the user manages their mac well.
    // Or check vm.page_free_count
    return CWIST_GIB(2); // Mock for Mac dev env safety to prevent constant switching
#else
    // FreeBSD
    u_int page_size;
    u_int free_count;
    size_t len = sizeof(page_size);

    if (sysctlbyname("vm.stats.vm.v_page_size", &page_size, &len, NULL, 0) == -1) return 0;
    len = sizeof(free_count);
    if (sysctlbyname("vm.stats.vm.v_free_count", &free_count, &len, NULL, 0) == -1) return 0;

    return (uint64_t)page_size * (uint64_t)free_count;
#endif
}

#else
/**
 * @brief Fallback RAM estimate for unsupported platforms.
 * @return Conservative mock availability used when no platform-specific API exists.
 */
uint64_t cwist_get_available_ram(void) {
    return CWIST_GIB(1); // Default Mock
}
#endif

/**
 * @brief Check whether the current available RAM is below a caller-supplied threshold.
 * @param threshold_bytes Threshold in bytes that marks memory as critical.
 * @return true when the estimated available RAM is below the threshold.
 */
bool cwist_is_ram_critical(uint64_t threshold_bytes) {
    uint64_t avail = cwist_get_available_ram();
    if (avail == 0) return false; // Could not detect, assume safe
    return avail < threshold_bytes;
}
