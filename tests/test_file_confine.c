#define _POSIX_C_SOURCE 200809L
#include "../src/sys/file_confine.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char link_path[PATH_MAX];
static char inside_path[PATH_MAX];
static char secret_path[PATH_MAX];
static atomic_bool stop_swap;

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(text, 1, strlen(text), f) == strlen(text));
    assert(fclose(f) == 0);
}

static void *swap_links(void *arg) {
    (void)arg;
    bool outside = false;
    while (!atomic_load_explicit(&stop_swap, memory_order_relaxed)) {
        unlink(link_path);
        assert(symlink(outside ? secret_path : inside_path, link_path) == 0);
        outside = !outside;
    }
    return NULL;
}

int main(void) {
    char base[] = "/tmp/cwist-file-confine-XXXXXX";
    char root[64], outside[64], canonical_root[PATH_MAX];
    assert(mkdtemp(base));
    snprintf(root, sizeof(root), "%s/root", base);
    snprintf(outside, sizeof(outside), "%s/outside", base);
    snprintf(inside_path, sizeof(inside_path), "%s/inside.txt", root);
    snprintf(secret_path, sizeof(secret_path), "%s/secret.txt", outside);
    snprintf(link_path, sizeof(link_path), "%s/swap.txt", root);
    assert(mkdir(root, 0700) == 0);
    assert(mkdir(outside, 0700) == 0);
    write_text(inside_path, "INSIDE");
    write_text(secret_path, "TOPSECRET");
    assert(realpath(root, canonical_root));

    int fd = cwist_file_open_confined(canonical_root, inside_path);
    assert(fd >= 0);
    char buf[16] = {0};
    assert(read(fd, buf, 6) == 6 && strcmp(buf, "INSIDE") == 0);
    close(fd);
    assert(symlink(secret_path, link_path) == 0);
    assert(cwist_file_open_confined(canonical_root, link_path) == -1);
    fd = open(secret_path, O_RDONLY);
    assert(fd >= 0 && !cwist_file_fd_is_inside(fd, canonical_root));
    close(fd);

    unlink(link_path);
    assert(symlink(inside_path, link_path) == 0);
    pthread_t swapper;
    assert(pthread_create(&swapper, NULL, swap_links, NULL) == 0);
    for (int i = 0; i < 2000; i++) {
        fd = cwist_file_open_confined(canonical_root, link_path);
        if (fd < 0) continue;
        memset(buf, 0, sizeof(buf));
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        assert(n < 0 || strcmp(buf, "TOPSECRET") != 0);
        close(fd);
    }
    atomic_store(&stop_swap, true);
    assert(pthread_join(swapper, NULL) == 0);

    unlink(link_path);
    unlink(inside_path);
    unlink(secret_path);
    assert(rmdir(root) == 0);
    assert(rmdir(outside) == 0);
    assert(rmdir(base) == 0);
    puts("confined descriptor checks reject symlink escapes: ok");
    return 0;
}
