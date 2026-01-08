#ifndef __CWIST_HTTP_H__
#define __CWIST_HTTP_H__

#include <cwistaw/smartstring.h>
#include <cwistaw/err/cwist_err.h>
#include <netinet/in.h>
#include <sys/socket.h>

/* --- Enums --- */

typedef enum cwist_http_method_t {
    CWIST_HTTP_GET,
    CWIST_HTTP_POST,
    CWIST_HTTP_PUT,
    CWIST_HTTP_DELETE,
    CWIST_HTTP_PATCH,
    CWIST_HTTP_HEAD,
    CWIST_HTTP_OPTIONS,
    CWIST_HTTP_UNKNOWN
} cwist_http_method_t;

typedef enum cwist_http_status_t {
    CWIST_HTTP_OK = 200,
    CWIST_HTTP_CREATED = 201,
    CWIST_HTTP_NO_CONTENT = 204,
    CWIST_HTTP_BAD_REQUEST = 400,
    CWIST_HTTP_UNAUTHORIZED = 401,
    CWIST_HTTP_FORBIDDEN = 403,
    CWIST_HTTP_NOT_FOUND = 404,
    CWIST_HTTP_INTERNAL_ERROR = 500,
    CWIST_HTTP_NOT_IMPLEMENTED = 501
} cwist_http_status_t;

/* --- Structures --- */

// Linked list for headers to handle multiple headers easily
typedef struct cwist_http_header_node {
    smartstring *key;
    smartstring *value;
    struct cwist_http_header_node *next;
} cwist_http_header_node;

typedef struct cwist_http_request {
    cwist_http_method_t method;
    smartstring *path;        // e.g., "/users/1"
    smartstring *query;       // e.g., "active=true" (parsed later)
    smartstring *version;     // e.g., "HTTP/1.1"
    cwist_http_header_node *headers;
    smartstring *body;
} cwist_http_request;

typedef struct cwist_http_response {
    smartstring *version;     // e.g., "HTTP/1.1"
    cwist_http_status_t status_code;
    smartstring *status_text; // e.g., "OK"
    cwist_http_header_node *headers;
    smartstring *body;
} cwist_http_response;

/* --- API Functions --- */

// Request Lifecycle
cwist_http_request *cwist_http_request_create(void);
void cwist_http_request_destroy(cwist_http_request *req);

// Response Lifecycle
cwist_http_response *cwist_http_response_create(void);
void cwist_http_response_destroy(cwist_http_response *res);

// Header Manipulation
cwist_error_t cwist_http_header_add(cwist_http_header_node **head, const char *key, const char *value);
char *cwist_http_header_get(cwist_http_header_node *head, const char *key); // Returns raw char* for convenience, NULL if not found
void cwist_http_header_free_all(cwist_http_header_node *head);

// Helper to convert method enum to string and vice versa
const char *cwist_http_method_to_string(cwist_http_method_t method);
cwist_http_method_t cwist_http_string_to_method(const char *method_str);

// TCP socket handler
// socket -> bind -> listen
int cwist_make_socket_ipv4(struct sockaddr_in *sockv4, const char *address, uint16_t port, uint16_t backlog);
cwist_error_t cwist_accept_socket(int server_fd, struct sockaddr *sockv4, void (*handler_func)(int client_fd));

#endif

extern const int CWIST_CREATE_SOCKET_FAILED;
extern const int CWIST_HTTP_UNAVAILABLE_ADDRESS;
extern const int CWIST_HTTP_BIND_FAILED;
extern const int CWIST_HTTP_SETSOCKOPT_FAILED;
extern const int CWIST_HTTP_LISTEN_FAILED;
