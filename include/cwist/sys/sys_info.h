/**
 * @file sys_info.h
 * @brief System Resource Monitoring Utilities.
 */

#ifndef __CWIST_SYS_INFO_H__
#define __CWIST_SYS_INFO_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Get the available (free + reclaimable) system RAM in bytes.
 *
 * Uses MemAvailable from /proc/meminfo on Linux, capped by the headroom under
 * the process's cgroup (v2 or v1) memory limit; sysinfo() free RAM is only a
 * fallback. Uses sysctl on BSD/macOS.
 *
 * @return Available RAM in bytes, or 0 if detection fails.
 */
uint64_t cwist_get_available_ram(void);

/**
 * @brief Check if system RAM is critically low.
 * @param threshold_bytes The minimum bytes that should be available.
 * @return true if available RAM < threshold_bytes.
 */
bool cwist_is_ram_critical(uint64_t threshold_bytes);

#endif
