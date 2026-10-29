/* CWIST on STM32F769I-DISCO: /hello and /json over real Ethernet.
 * Skeleton (clocks, ETH GPIO, PHY) comes from STM32CubeF7's
 * LwIP_HTTP_Server_Netconn_RTOS project, de-RTOSed to NO_SYS polling. */
#include "stm32f7xx_hal.h"

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/dhcp.h"
#include "lwip/stats.h"
#include "netif/ethernet.h"

#include "ethernetif.h"
#include "app.h"

#include <stdio.h>
#include <string.h>

extern ETH_HandleTypeDef EthHandle;
extern volatile unsigned rx_irq_count;

UART_HandleTypeDef huart1;
RNG_HandleTypeDef hrng;

static struct netif gnetif;

static void CPU_CACHE_Enable(void) {
    /* D-cache stays off: this CubeF7 HAL ETH driver does no cache
     * maintenance, so DMA buffers would go stale. I-cache only. */
    SCB_EnableICache();
}

static void SystemClock_Config(void) {
    RCC_ClkInitTypeDef RCC_ClkInitStruct;
    RCC_OscInitTypeDef RCC_OscInitStruct;

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM = 25;
    RCC_OscInitStruct.PLL.PLLN = 400;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ = 9;
    RCC_OscInitStruct.PLL.PLLR = 7;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) goto fail;
    if (HAL_PWREx_EnableOverDrive() != HAL_OK) goto fail;

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_7) != HAL_OK) goto fail;
    return;
fail:
    for (;;) {
    }
}

static void UART_Init(void) {
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    /* ST-LINK virtual COM port: USART1 TX=PA9 RX=PA10, AF7 */
    gpio.Pin = GPIO_PIN_9 | GPIO_PIN_10;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_HIGH;
    gpio.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &gpio);

    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart1) != HAL_OK) {
        for (;;) {
        }
    }
}

static void RNG_Init(void) {
    __HAL_RCC_RNG_CLK_ENABLE();
    hrng.Instance = RNG;
    if (HAL_RNG_Init(&hrng) != HAL_OK) {
        for (;;) {
        }
    }
}

static void Netif_Config(void) {
    ip_addr_t ipaddr, netmask, gw;
    ip_addr_set_zero_ip4(&ipaddr);
    ip_addr_set_zero_ip4(&netmask);
    ip_addr_set_zero_ip4(&gw);

    lwip_init();
    netif_add(&gnetif, &ipaddr, &netmask, &gw, NULL, &ethernetif_init, &ethernet_input);
    netif_set_default(&gnetif);
    netif_set_up(&gnetif);
    dhcp_start(&gnetif);
}

int main(void) {
    CPU_CACHE_Enable();
    HAL_Init();
    SystemClock_Config();
    UART_Init();
    RNG_Init();

    printf("cwist-f769: boot\r\n");

    Netif_Config();

    if (cwist_stm32_app_init() != 0) {
        printf("cwist-f769: app init FAILED\r\n");
        for (;;) {
        }
    }
    printf("cwist-f769: listening on port 80 (DHCP)\r\n");

    uint32_t link_tick = 0;
    uint32_t stats_tick = 0;
    uint32_t reported_ip = 0;
    for (;;) {
        ethernetif_poll(&gnetif);
        sys_check_timeouts();

        uint32_t now = HAL_GetTick();
        if (now - link_tick >= 100) {
            link_tick = now;
            ethernet_link_check(&gnetif);
        }
        if (now - stats_tick >= 5000) {
            stats_tick = now;
            printf("stats: link_rx=%u ip_rx=%u ip_drop=%u ip_chk=%u icmp_rx=%u tcp_rx=%u udp_rx=%u memp_err=%u\r\n",
                   (unsigned)lwip_stats.link.recv, (unsigned)lwip_stats.ip.recv,
                   (unsigned)lwip_stats.ip.drop, (unsigned)lwip_stats.ip.chkerr,
                   (unsigned)lwip_stats.icmp.recv, (unsigned)lwip_stats.tcp.recv,
                   (unsigned)lwip_stats.udp.recv, (unsigned)lwip_stats.memp[MEMP_PBUF_POOL]->err);
            printf("eth: mac_uc=%lu mac_crcerr=%lu dma_sr=%08lx rx_irq=%u gstate=%lx\r\n",
                   (unsigned long)EthHandle.Instance->MMCRGUFCR,
                   (unsigned long)EthHandle.Instance->MMCRFCECR,
                   (unsigned long)EthHandle.Instance->DMASR,
                   rx_irq_count, (unsigned long)EthHandle.gState);
        }

        uint32_t ip = gnetif.ip_addr.addr;
        if (ip != reported_ip && ip != 0) {
            reported_ip = ip;
            printf("cwist-f769: IP %u.%u.%u.%u\r\n",
                   (unsigned)(ip & 0xff), (unsigned)((ip >> 8) & 0xff),
                   (unsigned)((ip >> 16) & 0xff), (unsigned)(ip >> 24));
        }
    }
}
