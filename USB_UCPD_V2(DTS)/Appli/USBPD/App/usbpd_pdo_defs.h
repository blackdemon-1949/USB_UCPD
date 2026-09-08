/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usbpd_pdo_defs.h
  * @author  MCD Application Team
  * @brief   Header file for definition of PDO/APDO values for 2 ports(DRP/SNK) configuration
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __USBPD_PDO_DEF_H
#define __USBPD_PDO_DEF_H

#ifdef __cplusplus
 extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "usbpd_def.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Define   ------------------------------------------------------------------*/
#define PORT0_NB_SOURCEPDO         0U   /* Number of Source PDOs (applicable for port 0)   */
#define PORT0_NB_SINKPDO           6U   /* 5/9/12/15/20 V fixed + PPS APDO */
#define PORT1_NB_SOURCEPDO         0U   /* Number of Source PDOs (applicable for port 1)   */
#define PORT1_NB_SINKPDO           0U   /* Number of Sink PDOs (applicable for port 1)     */

/* USER CODE BEGIN Define */

/* USER CODE END Define */

/* Exported typedef ----------------------------------------------------------*/

/* USER CODE BEGIN typedef */

/**
  * @brief  USBPD Port PDO Structure definition
  *
  */
/* USER CODE END typedef */

/* Exported define -----------------------------------------------------------*/

/* USER CODE BEGIN Exported_Define */

#define USBPD_CORE_PDO_SRC_FIXED_MAX_CURRENT 3
#define USBPD_CORE_PDO_SNK_FIXED_MAX_CURRENT 3000

/* USER CODE END Exported_Define */

/* Exported constants --------------------------------------------------------*/

/* USER CODE BEGIN constants */

/* USER CODE END constants */

/* Exported macro ------------------------------------------------------------*/

/* USER CODE BEGIN macro */

/* USER CODE END macro */

/* Exported variables --------------------------------------------------------*/

/* USER CODE BEGIN variables */
/* USER CODE END variables */

#ifndef __USBPD_PWR_IF_C
extern uint32_t PORT0_PDO_ListSRC[USBPD_MAX_NB_PDO];
extern uint32_t PORT0_PDO_ListSNK[USBPD_MAX_NB_PDO];
#else
/* Definition of Source PDO for Port 0 */
uint32_t PORT0_PDO_ListSRC[USBPD_MAX_NB_PDO] =
{
  /* PDO 1 */ (0x00000000U),

  /* PDO 2 */ (0x00000000U),

  /* PDO 3 */ (0x00000000U),

  /* PDO 4 */ (0x00000000U),

  /* PDO 5 */ (0x00000000U),

  /* PDO 6 */ (0x00000000U),

  /* PDO 7 */ (0x00000000U),

};

/* Definition of Sink PDO for Port 0 */
uint32_t PORT0_PDO_ListSNK[USBPD_MAX_NB_PDO] =
{
  /* PDO 1 — 5 V / 3 A, higher-capability so the source offers >5 V */
  (
    USBPD_PDO_TYPE_FIXED                 |
    USBPD_PDO_SNK_FIXED_SET_VOLTAGE(5000U)         |
    USBPD_PDO_SNK_FIXED_SET_OP_CURRENT(3000U)     |
    USBPD_PDO_SNK_FIXED_FRS_NOT_SUPPORTED          |
    USBPD_PDO_SNK_FIXED_DRD_NOT_SUPPORTED          |
    USBPD_PDO_SNK_FIXED_USBCOMM_NOT_SUPPORTED      |
    USBPD_PDO_SNK_FIXED_EXT_POWER_NOT_AVAILABLE    |
    USBPD_PDO_SNK_FIXED_HIGHERCAPAB_SUPPORTED      |
    USBPD_PDO_SNK_FIXED_DRP_NOT_SUPPORTED
  ),

  /* PDO 2 — 9 V / 3 A */
  (
    USBPD_PDO_TYPE_FIXED                 |
    USBPD_PDO_SNK_FIXED_SET_VOLTAGE(9000U)         |
    USBPD_PDO_SNK_FIXED_SET_OP_CURRENT(3000U)
  ),

  /* PDO 3 — 12 V / 3 A */
  (
    USBPD_PDO_TYPE_FIXED                 |
    USBPD_PDO_SNK_FIXED_SET_VOLTAGE(12000U)        |
    USBPD_PDO_SNK_FIXED_SET_OP_CURRENT(3000U)
  ),

  /* PDO 4 — 15 V / 3 A */
  (
    USBPD_PDO_TYPE_FIXED                 |
    USBPD_PDO_SNK_FIXED_SET_VOLTAGE(15000U)        |
    USBPD_PDO_SNK_FIXED_SET_OP_CURRENT(3000U)
  ),

  /* PDO 5 — 20 V / 5 A */
  (
    USBPD_PDO_TYPE_FIXED                 |
    USBPD_PDO_SNK_FIXED_SET_VOLTAGE(20000U)        |
    USBPD_PDO_SNK_FIXED_SET_OP_CURRENT(5000U)
  ),

  /* PDO 6 — PPS 3.3–21 V / 3 A */
  (
    USBPD_PDO_TYPE_APDO                            |
    USBPD_PDO_SNK_APDO_PPS                         |
    USBPD_PDO_SNK_APDO_SET_MIN_VOLTAGE(3300U)      |
    USBPD_PDO_SNK_APDO_SET_MAX_VOLTAGE(21000U)     |
    USBPD_PDO_SNK_APDO_SET_MAX_CURRENT(3000U)
  ),

  /* PDO 7 */ (0x00000000U),
};

#endif

/* Exported functions --------------------------------------------------------*/

/* USER CODE BEGIN functions */

/* USER CODE END functions */

#ifdef __cplusplus
}
#endif

#endif /* __USBPD_PDO_DEF_H */
