#include <cwist/sys/port.h>

#include "stm32h7xx_hal.h"

#include <stdint.h>
#include <string.h>

extern RNG_HandleTypeDef hrng;
extern UART_HandleTypeDef huart3;

uint64_t cwist_port_monotonic_ns(void) {
    static uint32_t previous_ms;
    static uint64_t epoch_ns;
    uint32_t now_ms = HAL_GetTick();
    if (now_ms < previous_ms) epoch_ns += ((uint64_t)UINT32_MAX + 1u) * 1000000u;
    previous_ms = now_ms;
    return epoch_ns + (uint64_t)now_ms * 1000000u;
}

int cwist_port_entropy(void *buf, size_t len) {
    uint8_t *out = buf;
    while (len > 0) {
        uint32_t word;
        if (HAL_RNG_GenerateRandomNumber(&hrng, &word) != HAL_OK) return -1;
        size_t take = len < sizeof(word) ? len : sizeof(word);
        memcpy(out, &word, take);
        out += take;
        len -= take;
    }
    return 0;
}

int _write(int fd, const char *buf, int len) {
    (void)fd;
    return HAL_UART_Transmit(&huart3, (const uint8_t *)buf, (uint16_t)len, 1000) == HAL_OK ? len : -1;
}
