#ifndef SPIKE_SYS_MMAN_H
#define SPIKE_SYS_MMAN_H
#include <stddef.h>
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_FAILED ((void *)-1)
void *mmap(void *, size_t, int, int, int, long);
int munmap(void *, size_t);

int mprotect(void *, size_t, int);
#define PROT_NONE 0
#endif
