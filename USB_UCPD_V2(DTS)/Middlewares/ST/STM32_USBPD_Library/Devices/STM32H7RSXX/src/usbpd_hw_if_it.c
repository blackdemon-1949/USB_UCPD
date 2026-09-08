/**
  ******************************************************************************
  * @file    usbpd_hw_if_it.c
  * @author  MCD Application Team
  * @brief   This file contains HW interface interrupt routines.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2022 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "usbpd_devices_conf.h"
#include "usbpd_core.h"
#include "usbpd_hw_if.h"
#include "usbpd_trace.h"

/* Advanced PD Intelligence Engine raw-capture bridge.  The analyzer COPIES a
   bounded number of bytes out of the ST RX buffer so the PRL DMA ownership
   (`Ports[PortNum].ptr_RxBuff`) is completely untouched. */
extern void APIE_Analyzer_CaptureRaw(uint8_t PortNum, uint8_t sop, const uint8_t *buf,
                                     uint32_t n);

static void USBPD_CacheInvalidateRx(const void *address, uint32_t size)
{
  uintptr_t start = ((uintptr_t)address) & ~(uintptr_t)31U;
  uint32_t length = (uint32_t)(((uintptr_t)address + size - start + 31U) & ~(uintptr_t)31U);
  SCB_InvalidateDCache_by_Addr((uint32_t *)start, (int32_t)length);
  __DSB();
}

/* Diagnostic counters, dumped by the CLI `pd` command. */
USBPD_DbgCounters_t g_usbpd_dbg;
#if defined(_LOW_POWER)
#include "usbpd_lowpower.h"
#endif /* _LOW_POWER */
#if defined(_FRS)
#include "usbpd_timersserver.h"
#endif /* _FRS */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
/* Private functions ---------------------------------------------------------*/
void PORTx_IRQHandler(uint8_t PortNum);

void USBPD_PORT0_IRQHandler(void)
{
  PORTx_IRQHandler(USBPD_PORT_0);
}

void PORTx_IRQHandler(uint8_t PortNum)
{
  UCPD_TypeDef *hucpd = Ports[PortNum].husbpd;
  uint32_t _interrupt = LL_UCPD_ReadReg(hucpd, SR);
  static uint8_t ovrflag = 0;

  g_usbpd_dbg.ucpd_irq++;

  if ((hucpd->IMR & _interrupt) != 0u)
  {
    /* TXIS no need to enable it all the transfer are done by DMA */
    if (UCPD_SR_TXMSGDISC == (_interrupt & UCPD_SR_TXMSGDISC))
    {
      g_usbpd_dbg.txmsgdisc++;
      /* Message has been discarded */
      LL_UCPD_ClearFlag_TxMSGDISC(hucpd);
      SET_BIT(Ports[PortNum].hdmatx->CCR, DMA_CCR_SUSP | DMA_CCR_RESET);
      while ((Ports[PortNum].hdmatx->CCR & DMA_CCR_EN) == DMA_CCR_EN);
      Ports[PortNum].cbs.USBPD_HW_IF_TxCompleted(PortNum, 1);
      return;
    }

    if (UCPD_SR_TXMSGSENT == (_interrupt & UCPD_SR_TXMSGSENT))
    {
      g_usbpd_dbg.txmsgsent++;
      /* Message has been fully transferred */
      LL_UCPD_ClearFlag_TxMSGSENT(hucpd);
      SET_BIT(Ports[PortNum].hdmatx->CCR, DMA_CCR_SUSP | DMA_CCR_RESET);
      while ((Ports[PortNum].hdmatx->CCR & DMA_CCR_EN) == DMA_CCR_EN);
      Ports[PortNum].cbs.USBPD_HW_IF_TxCompleted(PortNum, 0);

#if defined(_LOW_POWER)
      UTIL_LPM_SetStopMode(0 == PortNum ? LPM_PE_0 : LPM_PE_1, UTIL_LPM_ENABLE);
#endif /* _LOW_POWER */
      return;
    }

    if (UCPD_SR_TXMSGABT == (_interrupt & UCPD_SR_TXMSGABT))
    {
      g_usbpd_dbg.txmsgabt++;
      LL_UCPD_ClearFlag_TxMSGABT(hucpd);
      SET_BIT(Ports[PortNum].hdmatx->CCR, DMA_CCR_SUSP | DMA_CCR_RESET);
      while ((Ports[PortNum].hdmatx->CCR &  DMA_CCR_EN) == DMA_CCR_EN);
      Ports[PortNum].cbs.USBPD_HW_IF_TxCompleted(PortNum, 2);
      return;
    }

    /* HRSTDISC : hard reset sending has been discarded */
    if (UCPD_SR_HRSTDISC == (_interrupt & UCPD_SR_HRSTDISC))
    {
      LL_UCPD_ClearFlag_TxHRSTDISC(hucpd);
      return;
    }

    /* TXUND : tx underrun detected */
    if (UCPD_SR_HRSTSENT == (_interrupt & UCPD_SR_HRSTSENT))
    {
      g_usbpd_dbg.txhrstsent++;
      /* Answer not expected by the stack */
      LL_UCPD_ClearFlag_TxHRSTSENT(hucpd);
      Ports[PortNum].cbs.USBPD_HW_IF_TX_HardResetCompleted(PortNum, USBPD_SOPTYPE_HARD_RESET);
      return;
    }

    /* TXUND : tx underrun detected */
    if (UCPD_SR_TXUND == (_interrupt & UCPD_SR_TXUND))
    {
      g_usbpd_dbg.txund++;
      /* Nothing to do.
         The port partner checks the message integrity with CRC, so PRL will repeat the sending.
         Can be used for debugging purpose */
      LL_UCPD_ClearFlag_TxUND(hucpd);
      return;
    }

    /* RXNE : not needed the stack only perform transfer by DMA */
    /* RXORDDET: not needed so stack will not enabled this interrupt */
    if (UCPD_SR_RXORDDET == (_interrupt & UCPD_SR_RXORDDET))
    {
      g_usbpd_dbg.rxorddet++;
      if (LL_UCPD_RXORDSET_CABLE_RESET == hucpd->RX_ORDSET)
      {
        /* Cable reset detected */
        Ports[PortNum].cbs.USBPD_HW_IF_RX_ResetIndication(PortNum, USBPD_SOPTYPE_CABLE_RESET);
      }
      LL_UCPD_ClearFlag_RxOrderSet(hucpd);
#if defined(_LOW_POWER)
      UTIL_LPM_SetStopMode(0 == PortNum ? LPM_PE_0 : LPM_PE_1, UTIL_LPM_DISABLE);
#endif /* _LOW_POWER */

      /* Forbid message sending */
      Ports[PortNum].RXStatus = USBPD_TRUE;
      return;
    }

    /* Check RXHRSTDET */
    if (UCPD_SR_RXHRSTDET == (_interrupt & UCPD_SR_RXHRSTDET))
    {
      g_usbpd_dbg.rxhrstdet++;
      Ports[PortNum].cbs.USBPD_HW_IF_RX_ResetIndication(PortNum, USBPD_SOPTYPE_HARD_RESET);
      LL_UCPD_ClearFlag_RxHRST(hucpd);
      return;
    }

    /* Check RXOVR */
    if (UCPD_SR_RXOVR == (_interrupt & UCPD_SR_RXOVR))
    {
      g_usbpd_dbg.rxovr++;
      /* Nothing to do, the message will be discarded and port Partner will try sending again. */
      ovrflag = 1;
      LL_UCPD_ClearFlag_RxOvr(hucpd);
      return;
    }

    /* Check RXMSGEND an Rx message has been received */
    if (UCPD_SR_RXMSGEND == (_interrupt & UCPD_SR_RXMSGEND))
    {
      Ports[PortNum].RXStatus = USBPD_FALSE;

      /* For DMA mode, add a check to ensure the number of data received matches
         the number of data received by UCPD */
      LL_UCPD_ClearFlag_RxMsgEnd(hucpd);

      /* Disable DMA */
      SET_BIT(Ports[PortNum].hdmarx->CCR, DMA_CCR_SUSP | DMA_CCR_RESET);
      while ((Ports[PortNum].hdmarx->CCR & DMA_CCR_EN) == DMA_CCR_EN);

      /* Ready for next transaction */
      WRITE_REG(Ports[PortNum].hdmarx->CDAR, (uint32_t)Ports[PortNum].ptr_RxBuff);
      MODIFY_REG(Ports[PortNum].hdmarx->CBR1, DMA_CBR1_BNDT, (SIZE_MAX_PD_TRANSACTION_UNCHUNK & DMA_CBR1_BNDT));

      /* Enable the DMA */
      SET_BIT(Ports[PortNum].hdmarx->CCR, DMA_CCR_EN);
#if defined(_LOW_POWER)
      UTIL_LPM_SetStopMode(0 == PortNum ? LPM_PE_0 : LPM_PE_1, UTIL_LPM_ENABLE);
#endif /* _LOW_POWER */

      if (((_interrupt & UCPD_SR_RXERR) == 0u) && (ovrflag == 0u))
      {
        g_usbpd_dbg.rxmsgend_ok++;
        USBPD_CacheInvalidateRx(Ports[PortNum].ptr_RxBuff, SIZE_MAX_PD_TRANSACTION_UNCHUNK);
        /* Record the received message header + ordered set for diagnostics.
           The message header is the first 16-bit word of the RX buffer. */
        g_usbpd_dbg.last_rx_hdr = (uint16_t)(Ports[PortNum].ptr_RxBuff[0] |
                                             (uint16_t)((uint16_t)Ports[PortNum].ptr_RxBuff[1] << 8U));
        g_usbpd_dbg.last_rx_sop = (uint8_t)(hucpd->RX_ORDSET & UCPD_RX_ORDSET_RXORDSET);
        /* Minimal analytics capture: copy a bounded number of bytes out of the
           ST RX buffer into the APIE ring.  The ST PRL buffer and its DMA
           re-arm are NOT touched (the copy happens after RXMSGEND confirms the
           transfer is complete). */
        {
          uint16_t _hdr = (uint16_t)(Ports[PortNum].ptr_RxBuff[0] |
                                     (uint16_t)((uint16_t)Ports[PortNum].ptr_RxBuff[1] << 8U));
          uint32_t _nobj = (uint32_t)((_hdr >> 12) & 0x07u);
          uint32_t _len  = 2u + (_nobj * 4u);
          APIE_Analyzer_CaptureRaw(PortNum,
                                   (uint8_t)(hucpd->RX_ORDSET & UCPD_RX_ORDSET_RXORDSET),
                                   Ports[PortNum].ptr_RxBuff, _len);
        }
        /* Rx message has been received without error */
        Ports[PortNum].cbs.USBPD_HW_IF_RX_Completed(PortNum, hucpd->RX_ORDSET & UCPD_RX_ORDSET_RXORDSET);
      }
      else
      {
        g_usbpd_dbg.rxmsgend_err++;
      }
      ovrflag = 0;
      return;
    }

    /* Check TYPECEVT1IE/TYPECEVT1IE || check TYPECEVT2IE/TYPECEVT2IE */
    if ((UCPD_SR_TYPECEVT1 == (_interrupt & UCPD_SR_TYPECEVT1))
        || (UCPD_SR_TYPECEVT2 == (_interrupt & UCPD_SR_TYPECEVT2)))
    {
      g_usbpd_dbg.typecevt++;
      /* Clear both interrupt */
      LL_UCPD_ClearFlag_TypeCEventCC1(hucpd);
      LL_UCPD_ClearFlag_TypeCEventCC2(hucpd);
      Ports[PortNum].USBPD_CAD_WakeUp();
      /* Wakeup CAD to check the detection event */
      return;
    }

#if defined(_FRS)
    /* Check FRSEVTIE */
    if (UCPD_SR_FRSEVT == (_interrupt & UCPD_SR_FRSEVT))
    {
      LL_UCPD_ClearFlag_FRS(hucpd);
      if ((USBPD_PORTPOWERROLE_SNK == Ports[PortNum].params->PE_PowerRole)
          && (Ports[PortNum].params->PE_SwapOngoing == USBPD_FALSE))
      {
        /* Confirm the FRS by checking if an RP is always present on the current CC line.
           We should wait for maximum FRS timing */
        USBPD_TIM_Start((TIM_identifier)(2 * PortNum), 150);
        while ((USBPD_TIM_IsExpired((TIM_identifier)(2u * PortNum)) == 0u));

        if ((0 != (hucpd->SR & (UCPD_SR_TYPEC_VSTATE_CC1 | UCPD_SR_TYPEC_VSTATE_CC2))) &&
            (USBPD_POWER_EXPLICITCONTRACT == Ports[PortNum].params->PE_Power))
        {
          /* Switch the power to take the control of VBUS.
             When VBUS go under VSAFE5V the sink shall switch ON VBUS in timing < tSrcFRSwap */
          BSP_USBPD_PWR_FRSVBUSEnable(PortNum);
          USBPD_TRACE_Add(USBPD_TRACE_DEBUG, PortNum, 0, "FRS received", 12u);
          Ports[PortNum].cbs.USBPD_HW_IF_TX_FRSReception(PortNum);
        }
      }
    }
#endif /* _FRS */
  }
}

