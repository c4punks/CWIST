/* Minimal Cortex-M "board" for the bare-metal smoke firmware: startup code,
 * a bounded _sbrk heap with high-water accounting, semihosting output, stack
 * painting, and the <cwist/sys/port.h> hooks.
 *
 * Under qemu-arm (user mode, BOARD_QEMU_USER) the emulator loads .data at its
 * VMA and owns SP and the FPU, so the reset path skips the LMA copy and the
 * CPACR write. On real hardware the entropy hook must read the RNG
 * peripheral; the xorshift here only exists to make the smoke deterministic.
 */
#include <cwist/sys/port.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern char _heap_start, _heap_end;
extern int main(void);
extern void __libc_init_array(void);
void board_exit(int code);

void Reset_Handler(void) {
#if defined(__ARM_FP) && !defined(BOARD_QEMU_USER)
    *(volatile uint32_t *)0xE000ED88 |= (0xFu << 20); /* CPACR: enable CP10/CP11 */
    __asm volatile("dsb\n\tisb");
#endif
#ifndef BOARD_QEMU_USER
    memcpy(&_sdata, &_sidata, (size_t)((char *)&_edata - (char *)&_sdata));
#endif
    memset(&_sbss, 0, (size_t)((char *)&_ebss - (char *)&_sbss));
    __libc_init_array();
    board_exit(main());
}

static void Default_Handler(void) {
    for (;;) {
    }
}

__attribute__((section(".isr_vector"), used)) const void *board_vectors[16] = {
    &_estack,
    (void *)Reset_Handler,
    (void *)Default_Handler, /* NMI */
    (void *)Default_Handler, /* HardFault */
    (void *)Default_Handler, /* MemManage */
    (void *)Default_Handler, /* BusFault */
    (void *)Default_Handler, /* UsageFault */
};

/* --- semihosting ------------------------------------------------------------ */

static int semihost(int op, const void *arg) {
    register int r0 __asm("r0") = op;
    register const void *r1 __asm("r1") = arg;
    __asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

void board_log(const char *s) { semihost(0x04 /* SYS_WRITE0 */, s); }

void board_exit(int code) {
    uint32_t args[2] = {0x20026 /* ADP_Stopped_ApplicationExit */, (uint32_t)code};
    semihost(0x20 /* SYS_EXIT_EXTENDED */, args);
    for (;;) {
    }
}

/* newlib's stdio and abort() land here. */
int _write(int fd, const char *buf, int len) {
    (void)fd; /* stdout and stderr both go to the debug console */
    for (int i = 0; i < len; i++) semihost(0x03 /* SYS_WRITEC */, &buf[i]);
    return len;
}

void _exit(int code) { board_exit(code); }

/* -nostartfiles drops crti/crtn; __libc_init_array still calls these. */
void _init(void) {}
void _fini(void) {}

/* --- heap ----------------------------------------------------------------- */

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

size_t board_heap_high_water(void) { return heap_high; }

/* --- stack high-water: paint below the current SP, scan after the run ------ */

#define BOARD_STACK_PAINT (48u * 1024u)
static uint32_t *paint_lo, *paint_hi;

__attribute__((noinline)) void board_stack_paint(void) {
    uint32_t *sp;
    __asm volatile("mov %0, sp" : "=r"(sp));
    paint_hi = sp - 64; /* keep clear of this frame */
    paint_lo = paint_hi - BOARD_STACK_PAINT / 4;
    for (uint32_t *p = paint_lo; p < paint_hi; p++) *p = 0xDEADBEEFu;
}

size_t board_stack_used(void) {
    uint32_t *p = paint_lo;
    while (p < paint_hi && *p == 0xDEADBEEFu) p++;
    return (size_t)((char *)paint_hi - (char *)p);
}

/* --- CWIST port hooks ------------------------------------------------------- */

static uint64_t fake_ns;
static uint32_t rng_state = 0x9E3779B9u;
unsigned board_entropy_calls;

uint64_t cwist_port_monotonic_ns(void) {
    fake_ns += 1000; /* SysTick/DWT would back this on hardware */
    return fake_ns;
}

int cwist_port_entropy(void *buf, size_t len) {
    board_entropy_calls++;
    uint8_t *out = buf;
    for (size_t i = 0; i < len; i++) {
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 17;
        rng_state ^= rng_state << 5;
        out[i] = (uint8_t)rng_state;
    }
    return 0;
}

#ifdef BOARD_RTC
/* An RTC behind newlib's time(); without it time() fails and CWIST omits Date. */
int _gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    tv->tv_sec = 1792886400 + (time_t)(fake_ns / 1000000000u); /* 2026-10-25 */
    tv->tv_usec = 0;
    return 0;
}
#endif
