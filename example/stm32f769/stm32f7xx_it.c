#include "stm32f7xx_hal.h"

extern ETH_HandleTypeDef EthHandle;

void SysTick_Handler(void) { HAL_IncTick(); }

void ETH_IRQHandler(void) { HAL_ETH_IRQHandler(&EthHandle); }
