/* NO_SYS lwIP Ethernet interface for STM32F769I-DISCO (LAN8742, RMII).
 * Adapted from STM32CubeF7 Projects/STM32F769I-Discovery/Applications/LwIP/
 * LwIP_HTTP_Server_Netconn_RTOS/Src/ethernetif.c: the RTOS threads and
 * semaphores are replaced with polling from the main loop. */
#include "stm32f7xx_hal.h"

#include "lwip/timeouts.h"
#include "lwip/snmp.h"
#include "netif/ethernet.h"
#include "netif/etharp.h"

#include "ethernetif.h"
#include "lan8742.h"

#include <stdio.h>
#include <stddef.h>
#include <string.h>

#define IFNAME0 's'
#define IFNAME1 't'

#define ETH_RX_BUFFER_CNT 10

typedef enum { RX_ALLOC_OK = 0x00, RX_ALLOC_ERROR = 0x01 } RxAllocStatusTypeDef;

typedef struct {
    struct pbuf_custom pbuf_custom;
    uint8_t buff[(ETH_RX_BUF_SIZE + 31) & ~31];
} RxBuff_t;

/* DMA must not touch the DTCM: the linker script keeps .bss in SRAM1/2. */
ETH_DMADescTypeDef DMARxDscrTab[ETH_RX_DESC_CNT] __attribute__((aligned(32)));
ETH_DMADescTypeDef DMATxDscrTab[ETH_TX_DESC_CNT] __attribute__((aligned(32)));

LWIP_MEMPOOL_DECLARE(RX_POOL, ETH_RX_BUFFER_CNT, sizeof(RxBuff_t), "Zero-copy RX PBUF pool");

static uint8_t RxAllocStatus;

ETH_HandleTypeDef EthHandle;
static ETH_TxPacketConfig TxConfig;
static lan8742_Object_t LAN8742;

static void low_level_init(struct netif *netif);
static struct pbuf *low_level_input(struct netif *netif);
static err_t low_level_output(struct netif *netif, struct pbuf *p);

int32_t ETH_PHY_IO_Init(void);
int32_t ETH_PHY_IO_DeInit(void);
int32_t ETH_PHY_IO_ReadReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t *pRegVal);
int32_t ETH_PHY_IO_WriteReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t RegVal);
int32_t ETH_PHY_IO_GetTick(void);
void pbuf_free_custom(struct pbuf *p);

lan8742_IOCtx_t LAN8742_IOCtx = {ETH_PHY_IO_Init, ETH_PHY_IO_DeInit, ETH_PHY_IO_WriteReg,
                                 ETH_PHY_IO_ReadReg, ETH_PHY_IO_GetTick};

static void eth_mac_start(struct netif *netif, int32_t link_state) {
    ETH_MACConfigTypeDef MACConf = {0};
    uint32_t duplex = ETH_FULLDUPLEX_MODE, speed = ETH_SPEED_100M;

    switch (link_state) {
    case LAN8742_STATUS_100MBITS_FULLDUPLEX:
        duplex = ETH_FULLDUPLEX_MODE; speed = ETH_SPEED_100M; break;
    case LAN8742_STATUS_100MBITS_HALFDUPLEX:
        duplex = ETH_HALFDUPLEX_MODE; speed = ETH_SPEED_100M; break;
    case LAN8742_STATUS_10MBITS_FULLDUPLEX:
        duplex = ETH_FULLDUPLEX_MODE; speed = ETH_SPEED_10M; break;
    case LAN8742_STATUS_10MBITS_HALFDUPLEX:
        duplex = ETH_HALFDUPLEX_MODE; speed = ETH_SPEED_10M; break;
    default:
        break;
    }

    HAL_ETH_GetMACConfig(&EthHandle, &MACConf);
    MACConf.DuplexMode = duplex;
    MACConf.Speed = speed;
    HAL_ETH_SetMACConfig(&EthHandle, &MACConf);
    HAL_ETH_Start_IT(&EthHandle);
    netif_set_up(netif);
    netif_set_link_up(netif);

    /* Clear any CRC errors latched while the link was coming up, so the
     * rev-A RMII watchdog can tell a healthy interface from a stuck one. */
    EthHandle.Instance->MMCCR |= ETH_MMCCR_CR;
}

static void low_level_init(struct netif *netif) {
    uint8_t macaddress[6] = {ETH_MAC_ADDR0, ETH_MAC_ADDR1, ETH_MAC_ADDR2,
                             ETH_MAC_ADDR3, ETH_MAC_ADDR4, ETH_MAC_ADDR5};

    EthHandle.Instance = ETH;
    EthHandle.Init.MACAddr = macaddress;
    EthHandle.Init.MediaInterface = HAL_ETH_RMII_MODE;
    EthHandle.Init.RxDesc = DMARxDscrTab;
    EthHandle.Init.TxDesc = DMATxDscrTab;
    EthHandle.Init.RxBuffLen = ETH_RX_BUF_SIZE;

    HAL_ETH_Init(&EthHandle);

    netif->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(netif->hwaddr, macaddress, ETH_HWADDR_LEN);
    netif->mtu = ETH_MAX_PAYLOAD;
    netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;

    LWIP_MEMPOOL_INIT(RX_POOL);

    memset(&TxConfig, 0, sizeof(ETH_TxPacketConfig));
    TxConfig.Attributes = ETH_TX_PACKETS_FEATURES_CSUM | ETH_TX_PACKETS_FEATURES_CRCPAD;
    TxConfig.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
    TxConfig.CRCPadCtrl = ETH_CRC_PAD_INSERT;

    LAN8742_RegisterBusIO(&LAN8742, &LAN8742_IOCtx);

    if (LAN8742_Init(&LAN8742) != LAN8742_STATUS_OK) {
        netif_set_link_down(netif);
        netif_set_down(netif);
        return;
    }

    int32_t link_state = LAN8742_GetLinkState(&LAN8742);
    if (link_state <= LAN8742_STATUS_LINK_DOWN) {
        netif_set_link_down(netif);
    } else {
        eth_mac_start(netif, link_state);
    }
}

static err_t low_level_output(struct netif *netif, struct pbuf *p) {
    (void)netif;
    struct pbuf *q = NULL;
    ETH_BufferTypeDef Txbuffer[ETH_TX_DESC_CNT];
    uint32_t i = 0;

    memset(Txbuffer, 0, sizeof(Txbuffer));

    for (q = p; q != NULL; q = q->next) {
        if (i >= ETH_TX_DESC_CNT) return ERR_IF;
        Txbuffer[i].buffer = q->payload;
        Txbuffer[i].len = q->len;
        if (i > 0) Txbuffer[i - 1].next = &Txbuffer[i];
        Txbuffer[i].next = NULL;
        i++;
    }

    TxConfig.Length = p->tot_len;
    TxConfig.TxBuffer = Txbuffer;
    TxConfig.pData = p;

    pbuf_ref(p);

    /* No TX-complete semaphore under NO_SYS: release finished descriptors
     * and retry for a bounded time. */
    uint32_t start = HAL_GetTick();
    for (;;) {
        if (HAL_ETH_Transmit_IT(&EthHandle, &TxConfig) == HAL_OK) return ERR_OK;
        if ((HAL_ETH_GetError(&EthHandle) & HAL_ETH_ERROR_BUSY) == 0 ||
            HAL_GetTick() - start > 50) {
            pbuf_free(p); /* balances the pbuf_ref above */
            return ERR_IF;
        }
        HAL_ETH_ReleaseTxPacket(&EthHandle);
    }
}

static struct pbuf *low_level_input(struct netif *netif) {
    (void)netif;
    struct pbuf *p = NULL;
    if (RxAllocStatus == RX_ALLOC_OK) {
        HAL_ETH_ReadData(&EthHandle, (void **)&p);
    }
    return p;
}

static void rmii_watchdog(void) {
    /* ES0290 (F769 rev A): the RMII receiver can latch bad data after reset;
     * toggling MII_RMII_SEL clears it. ST ships the same workaround as a
     * thread; here it runs from the poll loop, wrapped in a full MAC
     * stop/start so the receiver actually restarts. One toggle is not always
     * enough, so retry (up to 40 times, 500 ms apart) while the MAC reports
     * CRC errors but not a single good unicast frame; a healthy-but-quiet
     * LAN sees no CRC errors and is left alone. */
    static uint32_t last_reset;
    static unsigned attempts;
    if (HAL_GetREVID() != 0x1000) return;
    if (EthHandle.gState != HAL_ETH_STATE_STARTED) return;
    if (EthHandle.Instance->MMCRGUFCR > 0U) return;
    if (EthHandle.Instance->MMCRFCECR == 0U) return;
    if (attempts >= 40) return;
    if (HAL_GetTick() - last_reset < 500) return;
    last_reset = HAL_GetTick();
    attempts++;

    HAL_ETH_Stop_IT(&EthHandle);
    SYSCFG->PMC &= ~SYSCFG_PMC_MII_RMII_SEL;
    SYSCFG->PMC |= SYSCFG_PMC_MII_RMII_SEL;
    HAL_ETH_Start_IT(&EthHandle);
    EthHandle.Instance->MMCCR |= ETH_MMCCR_CR;
    printf("cwist-f769: RMII watchdog reset %u\r\n", attempts);
}

/* NO_SYS input pump: drain every pending frame into the stack. */
void ethernetif_poll(struct netif *netif) {
    struct pbuf *p;
    do {
        p = low_level_input(netif);
        if (p != NULL && netif->input(p, netif) != ERR_OK) {
            pbuf_free(p);
        }
    } while (p != NULL);

    rmii_watchdog();
}

/* Called every ~100 ms from the main loop; replaces ethernet_link_thread. */
void ethernet_link_check(struct netif *netif) {
    int32_t link_state = LAN8742_GetLinkState(&LAN8742);

    if (netif_is_link_up(netif) && link_state <= LAN8742_STATUS_LINK_DOWN) {
        HAL_ETH_Stop_IT(&EthHandle);
        netif_set_down(netif);
        netif_set_link_down(netif);
        printf("cwist-f769: link DOWN\r\n");
    } else if (!netif_is_link_up(netif) && link_state > LAN8742_STATUS_LINK_DOWN) {
        eth_mac_start(netif, link_state);
        printf("cwist-f769: link UP (%ld)\r\n", (long)link_state);
    }
}

err_t ethernetif_init(struct netif *netif) {
    LWIP_ASSERT("netif != NULL", (netif != NULL));

#if LWIP_NETIF_HOSTNAME
    netif->hostname = "cwist-f769";
#endif

    MIB2_INIT_NETIF(netif, snmp_ifType_ethernet_csmacd, 100000000);

    netif->name[0] = IFNAME0;
    netif->name[1] = IFNAME1;
    netif->output = etharp_output;
    netif->linkoutput = low_level_output;

    low_level_init(netif);
    return ERR_OK;
}

void pbuf_free_custom(struct pbuf *p) {
    struct pbuf_custom *custom_pbuf = (struct pbuf_custom *)p;
    LWIP_MEMPOOL_FREE(RX_POOL, custom_pbuf);
    if (RxAllocStatus == RX_ALLOC_ERROR) {
        RxAllocStatus = RX_ALLOC_OK;
    }
}

u32_t sys_now(void) { return HAL_GetTick(); }

void HAL_ETH_MspInit(ETH_HandleTypeDef *heth) {
    (void)heth;
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    /* RMII_REF_CLK PA1, RMII_MDIO PA2, RMII_MDC PC1, RMII_CRS_DV PA7,
     * RMII_RXD0 PC4, RMII_RXD1 PC5, RMII_TX_EN PG11, RMII_TXD0 PG13,
     * RMII_TXD1 PG14 */
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Alternate = GPIO_AF11_ETH;

    gpio.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7;
    HAL_GPIO_Init(GPIOA, &gpio);
    gpio.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOC, &gpio);
    gpio.Pin = GPIO_PIN_11 | GPIO_PIN_13 | GPIO_PIN_14;
    HAL_GPIO_Init(GPIOG, &gpio);

    /* 50 MHz RMII edges need the I/O compensation cell. */
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    HAL_EnableCompensationCell();

    HAL_NVIC_SetPriority(ETH_IRQn, 0x7, 0);
    HAL_NVIC_EnableIRQ(ETH_IRQn);

    __HAL_RCC_ETH_CLK_ENABLE();
}

void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *heth) { (void)heth; }
void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *heth) { (void)heth; }

void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *heth) {
    /* Wake nothing; the poll loop drains RX regardless. */
    (void)HAL_ETH_GetDMAError(heth);
}

int32_t ETH_PHY_IO_Init(void) {
    HAL_ETH_SetMDIOClockRange(&EthHandle);
    return 0;
}

int32_t ETH_PHY_IO_DeInit(void) { return 0; }

int32_t ETH_PHY_IO_ReadReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t *pRegVal) {
    return HAL_ETH_ReadPHYRegister(&EthHandle, DevAddr, RegAddr, pRegVal) != HAL_OK ? -1 : 0;
}

int32_t ETH_PHY_IO_WriteReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t RegVal) {
    return HAL_ETH_WritePHYRegister(&EthHandle, DevAddr, RegAddr, RegVal) != HAL_OK ? -1 : 0;
}

int32_t ETH_PHY_IO_GetTick(void) { return HAL_GetTick(); }

void HAL_ETH_RxAllocateCallback(uint8_t **buff) {
    struct pbuf_custom *p = LWIP_MEMPOOL_ALLOC(RX_POOL);
    if (p) {
        *buff = (uint8_t *)p + offsetof(RxBuff_t, buff);
        p->custom_free_function = pbuf_free_custom;
        pbuf_alloced_custom(PBUF_RAW, 0, PBUF_REF, p, *buff, ETH_RX_BUF_SIZE);
    } else {
        RxAllocStatus = RX_ALLOC_ERROR;
        *buff = NULL;
    }
}

void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length) {
    struct pbuf **ppStart = (struct pbuf **)pStart;
    struct pbuf **ppEnd = (struct pbuf **)pEnd;
    struct pbuf *p = (struct pbuf *)(buff - offsetof(RxBuff_t, buff));
    p->next = NULL;
    p->tot_len = 0;
    p->len = Length;

    if (!*ppStart) {
        *ppStart = p;
    } else {
        (*ppEnd)->next = p;
    }
    *ppEnd = p;

    for (p = *ppStart; p != NULL; p = p->next) {
        p->tot_len += Length;
    }
}

void HAL_ETH_TxFreeCallback(uint32_t *buff) { pbuf_free((struct pbuf *)buff); }
