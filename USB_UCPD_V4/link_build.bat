@echo off
set GCC="C:\ST\STM32CubeIDE_2.2.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740\tools\bin\arm-none-eabi-gcc.exe"

%GCC% apie.o apie_bkp.o apie_learn.o apie_ml.o apie_sdlog.o app_cli.o app_sd.o main.o sdmmc.o ^
 -T Appli/STM32H7R3Z8JX_ROMxspi1.ld ^
 -L Middlewares/ST/STM32_USBPD_Library/Core/lib ^
 -l:USBPDCORE_PD3_FULL_CM7_wc32.a ^
 -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard ^
 -specs=nano.specs -specs=nosys.specs ^
 -Wl,--gc-sections -Wl,-Map=build/Appli.map ^
 -o build/Appli.elf

if %errorlevel% neq 0 (
    echo Link failed
    exit /b %errorlevel%
) else (
    echo Link successful
    %GCC% -v --version
)