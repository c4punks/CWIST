#ifndef CWIST_F769_FORTRESS_DASHBOARD_H
#define CWIST_F769_FORTRESS_DASHBOARD_H

#include <cwist/sys/port/lwip.h>

#include <stddef.h>
#include <stdint.h>

int dashboard_init(void);
void dashboard_render(const cwist_lwip_server *server, uint32_t ipv4, uint32_t requests,
                      size_t payload_bytes);

#endif
