#ifndef CWIST_HTTP_CLIENT_FILE_H
#define CWIST_HTTP_CLIENT_FILE_H

#include <cwist/net/http/http.h>

int cwist_http_client_file_request(const char *url, cwist_http_method_t method, int timeout_ms,
                                   cwist_http_response **out_response);

#endif
