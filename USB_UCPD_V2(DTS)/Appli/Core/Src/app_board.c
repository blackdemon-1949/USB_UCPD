#include "app_board.h"
#include "app_log.h"
#include "app_pd.h"
#include "usb_device.h"
#include "usbpd_hw_if.h"

static APP_LED_Mode_t s_mode = APP_LED_HEARTBEAT;
static uint32_t s_last;

void APP_LED_Set(APP_LED_Mode_t mode)
{
  s_mode = mode;
}

void APP_LED_Task(void)
{
  uint32_t now = HAL_GetTick();
  uint32_t period;
  uint32_t on_ms;

  switch (s_mode)
  {
    case APP_LED_OFF:
      HAL_GPIO_WritePin(APP_LED_PORT, APP_LED_PIN, GPIO_PIN_RESET);
      return;
    case APP_LED_ON:
    case APP_LED_PD_CONTRACT:
      HAL_GPIO_WritePin(APP_LED_PORT, APP_LED_PIN, GPIO_PIN_SET);
      return;
    case APP_LED_FAULT:
      period = 120U;
      on_ms = 60U;
      break;
    case APP_LED_PD_WAIT:
      period = 250U;
      on_ms = 80U;
      break;
    case APP_LED_HEARTBEAT:
    default:
      period = 1000U;
      on_ms = 80U;
      break;
  }

  if ((now - s_last) >= period)
  {
    s_last = now;
  }
  if ((now - s_last) < on_ms)
  {
    HAL_GPIO_WritePin(APP_LED_PORT, APP_LED_PIN, GPIO_PIN_SET);
  }
  else
  {
    HAL_GPIO_WritePin(APP_LED_PORT, APP_LED_PIN, GPIO_PIN_RESET);
  }
}

void APP_BOARD_PrintInfo(void)
{
  extern USBD_HandleTypeDef hUsbDeviceHS;
  APP_LOG_Printf("board : WeAct STM32H7R3Z8J6  600 MHz\r\n");
  APP_LOG_Printf("flash : Puya PY25Q64HA  8 MB  XiP @ 0x90000000\r\n");
  APP_LOG_Printf("boot  : internal 64 KB @ 0x08000000\r\n");
  APP_LOG_Printf("sram  : AXI 0x24000000  DTCM 0x20000000\r\n");
  APP_LOG_Printf("usb   : OTG_HS CDC  state=%u\r\n", (unsigned)hUsbDeviceHS.dev_state);
  APP_LOG_Printf("ucpd  : UCPD1  CC1=PM0  CC2=PM1  (header, not Type-C HS)\r\n");
  APP_LOG_Printf("led   : PB2   key: PC13 (active low)\r\n");
  APP_LOG_Printf("tick  : %lu ms   sysclk: %lu Hz\r\n",
                 (unsigned long)HAL_GetTick(),
                 (unsigned long)SystemCoreClock);
}

void APP_BOARD_PrintUcpd(void)
{
  UCPD_TypeDef *u = UCPD1;
  uint32_t cfg1 = u->CFG1;
  uint32_t cr   = u->CR;
  uint32_t sr   = u->SR;

  APP_LOG_Write("---- UCPD1 ----\r\n");
  APP_LOG_Printf("CFG1  0x%08lX  psc=%lu tw=%lu ifrgap=%lu hbit=%lu  rxen=%u txen=%u en=%u\r\n",
                 (unsigned long)cfg1,
                 (unsigned long)((cfg1 & UCPD_CFG1_PSC_UCPDCLK) >> UCPD_CFG1_PSC_UCPDCLK_Pos),
                 (unsigned long)((cfg1 & UCPD_CFG1_TRANSWIN) >> UCPD_CFG1_TRANSWIN_Pos),
                 (unsigned long)((cfg1 & UCPD_CFG1_IFRGAP) >> UCPD_CFG1_IFRGAP_Pos),
                 (unsigned long)((cfg1 & UCPD_CFG1_HBITCLKDIV) >> UCPD_CFG1_HBITCLKDIV_Pos),
                 (unsigned)((cfg1 & UCPD_CFG1_RXDMAEN) ? 1 : 0),
                 (unsigned)((cfg1 & UCPD_CFG1_TXDMAEN) ? 1 : 0),
                 (unsigned)((cfg1 & UCPD_CFG1_UCPDEN) ? 1 : 0));
  APP_LOG_Printf("CFG2  0x%08lX  rxafilt=%u rxfiltdis=%u\r\n",
                 (unsigned long)u->CFG2,
                 (unsigned)((u->CFG2 & UCPD_CFG2_RXAFILTEN) ? 1 : 0),
                 (unsigned)((u->CFG2 & UCPD_CFG2_RXFILTDIS) ? 1 : 0));
  APP_LOG_Printf("CR    0x%08lX  anamode=%u submode=%u ccenable=%u phyrxen=%u phyccsel=%u\r\n",
                 (unsigned long)cr,
                 (unsigned)((cr & UCPD_CR_ANAMODE) ? 1 : 0),
                 (unsigned)((cr & UCPD_CR_ANASUBMODE) >> UCPD_CR_ANASUBMODE_Pos),
                 (unsigned)((cr & UCPD_CR_CCENABLE) >> UCPD_CR_CCENABLE_Pos),
                 (unsigned)((cr & UCPD_CR_PHYRXEN) ? 1 : 0),
                 (unsigned)((cr & UCPD_CR_PHYCCSEL) ? 2 : 1));
  APP_LOG_Printf("SR    0x%08lX  cc1=%lu cc2=%lu rxerr=%u rxovr=%u\r\n",
                 (unsigned long)sr,
                 (unsigned long)((sr & UCPD_SR_TYPEC_VSTATE_CC1) >> UCPD_SR_TYPEC_VSTATE_CC1_Pos),
                 (unsigned long)((sr & UCPD_SR_TYPEC_VSTATE_CC2) >> UCPD_SR_TYPEC_VSTATE_CC2_Pos),
                 (unsigned)((sr & UCPD_SR_RXERR) ? 1 : 0),
                 (unsigned)((sr & UCPD_SR_RXOVR) ? 1 : 0));
  APP_LOG_Printf("IMR   0x%08lX  RX_ORDSET=0x%08lX  RX_PAYSZ=%lu\r\n",
                 (unsigned long)u->IMR,
                 (unsigned long)u->RX_ORDSET,
                 (unsigned long)u->RX_PAYSZ);
  APP_LOG_Printf("rxbuf 0x%08lX (DMA %s)  txdma EN=%u  rxdma EN=%u\r\n",
                 (unsigned long)(uintptr_t)Ports[0].ptr_RxBuff,
                 (((uintptr_t)Ports[0].ptr_RxBuff >= 0x24000000UL) ? "AXI ok" : "!! not AXI !!"),
                 (unsigned)((Ports[0].hdmatx && (Ports[0].hdmatx->CCR & DMA_CCR_EN)) ? 1 : 0),
                 (unsigned)((Ports[0].hdmarx && (Ports[0].hdmarx->CCR & DMA_CCR_EN)) ? 1 : 0));
  APP_LOG_Printf("irq=%lu cevt=%lu ord=%lu msgOK=%lu msgERR=%lu ovr=%lu hrst=%lu\r\n",
                 (unsigned long)g_usbpd_dbg.ucpd_irq,
                 (unsigned long)g_usbpd_dbg.typecevt,
                 (unsigned long)g_usbpd_dbg.rxorddet,
                 (unsigned long)g_usbpd_dbg.rxmsgend_ok,
                 (unsigned long)g_usbpd_dbg.rxmsgend_err,
                 (unsigned long)g_usbpd_dbg.rxovr,
                 (unsigned long)g_usbpd_dbg.rxhrstdet);
  APP_LOG_Printf("txsent=%lu txdisc=%lu txhrst=%lu txabt=%lu txund=%lu\r\n",
                 (unsigned long)g_usbpd_dbg.txmsgsent,
                 (unsigned long)g_usbpd_dbg.txmsgdisc,
                 (unsigned long)g_usbpd_dbg.txhrstsent,
                 (unsigned long)g_usbpd_dbg.txmsgabt,
                 (unsigned long)g_usbpd_dbg.txund);
}
