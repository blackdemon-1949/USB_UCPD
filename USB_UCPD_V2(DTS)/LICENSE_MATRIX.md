# LICENSE_MATRIX

License, source and usage record for every component in the firmware.  The goal
of this matrix is to make the licensing posture explicit and to confirm that
**no GPL / copyleft source is copied into this firmware** in an incompatible
way, and that the APIE layer is original work.

Summary of the posture:

- The firmware combines **BSD-3-Clause**, **Apache-2.0** and **SLA0044 (ST
  proprietary)** components.
- **No GPL / AGPL / LGPL copyleft code** is used or copied.
- The **APIE intelligence layer** (`apie_*.c`/`apie_*.h`, `tools/`) is original
  work.  It does not copy any library source; it references normative spec field
  positions and public library APIs only.
- One component (`Boot/.../w25qxx_xspi.c`, WeAct) carries **no license
  statement** — treat as unspecified and verify before redistribution.

## Legend

- **Compatible?** — Is the license compatible with combining into a single
  firmware image that is not itself released under a copyleft license?
- **Copyleft?** — Does the license impose copyleft obligations that would be
  triggered by linking/reusing it in this firmware?

## Components

| Component | Source path | License | Copyleft? | Compatible? | Use in this firmware | License text location |
| --- | --- | --- | --- | --- | --- | --- |
| STM32H7RSxx HAL Driver | `Drivers/STM32H7RSxx_HAL_Driver/` | BSD-3-Clause | No | Yes | UCPD, XSPI, I2C, USART, GPIO, DMA | `Drivers/STM32H7RSxx_HAL_Driver/LICENSE.txt` |
| CMSIS-Core (ARM) | `Drivers/CMSIS/` | Apache-2.0 | No | Yes | M7 core, vector/startup support | `Drivers/CMSIS/LICENSE.txt` |
| CMSIS Device (ST) | `Drivers/CMSIS/Device/ST/STM32H7RSxx/` | Apache-2.0 | No | Yes | H7R3xx device header | `Drivers/CMSIS/Device/ST/STM32H7RSxx/LICENSE.txt` |
| STM32 USBPD Core | `Middlewares/ST/STM32_USBPD_Library/Core/` | SLA0044 (ST SW License Agreement) | No (permissive/proprietary) | Yes — under SLA0044 terms | PD stack: CAD/PRL/PE/DPM, PPS/EPR/AVS timers | `Middlewares/ST/STM32_USBPD_Library/Core/LICENSE.txt` |
| STM32 USBPD Device glue | `Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/` | SLA0044 | No | Yes — under SLA0044 terms | UCPD HW interface, DMA + RX/TX interrupts | `Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/LICENSE.txt` |
| STM32 USB Device Library (CDC) | `Middlewares/ST/STM32_USB_Device_Library/` | SLA0044 | No | Yes — under SLA0044 terms | USB CDC serial console | `Middlewares/ST/STM32_USB_Device_Library/LICENSE.txt` |
| STM32 ExtMem Manager | `Middlewares/ST/STM32_ExtMem_Manager/` | ST software; **no LICENSE file present** (header: "provided AS-IS") | No | Verify before redistribution | XIP boot over external NOR | (no LICENSE.txt in the component; see source header) |
| WeAct W25Qxx XSPI driver | `Boot/Core/Src/w25qxx_xspi.c`, `Boot/Core/Inc/w25qxx_xspi.h` | **None stated** ("By zhuyix 2021.01.10") | Unknown | Verify before redistribution | External NOR bring-up / XIP | (no license header) |
| ST startup / system | `Boot/Core/Startup/`, `Appli/Core/Startup/`, `system_stm32h7rsxx.c` | CMSIS default (Apache-2.0) | No | Yes | Vector table, system init | `Drivers/CMSIS/LICENSE.txt` |
| **APIE layer (new)** | `Appli/Core/Src/apie_*.c`, `Appli/Core/Inc/apie_*.h` | **Original work (this project)** | No | Yes | Analysis, ML, scheduler, DB, cable/EPR, CLI | (project-authored) |
| **Host tooling (new)** | `tools/apie_*.py`, `tools/apie_*.sh`, `tools/apie_decode_selftest.c` | **Original work (this project)** | No | Yes | Host decode verification / training / build checks | (project-authored) |
| Application glue + CLI | `Appli/Core/Src/app_pd.c`, `Appli/Core/Src/app_cli.c`, `Appli/Core/Src/main.c`, `Appli/USBPD/Target/usbpd_vdm_user.c` | Original project code (based on ST CubeMX template) | No | Yes | PD application / console / APIE hooks | (project-authored) |

## Actions taken to keep the firmware license-clean

1. **Original APIE implementation.**  No ST/third-party library source was
   copied into `apie_*.c`/`apie_*.h`.  Field layouts are written from the USB PD
   specification; where the spec and the ST headers were cross-checked, only the
   resulting constant/field positions are used, not ST code.
2. **No GPL/copyleft introduced.**  The two ambiguous components (WeAct XSPI
   driver, ExtMem Manager) are pre-existing Boot-path components that were
   already in the repository.  They are recorded as "verify before
   redistribution" rather than masked.  They are not GPL.
3. **SLA0044 usage.**  The ST USBPD / USB Device libraries are used as linked
   third-party binaries/sources under ST's SLA0044 terms, not relicensed.  The
   APIE layer does not modify them.

## Caveats / open items

- **WeAct W25Qxx driver license is unspecified** — do not redistribute without
  confirming the author's terms.  It lives only in the Boot project.
- **ExtMem Manager ships without a LICENSE.txt** in this checkout; the source
  headers say "provided AS-IS" when no LICENSE file accompanies it.
- **SLA0044 is ST-proprietary**: verify the exact redistribution terms for the
  ST middleware before commercial distribution.
