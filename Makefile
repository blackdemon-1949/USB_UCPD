#
# Makefile for USB_UCPD (STM32H7R3Z8Jx / WeAct H7R3 Core Board)
#
# Targets:
#   make            – build Appli (XSPI1 XiP image, linked with STM32H7R3Z8JX_ROMxspi1.ld)
#   make boot       – build the 64 KB internal-FLASH bootloader
#   make all        – build both Appli + Boot
#   make clean      – remove build artefacts
#
# Requires arm-none-eabi-gcc on PATH (or pass CCPREFIX=/path/to/bin/arm-none-eabi-).
#

CCPREFIX ?= arm-none-eabi-
CC       := $(CCPREFIX)gcc
AS       := $(CCPREFIX)gcc -x assembler-with-cpp
CP       := $(CCPREFIX)objcopy
SZ       := $(CCPREFIX)size
OD       := $(CCPREFIX)objdump

MCU   := -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard
COMMON_DEFS := -DUSE_HAL_DRIVER -DSTM32H7R3xx -DUSE_FULL_LL_DRIVER \
               -DUSBPD_PORT_COUNT=1 -D_SNK -D_TRACE -DUSBPDCORE_LIB_PD3_FULL
OPT   := -Os -ffunction-sections -fdata-sections -fno-common -fstack-usage
WARN  := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
          -Wno-sign-compare -Wno-unused-variable -Wno-unused-but-set-variable
CFLAGS  := $(MCU) $(OPT) $(WARN) -MMD -MP -g -std=gnu11
ASFLAGS := $(MCU) -g
LDFLAGS := $(MCU) -specs=nano.specs -specs=nosys.specs \
           -Wl,--gc-sections -Wl,-Map=$(@:.elf=.map),--cref -Wl,--print-memory-usage

# Shared include paths (root-level Drivers + Middlewares)
ROOT_INC := \
  -IDrivers/STM32H7RSxx_HAL_Driver/Inc \
  -IDrivers/STM32H7RSxx_HAL_Driver/Inc/Legacy \
  -IDrivers/CMSIS/Device/ST/STM32H7RSxx/Include \
  -IDrivers/CMSIS/Include \
  -IMiddlewares/ST/STM32_USBPD_Library/Core/inc \
  -IMiddlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/inc \
  -IMiddlewares/ST/STM32_USB_Device_Library/Core/Inc \
  -IMiddlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc \
  -IMiddlewares/Third_Party/FatFs/source \
  -IMiddlewares/Third_Party/FatFs/source/drivers/sd \
  -IMiddlewares/ST/STM32_ExtMem_Manager

# All root HAL sources (available for both targets; linker GC discards unused)
HAL_SRC_DIR := Drivers/STM32H7RSxx_HAL_Driver/Src
HAL_SRCS := $(wildcard $(HAL_SRC_DIR)/stm32h7rsxx_hal_*.c) \
           $(HAL_SRC_DIR)/stm32h7rsxx_ll_dlyb.c \
           $(HAL_SRC_DIR)/stm32h7rsxx_ll_sdmmc.c \
           $(HAL_SRC_DIR)/stm32h7rsxx_ll_usart.c
SYS_SRC   := Drivers/CMSIS/Device/ST/STM32H7RSxx/Source/Templates/system_stm32h7rsxx.c

# ============================================================================
# Appli (XSPI1 XiP application @ 0x90000000)
# ============================================================================
APP_DIR    := Appli
APP_BUILD  := build/appli
APP_BIN    := $(APP_BUILD)/usb_ucpd_app.elf

APP_INC := \
  -I$(APP_DIR)/Core/Inc \
  -I$(APP_DIR)/USBPD/App \
  -I$(APP_DIR)/USBPD/Target \
  -I$(APP_DIR)/USB_DEVICE/App \
  -I$(APP_DIR)/USB_DEVICE/Target \
  -I$(APP_DIR)/FATFS/App \
  -I$(APP_DIR)/FATFS/Target \
  $(ROOT_INC)

# CubeMX-generated core
APP_CUBE_SRCS := \
  $(APP_DIR)/Core/Src/main.c \
  $(APP_DIR)/Core/Src/gpio.c \
  $(APP_DIR)/Core/Src/gpdma.c \
  $(APP_DIR)/Core/Src/i2c.c \
  $(APP_DIR)/Core/Src/usart.c \
  $(APP_DIR)/Core/Src/ucpd.c \
  $(APP_DIR)/Core/Src/dts.c \
  $(APP_DIR)/Core/Src/sdmmc.c \
  $(APP_DIR)/Core/Src/stm32h7rsxx_hal_msp.c \
  $(APP_DIR)/Core/Src/stm32h7rsxx_it.c \
  $(APP_DIR)/Core/Src/sysmem.c \
  $(APP_DIR)/Core/Src/syscalls.c \
  $(SYS_SRC)

# V2 application-level sources (APIE, CLI, OLED, INA226, tracer, extensions)
APP_USER_SRCS := \
  $(APP_DIR)/Core/Src/apie.c \
  $(APP_DIR)/Core/Src/apie_analyzer.c \
  $(APP_DIR)/Core/Src/apie_bkp.c \
  $(APP_DIR)/Core/Src/apie_cable.c \
  $(APP_DIR)/Core/Src/apie_db.c \
  $(APP_DIR)/Core/Src/apie_decode.c \
  $(APP_DIR)/Core/Src/apie_ml.c \
  $(APP_DIR)/Core/Src/apie_plan.c \
  $(APP_DIR)/Core/Src/apie_profile.c \
  $(APP_DIR)/Core/Src/apie_selftest.c \
  $(APP_DIR)/Core/Src/apie_stats.c \
  $(APP_DIR)/Core/Src/apie_unknown.c \
  $(APP_DIR)/Core/Src/app_board.c \
  $(APP_DIR)/Core/Src/app_cli.c \
  $(APP_DIR)/Core/Src/app_log.c \
  $(APP_DIR)/Core/Src/app_oled.c \
  $(APP_DIR)/Core/Src/app_pd.c \
  $(APP_DIR)/Core/Src/app_profile.c \
  $(APP_DIR)/Core/Src/dtsmon.c \
  $(APP_DIR)/Core/Src/ext_dts.c \
  $(APP_DIR)/Core/Src/ext_i2c.c \
  $(APP_DIR)/Core/Src/ext_uart.c \
  $(APP_DIR)/Core/Src/ina226.c \
  $(APP_DIR)/Core/Src/oled_font.c \
  $(APP_DIR)/Core/Src/ssd1306.c \
  $(APP_DIR)/Core/Src/tracer_emb.c \
  $(APP_DIR)/Core/Src/tracer_emb_hw.c

# USBPD: project glue + STM32H7RSxx device layer (sources), plus the
# prebuilt ST USBPD Core library (CubeMX ships this as a .a for PD3_FULL).
APP_USBPD_APP_SRCS := \
  $(APP_DIR)/USBPD/App/usbpd.c \
  $(APP_DIR)/USBPD/App/usbpd_dpm_core.c \
  $(APP_DIR)/USBPD/App/usbpd_pwr_if.c \
  $(APP_DIR)/USBPD/Target/usbpd_dpm_user.c \
  $(APP_DIR)/USBPD/Target/usbpd_pwr_user.c \
  $(APP_DIR)/USBPD/Target/usbpd_vdm_user.c

APP_USBPD_DEV_SRCS := \
  Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/src/usbpd_cad_hw_if.c \
  Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/src/usbpd_hw.c \
  Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/src/usbpd_hw_if_it.c \
  Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/src/usbpd_phy.c \
  Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/src/usbpd_phy_hw_if.c \
  Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/src/usbpd_pwr_hw_if.c \
  Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/src/usbpd_timersserver.c

APP_USBPD_CORE_LIB := Middlewares/ST/STM32_USBPD_Library/Core/lib/USBPDCORE_PD3_FULL_CM7_wc32.a

# USB Device (CDC)
APP_USBD_SRCS := \
  $(APP_DIR)/USB_DEVICE/App/usb_device.c \
  $(APP_DIR)/USB_DEVICE/App/usbd_desc.c \
  $(APP_DIR)/USB_DEVICE/App/usbd_cdc_if.c \
  $(APP_DIR)/USB_DEVICE/Target/usbd_conf.c \
  Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_core.c \
  Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ctlreq.c \
  Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ioreq.c \
  Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Src/usbd_cdc.c

# FATFS + SD card (V4 new feature; sources included, kept idle at runtime for now)
APP_FATFS_SRCS := \
  $(APP_DIR)/FATFS/App/fatfs.c \
  Middlewares/Third_Party/FatFs/source/ff.c \
  Middlewares/Third_Party/FatFs/source/ff_gen_drv.c \
  Middlewares/Third_Party/FatFs/source/diskio.c \
  Middlewares/Third_Party/FatFs/source/ffunicode.c \
  Middlewares/Third_Party/FatFs/source/drivers/sd/sd_diskio.c

APP_STARTUP := $(APP_DIR)/Core/Startup/startup_stm32h7r3z8jx.s
APP_LD      := $(APP_DIR)/STM32H7R3Z8JX_ROMxspi1.ld

APP_C_SRCS := $(APP_CUBE_SRCS) $(APP_USER_SRCS) $(APP_USBPD_APP_SRCS) \
              $(APP_USBPD_DEV_SRCS) \
              Middlewares/ST/STM32_USBPD_Library/Core/src/usbpd_trace.c \
              $(APP_USBD_SRCS) $(APP_FATFS_SRCS) $(HAL_SRCS)
APP_OBJS   := $(patsubst %.c,$(APP_BUILD)/%.o,$(APP_C_SRCS)) \
              $(patsubst %.s,$(APP_BUILD)/%.o,$(APP_STARTUP))
APP_DEPS   := $(APP_OBJS:.o=.d)

# ============================================================================
# Boot (64 KB internal FLASH bootloader @ 0x08000000)
# ============================================================================
BOOT_DIR    := Boot
BOOT_BUILD  := build/boot
BOOT_BIN    := $(BOOT_BUILD)/usb_ucpd_boot.elf

BOOT_INC := \
  -I$(BOOT_DIR)/Core/Inc \
  $(ROOT_INC) \
  -IMiddlewares/ST/STM32_ExtMem_Manager/boot

BOOT_SRCS := \
  $(BOOT_DIR)/Core/Src/main.c \
  $(BOOT_DIR)/Core/Src/gpio.c \
  $(BOOT_DIR)/Core/Src/gpdma.c \
  $(BOOT_DIR)/Core/Src/xspi.c \
  $(BOOT_DIR)/Core/Src/extmem_manager.c \
  $(BOOT_DIR)/Core/Src/w25qxx_xspi.c \
  $(BOOT_DIR)/Core/Src/stm32h7rsxx_hal_msp.c \
  $(BOOT_DIR)/Core/Src/stm32h7rsxx_it.c \
  $(BOOT_DIR)/Core/Src/sysmem.c \
  $(BOOT_DIR)/Core/Src/syscalls.c \
  Middlewares/ST/STM32_ExtMem_Manager/boot/stm32_boot_xip.c \
  $(SYS_SRC) \
  $(HAL_SRCS)

BOOT_STARTUP := $(BOOT_DIR)/Core/Startup/startup_stm32h7r3z8jx.s
BOOT_LD      := $(BOOT_DIR)/STM32H7R3Z8JX_FLASH.ld

BOOT_OBJS := $(patsubst %.c,$(BOOT_BUILD)/%.o,$(BOOT_SRCS)) \
             $(patsubst %.s,$(BOOT_BUILD)/%.o,$(BOOT_STARTUP))
BOOT_DEPS := $(BOOT_OBJS:.o=.d)

# ============================================================================
# Phony targets
# ============================================================================
.PHONY: all clean boot appli

all: appli boot

appli: $(APP_BIN)
boot:  $(BOOT_BIN)

# Link Appli
$(APP_BIN): $(APP_OBJS) $(APP_USBPD_CORE_LIB) $(APP_LD)
	@mkdir -p $(dir $@)
	$(CC) $(APP_OBJS) $(APP_USBPD_CORE_LIB) $(LDFLAGS) -T$(APP_LD) -o $@
	$(SZ) $@
	$(CP) -O ihex $@ $(@:.elf=.hex)
	$(CP) -O binary $@ $(@:.elf=.bin)
	@echo "Built $@"

# Link Boot
$(BOOT_BIN): $(BOOT_OBJS) $(BOOT_LD)
	@mkdir -p $(dir $@)
	$(CC) $(BOOT_OBJS) $(LDFLAGS) -T$(BOOT_LD) -o $@
	$(SZ) $@
	$(CP) -O ihex $@ $(@:.elf=.hex)
	$(CP) -O binary $@ $(@:.elf=.bin)
	@echo "Built $@"

# Pattern rules: compile C / assemble S
$(APP_BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(COMMON_DEFS) $(APP_INC) -c $< -o $@

$(APP_BUILD)/%.o: %.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $(COMMON_DEFS) $(APP_INC) -c $< -o $@

$(BOOT_BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(COMMON_DEFS) $(BOOT_INC) -c $< -o $@

$(BOOT_BUILD)/%.o: %.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $(COMMON_DEFS) $(BOOT_INC) -c $< -o $@

clean:
	rm -rf build

# Header dependency tracking
-include $(APP_DEPS) $(BOOT_DEPS)
