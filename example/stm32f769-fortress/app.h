#ifndef CWIST_F769_FORTRESS_APP_H
#define CWIST_F769_FORTRESS_APP_H

#include <stdint.h>

#define FORTRESS_MAX_CONNS 288
#define FORTRESS_MAX_PAYLOAD (1024u * 1024u)
#define FORTRESS_DTCM __attribute__((section(".dtcm_bss")))

int cwist_stm32_app_init(void);
void cwist_stm32_app_update(uint32_t ipv4);

#endif
