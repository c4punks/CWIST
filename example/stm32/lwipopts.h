#ifndef CWIST_STM32_LWIPOPTS_H
#define CWIST_STM32_LWIPOPTS_H

#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_TIMERS 1
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0

#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_ICMP 1
#define LWIP_RAW 0
#define LWIP_DHCP 1
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_DNS 1
#define LWIP_IGMP 0
#define LWIP_NETIF_LINK_CALLBACK 1

#define MEM_ALIGNMENT 4
#define MEM_SIZE (16 * 1024)
#define MEMP_NUM_PBUF 16
#define MEMP_NUM_TCP_PCB 6
#define MEMP_NUM_TCP_PCB_LISTEN 1
#define MEMP_NUM_TCP_SEG 16
#define PBUF_POOL_SIZE 12

#define TCP_MSS 1460
#define TCP_SND_BUF (2 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * TCP_SND_BUF) / TCP_MSS)
#define TCP_WND (4 * TCP_MSS)
#define TCP_LISTEN_BACKLOG 1
#define SO_REUSE 1

#define LWIP_STATS 0
#define LWIP_DEBUG 0

#endif
