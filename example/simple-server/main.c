#include <cwist/http.h>
#include <cwist/sstring.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define BUFFER_SIZE 8192
#define PORT 8080

// Return index (after "\r\n\r\n") or -1 if not found
static int find_header_end(const char *buf, size_t len) {
    for (size_t i = 3; i < len; i++) {
        if (buf[i - 3] == '\r' && buf[i - 2] == '\n' && buf[i - 1] == '\r' && buf[i] == '\n')
            return (int)(i + 1);
    }
    return -1;
}

// Very small Content-Length parser (headers only). Returns -1 if missing/invalid.
static long parse_content_length(const char *headers, size_t header_len) {
    const char *p = headers;
    const char *end = headers + header_len;

    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) break;

        if (strncasecmp(p, "Content-Length:", 15) == 0) {
            const char *q = p + 15;
            while (q < line_end && (*q == ' ' || *q == '\t')) q++;
            char *stop = NULL;
            long v = strtol(q, &stop, 10);
            if (stop == q || v < 0) return -1;
            return v;
        }
        p = line_end + 1;
    }
    return -1;
}

// Detect "Transfer-Encoding: chunked" in headers (case-insensitive). Returns 1 if chunked.
static int has_chunked_encoding(const char *headers, size_t header_len) {
    const char *p = headers;
    const char *end = headers + header_len;

    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) break;

        if (strncasecmp(p, "Transfer-Encoding:", 18) == 0) {
            const char *q = p + 18;
            while (q < line_end && (*q == ' ' || *q == '\t')) q++;
            // Quick check for "chunked" token
            // (Not a full RFC parser; good enough for demo)
            if ((line_end - q) >= 7 && strncasecmp(q, "chunked", 7) == 0) return 1;
        }
        p = line_end + 1;
    }
    return 0;
}

// Helper to send a simple error response (always closes)
static void send_error_response_close(int client_fd, int code, const char *msg) {
    cwist_http_response *res = cwist_http_response_create();
    res->keep_alive = 0;
    res->status_code = code;
    cwist_sstring_assign(res->status_text, (char *)msg);

    char body[256];
    snprintf(body, sizeof(body), "{\"error\": \"%s\"}", msg);
    cwist_sstring_assign(res->body, body);

    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_http_header_add(&res->headers, "Connection", "close");
    cwist_http_header_add(&res->headers, "Server", "Cwist-Simple/1.0");

    cwist_http_send_response(client_fd, res);
    cwist_http_response_destroy(res);
}

// Decide whether client requests connection close
static int request_wants_close(cwist_http_request *req) {
    char *conn = cwist_http_header_get(req->headers, "Connection");
    if (!conn) return 0; // assume keep-alive by default (HTTP/1.1 typical)
    return (strcasecmp(conn, "close") == 0);
}

// Actual request handler logic (keep-alive capable)
void handle_client(int client_fd, void *ctx) {
    (void)ctx;
    char buffer[BUFFER_SIZE];
    size_t buf_len = 0;

    while (1) {
        // Read more data if we don't have at least a full header
        ssize_t n = recv(client_fd, buffer + buf_len, (BUFFER_SIZE - 1) - buf_len, 0);
        if (n < 0) {
            perror("recv failed");
            break;
        }
        if (n == 0) {
            // Client closed connection
            break;
        }

        buf_len += (size_t)n;
        buffer[buf_len] = '\0';

        // Process as many complete requests as possible from the buffer
        while (1) {
            int header_end = find_header_end(buffer, buf_len);
            if (header_end < 0) {
                // Need more data for headers
                if (buf_len >= BUFFER_SIZE - 1) {
                    // Headers too big for demo buffer
                    send_error_response_close(client_fd, 413, "Request Entity Too Large");
                    goto out;
                }
                break;
            }

            // Demo: reject chunked requests (not implemented)
            if (has_chunked_encoding(buffer, (size_t)header_end)) {
                send_error_response_close(client_fd, 501, "Chunked Transfer-Encoding Not Implemented");
                goto out;
            }

            long cl = parse_content_length(buffer, (size_t)header_end);
            if (cl < 0) cl = 0;

            size_t total_needed = (size_t)header_end + (size_t)cl;
            if (total_needed > BUFFER_SIZE - 1) {
                send_error_response_close(client_fd, 413, "Request Entity Too Large");
                goto out;
            }

            if (buf_len < total_needed) {
                // Need more body
                break;
            }

            // We have one complete request in buffer[0..total_needed)
            char saved = buffer[total_needed];
            buffer[total_needed] = '\0';

            cwist_http_request *req = cwist_http_parse_request(buffer);

            buffer[total_needed] = saved;

            if (!req) {
                // Malformed request (we had complete headers/body)
                send_error_response_close(client_fd, CWIST_HTTP_BAD_REQUEST, "Bad Request");
                goto out;
            }

            printf("[%s] %s\n", cwist_http_method_to_string(req->method), req->path->data);

            // Prepare Response
            cwist_http_response *res = cwist_http_response_create();
            cwist_http_header_add(&res->headers, "Server", "Cwist-Simple/1.0");

            int close_after = request_wants_close(req);
            res->keep_alive = close_after ? 0 : 1;

            if (close_after) {
                cwist_http_header_add(&res->headers, "Connection", "close");
            } else {
                // Explicit is fine for demo; HTTP/1.1 may omit it.
                cwist_http_header_add(&res->headers, "Connection", "keep-alive");
            }

            // Routing Logic
            if (strcmp(req->path->data, "/") == 0 && req->method == CWIST_HTTP_GET) {
                res->status_code = CWIST_HTTP_OK;
                cwist_sstring_assign(res->status_text, "OK");
                cwist_http_header_add(&res->headers, "Content-Type", "text/html");

                cwist_sstring_assign(res->body,
                    "<html>"
                    "<head><title>Cwist Server</title></head>"
                    "<body>"
                    "<h1>Hello from Cwist!</h1>"
                    "<p>This is a robust, simple example server.</p>"
                    "<a href='/health'>Check Health</a> | <a href='/json'>Get JSON</a>"
                    "</body>"
                    "</html>"
                );
            }
            else if (strcmp(req->path->data, "/health") == 0 && req->method == CWIST_HTTP_GET) {
                res->status_code = CWIST_HTTP_OK;
                cwist_sstring_assign(res->status_text, "OK");
                cwist_http_header_add(&res->headers, "Content-Type", "application/json");
                cwist_sstring_assign(res->body, "{\"status\": \"ok\", \"uptime\": \"forever\"}");
            }
            else if (strcmp(req->path->data, "/echo") == 0 && req->method == CWIST_HTTP_POST) {
                res->status_code = CWIST_HTTP_OK;
                cwist_sstring_assign(res->status_text, "OK");

                char *ct = cwist_http_header_get(req->headers, "Content-Type");
                if (ct) cwist_http_header_add(&res->headers, "Content-Type", ct);

                cwist_sstring_assign(res->body, req->body->data);
            }
            else {
                res->status_code = CWIST_HTTP_NOT_FOUND;
                cwist_sstring_assign(res->status_text, "Not Found");
                cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
                cwist_sstring_assign(res->body, "404 - Not Found");
            }

            cwist_http_send_response(client_fd, res);

            cwist_http_response_destroy(res);
            cwist_http_request_destroy(req);

            // Consume this request from buffer
            size_t remain = buf_len - total_needed;
            if (remain > 0) memmove(buffer, buffer + total_needed, remain);
            buf_len = remain;
            buffer[buf_len] = '\0';

            if (close_after) goto out;

            // Continue loop to see if another full request is already buffered
        }
    }

out:
    close(client_fd);
}

int main() {
    struct sockaddr_in server_addr;

    // Create and bind socket
    // Backlog increased to 512 to handle high concurrency
    int server_fd = cwist_make_socket_ipv4(&server_addr, "0.0.0.0", PORT, 512);
    if (server_fd < 0) {
        fprintf(stderr, "Failed to start server. Error code: %d\n", server_fd);
        return 1;
    }

    printf("Server listening on http://localhost:%d\n", PORT);
    printf("Ctrl+C to stop.\n");

    // Start server loop (threaded)
    cwist_server_config config;
    memset(&config, 0, sizeof(config));
    config.use_threading = true;

    cwist_http_server_loop(server_fd, &config, handle_client, NULL);
    return 0;
}

