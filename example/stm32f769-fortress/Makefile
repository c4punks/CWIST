CUBE   ?= $(HOME)/STM32CubeF7
CWIST  := $(abspath $(CURDIR)/../..)
BASE   := $(abspath $(CURDIR)/../stm32f769)
PRJ     = $(CUBE)/Projects/STM32F769I-Discovery/Applications/LwIP/LwIP_HTTP_Server_Netconn_RTOS
BSP     = $(CUBE)/Drivers/BSP/STM32F769I-Discovery
COMP    = $(CUBE)/Drivers/BSP/Components
CROSS  ?= arm-none-eabi-
BUILD   = build

ARCH    = -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard
DEFS    = -DUSE_HAL_DRIVER -DSTM32F769xx -DCWIST_BAREMETAL \
          -D_POSIX_THREADS=200809L -D_POSIX_READER_WRITER_LOCKS=200809L \
          -D_POSIX_TIMERS=200809L -D_POSIX_MONOTONIC_CLOCK=200809L \
          -D_POSIX_PRIORITY_SCHEDULING=200809L -D_Thread_local= -D__thread=
INCS    = -I. -I$(BASE) \
          -I$(CUBE)/Drivers/STM32F7xx_HAL_Driver/Inc \
          -I$(CUBE)/Drivers/STM32F7xx_HAL_Driver/Inc/Legacy \
          -I$(CUBE)/Drivers/CMSIS/Device/ST/STM32F7xx/Include \
          -I$(CUBE)/Drivers/CMSIS/Core/Include \
          -I$(CUBE)/Drivers/BSP/Components/lan8742 \
          -I$(BSP) -I$(CUBE)/Utilities/Fonts \
          -I$(CWIST)/include -I$(CWIST)/lib -I$(CWIST)/lib/cjson \
          -I$(CWIST)/lib/boringssl/include -I$(CWIST)/lib/libttak/include \
          -I$(CWIST)/lib/sqlite3 -I$(CWIST)/lib/lwip/src/include -I$(CWIST)/tests/lwip
CFLAGS  = -std=c17 -Os -Wall $(ARCH) --specs=nano.specs \
          -ffunction-sections -fdata-sections $(DEFS) $(INCS)

HAL_SRC = $(addprefix $(CUBE)/Drivers/STM32F7xx_HAL_Driver/Src/, \
          stm32f7xx_hal.c stm32f7xx_hal_rcc.c stm32f7xx_hal_rcc_ex.c \
          stm32f7xx_hal_cortex.c stm32f7xx_hal_gpio.c stm32f7xx_hal_eth.c \
          stm32f7xx_hal_uart.c stm32f7xx_hal_rng.c stm32f7xx_hal_pwr.c \
          stm32f7xx_hal_pwr_ex.c stm32f7xx_hal_flash.c stm32f7xx_hal_flash_ex.c \
          stm32f7xx_hal_dma.c stm32f7xx_hal_dma2d.c stm32f7xx_hal_i2c.c \
          stm32f7xx_hal_i2c_ex.c stm32f7xx_hal_ltdc.c stm32f7xx_hal_ltdc_ex.c \
          stm32f7xx_hal_dsi.c stm32f7xx_hal_sdram.c stm32f7xx_ll_fmc.c)
LWIP_SRC = $(wildcard $(CWIST)/lib/lwip/src/core/*.c) \
           $(wildcard $(CWIST)/lib/lwip/src/core/ipv4/*.c) \
           $(CWIST)/lib/lwip/src/netif/ethernet.c
BOARD_SRC = $(BSP)/stm32f769i_discovery.c $(BSP)/stm32f769i_discovery_sdram.c \
            $(BSP)/stm32f769i_discovery_lcd.c $(COMP)/otm8009a/otm8009a.c \
            $(COMP)/nt35510/nt35510.c $(COMP)/adv7533/adv7533.c
APP_SRC = main.c app.c dashboard.c \
          $(BASE)/ethernetif.c $(BASE)/port.c $(BASE)/syscalls.c $(BASE)/stm32f7xx_it.c \
          $(PRJ)/Src/system_stm32f7xx.c $(CUBE)/Drivers/BSP/Components/lan8742/lan8742.c \
          $(BOARD_SRC) $(CWIST)/src/sys/port/lwip.c $(HAL_SRC) $(LWIP_SRC)
ASM_SRC = $(PRJ)/SW4STM32/startup_stm32f769xx.s

OBJS = $(patsubst %.c,$(BUILD)/%.o,$(notdir $(filter %.c,$(APP_SRC)))) \
       $(BUILD)/startup_stm32f769xx.o
VPATH = . $(BASE) $(sort $(dir $(APP_SRC)))
CONFIG_DEPS = lwipopts.h $(BASE)/stm32f7xx_hal_conf.h $(BASE)/ethernetif.h app.h dashboard.h

all: $(BUILD)/cwist-f769-fortress.elf

$(CWIST)/libcwist_baremetal_fortress.a:
	$(MAKE) -C $(CWIST) baremetal BAREMETAL_BUILD_DIR=.baremetal-fortress-build \
	    BAREMETAL_ARCHIVE=libcwist_baremetal_fortress.a \
	    BAREMETAL_EXTRA_CFLAGS=-DCWIST_ARENA_DEFAULT_GENERATION_BYTES=4096

$(BUILD)/%.o: %.c $(CONFIG_DEPS)
	@mkdir -p $(BUILD)
	$(CROSS)gcc $(CFLAGS) -c -o $@ $<

$(BUILD)/startup_stm32f769xx.o: $(ASM_SRC)
	@mkdir -p $(BUILD)
	$(CROSS)gcc $(CFLAGS) -c -o $@ $<

$(BUILD)/cwist-f769-fortress.elf: $(OBJS) $(CWIST)/libcwist_baremetal_fortress.a stm32f769ni_flash.ld
	$(CROSS)gcc $(CFLAGS) --specs=nosys.specs -nostartfiles -T stm32f769ni_flash.ld \
	    -Wl,--gc-sections -Wl,-Map=$(BUILD)/cwist-f769-fortress.map -o $@ \
	    $(OBJS) $(CWIST)/libcwist_baremetal_fortress.a -lm
	$(CROSS)size $@

flash: $(BUILD)/cwist-f769-fortress.elf
	openocd -f interface/stlink.cfg -f target/stm32f7x.cfg \
	    -c "program $(BUILD)/cwist-f769-fortress.elf verify reset" -c shutdown

clean:
	rm -rf $(BUILD)

.PHONY: all flash clean
