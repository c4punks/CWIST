#include <cwistaw/http.h>
#include <cwistaw/smartstring.h>
#include <cwistaw/err/cwist_err.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>

#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

const int CWIST_CREATE_SOCKET_FAILED     = -1;
const int CWIST_HTTP_UNAVAILABLE_ADDRESS = -2;
const int CWIST_HTTP_BIND_FAILED         = -3;
const int CWIST_HTTP_SETSOCKOPT_FAILED   = -4;
const int CWIST_HTTP_LISTEN_FAILED       = -5;

/* --- Helpers --- */

const char *cwist_http_method_to_string(cwist_http_method_t method) {
    switch (method) {
        case CWIST_HTTP_GET: return "GET";
        case CWIST_HTTP_POST: return "POST";
        case CWIST_HTTP_PUT: return "PUT";
        case CWIST_HTTP_DELETE: return "DELETE";
        case CWIST_HTTP_PATCH: return "PATCH";
        case CWIST_HTTP_HEAD: return "HEAD";
        case CWIST_HTTP_OPTIONS: return "OPTIONS";
        default: return "UNKNOWN";
    }
}

cwist_http_method_t cwist_http_string_to_method(const char *method_str) {
    if (strcmp(method_str, "GET") == 0) return CWIST_HTTP_GET;
    if (strcmp(method_str, "POST") == 0) return CWIST_HTTP_POST;
    if (strcmp(method_str, "PUT") == 0) return CWIST_HTTP_PUT;
    if (strcmp(method_str, "DELETE") == 0) return CWIST_HTTP_DELETE;
    if (strcmp(method_str, "PATCH") == 0) return CWIST_HTTP_PATCH;
    if (strcmp(method_str, "HEAD") == 0) return CWIST_HTTP_HEAD;
    if (strcmp(method_str, "OPTIONS") == 0) return CWIST_HTTP_OPTIONS;
    return CWIST_HTTP_UNKNOWN;
}

/* --- Header Manipulation --- */

cwist_error_t cwist_http_header_add(cwist_http_header_node **head, const char *key, const char *value) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    
    cwist_http_header_node *node = (cwist_http_header_node *)malloc(sizeof(cwist_http_header_node));
    if (!node) {
        err = make_error(CWIST_ERR_JSON);
        err.error.err_json = cJSON_CreateObject();
        cJSON_AddStringToObject(err.error.err_json, "http_error", "Failed to allocate header");
        return err;
    }

    node->key = smartstring_create();
    node->value = smartstring_create();
    node->next = NULL;

    smartstring_assign(node->key, (char *)key);
    smartstring_assign(node->value, (char *)value);

    node->next = *head;
    *head = node;

    err.error.err_i16 = 0; // Success
    return err;
}

char *cwist_http_header_get(cwist_http_header_node *head, const char *key) {
    cwist_http_header_node *curr = head;
    while (curr) {
        // case-insensitive comparison for headers is standard, but keeping it strict for now for simplicity
        if (curr->key->data && strcmp(curr->key->data, key) == 0) {
            return curr->value->data;
        }
        curr = curr->next;
    }
    return NULL;
}

void cwist_http_header_free_all(cwist_http_header_node *head) {
    cwist_http_header_node *curr = head;
    while (curr) {
        cwist_http_header_node *next = curr->next;
        smartstring_destroy(curr->key);
        smartstring_destroy(curr->value);
        free(curr);
        curr = next;
    }
}

/* --- Request Lifecycle --- */

cwist_http_request *cwist_http_request_create(void) {
    cwist_http_request *req = (cwist_http_request *)malloc(sizeof(cwist_http_request));
    if (!req) return NULL;

    req->method = CWIST_HTTP_GET; // Default
    req->path = smartstring_create();
    req->query = smartstring_create();
    req->version = smartstring_create();
    req->headers = NULL;
    req->body = smartstring_create();

    // Defaults
    smartstring_assign(req->version, "HTTP/1.1");
    smartstring_assign(req->path, "/");

    return req;
}

void cwist_http_request_destroy(cwist_http_request *req) {
    if (req) {
        smartstring_destroy(req->path);
        smartstring_destroy(req->query);
        smartstring_destroy(req->version);
        smartstring_destroy(req->body);
        cwist_http_header_free_all(req->headers);
        free(req);
    }
}

/* --- Response Lifecycle --- */

cwist_http_response *cwist_http_response_create(void) {
    cwist_http_response *res = (cwist_http_response *)malloc(sizeof(cwist_http_response));
    if (!res) return NULL;

    res->version = smartstring_create();
    res->status_code = CWIST_HTTP_OK;
    res->status_text = smartstring_create();
    res->headers = NULL;
    res->body = smartstring_create();

    // Defaults
    smartstring_assign(res->version, "HTTP/1.1");
    smartstring_assign(res->status_text, "OK");

    return res;
}

void cwist_http_response_destroy(cwist_http_response *res) {
    if (res) {
        smartstring_destroy(res->version);
        smartstring_destroy(res->status_text);
        smartstring_destroy(res->body);
        cwist_http_header_free_all(res->headers);
        free(res);
    }
}

cwist_http_request *cwist_http_parse_request(const char *raw_request) {
    if (!raw_request) return NULL;

    cwist_http_request *req = cwist_http_request_create();
    if (!req) return NULL;
    
    const char *line_start = raw_request;
    const char *line_end = strstr(line_start, "\r\n");
    if (!line_end) { 
        cwist_http_request_destroy(req); 
        return NULL; 
    }

    // 1. Request Line
    int request_line_len = line_end - line_start;
    char *request_line = (char*)malloc(request_line_len + 1);
    if (!request_line) {
        cwist_http_request_destroy(req);
        return NULL;
    }
    strncpy(request_line, line_start, request_line_len);
    request_line[request_line_len] = '\0';
    
    char *method_str = strtok(request_line, " ");
    char *path_str = strtok(NULL, " ");
    char *version_str = strtok(NULL, " ");
    
    if (method_str) req->method = cwist_http_string_to_method(method_str);
    if (path_str) smartstring_assign(req->path, path_str);
    if (version_str) smartstring_assign(req->version, version_str);
    
    free(request_line);

    // 2. Headers
    line_start = line_end + 2; // Skip \r\n
    while ((line_end = strstr(line_start, "\r\n")) != NULL) {
        if (line_end == line_start) {
            // Empty line found, body follows
            line_start += 2;
            break;
        }
        
        int header_len = line_end - line_start;
        char *header_line = (char*)malloc(header_len + 1);
        if (header_line) {
            strncpy(header_line, line_start, header_len);
            header_line[header_len] = '\0';
            
            char *colon = strchr(header_line, ':');
            if (colon) {
                *colon = '\0';
                char *key = header_line;
                char *value = colon + 1;
                while (*value == ' ') value++; // Trim leading space
                
                cwist_http_header_add(&req->headers, key, value);
            }
            free(header_line);
        }
        
        line_start = line_end + 2;
    }

    // 3. Body
    if (*line_start) {
        smartstring_assign(req->body, (char*)line_start);
    }

    return req;
}

cwist_error_t cwist_http_send_response(int client_fd, cwist_http_response *res) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    
    if (client_fd < 0 || !res) {
        err.error.err_i16 = -1;
        return err;
    }

    smartstring *response_str = smartstring_create();
    
    // Status Line
    char status_line[256];
    snprintf(status_line, sizeof(status_line), "%s %d %s\r\n", 
             res->version->data ? res->version->data : "HTTP/1.1", 
             res->status_code, 
             res->status_text->data ? res->status_text->data : "OK");
    smartstring_append(response_str, status_line);
    
    // Headers
    cwist_http_header_node *curr = res->headers;
    while (curr) {
        if (curr->key->data && curr->value->data) {
            smartstring_append(response_str, curr->key->data);
            smartstring_append(response_str, ": ");
            smartstring_append(response_str, curr->value->data);
            smartstring_append(response_str, "\r\n");
        }
        curr = curr->next;
    }
    
    // End of headers
    smartstring_append(response_str, "\r\n");
    
    // Body
    if (res->body->data) {
        smartstring_append(response_str, res->body->data);
    }
    
    // Send
    ssize_t sent = send(client_fd, response_str->data, response_str->size, 0);
    if (sent < 0) {
        err.error.err_i16 = -1;
    } else {
        err.error.err_i16 = 0;
    }
    
    smartstring_destroy(response_str);
    return err;
}

/* --- Socket Manipulation --- */

int cwist_make_socket_ipv4(struct sockaddr_in *sockv4, const char *address, uint16_t port, uint16_t backlog) {
  int server_fd = -1;
  int opt = 1;
  in_addr_t addr = inet_addr(address);

  if(addr == INADDR_NONE) {
    return CWIST_HTTP_UNAVAILABLE_ADDRESS;
  }

  if((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    cJSON *err_json = cJSON_CreateObject();
    cJSON_AddStringToObject(err_json, "err", "Failed to create IPv4 socket");
    char *cjson_error_log = cJSON_Print(err_json);
    perror(cjson_error_log);
    free(cjson_error_log);
    cJSON_Delete(err_json);

    return CWIST_CREATE_SOCKET_FAILED;
  }

  if(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
    cJSON *err_json = cJSON_CreateObject();
    cJSON_AddStringToObject(err_json, "err", "Failed to set up IPv4 socket options");
    char *cjson_error_log = cJSON_Print(err_json);
    perror(cjson_error_log);
    free(cjson_error_log);
    cJSON_Delete(err_json);

    return CWIST_HTTP_SETSOCKOPT_FAILED;  
  }

  sockv4->sin_family = AF_INET;
  sockv4->sin_addr.s_addr = addr;
  sockv4->sin_port = htons(port);

  if(bind(server_fd, (struct sockaddr *)sockv4, sizeof(struct sockaddr_in)) < 0) {
    cJSON *err_json = cJSON_CreateObject();
    cJSON_AddStringToObject(err_json, "err", "Failed to bind IPv4 socket");
    char *cjson_error_log = cJSON_Print(err_json);
    perror(cjson_error_log);
    free(cjson_error_log);
    cJSON_Delete(err_json);

    return CWIST_HTTP_BIND_FAILED;
  }

  if(listen(server_fd, backlog) < 0) {
    cJSON *err_json = cJSON_CreateObject();
    char err_msg[128];
    char err_format[128] = "Failed to listen at %s:%d";
    snprintf(err_msg, 127, err_format, address, port);

    cJSON_AddStringToObject(err_json, "err", err_msg);
    char *cjson_error_log = cJSON_Print(err_json);
    perror(cjson_error_log);
    free(cjson_error_log);
    cJSON_Delete(err_json);

    return CWIST_HTTP_LISTEN_FAILED;
  }

  return server_fd;
}

cwist_error_t cwist_accept_socket(int server_fd, struct sockaddr *sockv4, void (*handler_func)(int client_fd)) {
  int client_fd = -1;
  struct sockaddr_in peer_addr;
  socklen_t addrlen = sizeof(peer_addr);

  while(true) { // TODO: ADD MULTIPROCESSING SUPPORT
    if((client_fd = accept(server_fd, (struct sockaddr *)&peer_addr, &addrlen)) < 0) {
      if (errno == EINTR) continue;

      cJSON *err_json = cJSON_CreateObject();
      cJSON_AddStringToObject(err_json, "err", "Failed to accept socket");
      char *cjson_error_log = cJSON_Print(err_json);
      perror(cjson_error_log);
      free(cjson_error_log);
      cJSON_Delete(err_json);

      if (errno == EBADF || errno == EINVAL || errno == ENOTSOCK) {
          fprintf(stderr, "Fatal socket error %d. Exiting accept loop.\n", errno);
          break;
      }
      continue;
    }

    if (sockv4) {
      memcpy(sockv4, &peer_addr, sizeof(peer_addr));
    }

    handler_func(client_fd);
  }

  cwist_error_t err = make_error(CWIST_ERR_INT16);
  err.error.err_i16 = -1;
  return err;
}
