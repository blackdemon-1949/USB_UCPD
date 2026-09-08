# THIRD_PARTY_SOURCES

Every external/third-party software component that is part of this firmware,
where it lives, what it is used for, and its license/availability.  This is the
single source of truth for "what did we bring in and under what terms".

Status labels are as defined in the task: `IMPLEMENTED`, `BUILD VERIFIED`,
`HOST VERIFIED`, `HARDWARE VERIFIED`, `RESEARCHED`, `OBSERVATION ONLY`,
`HARDWARE-LIMITED`, `DISABLED`, `UNTESTED`, `FUTURE`.

> **Compatibility note.** The APIE intelligence layer (everything under
> `Appli/Core/Src/apie_*.c`, `Appli/Core/Inc/apie_*.h`, `tools/*.py`,
> `tools/*.sh`) is original work authored for this project.  It does **not**
> copy GPL/copyleft code.  It references the *normative field layouts* of the
> USB Power Delivery specification and the **documented public API** of the ST
> USBPD / USB Device / HAL libraries.  No ST or third-party library source is
> copied into the APIE layer.

## Vendored components (already present in the repository)

| Component | Path | Purpose | License | Status |
| --- | --- | --- | --- | --- |
| STM32H7RSxx HAL Driver | `Drivers/STM32H7RSxx_HAL_Driver/` | Peripheral HAL + low-level drivers (UCPD, XSPI, I2C, USART, GPIO, DMA, timers) | BSD-3-Clause | BUILD VERIFIED |
| CMSIS-Core (ARM) | `Drivers/CMSIS/Core/` | ARM Cortex-M7 CMSIS core (templates, startup support) | Apache-2.0 | BUILD VERIFIED |
| CMSIS Device (ST) | `Drivers/CMSIS/Device/ST/STM32H7RSxx/` | STM32H7R3xx device header, IRQ/vector definitions | Apache-2.0 | BUILD VERIFIED |
| STM32 USBPD Library (Core) | `Middlewares/ST/STM32_USBPD_Library/Core/` | USB Power Delivery: CAD, PRL, PE, DPM core + PPS/EPR/AVS protocol timers | SLA0044 (ST Software License Agreement) | BUILD VERIFIED |
| STM32 USBPD Library (Device glue) | `Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/` | UCPD hardware interface, DMA + RX/TX IT handling | SLA0044 | BUILD VERIFIED |
| STM32 USB Device Library (CDC) | `Middlewares/ST/STM32_USB_Device_Library/` | USB Device core + CDC class (the serial console) | SLA0044 | BUILD VERIFIED |
| STM32 ExtMem Manager (XIP boot) | `Middlewares/ST/STM32_ExtMem_Manager/` | XIP boot over external NOR, SFDP/NOR drivers | ST software; **no LICENSE file present** (header says "provided AS-IS") | BUILD VERIFIED |
| WeAct W25Qxx XSPI driver | `Boot/Core/Src/w25qxx_xspi.c`, `Boot/Core/Inc/w25qxx_xspi.h` | External NOR (Puya PY25Q64HA) XSPI bring-up (reset, QE, 0xEB 1-4-4) | **No license statement** ("By zhuyix 2021.01.10") | BUILD VERIFIED |
| ST startup / system | `Boot/Core/Startup/`, `Appli/Core/Startup/`, `Boot/Core/Src/system_*.c`, `Appli/Core/Src/system_*.c` | Vector table + system init | Apache-2.0 (CMSIS) | BUILD VERIFIED |

## Components authored by this project (not third-party)

| Component | Path | License | Status |
| --- | --- | --- | --- |
| APIE decoder | `Appli/Core/Src/apie_decode.c` | Original | HOST VERIFIED + BUILD VERIFIED (no HW) |
| APIE analyzer + txn engine + features | `Appli/Core/Src/apie_analyzer.c` | Original | BUILD VERIFIED |
| APIE statistics | `Appli/Core/Src/apie_stats.c` | Original | BUILD VERIFIED |
| APIE ML (online NB + logistic seed) | `Appli/Core/Src/apie_ml.c` | Original | BUILD VERIFIED (seed model UNTESTED on HW) |
| APIE source profile | `Appli/Core/Src/apie_profile.c` | Original | BUILD VERIFIED |
| APIE unknown analyzer | `Appli/Core/Src/apie_unknown.c` | Original | BUILD VERIFIED |
| APIE scheduler / IG / experiments | `Appli/Core/Src/apie_plan.c` | Original | BUILD VERIFIED |
| APIE knowledge DB | `Appli/Core/Src/apie_db.c` | Original | BUILD VERIFIED (NOR persist DISABLED) |
| APIE cable + EPR awareness | `Appli/Core/Src/apie_cable.c` | Original | BUILD VERIFIED; EPR HARDWARE-LIMITED |
| APIE orchestrator + CLI | `Appli/Core/Src/apie.c` | Original | BUILD VERIFIED |
| Application PD glue (APIE hooks) | `Appli/Core/Src/app_pd.c`, `Appli/USBPD/Target/usbpd_vdm_user.c` | Original | BUILD VERIFIED |
| Host decoder (Python) | `tools/apie_decode.py` | Original | HOST VERIFIED |
| Host training pipeline | `tools/train_apie.py` | Original | HOST VERIFIED |
| Firmware decoder self-test | `tools/apie_decode_selftest.c`, `tools/apie_selftest.sh` | Original | HOST VERIFIED |
| Build/analysis tooling | `tools/check_syntax.sh`, `tools/check_symbols.py`, `tools/check_arm_build.py` | Original | BUILD VERIFIED |

## Documentation / reference sources (NOT vendored, recorded for provenance)

These are standards and credible references that the APIE field decodings and
policy rules are based on.  None of their source code is copied; only
*field positions* and *documented behaviour* that the specification licenses
for implementation use are referenced.

| Reference | What it provides | License of the reference |
| --- | --- | --- |
| USB Power Delivery Specification Revision 3.0 / 3.1 (USB-IF) | Message headers, PDO/APDO (PPS/AVS), VDM/SVDM field layouts, State machine timing | USB-IF public specification (free, published for use by implementers) |
| USB Type-C Specification Revision 2.0/2.1 (USB-IF) | SOP/SOP'/SOP'' ordered sets, plug orientation, CC detection semantics | USB-IF public specification |
| ST USBPD Library headers (`usbpd_def.h`) | The **authoritative bit-field struct** for PDO/APDO/AVS used by the ST stack (match-checked against the spec in this work) | SLA0044 |
| USB Implementers Forum PD 3.0 "Coming to Terms with PPS" (usb.org) | PPS voltage/current step rules (20 mV / 50 mA), PDP relationship | USB-IF presentation |
| ST Community USBPD PPS sink example (G0B1RE) | Cross-checked the APDO PPS struct field names and voltage/current scales | Forum example (used as a cross-check, not copied) |

---

*Generated as part of the APIE integration.  Keep in sync with the repository
contents; if a third-party component is added/removed, update this table.*
