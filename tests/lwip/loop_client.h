/* In-process HTTP client for the lwIP adapter tests: raw-API client pcbs
 * talking to the adapter over lwIP's loopback netif, driven by a fake
 * millisecond clock (NO_SYS, no threads). Shared by tests/test_lwip_adapter.c
 * (host) and tests/baremetal/hello_lwip.c (Cortex-M under qemu-arm). */
#ifndef CWIST_TEST_LOOP_CLIENT_H
#define CWIST_TEST_LOOP_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct loop_client {
    struct tcp_pcb *pcb;
    bool connected;
    bool closed;   /* peer sent FIN */
    bool errored;  /* reset or aborted */
    char *rx;      /* everything received, NUL-terminated */
    size_t rx_len;
    size_t rx_cap;
    const char *pending; /* bytes not yet accepted by tcp_write */
    size_t pending_len;
} loop_client;

/** lwip_init() and reset the fake clock. */
void loop_init(void);
/** Advance the fake clock by @p ms, delivering loopback packets and timers. */
void loop_run(uint32_t ms);
/** Current fake time (also what sys_now() returns). */
uint32_t loop_now(void);

/** Connect to 127.0.0.1:@p port; runs the loop until connected or failed. */
bool loop_connect(loop_client *c, uint16_t port);
/** Queue @p len bytes (kept by reference until sent) and run until flushed. */
void loop_send(loop_client *c, const char *data, size_t len);
/** Run until @p c has received at least @p n responses or @p max_ms elapse. */
bool loop_wait_responses(loop_client *c, int n, uint32_t max_ms);
/** Run until the peer closes or resets @p c, or @p max_ms elapse. */
bool loop_wait_closed(loop_client *c, uint32_t max_ms);
/** Count complete "HTTP/1.1 " status lines (excluding 100 Continue) in rx. */
int loop_response_count(const loop_client *c);
/** Close (or abort) the client pcb and free its buffer. */
void loop_close(loop_client *c);

#endif /* CWIST_TEST_LOOP_CLIENT_H */
