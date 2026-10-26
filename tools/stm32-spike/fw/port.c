/* Phase 0 spike port layer: startup, heap accounting, semihosting log, and
 * ENOSYS stubs for every POSIX symbol the core still references. */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

/* ---- startup ---------------------------------------------------------- */
extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern int main(void);
extern void __libc_init_array(void);
void spike_exit(int code);
void spike_tls_init(void);

void Reset_Handler(void) {
#if defined(__ARM_FP) && !defined(SPIKE_QEMU_USER)
    /* enable CP10/CP11 (FPU) before any float code runs */
    *(volatile uint32_t *)0xE000ED88 |= (0xFu << 20);
    __asm volatile("dsb; isb");
#endif
#ifndef SPIKE_QEMU_USER /* qemu-arm loads .data at its VMA already; the LMA copy is not mapped */
    memcpy(&_sdata, &_sidata, (size_t)((char *)&_edata - (char *)&_sdata));
#endif
    memset(&_sbss, 0, (size_t)((char *)&_ebss - (char *)&_sbss));
    spike_tls_init();
    __libc_init_array();
    spike_exit(main());
}

static void Default_Handler(void) {
    for (;;) {
    }
}

__attribute__((section(".isr_vector"), used)) const void *spike_vectors[16] = {
    &_estack, (void *)Reset_Handler, (void *)Default_Handler, (void *)Default_Handler,
    (void *)Default_Handler, (void *)Default_Handler, (void *)Default_Handler,
};

/* ---- semihosting ------------------------------------------------------ */
static int semihost(int op, const void *arg) {
    register int r0 __asm("r0") = op;
    register const void *r1 __asm("r1") = arg;
    __asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

void spike_log(const char *s) { semihost(0x04 /* SYS_WRITE0 */, s); }

void spike_exit(int code) {
    /* ADP_Stopped_ApplicationExit with exit code (SYS_EXIT_EXTENDED) */
    uint32_t args[2] = {0x20026, (uint32_t)code};
    semihost(0x20 /* SYS_EXIT_EXTENDED */, args);
    for (;;) {
    }
}

/* ---- heap ------------------------------------------------------------- */
extern char _heap_start, _heap_end;
static char *heap_brk = &_heap_start;
static size_t heap_high;

void *_sbrk(ptrdiff_t incr) {
    char *prev = heap_brk;
    if (heap_brk + incr > &_heap_end) {
        errno = ENOMEM;
        return (void *)-1;
    }
    heap_brk += incr;
    if ((size_t)(heap_brk - &_heap_start) > heap_high) heap_high = (size_t)(heap_brk - &_heap_start);
    return prev;
}

size_t spike_heap_high_water(void) { return heap_high; }

/* ---- time: a fake monotonic tick (SysTick would back this on hardware) --- */
static uint64_t fake_ns;
int clock_gettime(clockid_t id, struct timespec *ts) {
    (void)id;
    fake_ns += 1000;
    ts->tv_sec = (time_t)(fake_ns / 1000000000u);
    ts->tv_nsec = (long)(fake_ns % 1000000000u);
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    (void)rem;
    fake_ns += (uint64_t)req->tv_sec * 1000000000u + (uint64_t)req->tv_nsec;
    return 0;
}

int sched_yield(void) { return 0; }

time_t timegm(struct tm *tm) {
    /* days-from-civil, UTC */
    int y = tm->tm_year + 1900, m = tm->tm_mon + 1;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)tm->tm_mday - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    long days = (long)era * 146097L + (long)doe - 719468L;
    return (time_t)(days * 86400L + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec);
}

/* ---- ENOSYS stubs: reaching any of these on hardware is a port bug ------- */
#define ENOSYS_STUB(ret, name, ...)                                                               \
    ret name(__VA_ARGS__) {                                                                        \
        spike_log("ENOSYS: " #name "\n");                                                          \
        errno = ENOSYS;                                                                            \
        return (ret)-1;                                                                            \
    }

struct msghdr;
struct pollfd;
typedef struct spike_dir DIR;
struct dirent;

ENOSYS_STUB(ssize_t, recv, int a, void *b, size_t c, int d)
ENOSYS_STUB(ssize_t, send, int a, const void *b, size_t c, int d)
ENOSYS_STUB(ssize_t, sendmsg, int a, const struct msghdr *b, int c)
ENOSYS_STUB(int, shutdown, int a, int b)
ENOSYS_STUB(int, poll, struct pollfd *a, unsigned b, int c)
ENOSYS_STUB(int, mprotect, void *a, size_t b, int c)
ENOSYS_STUB(int, munmap, void *a, size_t b)
ENOSYS_STUB(int, closedir, DIR *a)

void *mmap(void *a, size_t b, int c, int d, int e, long f) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    spike_log("ENOSYS: mmap\n");
    errno = ENOSYS;
    return (void *)-1;
}
DIR *opendir(const char *p) { (void)p; errno = ENOSYS; return NULL; }
struct dirent *readdir(DIR *d) { (void)d; return NULL; }

/* ---- thread-local storage: one static block, since there is one thread ---- */
/* ARM EABI variant I: TP points at an 8-byte TCB, the TLS image follows. */
extern char _stdata, _etdata, _stbss, _etbss;
#define SPIKE_TLS_BYTES 8192
static uint8_t spike_tls[8 + SPIKE_TLS_BYTES] __attribute__((aligned(8)));

void spike_tls_init(void) {
    size_t td = (size_t)(&_etdata - &_stdata), tb = (size_t)(&_etbss - &_stbss);
    if (td + tb > SPIKE_TLS_BYTES) {
        spike_log("TLS image too large\n");
        spike_exit(2);
    }
    memcpy(spike_tls + 8, &_stdata, td);
    memset(spike_tls + 8 + td, 0, tb);
}

__attribute__((naked)) void *__aeabi_read_tp(void) {
    /* must preserve every register except r0 */
    __asm volatile("ldr r0, =spike_tls\n bx lr");
}

/* ---- 64-bit atomics: Cortex-M has no LDREXD, so mask interrupts ---------- */
static inline uint32_t irq_save(void) {
    uint32_t p;
    __asm volatile("mrs %0, primask\n cpsid i" : "=r"(p)::"memory");
    return p;
}
static inline void irq_restore(uint32_t p) { __asm volatile("msr primask, %0" ::"r"(p) : "memory"); }

uint64_t __atomic_load_8(const volatile void *m, int o) {
    (void)o;
    uint32_t p = irq_save();
    uint64_t v = *(const volatile uint64_t *)m;
    irq_restore(p);
    return v;
}
void __atomic_store_8(volatile void *m, uint64_t v, int o) {
    (void)o;
    uint32_t p = irq_save();
    *(volatile uint64_t *)m = v;
    irq_restore(p);
}
uint64_t __atomic_fetch_add_8(volatile void *m, uint64_t v, int o) {
    (void)o;
    uint32_t p = irq_save();
    uint64_t old = *(volatile uint64_t *)m;
    *(volatile uint64_t *)m = old + v;
    irq_restore(p);
    return old;
}
_Bool __atomic_compare_exchange_8(volatile void *m, void *exp, uint64_t des, int s, int f) {
    (void)s;
    (void)f;
    uint32_t p = irq_save();
    uint64_t cur = *(volatile uint64_t *)m;
    _Bool ok = cur == *(uint64_t *)exp;
    if (ok) *(volatile uint64_t *)m = des;
    else *(uint64_t *)exp = cur;
    irq_restore(p);
    return ok;
}

void _init(void) {}
void _fini(void) {}

/* ---- stack high-water: paint below the current SP, scan after the run ---- */
#define SPIKE_STACK_PAINT (48u * 1024u)
static uint32_t *paint_lo, *paint_hi;

__attribute__((noinline)) void spike_stack_paint(void) {
    uint32_t *sp;
    __asm volatile("mov %0, sp" : "=r"(sp));
    paint_hi = sp - 64; /* leave this frame alone */
    paint_lo = paint_hi - SPIKE_STACK_PAINT / 4;
    for (uint32_t *p = paint_lo; p < paint_hi; p++) *p = 0xDEADBEEFu;
}

size_t spike_stack_used(void) {
    uint32_t *p = paint_lo;
    while (p < paint_hi && *p == 0xDEADBEEFu) p++;
    return (size_t)((char *)paint_hi - (char *)p);
}

/* ---- wall clock: an RTC would back this; without it time() is -1 and CWIST
 * emits a Date header of NUL bytes (see the Phase 0 report). ---------------- */
struct timeval;
int _gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    struct { long long tv_sec; long tv_usec; } *t = (void *)tv;
    if (t) {
        t->tv_sec = 1792886400LL + (long long)(fake_ns / 1000000000u); /* 2026-10-25 */
        t->tv_usec = 0;
    }
    return 0;
}
