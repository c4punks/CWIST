/* lwIP options for CWIST on STM32F769I-DISCO: the CWIST adapter profile
 * (example/stm32/lwipopts.h) plus the hardware checksum offload and custom
 * pbuf support the CubeF7 zero-copy ethernetif relies on. */
#ifndef CWIST_F769_LWIPOPTS_H
#define CWIST_F769_LWIPOPTS_H

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
#define MEMP_NUM_SYS_TIMEOUT 10
#define PBUF_POOL_SIZE 12
#define PBUF_POOL_BUFSIZE 1524

#define TCP_MSS 1460
#define TCP_SND_BUF (2 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * TCP_SND_BUF) / TCP_MSS)
#define TCP_WND (4 * TCP_MSS)
#define TCP_LISTEN_BACKLOG 1
#define SO_REUSE 1

/* CubeF7 ethernetif is zero-copy with custom pbufs and hardware checksums. */
#define LWIP_SUPPORT_CUSTOM_PBUF 1
#define CHECKSUM_GEN_IP 0
#define CHECKSUM_GEN_UDP 0
#define CHECKSUM_GEN_TCP 0
#define CHECKSUM_GEN_ICMP 0
#define CHECKSUM_CHECK_IP 0
#define CHECKSUM_CHECK_UDP 0
#define CHECKSUM_CHECK_TCP 0
#define CHECKSUM_CHECK_ICMP 0

#define LWIP_STATS 1
#define LWIP_STATS_DISPLAY 0
#define LWIP_DEBUG 0

#endif
