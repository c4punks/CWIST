#define _POSIX_C_SOURCE 200809L

#include <cwist/https.h>
#include <cwist/sstring.h>
#include <cwist/err/cwist_err.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>

/* --- Internal Error Helpers --- */

static cwist_error_t make_ssl_error(const char *msg) {
    cwist_error_t err = make_error(CWIST_ERR_JSON);
    err.error.err_json = cJSON_CreateObject();
    
    unsigned long ssl_err = ERR_get_error();
    char buf[256];
    ERR_error_string_n(ssl_err, buf, sizeof(buf));
    
    cJSON_AddStringToObject(err.error.err_json, "module", "https");
    cJSON_AddStringToObject(err.error.err_json, "message", msg);
    cJSON_AddStringToObject(err.error.err_json, "openssl_error", buf);
    
    return err;
}

/* --- Context Management --- */

cwist_error_t cwist_https_init_context(cwist_https_context **ctx, const char *cert_path, const char *key_path) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    
    if (!ctx || !cert_path || !key_path) {
        err.error.err_i16 = -1;
        return err;
    }

    // Initialize OpenSSL
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    const SSL_METHOD *method = TLS_server_method();
    SSL_CTX *ssl_ctx = SSL_CTX_new(method);
    if (!ssl_ctx) {
        return make_ssl_error("Unable to create SSL context");
    }

    // Load Cert and Key
    if (SSL_CTX_use_certificate_file(ssl_ctx, cert_path, SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Unable to load certificate");
    }

    if (SSL_CTX_use_PrivateKey_file(ssl_ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Unable to load private key");
    }

    // Verify key matches cert
    if (!SSL_CTX_check_private_key(ssl_ctx)) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Private key does not match certificate");
    }

    *ctx = (cwist_https_context*)malloc(sizeof(cwist_https_context));
    if (!*ctx) {
        SSL_CTX_free(ssl_ctx);
        err.error.err_i16 = -1;
        return err;
    }
    (*ctx)->ctx = ssl_ctx;

    err.error.err_i16 = 0; // Success
    return err;
}

void cwist_https_destroy_context(cwist_https_context *ctx) {
    if (ctx) {
        if (ctx->ctx) {
            SSL_CTX_free(ctx->ctx);
        }
        free(ctx);
        EVP_cleanup();
    }
}

/* --- Connection Handling --- */

cwist_error_t cwist_https_accept(cwist_https_context *ctx, int client_fd, cwist_https_connection **conn) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    
    if (!ctx || !ctx->ctx || client_fd < 0) {
        err.error.err_i16 = -1;
        return err;
    }

    SSL *ssl = SSL_new(ctx->ctx);
    if (!ssl) {
        return make_ssl_error("Failed to create SSL structure");
    }

    SSL_set_fd(ssl, client_fd);

    if (SSL_accept(ssl) <= 0) {
        // Handshake failed
        // We capture the error before freeing
        cwist_error_t ssl_err = make_ssl_error("SSL handshake failed");
        SSL_free(ssl);
        return ssl_err;
    }

    *conn = (cwist_https_connection*)malloc(sizeof(cwist_https_connection));
    if (!*conn) {
        SSL_free(ssl);
        err.error.err_i16 = -1;
        return err;
    }

    (*conn)->fd = client_fd;
    (*conn)->ssl = ssl;

    err.error.err_i16 = 0;
    return err;
}

void cwist_https_close_connection(cwist_https_connection *conn) {
    if (conn) {
        if (conn->ssl) {
            SSL_shutdown(conn->ssl);
            SSL_free(conn->ssl);
        }
        if (conn->fd >= 0) {
            close(conn->fd);
        }
        free(conn);
    }
}

/* --- I/O Operations --- */

cwist_http_request *cwist_https_receive_request(cwist_https_connection *conn) {
    if (!conn || !conn->ssl) return NULL;

    // Read buffer
    // For simplicity, we read up to 8KB. Real implementations handle buffering/chunking.
    char buffer[8192]; 
    int bytes = SSL_read(conn->ssl, buffer, sizeof(buffer) - 1);
    
    if (bytes <= 0) {
        // Error or closed
        return NULL;
    }

    buffer[bytes] = '\0';
    
    // Delegate to existing HTTP parser
    return cwist_http_parse_request(buffer);
}

cwist_error_t cwist_https_send_response(cwist_https_connection *conn, cwist_http_response *res) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    if (!conn || !conn->ssl || !res) {
        err.error.err_i16 = -1;
        return err;
    }

    // 1. Serialize using existing HTTP logic
    cwist_sstring *response_str = cwist_http_stringify_response(res);
    if (!response_str) {
        err.error.err_i16 = -1;
        return err;
    }

    // 2. Send over SSL
    const char *p = response_str->data;
    int left = (int)response_str->size;
    int total_sent = 0;

    err.error.err_i16 = 0; // Assume success initially

    while (left > 0) {
        int sent = SSL_write(conn->ssl, p, left);
        if (sent <= 0) {
            int ssl_err = SSL_get_error(conn->ssl, sent);
            if (ssl_err == SSL_ERROR_WANT_WRITE || ssl_err == SSL_ERROR_WANT_READ) {
                continue; // Retry
            }
            err = make_ssl_error("SSL write failed");
            break;
        }
        p += sent;
        left -= sent;
        total_sent += sent;
    }

    cwist_sstring_destroy(response_str);
    return err;
}

/* --- Server Loop --- */

cwist_error_t cwist_https_server_loop(int server_fd, cwist_https_context *ctx, void (*handler)(cwist_https_connection *conn)) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    
    if (server_fd < 0 || !ctx || !handler) {
        err.error.err_i16 = -1;
        return err;
    }

    while (1) {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&addr, &len);

        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("Unable to accept");
            continue; 
        }

        // Fork or Thread could go here. For now, iterative.
        // Doing handshake in the main loop blocks other connections.
        // Ideally this should be threaded/forked as in http.c
        
        cwist_https_connection *conn = NULL;
        cwist_error_t hs_err = cwist_https_accept(ctx, client_fd, &conn);
        
        if (hs_err.errtype == CWIST_ERR_INT16 && hs_err.error.err_i16 == 0) {
            // Success
            handler(conn);
            cwist_https_close_connection(conn);
        } else {
            // Handshake failure
            if (hs_err.errtype == CWIST_ERR_JSON) {
                char *json_str = cJSON_Print(hs_err.error.err_json);
                fprintf(stderr, "HTTPS Error: %s\n", json_str);
                free(json_str);
                cJSON_Delete(hs_err.error.err_json);
            }
            close(client_fd);
        }
    }
    
    return err;
}
