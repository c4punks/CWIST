#ifndef CWIST_F769_ETHERNETIF_H
#define CWIST_F769_ETHERNETIF_H

#include "lwip/err.h"
#include "lwip/netif.h"

err_t ethernetif_init(struct netif *netif);
void ethernetif_poll(struct netif *netif);
void ethernet_link_check(struct netif *netif);

#endif
