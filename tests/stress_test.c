#include <cwist/sys/app/app.h>
#include <cwist/net/http/http.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#define AB_CMD "ab -k -n 20000 -c 64 -p payload.json -T application/json http://127.0.0.1:31744/api"
/* Single-request probe: distinguishes a wedged server (real bug) from an ab
 * client stall (observed intermittently under ASan on loaded CI runners,
 * where ab reports one unfinished request after a 30s poll timeout). */
#define PROBE_CMD "curl -sf -m 5 -o /dev/null http://127.0.0.1:31744/api"
#define MAX_ATTEMPTS 3

void api_handler(cwist_http_request *req, cwist_http_response *res) {
    // Check if body is received
    if (req->body && req->body->size > 0) {
        cwist_sstring_assign_len(res->body, req->body->data, req->body->size);
    } else {
        cwist_sstring_assign(res->body, "{\"status\":\"ok\"}");
    }
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
}

void *run_server(void *arg) {
    (void)arg;
    cwist_app *app = cwist_app_create();
    cwist_app_post(app, "/api", api_handler);
    cwist_app_listen(app, 31744);
    cwist_app_destroy(app);
    return NULL;
}

int main() {
    pthread_t server_thread;
    if (pthread_create(&server_thread, NULL, run_server, NULL) != 0) {
        perror("Failed to create server thread");
        return 1;
    }

    // Wait for server to start
    sleep(1);

    // Prepare payload
    FILE *f = fopen("payload.json", "w");
    if (!f) {
        perror("Failed to create payload.json");
        return 1;
    }
    fprintf(f, "{\"test\":\"data\", \"more\": [1,2,3]}");
    fclose(f);

    // Run ab command. ab occasionally stalls on a single request under load
    // (see PR #209 CI): probe server health before retrying so a real server
    // hang still fails the test.
    int ret = 1;
    for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
        printf("Running ab stress test (attempt %d/%d)...\n", attempt, MAX_ATTEMPTS);
        fflush(stdout);
        ret = system(AB_CMD);
        if (ret == 0) break;
        fprintf(stderr, "ab exited with code %d\n", ret);
        if (system(PROBE_CMD) != 0) {
            fprintf(stderr, "server is not responding; treating as a real failure\n");
            unlink("payload.json");
            return 1;
        }
        if (attempt == MAX_ATTEMPTS) {
            fprintf(stderr, "ab stalled %d times while the server stayed healthy\n", MAX_ATTEMPTS);
            unlink("payload.json");
            return 1;
        }
        fprintf(stderr, "server is healthy; retrying ab\n");
    }

    printf("ab stress test passed!\n");

    // Clean up
    unlink("payload.json");

    // In a real test we might want to kill the server thread, but since this is a one-off test
    // script, exiting main is fine if we are done.

    return 0;
}
