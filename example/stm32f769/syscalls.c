/* newlib syscalls: only _sbrk (heap over the ld-provided `end`); _write lives
 * in port.c (UART), the rest comes from nosys.specs stubs. */
#include <errno.h>
#include <sys/types.h>

extern char end; /* set by the linker script at the end of bss */
static char *heap_brk;
extern char _estack;

caddr_t _sbrk(int incr) {
    if (!heap_brk) heap_brk = &end;
    char *prev = heap_brk;
    if (heap_brk + incr > &_estack - 0x4000) { /* keep the reserved stack clear */
        errno = ENOMEM;
        return (caddr_t)-1;
    }
    heap_brk += incr;
    return (caddr_t)prev;
}

/* -nostartfiles drops crti/crtn; __libc_init_array still references these. */
void _init(void) {}
void _fini(void) {}
