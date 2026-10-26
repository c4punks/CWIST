/* See loop_client.h. */
#include "loop_client.h"

#include "lwip/init.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/sys.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"

#include <stdlib.h>
#include <string.h>

static uint32_t g_now_ms;

u32_t sys_now(void) { return g_now_ms; }

uint32_t loop_now(void) { return g_now_ms; }

void loop_init(void) {
    g_now_ms = 1;
    lwip_init();
}

void loop_run(uint32_t ms) {
    for (uint32_t t = 0; t <= ms; t += 5) {
        netif_poll_all();
        sys_check_timeouts();
        g_now_ms += 5;
    }
    netif_poll_all();
}

static void flush_pending(loop_client *c) {
    while (c->pcb && c->pending_len > 0) {
        size_t room = tcp_sndbuf(c->pcb);
        if (room == 0) break;
        size_t n = c->pending_len < room ? c->pending_len : room;
        if (tcp_write(c->pcb, c->pending, (u16_t)n, 0) != ERR_OK) break;
        c->pending += n;
        c->pending_len -= n;
    }
    if (c->pcb) tcp_output(c->pcb);
}

static err_t cl_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    loop_client *c = arg;
    (void)err;
    if (!p) {
        c->closed = true;
        return ERR_OK;
    }
    if (c->rx_len + p->tot_len + 1 > c->rx_cap) {
        size_t cap = c->rx_cap ? c->rx_cap : 1024;
        while (cap < c->rx_len + p->tot_len + 1) cap *= 2;
        c->rx = realloc(c->rx, cap);
        c->rx_cap = cap;
    }
    pbuf_copy_partial(p, c->rx + c->rx_len, p->tot_len, 0);
    c->rx_len += p->tot_len;
    c->rx[c->rx_len] = '\0';
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t cl_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    (void)pcb;
    (void)len;
    flush_pending(arg);
    return ERR_OK;
}

static void cl_err(void *arg, err_t err) {
    loop_client *c = arg;
    (void)err;
    c->pcb = NULL;
    c->errored = true;
}

static err_t cl_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    loop_client *c = arg;
    (void)pcb;
    c->connected = err == ERR_OK;
    return ERR_OK;
}

bool loop_connect(loop_client *c, uint16_t port) {
    memset(c, 0, sizeof(*c));
    c->pcb = tcp_new();
    if (!c->pcb) return false;
    tcp_arg(c->pcb, c);
    tcp_recv(c->pcb, cl_recv);
    tcp_sent(c->pcb, cl_sent);
    tcp_err(c->pcb, cl_err);
    ip_addr_t lo;
    IP_ADDR4(&lo, 127, 0, 0, 1);
    if (tcp_connect(c->pcb, &lo, port, cl_connected) != ERR_OK) return false;
    for (int i = 0; i < 100 && !c->connected && !c->errored; i++) loop_run(5);
    return c->connected && !c->errored;
}

void loop_send(loop_client *c, const char *data, size_t len) {
    c->pending = data;
    c->pending_len = len;
    flush_pending(c);
    for (int i = 0; i < 200 && c->pcb && c->pending_len > 0; i++) loop_run(5);
    loop_run(20);
}

int loop_response_count(const loop_client *c) {
    int n = 0;
    for (const char *p = c->rx; p && (p = strstr(p, "HTTP/1.1 ")) != NULL; p += 9) {
        if (strncmp(p + 9, "100 ", 4) != 0) n++;
    }
    return n;
}

/* A response is complete when its head ended and Content-Length body bytes followed. */
static int complete_responses(const loop_client *c) {
    int n = 0;
    const char *p = c->rx, *end = c->rx + c->rx_len;
    while (p && p < end) {
        const char *head = strstr(p, "\r\n\r\n");
        if (!head) break;
        head += 4;
        if (strncmp(p, "HTTP/1.1 100 ", 13) == 0) {
            p = head;
            continue;
        }
        size_t cl = 0;
        const char *h = strstr(p, "Content-Length: ");
        if (h && h < head) cl = strtoul(h + 16, NULL, 10);
        if ((size_t)(end - head) < cl) break;
        n++;
        p = head + cl;
    }
    return n;
}

bool loop_wait_responses(loop_client *c, int n, uint32_t max_ms) {
    for (uint32_t t = 0; t < max_ms; t += 10) {
        if (!c->rx || complete_responses(c) < n) {
            loop_run(10);
            continue;
        }
        return true;
    }
    return c->rx && complete_responses(c) >= n;
}

bool loop_wait_closed(loop_client *c, uint32_t max_ms) {
    for (uint32_t t = 0; t < max_ms && !c->closed && !c->errored; t += 10) loop_run(10);
    return c->closed || c->errored;
}

void loop_close(loop_client *c) {
    if (c->pcb) {
        tcp_arg(c->pcb, NULL);
        tcp_recv(c->pcb, NULL);
        tcp_sent(c->pcb, NULL);
        tcp_err(c->pcb, NULL);
        if (tcp_close(c->pcb) != ERR_OK) tcp_abort(c->pcb);
        c->pcb = NULL;
    }
    free(c->rx);
    c->rx = NULL;
    c->rx_len = c->rx_cap = 0;
    loop_run(20);
}
