/* libFuzzer entry point for HTTP/3 QPACK header validation, sequenced body
 * assembly, WebTransport varints, and QUIC/HTTP/3 packet processing. */
#if defined(__APPLE__)
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#elif !defined(__FreeBSD__) && !defined(__NetBSD__) && !defined(__OpenBSD__) && \
    !defined(__DragonFly__)
#define _POSIX_C_SOURCE 200809L
#endif

#include <lsquic.h>
#include <lsxpack_header.h>

static int g_fuzz_dummy_stream;
static void *g_fuzz_hset = NULL;

static inline void *fuzz_lsquic_stream_get_hset(lsquic_stream_t *s) {
    if (s == (lsquic_stream_t *)&g_fuzz_dummy_stream) return g_fuzz_hset;
    return lsquic_stream_get_hset(s);
}

static inline int fuzz_lsquic_stream_wantread(lsquic_stream_t *s, int is_want) {
    if (s == (lsquic_stream_t *)&g_fuzz_dummy_stream) return 0;
    return lsquic_stream_wantread(s, is_want);
}

static inline int fuzz_lsquic_stream_shutdown(lsquic_stream_t *s, int how) {
    if (s == (lsquic_stream_t *)&g_fuzz_dummy_stream) return 0;
    return lsquic_stream_shutdown(s, how);
}

static inline int fuzz_lsquic_stream_wantwrite(lsquic_stream_t *s, int is_want) {
    if (s == (lsquic_stream_t *)&g_fuzz_dummy_stream) return 0;
    return lsquic_stream_wantwrite(s, is_want);
}

#define lsquic_stream_get_hset fuzz_lsquic_stream_get_hset
#define lsquic_stream_wantread fuzz_lsquic_stream_wantread
#define lsquic_stream_shutdown fuzz_lsquic_stream_shutdown
#define lsquic_stream_wantwrite fuzz_lsquic_stream_wantwrite

#include "../src/net/http/http3.c"

static cwist_http3_context *g_fuzz_h3_ctx = NULL;
static lsquic_engine_t *g_fuzz_h3_engine = NULL;
static struct lsquic_engine_settings g_fuzz_h3_settings;
static struct lsquic_engine_api g_fuzz_h3_api;

static int fuzz_packets_out(void *ctx, const struct lsquic_out_spec *specs, unsigned n_specs) {
    (void)ctx;
    (void)specs;
    return (int)n_specs;
}

static void fuzz_h3_handler(void *user_ctx, cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    (void)user_ctx;
    if (res && res->body) {
        res->status_code = CWIST_HTTP_OK;
        cwist_sstring_assign(res->body, "ok");
    }
}

static void fuzz_h3_cleanup(void) {
    if (g_fuzz_h3_engine) {
        lsquic_engine_destroy(g_fuzz_h3_engine);
        g_fuzz_h3_engine = NULL;
    }
    if (g_fuzz_h3_ctx) {
        cwist_h3_hset_sweep(g_fuzz_h3_ctx);
        cwist_http3_destroy_context(g_fuzz_h3_ctx);
        g_fuzz_h3_ctx = NULL;
    }
}

static void fuzz_h3_ensure_engine(void) {
    if (g_fuzz_h3_engine) return;
    cwist_error_t err = cwist_http3_init_context_ephemeral(&g_fuzz_h3_ctx);
    cwist_error_dispose(&err);
    if (!g_fuzz_h3_ctx) return;
    g_fuzz_h3_ctx->handler = fuzz_h3_handler;

    lsquic_engine_init_settings(&g_fuzz_h3_settings, LSENG_SERVER | LSENG_HTTP);
    g_fuzz_h3_settings.es_ql_bits = 0;

    memset(&g_fuzz_h3_api, 0, sizeof(g_fuzz_h3_api));
    g_fuzz_h3_api.ea_settings = &g_fuzz_h3_settings;
    g_fuzz_h3_api.ea_stream_if = &cwist_h3_stream_if;
    g_fuzz_h3_api.ea_stream_if_ctx = g_fuzz_h3_ctx;
    g_fuzz_h3_api.ea_packets_out = fuzz_packets_out;
    g_fuzz_h3_api.ea_packets_out_ctx = NULL;
    g_fuzz_h3_api.ea_get_ssl_ctx = cwist_h3_get_ssl_ctx;
    g_fuzz_h3_api.ea_hsi_if = &cwist_h3_hset_if;
    g_fuzz_h3_api.ea_hsi_ctx = g_fuzz_h3_ctx;

    g_fuzz_h3_engine = lsquic_engine_new(LSENG_SERVER | LSENG_HTTP, &g_fuzz_h3_api);
    atexit(fuzz_h3_cleanup);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 128 * 1024) return 0;

    /* 1. Response header name normalization and value safety checks. */
    if (size > 0 && size <= 1024) {
        char *str = (char *)cwist_alloc(size + 1);
        if (str) {
            memcpy(str, data, size);
            str[size] = '\0';
            char norm[256];
            (void)cwist_http3_normalize_response_header_name(str, norm, sizeof(norm));
            (void)cwist_http3_response_header_value_is_safe(str);
            cwist_free(str);
        }
    }

#ifdef CWIST_WT_STOCK_LSQUIC
    /* 2. WebTransport QUIC varint read/write round-trip. */
    {
        uint64_t val = 0;
        size_t consumed = h3_wt_varint_read(data, size, &val);
        if (consumed > 0 && val < (UINT64_C(1) << 62)) {
            unsigned char vbuf[8];
            size_t written = h3_wt_varint_write(vbuf, val);
            uint64_t val2 = 0;
            (void)h3_wt_varint_read(vbuf, written, &val2);
        }
    }
#endif

    /* 3. HTTP/3 QPACK header-set callbacks & RFC 9114 Section 4.3.1 validation. */
    lsquic_stream_t *dummy_stream = (lsquic_stream_t *)&g_fuzz_dummy_stream;
    cwist_h3_hset_t *hset = (cwist_h3_hset_t *)cwist_h3_hsi_create(NULL, dummy_stream, 0);
    if (hset) {
        size_t off = 0;
        while (off + 2 <= size && hset->count < H3_MAX_HEADERS) {
            size_t nlen = data[off];
            size_t vlen = data[off + 1];
            off += 2;
            if (nlen > size - off) nlen = size - off;
            const uint8_t *nptr = data + off;
            off += nlen;
            if (vlen > size - off) vlen = size - off;
            const uint8_t *vptr = data + off;
            off += vlen;

            size_t req_space = nlen + vlen;
            struct lsxpack_header *xhdr = cwist_h3_hsi_prepare(hset, NULL, req_space);
            if (!xhdr) break;
            if (req_space > 0) {
                memcpy(hset->decode_buf + hset->decode_off, nptr, nlen);
                memcpy(hset->decode_buf + hset->decode_off + nlen, vptr, vlen);
            }
            lsxpack_header_set_offset2(xhdr, hset->decode_buf + hset->decode_off, 0, nlen, nlen,
                                       vlen);
            if (cwist_h3_hsi_process_header(hset, xhdr) != 0) break;
        }
        (void)cwist_h3_hsi_process_header(hset, NULL);

        h3_stream_ctx_t st;
        memset(&st, 0, sizeof(st));
        st.stream = dummy_stream;
        st.req = cwist_http_request_create();
        if (st.req) {
            cwist_sstring_assign(st.req->version, "HTTP/3");
            g_fuzz_hset = hset;
            (void)h3_process_stream_headers(dummy_stream, &st);
            g_fuzz_hset = NULL;
        }
        if (st.res) cwist_http_response_destroy(st.res);
        if (st.req) cwist_http_request_destroy(st.req);
        cwist_h3_hsi_discard(hset);
    }

    /* 4. Sequenced body chunk reassembly (h3_seq_append_and_feed). */
    {
        h3_stream_ctx_t st;
        memset(&st, 0, sizeof(st));
        size_t pos = 0;
        while (pos < size) {
            size_t step = (size_t)data[pos] + 1;
            pos++;
            if (step > size - pos) step = size - pos;
            if (h3_seq_append_and_feed(&st, data + pos, step) != 0) break;
            pos += step;
        }
        cwist_seq_assembler_destroy(st.body_assembler);
        free(st.seq_buf);
    }

    /* 5. QUIC + HTTP/3 packet-in processing on an active lsquic server engine. */
    if (size > 0 && size <= 1500) {
        fuzz_h3_ensure_engine();
        if (g_fuzz_h3_engine) {
            struct sockaddr_in local_sa;
            struct sockaddr_in peer_sa;
            memset(&local_sa, 0, sizeof(local_sa));
            memset(&peer_sa, 0, sizeof(peer_sa));
            local_sa.sin_family = AF_INET;
            local_sa.sin_port = htons(4433);
            local_sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            peer_sa.sin_family = AF_INET;
            peer_sa.sin_port = htons(54321);
            peer_sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            (void)lsquic_engine_packet_in(g_fuzz_h3_engine, data, size,
                                          (struct sockaddr *)&local_sa, (struct sockaddr *)&peer_sa,
                                          g_fuzz_h3_ctx, 0);
            lsquic_engine_process_conns(g_fuzz_h3_engine);
            if (lsquic_engine_has_unsent_packets(g_fuzz_h3_engine)) {
                lsquic_engine_send_unsent_packets(g_fuzz_h3_engine);
            }
        }
    }

    return 0;
}
