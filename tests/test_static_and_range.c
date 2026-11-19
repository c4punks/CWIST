/**
 * @file test_static_and_range.c
 * @brief Test static file security (path traversal, symlink, URL decode)
 *        and HTTP Range request support.
 */

#include <cwist/sys/app/app.h>
#include <cwist/sys/app/shutdown.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>

#define TEST_PORT 19998
#define TEST_HOST "127.0.0.1"

/* strcasestr() is a GNU extension missing on macOS; local ASCII-only variant. */
static const char *test_strcasestr(const char *haystack, const char *needle) {
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++;
            n++;
        }
        if (!*n) return haystack;
    }
    return NULL;
}

static int connect_to_server(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TEST_PORT),
    };
    inet_pton(AF_INET, TEST_HOST, &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_request(int fd, const char *req) {
    size_t len = strlen(req);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, req + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int read_response(int fd, char *buf, size_t buf_size) {
    size_t total = 0;
    size_t want = 0; /* header end + Content-Length once known */
    while (total < buf_size - 1) {
        ssize_t n = recv(fd, buf + total, buf_size - 1 - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        /* Stop as soon as the full response (headers + declared body) has
         * arrived; the server keeps the connection alive, so waiting for
         * EOF would block forever. */
        if (want == 0) {
            char *end = strstr(buf, "\r\n\r\n");
            if (end) {
                size_t header_len = (size_t)(end + 4 - buf);
                char *cl = strcasestr(buf, "Content-Length:");
                if (cl && cl < end) {
                    want = header_len + (size_t)strtoul(cl + 15, NULL, 10);
                } else {
                    want = header_len;
                }
            }
        }
        if (want && total >= want) break;
    }
    buf[total] = '\0';
    return (int)total;
}

static bool response_has_code(const char *response, const char *code) {
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "HTTP/1.1 %s", code);
    return strstr(response, prefix) != NULL;
}

static bool response_has_header(const char *response, const char *header) {
    return test_strcasestr(response, header) != NULL;
}

static void response_header_value(const char *response, const char *name, char *out, size_t cap) {
    out[0] = '\0';
    const char *header = test_strcasestr(response, name);
    const char *end = strstr(response, "\r\n\r\n");
    if (!header || !end || header >= end) return;
    const char *colon = strchr(header, ':');
    if (!colon || colon >= end) return;
    const char *value = colon + 1;
    while (value < end && (*value == ' ' || *value == '\t')) value++;
    const char *value_end = strstr(value, "\r\n");
    if (!value_end || value_end > end) return;
    size_t len = (size_t)(value_end - value);
    if (len >= cap) len = cap - 1;
    memcpy(out, value, len);
    out[len] = '\0';
}

int main(void) {
    printf("Testing static file security and range requests...\n");

    char tmpdir[] = "/tmp/cwist_static_test_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        perror("mkdtemp");
        return 1;
    }

    char index_path[256];
    snprintf(index_path, sizeof(index_path), "%s/index.html", tmpdir);
    FILE *f = fopen(index_path, "w");
    if (!f) {
        perror("fopen");
        return 1;
    }
    fprintf(f, "Hello World");
    fclose(f);

    char plus_path[256];
    snprintf(plus_path, sizeof(plus_path), "%s/c++.txt", tmpdir);
    f = fopen(plus_path, "w");
    if (!f) {
        perror("fopen");
        return 1;
    }
    fprintf(f, "plus");
    fclose(f);

    /* Create a symlink escape target */
    char secret_path[256];
    snprintf(secret_path, sizeof(secret_path), "%s/secret.txt", tmpdir);
    f = fopen(secret_path, "w");
    if (f) {
        fprintf(f, "TOPSECRET");
        fclose(f);
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* Exercise static-cache creation and teardown across a worker fork,
         * regardless of the runner's CPU count or inherited environment. */
        if (setenv("CWIST_WORKERS", "2", 1) != 0 || setenv("CWIST_C1M_MODE", "false", 1) != 0) {
            perror("setenv");
            _exit(1);
        }
        cwist_app *app = cwist_app_create();
        if (!app) {
            fprintf(stderr, "Failed to create app\n");
            _exit(1);
        }
        cwist_app_static(app, "/static", tmpdir);
        g_cwist_drain_timeout_sec = 1;
        int rc = cwist_app_listen(app, TEST_PORT);
        cwist_app_destroy(app);
        _exit(rc == 0 ? 0 : 1);
    }

    /* Parent: give server time to start */
    usleep(400000);

    char buf[4096];
    char etag[128] = {0};
    char last_modified[128] = {0};
    int fd;
    int failures = 0;

    /* Test 1: Normal static file request */
    fd = connect_to_server();
    if (fd >= 0) {
        send_request(fd, "GET /static/index.html HTTP/1.1\r\nHost: localhost\r\n\r\n");
        read_response(fd, buf, sizeof(buf));
        if (!response_has_code(buf, "200")) {
            fprintf(stderr, "FAIL: Expected 200 OK for normal request, got:\n%s\n", buf);
            failures++;
        } else if (!response_has_header(buf, "Accept-Ranges: bytes")) {
            fprintf(stderr, "FAIL: Missing Accept-Ranges header\n");
            failures++;
        }
        response_header_value(buf, "ETag", etag, sizeof(etag));
        response_header_value(buf, "Last-Modified", last_modified, sizeof(last_modified));
        close(fd);
    } else {
        fprintf(stderr, "FAIL: Could not connect for normal request\n");
        failures++;
    }

    /* Test 2: Path traversal via .. */
    fd = connect_to_server();
    if (fd >= 0) {
        send_request(fd, "GET /static/../../../etc/passwd HTTP/1.1\r\nHost: localhost\r\n\r\n");
        read_response(fd, buf, sizeof(buf));
        if (!response_has_code(buf, "403")) {
            fprintf(stderr, "FAIL: Expected 403 for path traversal, got:\n%s\n", buf);
            failures++;
        }
        close(fd);
    } else {
        fprintf(stderr, "FAIL: Could not connect for traversal test\n");
        failures++;
    }

    /* Test 3: URL-encoded path traversal */
    fd = connect_to_server();
    if (fd >= 0) {
        send_request(fd,
                     "GET /static/%2e%2e/%2e%2e/secret.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
        read_response(fd, buf, sizeof(buf));
        if (!response_has_code(buf, "403")) {
            fprintf(stderr, "FAIL: Expected 403 for URL-encoded traversal, got:\n%s\n", buf);
            failures++;
        }
        close(fd);
    } else {
        fprintf(stderr, "FAIL: Could not connect for encoded traversal test\n");
        failures++;
    }

    /* Test 4: Range request */
    fd = connect_to_server();
    if (fd >= 0) {
        send_request(
            fd, "GET /static/index.html HTTP/1.1\r\nHost: localhost\r\nRange: bytes=0-4\r\n\r\n");
        read_response(fd, buf, sizeof(buf));
        if (!response_has_code(buf, "206")) {
            fprintf(stderr, "FAIL: Expected 206 for range request, got:\n%s\n", buf);
            failures++;
        } else if (!response_has_header(buf, "Content-Range: bytes 0-4/11")) {
            fprintf(stderr, "FAIL: Missing or incorrect Content-Range header:\n%s\n", buf);
            failures++;
        } else if (!strstr(buf, "Hello")) {
            fprintf(stderr, "FAIL: Body does not contain 'Hello':\n%s\n", buf);
            failures++;
        }
        close(fd);
    } else {
        fprintf(stderr, "FAIL: Could not connect for range test\n");
        failures++;
    }

    /* Test 5: Suffix range request (last 5 bytes) */
    fd = connect_to_server();
    if (fd >= 0) {
        send_request(
            fd, "GET /static/index.html HTTP/1.1\r\nHost: localhost\r\nRange: bytes=-5\r\n\r\n");
        read_response(fd, buf, sizeof(buf));
        if (!response_has_code(buf, "206")) {
            fprintf(stderr, "FAIL: Expected 206 for suffix range request, got:\n%s\n", buf);
            failures++;
        } else if (!strstr(buf, "World")) {
            fprintf(stderr, "FAIL: Body does not contain 'World':\n%s\n", buf);
            failures++;
        }
        close(fd);
    } else {
        fprintf(stderr, "FAIL: Could not connect for suffix range test\n");
        failures++;
    }

    struct {
        char headers[512];
        const char *status;
        bool full_body;
        bool forbid_content_range;
    } conditional[8] = {0};
    snprintf(conditional[0].headers, sizeof(conditional[0].headers), "If-None-Match: W/%s\r\n", etag);
    conditional[0].status = "304";
    snprintf(conditional[1].headers, sizeof(conditional[1].headers),
             "If-None-Match: \"nope\", W/%s\r\n", etag);
    conditional[1].status = "304";
    snprintf(conditional[2].headers, sizeof(conditional[2].headers), "If-None-Match: *\r\n");
    conditional[2].status = "304";
    snprintf(conditional[3].headers, sizeof(conditional[3].headers),
             "If-None-Match: \"nope\"\r\nIf-Modified-Since: %s\r\n", last_modified);
    conditional[3].status = "200";
    conditional[3].full_body = true;
    conditional[3].forbid_content_range = true;
    snprintf(conditional[4].headers, sizeof(conditional[4].headers),
             "Range: bytes=0-4\r\nIf-Range: %s\r\n", etag);
    conditional[4].status = "206";
    snprintf(conditional[5].headers, sizeof(conditional[5].headers),
             "Range: bytes=0-4\r\nIf-Range: W/%s\r\n", etag);
    conditional[5].status = "200";
    conditional[5].full_body = true;
    conditional[5].forbid_content_range = true;
    snprintf(conditional[6].headers, sizeof(conditional[6].headers),
             "Range: bytes=0-4\r\nIf-Range: %s\r\n", last_modified);
    conditional[6].status = "206";
    snprintf(conditional[7].headers, sizeof(conditional[7].headers),
             "Range: bytes=0-1,5-6\r\n");
    conditional[7].status = "200";
    conditional[7].full_body = true;
    conditional[7].forbid_content_range = true;

    for (size_t i = 0; i < sizeof(conditional) / sizeof(conditional[0]); i++) {
        fd = connect_to_server();
        if (fd < 0) {
            fprintf(stderr, "FAIL: Could not connect for conditional request test\n");
            failures++;
            continue;
        }
        char request[1024];
        snprintf(request, sizeof(request), "GET /static/index.html HTTP/1.1\r\nHost: localhost\r\n%s\r\n",
                 conditional[i].headers);
        send_request(fd, request);
        read_response(fd, buf, sizeof(buf));
        if (!response_has_code(buf, conditional[i].status)) {
            fprintf(stderr, "FAIL: Expected %s for conditional request, got:\n%s\n",
                    conditional[i].status, buf);
            failures++;
        }
        if (conditional[i].full_body && !strstr(buf, "Hello World")) {
            fprintf(stderr, "FAIL: Expected full representation, got:\n%s\n", buf);
            failures++;
        }
        if (conditional[i].forbid_content_range && response_has_header(buf, "Content-Range")) {
            fprintf(stderr, "FAIL: Range was not ignored, got:\n%s\n", buf);
            failures++;
        }
        close(fd);
    }

    struct {
        const char *target;
        const char *status;
    } decode_cases[] = {
        {"/static/c++.txt", "200"},
        {"/static/c%2B%2B.txt", "200"},
        {"/static/index.html%00.txt", "400"},
        {"/static/index.html%0a.txt", "400"},
        {"/static/index.html%", "400"},
    };
    for (size_t i = 0; i < sizeof(decode_cases) / sizeof(decode_cases[0]); i++) {
        fd = connect_to_server();
        if (fd < 0) {
            fprintf(stderr, "FAIL: Could not connect for path decoding test\n");
            failures++;
            continue;
        }
        char request[512];
        snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: localhost\r\n\r\n",
                 decode_cases[i].target);
        send_request(fd, request);
        read_response(fd, buf, sizeof(buf));
        if (!response_has_code(buf, decode_cases[i].status)) {
            fprintf(stderr, "FAIL: Expected %s for %s, got:\n%s\n", decode_cases[i].status,
                    decode_cases[i].target, buf);
            failures++;
        }
        close(fd);
    }

    /* Cleanup */
    if (kill(pid, SIGTERM) < 0 && errno != ESRCH) {
        perror("kill SIGTERM");
        failures++;
    }
    int status = 0;
    int waited = 0;
    pid_t reaped = 0;
    while (waited < 50) {
        reaped = waitpid(pid, &status, WNOHANG);
        if (reaped == pid) break;
        if (reaped < 0) {
            if (errno == EINTR) continue;
            perror("waitpid");
            failures++;
            break;
        }
        usleep(100000);
        waited++;
    }
    if (waited >= 50) {
        fprintf(stderr, "FAIL: Server did not exit in time, sending SIGKILL\n");
        failures++;
        if (kill(pid, SIGKILL) < 0 && errno != ESRCH) {
            perror("kill SIGKILL");
        }
        do {
            reaped = waitpid(pid, &status, 0);
        } while (reaped < 0 && errno == EINTR);
        if (reaped < 0) {
            perror("waitpid after SIGKILL");
        }
    }
    if (reaped == pid && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
        fprintf(stderr, "FAIL: Server exited abnormally (status=%d)\n", status);
        failures++;
    }

    unlink(index_path);
    unlink(plus_path);
    unlink(secret_path);
    rmdir(tmpdir);

    if (failures == 0) {
        printf("Passed static file security and range request tests.\n");
        return 0;
    }
    fprintf(stderr, "Failed %d test(s).\n", failures);
    return 1;
}
