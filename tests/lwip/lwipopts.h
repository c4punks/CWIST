/* lwIP configuration for the CWIST adapter tests (tests/test_lwip_adapter.c
 * on the host, tests/baremetal/hello_lwip.c on Cortex-M): NO_SYS raw API,
 * IPv4 TCP only, traffic carried by the loopback netif so client and server
 * pcbs run in one process. Sized like a small MCU: a few connections, a
 * 2-segment send window. */
#ifndef CWIST_TEST_LWIPOPTS_H
#define CWIST_TEST_LWIPOPTS_H

#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_TIMERS 1
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0

#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_TCP 1
#define LWIP_UDP 0
#define LWIP_ICMP 0
#define LWIP_RAW 0
#define LWIP_DHCP 0
#define LWIP_ARP 0
#define LWIP_ETHERNET 0
#define LWIP_DNS 0
#define LWIP_IGMP 0

#define LWIP_NETIF_LOOPBACK 1
#define LWIP_LOOPBACK_MAX_PBUFS 0

#define MEM_ALIGNMENT __SIZEOF_POINTER__ /* 4 on Cortex-M, 8 on 64-bit hosts */
#define MEM_SIZE (24 * 1024)
#define MEMP_NUM_PBUF 32
#define MEMP_NUM_TCP_PCB 8
#define MEMP_NUM_TCP_PCB_LISTEN 2
#define MEMP_NUM_TCP_SEG 32
#define PBUF_POOL_SIZE 16

#define TCP_MSS 1460
#define TCP_SND_BUF (2 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * TCP_SND_BUF) / TCP_MSS)
#define TCP_WND (4 * TCP_MSS)
#define TCP_LISTEN_BACKLOG 1
#define SO_REUSE 1

/* Host build: CWIST's public headers already pull in <netinet/in.h>, whose
 * htons()/ntohl() macros lwIP must not redefine. */
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS 1
#endif

#define LWIP_STATS 0
#define LWIP_DEBUG 0

#endif /* CWIST_TEST_LWIPOPTS_H */
