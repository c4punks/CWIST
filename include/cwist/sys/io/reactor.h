#ifndef __CWIST_SYS_IO_REACTOR_H__
#define __CWIST_SYS_IO_REACTOR_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*cwist_reactor_cb_t)(int fd, void *ctx);

typedef struct cwist_reactor cwist_reactor_t;

/* Per-event inline payload capacity. The reactor copies the caller's
 * payload into a pooled, zero-initialized slot at add/mod time, so the
 * callback never depends on caller stack or heap lifetime. */
#define CWIST_REACTOR_PAYLOAD_SIZE 64

cwist_reactor_t *cwist_reactor_create(void);
void cwist_reactor_destroy(cwist_reactor_t *reactor);

/* cb receives a pointer to the slot's inline payload (the copied bytes),
 * never NULL. Slots are zeroed on checkout, preserving calloc semantics. */
bool cwist_reactor_add(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb,
                       const void *payload, size_t payload_size);
bool cwist_reactor_mod(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb,
                       const void *payload, size_t payload_size);
bool cwist_reactor_del(cwist_reactor_t *reactor, int fd);

void cwist_reactor_run(cwist_reactor_t *reactor);
void cwist_reactor_stop(cwist_reactor_t *reactor);

/* Foreign-thread completion posting: cwist_reactor_post pushes a node onto a
 * Treiber MPSC stack and wakes the reactor's run thread through an internal
 * eventfd (Linux) / pipe (BSD/macOS).  The run thread drains the stack and
 * invokes each node's callback on the owner thread.  The node is caller-owned
 * (typically embedded in the completion object) and may be recycled once its
 * callback has run. */
typedef struct cwist_reactor_post {
    struct cwist_reactor_post *next;
    void (*cb)(void *ctx);
    void *ctx;
} cwist_reactor_post_t;

bool cwist_reactor_post(cwist_reactor_t *reactor, cwist_reactor_post_t *node);

/* One-shot timers run on the reactor's run thread.  The timer is caller-owned
 * (typically embedded in the object it belongs to); the reactor keeps a
 * min-heap of armed timers and shortens its poll wait to the earliest
 * deadline, so an idle reactor with no armed timers never wakes for them.
 *
 * Arm, cancel and the callback all run on the run thread (or before the
 * reactor starts running).  Re-arming from inside the callback is allowed.
 * Foreign threads reach a timer through cwist_reactor_post(). */
typedef struct cwist_reactor_timer {
    uint64_t deadline_ns; /* CLOCK_MONOTONIC */
    uint32_t heap_slot;   /* 0 when not armed, else heap index + 1 */
    void (*cb)(void *ctx);
    void *ctx;
} cwist_reactor_timer_t;

void cwist_reactor_timer_init(cwist_reactor_timer_t *timer, void (*cb)(void *ctx), void *ctx);
/* (Re-)arm @p timer to fire once after @p delay_us microseconds. */
bool cwist_reactor_timer_arm(cwist_reactor_t *reactor, cwist_reactor_timer_t *timer,
                             uint64_t delay_us);
void cwist_reactor_timer_cancel(cwist_reactor_t *reactor, cwist_reactor_timer_t *timer);
bool cwist_reactor_timer_armed(const cwist_reactor_timer_t *timer);

#ifdef __cplusplus
}
#endif

#endif
