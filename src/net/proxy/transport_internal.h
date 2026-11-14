#ifndef CWIST_NET_PROXY_TRANSPORT_INTERNAL_H
#define CWIST_NET_PROXY_TRANSPORT_INTERNAL_H

#include <cwist/sys/err/cwist_err.h>
#include <stddef.h>

#define CWIST_PROXY_TRANSPORT_TCP 1
#define CWIST_PROXY_TRANSPORT_UDP 2

cwist_error_t cwist_proxy_transport_add(int transport, unsigned short origin_port,
                                        const char *target_host, unsigned short target_port);
size_t cwist_proxy_transport_count(void);
int cwist_proxy_transport_start(void);
void cwist_proxy_transport_stop(void);
void cwist_proxy_transport_reset(void);

#endif
