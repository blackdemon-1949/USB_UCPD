/**
  ******************************************************************************
  * @file    app_learn_store.h
  * @brief   Learned-signature mirror in the external NOR store (see
  *          app_learn_store.c).  Both calls are safe when no NOR device is
  *          present and return 0 in that case.
  ******************************************************************************
  */
#ifndef APP_LEARN_STORE_H
#define APP_LEARN_STORE_H

#include <stdint.h>

uint8_t APIE_Learn_SaveStore(void);
uint8_t APIE_Learn_LoadStore(void);

#endif /* APP_LEARN_STORE_H */
