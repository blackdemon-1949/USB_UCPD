# ==============================================================================
#  USB_UCPD_V4 - GNU Arm Embedded toolchain build
#
#  Builds the two STM32H7R3Z8Jx projects without STM32CubeIDE:
#
#    make            -> bootloader + application
#    make appli      -> application only  (Appli/STM32H7R3Z8JX_ROMxspi1.ld)
#    make boot       -> bootloader only   (Boot/STM32H7R3Z8JX_FLASH.ld)
#    make clean      -> remove build/ and generated .elf/.bin/.hex/.map
#
#  The compiler flags, defines, include paths, source file list and linker
#  scripts are exactly the ones stored in Appli/.cproject, Boot/.cproject and
#  Appli/.project, so a CubeIDE build and this build produce equivalent
#  objects (only the libc differs, see TOOLCHAIN notes below).
#
#  Toolchain: any arm-none-eabi-gcc >= 11.  Override with
#      make TOOLCHAIN_DIR=/path/to/bin
#  The image links against picolibc (libc.a/libm.a) found through
#  picolibc.specs, which is what the repository's self-contained toolchain
#  in Tools/fetch_toolchain.sh installs.  If your toolchain ships newlib
#  nano instead, build with  make LIBC=newlib
# ==============================================================================

# ------------------------------------------------------------------ toolchain
TOOLCHAIN_DIR ?=
ifneq ($(TOOLCHAIN_DIR),)
CROSS := $(TOOLCHAIN_DIR)/arm-none-eabi-
else
CROSS := arm-none-eabi-
endif

CC      := $(CROSS)gcc
AS      := $(CC)
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size
NM      := $(CROSS)nm

# ------------------------------------------------------------------- libc
LIBC ?= picolibc
ifeq ($(LIBC),picolibc)
LIBSPECS := -specs=picolibc.specs -nostartfiles
else
LIBSPECS := -specs=nano.specs -specs=nosys.specs
endif

# ------------------------------------------------------------------- flags
CPU      := -mcpu=cortex-m7
FPU      := -mfpu=fpv5-d16 -mfloat-abi=hard
THUMB    := -mthumb
COMMON   := $(CPU) $(FPU) $(THUMB) -g3 -O0 $(LIBSPECS) -Wall \
            -ffunction-sections -fdata-sections

# Linker: map + garbage collect + printable floats (nano only, harmless otherwise)
LDFLAGS_COMMON := $(CPU) $(FPU) $(THUMB) $(LIBSPECS) -Wl,--gc-sections \
                  -Wl,--print-memory-usage -static

# ==============================================================================
#  Application (Appli)
# ==============================================================================
APPLI_DIR    := Appli
APPLI_DEFS   := -DDEBUG -DUSE_HAL_DRIVER -DSTM32H7R3xx -DUSE_FULL_LL_DRIVER \
                -DUSBPD_PORT_COUNT=1 -D_SNK -D_TRACE -DUSBPDCORE_LIB_PD3_FULL
APPLI_INCS   := \
  -I$(APPLI_DIR)/Core/Inc \
  -I$(APPLI_DIR)/FATFS/Target \
  -I$(APPLI_DIR)/FATFS/App \
  -I$(APPLI_DIR)/USBPD/App \
  -I$(APPLI_DIR)/USBPD/Target \
  -I$(APPLI_DIR)/USB_DEVICE/App \
  -I$(APPLI_DIR)/USB_DEVICE/Target \
  -IDrivers/STM32H7RSxx_HAL_Driver/Inc \
  -IDrivers/STM32H7RSxx_HAL_Driver/Inc/Legacy \
  -IMiddlewares/Third_Party/FatFs/source \
  -IMiddlewares/Third_Party/FatFs/source/drivers/sd \
  -IMiddlewares/Third_Party/FatFs/source/drivers/template \
  -IMiddlewares/ST/STM32_USBPD_Library/Core/inc \
  -IMiddlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/inc \
  -IMiddlewares/ST/STM32_USB_Device_Library/Core/Inc \
  -IMiddlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc \
  -IDrivers/CMSIS/Device/ST/STM32H7RSxx/Include \
  -IDrivers/CMSIS/Include
APPLI_LDSCRIPT := $(APPLI_DIR)/STM32H7R3Z8JX_ROMxspi1.ld
APPLI_LIB      := Middlewares/ST/STM32_USBPD_Library/Core/lib/USBPDCORE_PD3_FULL_CM7_wc32.a

# SD card / FatFs.  Keep in step with APP_SD_ENABLED in app_config.h: with
# SD_ENABLED=0 these sources are left out of the build entirely (the files
# themselves stay in the project, so an STM32CubeIDE build is unaffected).
SD_ENABLED ?= 0
ifeq ($(SD_ENABLED),1)
APPLI_SD_SRC := \
  $(wildcard $(APPLI_DIR)/FATFS/App/*.c) \
  Middlewares/Third_Party/FatFs/source/diskio.c \
  Middlewares/Third_Party/FatFs/source/ff.c \
  Middlewares/Third_Party/FatFs/source/ff_gen_drv.c \
  Middlewares/Third_Party/FatFs/source/ffunicode.c \
  Middlewares/Third_Party/FatFs/source/drivers/sd/sd_diskio.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_sd.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_sd_ex.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_ll_sdmmc.c
endif

APPLI_SRC := \
  $(APPLI_DIR)/Core/Startup/startup_stm32h7r3z8jx.s \
  $(wildcard $(APPLI_DIR)/Core/Src/*.c) \
  $(APPLI_SD_SRC) \
  $(APPLI_DIR)/USBPD/App/usbpd.c \
  $(APPLI_DIR)/USBPD/App/usbpd_dpm_core.c \
  $(APPLI_DIR)/USBPD/App/usbpd_pwr_if.c \
  $(APPLI_DIR)/USBPD/Target/usbpd_dpm_user.c \
  $(APPLI_DIR)/USBPD/Target/usbpd_pwr_user.c \
  $(APPLI_DIR)/USBPD/Target/usbpd_vdm_user.c \
  $(APPLI_DIR)/USB_DEVICE/App/usbd_cdc_if.c \
  $(APPLI_DIR)/USB_DEVICE/App/usbd_desc.c \
  $(APPLI_DIR)/USB_DEVICE/App/usb_device.c \
  $(APPLI_DIR)/USB_DEVICE/Target/usbd_conf.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_cortex.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_dma.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_dma_ex.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_dts.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_exti.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_flash.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_flash_ex.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_gpio.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_i2c.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_i2c_ex.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_pcd.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_pcd_ex.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_pwr.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_pwr_ex.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_rcc.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_rcc_ex.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_uart.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_uart_ex.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_ll_dlyb.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_ll_dma.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_ll_exti.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_ll_gpio.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_ll_rcc.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_ll_tim.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_ll_ucpd.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_ll_usart.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_ll_usb.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_ll_utils.c \
  Middlewares/ST/STM32_USBPD_Library/Core/src/usbpd_trace.c \
  Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/src/usbpd_cad_hw_if.c \
  Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/src/usbpd_hw.c \
  Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/src/usbpd_hw_if_it.c \
  Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/src/usbpd_phy.c \
  Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/src/usbpd_phy_hw_if.c \
  Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/src/usbpd_pwr_hw_if.c \
  Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/src/usbpd_timersserver.c \
  Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_core.c \
  Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ctlreq.c \
  Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ioreq.c \
  Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Src/usbd_cdc.c \

# ==============================================================================
#  Bootloader (Boot) - DO NOT CHANGE: boot/FLASH.ld and its config are frozen
# ==============================================================================
BOOT_DIR    := Boot
BOOT_DEFS   := -DDEBUG -DUSE_HAL_DRIVER -DSTM32H7R3xx
BOOT_INCS   := \
  -I$(BOOT_DIR)/Core/Inc \
  -IDrivers/STM32H7RSxx_HAL_Driver/Inc \
  -IDrivers/STM32H7RSxx_HAL_Driver/Inc/Legacy \
  -IMiddlewares/ST/STM32_ExtMem_Manager \
  -IMiddlewares/ST/STM32_ExtMem_Manager/boot \
  -IMiddlewares/ST/STM32_ExtMem_Manager/sal \
  -IMiddlewares/ST/STM32_ExtMem_Manager/nor_sfdp \
  -IMiddlewares/ST/STM32_ExtMem_Manager/psram \
  -IMiddlewares/ST/STM32_ExtMem_Manager/sdcard \
  -IMiddlewares/ST/STM32_ExtMem_Manager/user \
  -IDrivers/CMSIS/Device/ST/STM32H7RSxx/Include \
  -IDrivers/CMSIS/Include
BOOT_LDSCRIPT := $(BOOT_DIR)/STM32H7R3Z8JX_FLASH.ld

BOOT_SRC := \
  $(BOOT_DIR)/Core/Startup/startup_stm32h7r3z8jx.s \
  $(wildcard $(BOOT_DIR)/Core/Src/*.c) \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_cortex.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_dma.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_dma_ex.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_exti.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_flash.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_flash_ex.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_gpio.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_pwr.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_pwr_ex.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_rcc.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_rcc_ex.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_xspi.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_ll_exti.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_ll_gpio.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_ll_rcc.c \
  Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_ll_utils.c \
  Middlewares/ST/STM32_ExtMem_Manager/stm32_extmem.c \
  Middlewares/ST/STM32_ExtMem_Manager/boot/stm32_boot_xip.c \
  Middlewares/ST/STM32_ExtMem_Manager/sal/stm32_sal_sd.c \
  Middlewares/ST/STM32_ExtMem_Manager/sal/stm32_sal_xspi.c \
  Middlewares/ST/STM32_ExtMem_Manager/nor_sfdp/stm32_sfdp_data.c \
  Middlewares/ST/STM32_ExtMem_Manager/nor_sfdp/stm32_sfdp_driver.c \
  Middlewares/ST/STM32_ExtMem_Manager/psram/stm32_psram_driver.c \
  Middlewares/ST/STM32_ExtMem_Manager/sdcard/stm32_sdcard_driver.c \
  Middlewares/ST/STM32_ExtMem_Manager/user/stm32_user_driver.c

# ==============================================================================
#  Build rules
# ==============================================================================
BUILD_DIR := build

.PHONY: all appli boot clean size help

all: appli boot

appli: $(APPLI_DIR)/USB_UCPD_Appli.elf $(APPLI_DIR)/USB_UCPD_Appli.bin $(APPLI_DIR)/USB_UCPD_Appli.hex
boot:  $(BOOT_DIR)/USB_UCPD_Boot.elf  $(BOOT_DIR)/USB_UCPD_Boot.bin  $(BOOT_DIR)/USB_UCPD_Boot.hex

# --- generic compile rules (one object dir per project) ----------------------
$(BUILD_DIR)/appli/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(COMMON) $(APPLI_DEFS) $(APPLI_INCS) $< -o $@ \
	  -MMD -MP -MF $(@:.o=.d)

$(BUILD_DIR)/appli/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) -c $(COMMON) $(APPLI_DEFS) $(APPLI_INCS) $< -o $@

$(BUILD_DIR)/boot/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(COMMON) $(BOOT_DEFS) $(BOOT_INCS) $< -o $@ \
	  -MMD -MP -MF $(@:.o=.d)

$(BUILD_DIR)/boot/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) -c $(COMMON) $(BOOT_DEFS) $(BOOT_INCS) $< -o $@

# --- link -------------------------------------------------------------------
APPLI_OBJS := $(addprefix $(BUILD_DIR)/appli/,$(APPLI_SRC:.c=.o))
APPLI_OBJS := $(APPLI_OBJS:.s=.o)
BOOT_OBJS  := $(addprefix $(BUILD_DIR)/boot/,$(BOOT_SRC:.c=.o))
BOOT_OBJS  := $(BOOT_OBJS:.s=.o)

$(APPLI_DIR)/USB_UCPD_Appli.elf: $(APPLI_OBJS) $(APPLI_LIB) $(APPLI_LDSCRIPT)
	$(CC) $(APPLI_OBJS) $(APPLI_LIB) $(LDFLAGS_COMMON) \
	  -T$(APPLI_LDSCRIPT) -Wl,-Map=$(@:.elf=.map) -o $@
	$(SIZE) $@ /dev/null 2>/dev/null || $(SIZE) $@ > /dev/null

$(BOOT_DIR)/USB_UCPD_Boot.elf: $(BOOT_OBJS) $(BOOT_LDSCRIPT)
	$(CC) $(BOOT_OBJS) $(LDFLAGS_COMMON) \
	  -T$(BOOT_LDSCRIPT) -Wl,-Map=$(@:.elf=.map) -o $@
	$(SIZE) $@ /dev/null 2>/dev/null || $(SIZE) $@ > /dev/null

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@
%.hex: %.elf
	$(OBJCOPY) -O ihex $< $@

# --- reporting --------------------------------------------------------------
size: appli boot
	@echo "==== application ===="; $(SIZE) $(APPLI_DIR)/USB_UCPD_Appli.elf
	@echo "==== bootloader ====="; $(SIZE) $(BOOT_DIR)/USB_UCPD_Boot.elf

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(APPLI_DIR)/USB_UCPD_Appli.elf $(APPLI_DIR)/USB_UCPD_Appli.bin \
	      $(APPLI_DIR)/USB_UCPD_Appli.hex $(APPLI_DIR)/USB_UCPD_Appli.map
	rm -f $(BOOT_DIR)/USB_UCPD_Boot.elf $(BOOT_DIR)/USB_UCPD_Boot.bin \
	      $(BOOT_DIR)/USB_UCPD_Boot.hex $(BOOT_DIR)/USB_UCPD_Boot.map

help:
	@echo "make [appli|boot|all|size|clean]"
	@echo "  TOOLCHAIN_DIR=/opt/arm-gnu-toolchain/bin   compiler location"
	@echo "  LIBC=picolibc|newlib                       C library flavour"

-include $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)
