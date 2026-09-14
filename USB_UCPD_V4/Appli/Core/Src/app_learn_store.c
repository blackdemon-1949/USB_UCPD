/**
  ******************************************************************************
  * @file    app_learn_store.c
  * @brief   Mirror the learned-signature engine into the external NOR store.
  *
  * The engine's own home is the backup SRAM (apie_bkp.c), which is small and
  * battery-backed.  This file adds a second, much larger home in the external
  * NOR flash so the learned data survives a dead battery, so it can be handed
  * over to another unit, and so the user has room for their own data next to
  * it.  Both directions are optional: with no NOR device present the calls
  * fail harmlessly and the engine keeps working from the backup SRAM.
  *
  * The engine exports one blob (APIE_Learn_Export); it is larger than a single
  * store record, so it is split into APP_STORE_LEARN_CHUNK bytes per record
  * with a small header, and reassembled on load.  A torn write can therefore
  * only cost the chunk it happened in, and the header/CRC of each record
  * rejects a half-written chunk.
  ******************************************************************************
  */
#include "app_learn_store.h"
#include "app_config.h"
#include "app_log.h"
#include "app_store.h"
#include "apie_learn.h"
#include <string.h>

#define LEARN_BLOB_MAX      16384U   /* 64 signatures: ~6 KB today, room to grow */
#define LEARN_CHUNK_BYTES   (APP_STORE_MAX_PAYLOAD - 16U)
#define LEARN_HDR_MAGIC     0x4C454152UL   /* 'LEAR' */
#define LEARN_HDR_VERSION   0x0002U

static uint8_t s_blob[LEARN_BLOB_MAX];

static uint32_t learn_put_u32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8) & 0xFFU);
  p[2] = (uint8_t)((v >> 16) & 0xFFU);
  p[3] = (uint8_t)((v >> 24) & 0xFFU);
  return 4U;
}

static uint32_t learn_get_u32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint8_t APIE_Learn_SaveStore(void)
{
#if !APP_STORE_ENABLED
  APP_LOG_Write("learn: the external NOR store is disabled at build time\r\n");
  return 0U;
#else
  uint32_t total;
  uint16_t chunks;
  uint32_t written = 0U;

  if (APP_STORE_Ready() == 0U)
  {
    APP_LOG_Write("learn: no NOR store - nothing saved ('store format' to create it)\r\n");
    return 0U;
  }

  total = (uint32_t)APIE_Learn_Export(s_blob, (uint16_t)sizeof(s_blob));
  if ((total == 0U) || (total > sizeof(s_blob)))
  {
    APP_LOG_Write("learn: export failed\r\n");
    return 0U;
  }

  chunks = (uint16_t)((total + LEARN_CHUNK_BYTES - 1U) / LEARN_CHUNK_BYTES);
  if (chunks > APP_STORE_T_LEARN_CHUNKS)
  {
    APP_LOG_Printf("learn: %lu bytes needs %u chunks, only %u slots\r\n",
                   (unsigned long)total, (unsigned)chunks,
                   (unsigned)APP_STORE_T_LEARN_CHUNKS);
    return 0U;
  }

  for (uint16_t c = 0U; c < chunks; c++)
  {
    uint8_t rec[APP_STORE_MAX_PAYLOAD];
    uint32_t off = (uint32_t)c * LEARN_CHUNK_BYTES;
    uint32_t len = total - off;
    uint32_t hdr = 0U;

    if (len > LEARN_CHUNK_BYTES)
    {
      len = LEARN_CHUNK_BYTES;
    }
    hdr += learn_put_u32(&rec[hdr], LEARN_HDR_MAGIC);
    hdr += learn_put_u32(&rec[hdr], ((uint32_t)LEARN_HDR_VERSION << 16) | (uint32_t)c);
    hdr += learn_put_u32(&rec[hdr], total);
    hdr += learn_put_u32(&rec[hdr], len);
    memcpy(&rec[hdr], &s_blob[off], len);

    if (APP_STORE_Write((uint8_t)(APP_STORE_T_LEARN_CHUNK0 + c), rec, hdr + len) != 0)
    {
      APP_LOG_Printf("learn: chunk %u write failed\r\n", (unsigned)c);
      return 0U;
    }
    written += len;
  }

  APP_LOG_Printf("learn: saved %lu bytes in %u NOR chunk(s)\r\n",
                 (unsigned long)written, (unsigned)chunks);
  return 1U;
#endif
}

uint8_t APIE_Learn_LoadStore(void)
{
#if !APP_STORE_ENABLED
  return 0U;
#else
  uint32_t total = 0U;
  uint32_t have = 0U;
  uint8_t  seen[APP_STORE_T_LEARN_CHUNKS];
  uint8_t  chunks = 0U;

  if (APP_STORE_Ready() == 0U)
  {
    return 0U;
  }
  memset(seen, 0, sizeof(seen));

  for (uint16_t c = 0U; c < APP_STORE_T_LEARN_CHUNKS; c++)
  {
    uint8_t rec[APP_STORE_MAX_PAYLOAD];
    uint32_t len = 0U;
    uint32_t off, paylen, idx;

    if (APP_STORE_Read((uint8_t)(APP_STORE_T_LEARN_CHUNK0 + c), rec, sizeof(rec), &len) != 0)
    {
      continue;
    }
    if (len < 16U)
    {
      continue;
    }
    if (learn_get_u32(&rec[0]) != LEARN_HDR_MAGIC)
    {
      continue;
    }
    if (((learn_get_u32(&rec[4]) >> 16) & 0xFFFFU) != LEARN_HDR_VERSION)
    {
      continue;
    }
    idx    = learn_get_u32(&rec[4]) & 0xFFFFU;
    total  = learn_get_u32(&rec[8]);
    paylen = learn_get_u32(&rec[12]);
    off    = idx * LEARN_CHUNK_BYTES;

    if ((idx >= APP_STORE_T_LEARN_CHUNKS) || (paylen > LEARN_CHUNK_BYTES) ||
        (len < (16U + paylen)) || (total > sizeof(s_blob)) ||
        ((off + paylen) > sizeof(s_blob)))
    {
      continue;                       /* corrupt or foreign record */
    }
    memcpy(&s_blob[off], &rec[16], paylen);
    if (seen[idx] == 0U)
    {
      seen[idx] = 1U;
      chunks++;
      have += paylen;
    }
  }

  if ((total == 0U) || (have < total))
  {
    APP_LOG_Printf("learn: NOR image incomplete (%lu/%lu bytes)\r\n",
                   (unsigned long)have, (unsigned long)total);
    return 0U;
  }

  if (APIE_Learn_Import(s_blob, (uint16_t)total) == 0U)
  {
    APP_LOG_Write("learn: NOR image rejected by the engine\r\n");
    return 0U;
  }

  APP_LOG_Printf("learn: restored %lu bytes from %u NOR chunk(s)\r\n",
                 (unsigned long)total, (unsigned)chunks);
  return 1U;
#endif
}
