/**
  ******************************************************************************
  * @file    irq_priority.h
  * @brief   Central NVIC pre-emption priority map for the PD bench (Appli).
  *
  * Every interrupt in the firmware takes its priority from this header so the
  * ordering is stated once, in one place, and is independent of the order in
  * which the peripherals happen to be initialised.
  *
  * Grouping: NVIC_PRIORITYGROUP_4 == PRIGROUP 3 in SCB->AIRCR.  With
  * __NVIC_PRIO_BITS == 4 that gives 4 bits of pre-emption priority (0..15) and
  * 0 sub-priority bits, so the numbers below are the *raw* 4-bit values handed
  * to NVIC_SetPriority() / HAL_NVIC_SetPriority(): lower == more urgent, and
  * there is no sub-priority to confuse matters.
  *
  * NVIC_EncodePriority(NVIC_GetPriorityGrouping(), n, 0) == n under this
  * grouping, so both the HAL_NVIC_SetPriority(irq, n, 0) and the
  * NVIC_SetPriority(irq, n) spellings select the same pre-emption level.
  *
  * Required ordering (hard requirement):
  *
  *     UCPD  >  CDC (USB OTG_HS)  >  USART1  >  everything else
  *
  ******************************************************************************
  */

#ifndef IRQ_PRIORITY_H
#define IRQ_PRIORITY_H

#include "stm32h7rsxx_hal.h"

/* ---------------------------------------------------------------------------
   The grouping.  HAL_Init() already installs this, but it is restated and
   applied explicitly in main() so the levels below cannot silently change
   meaning if the HAL default or a CubeMX regeneration ever differs.
   --------------------------------------------------------------------------- */
#define IRQ_PRIORITY_GROUP            NVIC_PRIORITYGROUP_4

/* ---------------------------------------------------------------------------
   0 - UCPD link: the highest priority in the system, no exceptions.
   ---------------------------------------------------------------------------
   PD negotiation is the most timing-sensitive thing on the board; a delayed
   UCPD interrupt loses a GoodCRC / drops a message and the contract collapses.
   The two GPDMA1 channels that move UCPD1 RX/TX bytes sit at the same level:
   they are part of the UCPD data path, and at an equal priority they can never
   be pre-empted by a lower-priority stream either. */
#define IRQ_PRIO_UCPD                0U
#define IRQ_PRIO_UCPD_DMA            IRQ_PRIO_UCPD

/* ---------------------------------------------------------------------------
   1 - CDC console: USB OTG_HS.
   ---------------------------------------------------------------------------
   Second only to UCPD.  Enumeration and the control endpoint are timing
   critical: if the host's GET_DESCRIPTOR is not answered in time Windows
   reports "device descriptor request failed" / Code 10 / unknown device. */
#define IRQ_PRIO_CDC_USB             1U

/* ---------------------------------------------------------------------------
   2 - USART1: the USBPD TRACER_EMB trace port.
   ---------------------------------------------------------------------------
   Third, and the highest of the "everything else" group. */
#define IRQ_PRIO_USART1              2U

/* ---------------------------------------------------------------------------
   3 - GPDMA1 channel 2: the DMA that feeds USART1.
   ---------------------------------------------------------------------------
   One step below the UART it serves, so a USART1 ISR is never held off by its
   own DMA completion.  NOTE: tracer_emb_hw.c falls back to priority 0 -
   the highest in the system - when TRACER_EMB_TX_DMA_PRIORITY is not defined.
   This macro must stay defined. */
#define IRQ_PRIO_TRACE_DMA           3U

/* ---------------------------------------------------------------------------
   4 - USART2: the second (serial) console.
   --------------------------------------------------------------------------- */
#define IRQ_PRIO_CONSOLE             4U

/* ---------------------------------------------------------------------------
   5 - everything else (I2C2 / INA226, DTS, ...).
   ---------------------------------------------------------------------------
   SysTick stays at TICK_INT_PRIORITY (15) so the HAL time base never
   pre-empts a real-time stream. */
#define IRQ_PRIO_DEFAULT             5U

#endif /* IRQ_PRIORITY_H */
