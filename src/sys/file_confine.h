#ifndef CWIST_SYS_FILE_CONFINE_H
#define CWIST_SYS_FILE_CONFINE_H

#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static inline bool cwist_file_path_is_inside(const char *path, const char *root) {
    size_t root_len = strlen(root);
    if (root_len == 1 && root[0] == '/') return true;
    return strncmp(path, root, root_len) == 0 &&
           (path[root_len] == '\0' || path[root_len] == '/');
}

static inline bool cwist_file_fd_is_inside(int fd, const char *root) {
    char actual[PATH_MAX];
#if defined(__linux__)
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(proc_path, actual, sizeof(actual) - 1);
    if (n < 0) return false;
    actual[n] = '\0';
#elif defined(__APPLE__)
    if (fcntl(fd, F_GETPATH, actual) != 0) return false;
#elif defined(__FreeBSD__) || defined(__NetBSD__)
    char dev_path[64];
    snprintf(dev_path, sizeof(dev_path), "/dev/fd/%d", fd);
    if (!realpath(dev_path, actual)) return false;
#else
    return false;
#endif
    return cwist_file_path_is_inside(actual, root);
}

static inline int cwist_file_open_confined(const char *root, const char *path) {
    if (!root || !path) return -1;
    char canonical[PATH_MAX];
    if (!realpath(path, canonical) || !cwist_file_path_is_inside(canonical, root)) return -1;
    int fd = open(canonical, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || !cwist_file_fd_is_inside(fd, root)) {
        close(fd);
        return -1;
    }
    return fd;
}

#endif
