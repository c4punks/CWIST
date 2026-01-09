#include <cwistaw/http.h>
#include <cwistaw/smartstring.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#define BUFFER_SIZE 8192
#define PORT 8080

// Helper to send a simple error response
void send_error_response(int client_fd, int code, const char *msg) {
    cwist_http_response *res = cwist_http_response_create();
    res->status_code = code;
    smartstring_assign(res->status_text, (char*)msg); // Cast const away safely as we assign copy
    
    char body[256];
    snprintf(body, sizeof(body), "{\"error\": \"%s\"}", msg);
    smartstring_assign(res->body, body);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    
    cwist_http_send_response(client_fd, res);
    cwist_http_response_destroy(res);
}

// Actual request handler logic
void handle_client(int client_fd) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
    
    if (bytes_read < 0) {
        perror("recv failed");
        close(client_fd);
        return;
    }
    
    if (bytes_read == 0) {
        // Client closed connection
        close(client_fd);
        return;
    }

    buffer[bytes_read] = '\0';

    // Parse Request
    cwist_http_request *req = cwist_http_parse_request(buffer);
    if (!req) {
        // Malformed request
        send_error_response(client_fd, CWIST_HTTP_BAD_REQUEST, "Bad Request");
        close(client_fd);
        return;
    }

    printf("[%s] %s\n", cwist_http_method_to_string(req->method), req->path->data);

    // Prepare Response
    cwist_http_response *res = cwist_http_response_create();
    cwist_http_header_add(&res->headers, "Server", "Cwistaw-Simple/1.0");
    cwist_http_header_add(&res->headers, "Connection", "close");

    // Routing Logic
    if (strcmp(req->path->data, "/") == 0 && req->method == CWIST_HTTP_GET) {
        res->status_code = CWIST_HTTP_OK;
        smartstring_assign(res->status_text, "OK");
        cwist_http_header_add(&res->headers, "Content-Type", "text/html");
        
        smartstring_assign(res->body, 
            "<html>"
            "<head><title>Cwistaw Server</title></head>"
            "<body>"
            "<h1>Hello from Cwistaw!</h1>"
            "<p>This is a robust, simple example server.</p>"
            "<a href='/health'>Check Health</a> | <a href='/json'>Get JSON</a>"
            "</body>"
            "</html>"
        );
    } 
    else if (strcmp(req->path->data, "/health") == 0 && req->method == CWIST_HTTP_GET) {
        res->status_code = CWIST_HTTP_OK;
        smartstring_assign(res->status_text, "OK");
        cwist_http_header_add(&res->headers, "Content-Type", "application/json");
        smartstring_assign(res->body, "{\"status\": \"ok\", \"uptime\": \"forever\"}");
    }
    else if (strcmp(req->path->data, "/echo") == 0 && req->method == CWIST_HTTP_POST) {
        res->status_code = CWIST_HTTP_OK;
        smartstring_assign(res->status_text, "OK");
        // Echo back content type if present
        char *ct = cwist_http_header_get(req->headers, "Content-Type");
        if (ct) {
            cwist_http_header_add(&res->headers, "Content-Type", ct);
        }
        smartstring_assign(res->body, req->body->data);
    }
    else {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        smartstring_assign(res->status_text, "Not Found");
        cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
        smartstring_assign(res->body, "404 - Not Found");
    }

    // Send and Clean up
    cwist_http_send_response(client_fd, res);
    
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
    close(client_fd);
}

int main() {
    struct sockaddr_in server_addr;
    
    // Create and bind socket
    int server_fd = cwist_make_socket_ipv4(&server_addr, "0.0.0.0", PORT, 10);
    
    if (server_fd < 0) {
        fprintf(stderr, "Failed to start server. Error code: %d\n", server_fd);
        return 1;
    }

    printf("Server listening on http://localhost:%d\n", PORT);
    printf("Ctrl+C to stop.\n");

    // Start accepting connections
    // Note: cwist_accept_socket is a blocking loop in current implementation
    cwist_accept_socket(server_fd, (struct sockaddr*)&server_addr, handle_client);

    return 0;
}
