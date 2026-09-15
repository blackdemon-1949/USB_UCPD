/**
  ******************************************************************************
  * @file    app_store.c
  * @brief   NOR journal store - see app_store.h.
  *
  * All flash traffic goes through EXT_NOR_* (RAM-resident writer with timeouts
  * and a latch-off failsafe).  This layer only decides *what* to write: it
  * keeps the append head cached in RAM and mirrored in the CMOS settings, so
  * a write costs one page program and a boot costs one superblock read when
  * the hint is trustworthy.
  ******************************************************************************
  */
#include "app_store.h"
#include "app_cmos.h"
#include "app_log.h"
#include "ext_nor.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "main.h"

#define STORE_MAGIC      0x54535845UL   /* 'EXST' */
#define STORE_VERSION    1U
#define REC_MAGIC        0x31434552UL   /* 'REC1' */
#define REC_HDR_SIZE     24U
#define STORE_SECTOR0    (EXT_NOR_STORE_OFF)
#define STORE_DATA_OFF   (EXT_NOR_STORE_OFF + EXT_NOR_SECTOR_SIZE)
#define STORE_DATA_LEN   (EXT_NOR_STORE_SIZE - EXT_NOR_SECTOR_SIZE)

typedef struct
{
  uint32_t magic;
  uint32_t version;
  uint32_t sectors;
  uint32_t data_off;
  uint32_t crc;
  uint32_t rsv[3];
} store_super_t;

typedef struct
{
  uint32_t magic;
  uint32_t seq;
  uint32_t type;
  uint32_t len;
  uint32_t crc;
  uint32_t rsv;
} store_rec_t;

static uint8_t  s_ready;
static uint8_t  s_formatted;
static uint32_t s_head;          /* absolute NOR offset of the next free byte */
static uint32_t s_seq;           /* sequence of the last written record       */
static uint32_t s_records;       /* records seen during the last scan         */
static uint8_t  s_records_known; /* 0 = mounted from the CMOS hint, not counted */
static uint32_t s_writes;        /* records written this session              */
static uint32_t s_sector;        /* sector currently being filled (index)     */
static uint32_t s_last_event_ms;

/* -------------------------------------------------------------------------- */
static uint32_t store_crc32(const uint8_t *d, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFUL;

  for (uint32_t i = 0U; i < len; i++)
  {
    crc ^= d[i];
    for (uint32_t b = 0U; b < 8U; b++)
    {
      crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)(-(int32_t)(crc & 1U)));
    }
  }
  return ~crc;
}

static int store_read(uint32_t off, void *buf, uint32_t len)
{
  return EXT_NOR_Read(off, buf, len);
}

static int store_write_rec(uint8_t type, const void *data, uint32_t len);

/* -------------------------------------------------------------------------- */
/*  Superblock                                                                 */
/* -------------------------------------------------------------------------- */
static uint8_t store_super_valid(void)
{
  store_super_t sp;

  if (store_read(STORE_SECTOR0, &sp, sizeof(sp)) != EXT_NOR_OK)
  {
    return 0U;
  }
  if ((sp.magic != STORE_MAGIC) || (sp.version != STORE_VERSION) ||
      (sp.sectors != EXT_NOR_STORE_SECTORS) || (sp.data_off != STORE_DATA_OFF))
  {
    return 0U;
  }
  return (store_crc32((const uint8_t *)&sp, 16U) == sp.crc) ? 1U : 0U;
}

static int store_super_write(void)
{
  store_super_t sp;
  int rc;

  memset(&sp, 0, sizeof(sp));
  sp.magic = STORE_MAGIC;
  sp.version = STORE_VERSION;
  sp.sectors = EXT_NOR_STORE_SECTORS;
  sp.data_off = STORE_DATA_OFF;
  sp.crc = store_crc32((const uint8_t *)&sp, 16U);

  rc = EXT_NOR_EraseSector(STORE_SECTOR0);
  if (rc != EXT_NOR_OK)
  {
    return rc;
  }
  return EXT_NOR_Program(STORE_SECTOR0, &sp, sizeof(sp));
}

/* -------------------------------------------------------------------------- */
/*  Record helpers                                                             */
/* -------------------------------------------------------------------------- */
static uint8_t store_rec_valid(const store_rec_t *r)
{
  if ((r->magic != REC_MAGIC) || (r->type == 0U) ||
      (r->len > APP_STORE_MAX_PAYLOAD))
  {
    return 0U;
  }
  return 1U;
}

/* Walk the records of one sector.  Returns the offset just past the last valid
 * record (the sector's append point) and, through the pointers, the newest
 * sequence/type found. */
static uint32_t store_walk_sector(uint32_t sec_off, uint32_t *best_seq, int *have_best)
{
  uint32_t off = sec_off;
  uint32_t limit = sec_off + EXT_NOR_SECTOR_SIZE;

  for (;;)
  {
    store_rec_t r;

    if ((off + REC_HDR_SIZE) > limit)
    {
      break;
    }
    if (store_read(off, &r, sizeof(r)) != EXT_NOR_OK)
    {
      break;
    }
    if (!store_rec_valid(&r))
    {
      break;                     /* erased or corrupt: the sector ends here  */
    }
    s_records++;
    if ((*have_best == 0) || (r.seq >= *best_seq))
    {
      *best_seq = r.seq;
      *have_best = 1;
    }
    off += REC_HDR_SIZE + ((r.len + 3U) & ~3U);
  }
  return off;
}

/* Full scan of the journal: rebuilds the head and the sequence counter. */
static void store_scan(void)
{
  uint32_t best_seq = 0U;
  int have_best = 0;
  uint32_t best_end = STORE_DATA_OFF;
  uint32_t sector;

  s_records = 0U;
  s_records_known = 1U;          /* the loop below really counts them */

  for (sector = 0U; sector < (STORE_DATA_LEN / EXT_NOR_SECTOR_SIZE); sector++)
  {
    uint32_t sec_off = STORE_DATA_OFF + (sector * EXT_NOR_SECTOR_SIZE);
    uint32_t end = store_walk_sector(sec_off, &best_seq, &have_best);

    if (end > sec_off)
    {
      best_end = end;            /* remember the append point of the newest
                                    sector we saw (sectors are used in order) */
      s_sector = sector;
    }
  }

  s_seq = best_seq;
  s_head = have_best ? best_end : STORE_DATA_OFF;
  if (have_best && (s_head >= (STORE_DATA_OFF + STORE_DATA_LEN)))
  {
    s_head = STORE_DATA_OFF;
  }
}

/* Try the CMOS hint first: it must be inside the window and either erased or
 * hold a valid record header.  Anything else means "rescan". */
static uint8_t store_head_from_hint(void)
{
  app_settings_t *s = APP_CMOS();
  store_rec_t r;
  uint32_t hint = s->store_gen;

  if ((hint < STORE_DATA_OFF) || (hint >= (STORE_DATA_OFF + STORE_DATA_LEN)))
  {
    return 0U;
  }
  if (store_read(hint, &r, sizeof(r)) != EXT_NOR_OK)
  {
    return 0U;
  }
  if (store_rec_valid(&r))
  {
    hint += REC_HDR_SIZE + ((r.len + 3U) & ~3U);
    if (hint >= (STORE_DATA_OFF + STORE_DATA_LEN))
    {
      return 0U;
    }
  }
  s_head = hint;
  s_seq = 0U;                    /* unknown after a warm boot: rebuilt lazily */
  s_sector = (hint - STORE_DATA_OFF) / EXT_NOR_SECTOR_SIZE;
  return 1U;
}

/* Update the CMOS hint (cheap: backup SRAM, no flash wear). */
static void store_hint_save(void)
{
  app_settings_t *s = APP_CMOS();

  s->store_gen = s_head;
  APP_CMOS_Save();
}

/* -------------------------------------------------------------------------- */
/*  Write                                                                      */
/* -------------------------------------------------------------------------- */
static int store_write_rec(uint8_t type, const void *data, uint32_t len)
{
  store_rec_t r;
  uint32_t total = REC_HDR_SIZE + ((len + 3U) & ~3U);
  uint32_t sec_end = STORE_DATA_OFF + ((s_sector + 1U) * EXT_NOR_SECTOR_SIZE);
  int rc;
  uint8_t hdr[REC_HDR_SIZE];

  if (!s_ready)
  {
    return -1;
  }
  if (len > APP_STORE_MAX_PAYLOAD)
  {
    return -1;
  }

  /* Move to the next sector when this record no longer fits (wrap at the end
   * of the window: the oldest data is sacrificed, newest wins on read). */
  if ((s_head + total) > sec_end)
  {
    s_sector++;
    if (s_sector >= (STORE_DATA_LEN / EXT_NOR_SECTOR_SIZE))
    {
      s_sector = 0U;
    }
    s_head = STORE_DATA_OFF + (s_sector * EXT_NOR_SECTOR_SIZE);
    rc = EXT_NOR_EraseSector(s_head);
    if (rc != EXT_NOR_OK)
    {
      s_ready = 0U;              /* failsafe: stop using a failing flash    */
      APP_STORE_Event("store: sector erase failed, store disabled");
      return -1;
    }
  }

  r.magic = REC_MAGIC;
  r.seq   = ++s_seq;
  r.type  = type;
  r.len   = len;
  r.crc   = store_crc32((const uint8_t *)data, len);
  r.rsv   = 0U;

  memcpy(hdr, &r, sizeof(hdr));
  rc = EXT_NOR_Program(s_head, hdr, REC_HDR_SIZE);
  if (rc == EXT_NOR_OK)
  {
    rc = EXT_NOR_Program(s_head + REC_HDR_SIZE, data, len);
  }
  if (rc != EXT_NOR_OK)
  {
    s_ready = 0U;
    return -1;
  }

  s_head += total;
  s_writes++;
  store_hint_save();
  return 0;
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                 */
/* -------------------------------------------------------------------------- */
uint8_t APP_STORE_Init(void)
{
  s_ready = 0U;
  s_formatted = 0U;

  if (!EXT_NOR_Present())
  {
    APP_LOG_Write("[store] no NOR device: persistent store unavailable\r\n");
    return 0U;
  }

  if (!EXT_NOR_Ready())
  {
    APP_LOG_Write("[store] NOR writes are latched off (earlier failure)\r\n");
    return 0U;
  }

  if (!store_super_valid())
  {
    APP_LOG_Write("[store] no valid superblock - formatting the reserved 1 MB\r\n");
    if (store_super_write() != EXT_NOR_OK)
    {
      APP_LOG_Write("[store] format failed: store stays unavailable\r\n");
      return 0U;
    }
    s_formatted = 1U;
    s_head = STORE_DATA_OFF;
    s_sector = 0U;
    s_seq = 0U;
    store_hint_save();
  }
  else if (!store_head_from_hint())
  {
    store_scan();
    store_hint_save();
  }

  s_ready = 1U;
  /* "mounted" through the CMOS hint skips the scan, so the record count is
     simply not known yet - say so instead of printing a misleading 0. */
  if (s_records_known)
  {
    APP_LOG_Printf("[store] ready: %s, head=0x%06lX, records=%lu, writes=%lu\r\n",
                   s_formatted ? "formatted" : "mounted",
                   (unsigned long)s_head, (unsigned long)s_records,
                   (unsigned long)s_writes);
  }
  else
  {
    APP_LOG_Printf("[store] ready: %s, head=0x%06lX, records=(not scanned), writes=%lu\r\n",
                   s_formatted ? "formatted" : "mounted",
                   (unsigned long)s_head, (unsigned long)s_writes);
  }
  return 1U;
}

uint8_t APP_STORE_Ready(void)     { return s_ready; }
uint8_t APP_STORE_Formatted(void) { return s_formatted; }

int APP_STORE_Read(uint8_t type, void *buf, uint32_t max, uint32_t *out_len)
{
  uint32_t sector;
  uint32_t best_seq = 0U;
  uint8_t have = 0U;
  static uint8_t payload[APP_STORE_MAX_PAYLOAD];   /* .bss, reused           */

  if (!s_ready)
  {
    return -1;
  }
  if (max > APP_STORE_MAX_PAYLOAD)
  {
    max = APP_STORE_MAX_PAYLOAD;
  }

  for (sector = 0U; sector < (STORE_DATA_LEN / EXT_NOR_SECTOR_SIZE); sector++)
  {
    uint32_t off = STORE_DATA_OFF + (sector * EXT_NOR_SECTOR_SIZE);
    uint32_t limit = off + EXT_NOR_SECTOR_SIZE;

    while ((off + REC_HDR_SIZE) <= limit)
    {
      store_rec_t r;

      if (store_read(off, &r, sizeof(r)) != EXT_NOR_OK)
      {
        break;
      }
      if (!store_rec_valid(&r))
      {
        break;
      }
      if ((r.type == type) && ((!have) || (r.seq >= best_seq)))
      {
        uint32_t len = (r.len > APP_STORE_MAX_PAYLOAD) ? APP_STORE_MAX_PAYLOAD : r.len;

        if ((r.len <= APP_STORE_MAX_PAYLOAD) &&
            (store_read(off + REC_HDR_SIZE, payload, len) == EXT_NOR_OK) &&
            (store_crc32(payload, len) == r.crc))
        {
          best_seq = r.seq;
          have = 1U;
          if (out_len != NULL)
          {
            *out_len = len;
          }
          if (buf != NULL)
          {
            memcpy(buf, payload, (len < max) ? len : max);
            if (len > max)
            {
              have = 2U;           /* truncated */
            }
          }
        }
      }
      off += REC_HDR_SIZE + ((r.len + 3U) & ~3U);
    }
  }

  if (have == 0U)
  {
    return -1;
  }
  return (have == 1U) ? 0 : -2;
}

int APP_STORE_Write(uint8_t type, const void *data, uint32_t len)
{
  return store_write_rec(type, data, len);
}

int APP_STORE_Delete(uint8_t type)
{
  uint8_t none = 0U;

  /* A zero-length record is the tombstone: read it back with len == 0. */
  return store_write_rec(type, &none, 0U);
}

int APP_STORE_Format(void)
{
  uint32_t sector;
  int rc;

  if (!EXT_NOR_Ready())
  {
    return -1;
  }
  s_ready = 1U;                  /* allow the erase below             */
  for (sector = 0U; sector < EXT_NOR_STORE_SECTORS; sector++)
  {
    rc = EXT_NOR_EraseSector(EXT_NOR_STORE_OFF + (sector * EXT_NOR_SECTOR_SIZE));
    if (rc != EXT_NOR_OK)
    {
      s_ready = 0U;
      return -1;
    }
  }
  if (store_super_write() != EXT_NOR_OK)
  {
    s_ready = 0U;
    return -1;
  }
  s_head = STORE_DATA_OFF;
  s_sector = 0U;
  s_seq = 0U;
  s_records = 0U;
  s_records_known = 1U;
  s_writes = 0U;
  s_formatted = 1U;
  store_hint_save();
  return 0;
}

void APP_STORE_Event(const char *fmt, ...)
{
  char text[160];
  va_list ap;
  int n;
  uint32_t now = HAL_GetTick();

  va_start(ap, fmt);
  n = vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);

  if (n <= 0)
  {
    return;
  }

  /* Rate limit so a chatty event can never wear the flash out. */
  if ((now - s_last_event_ms) < 200U)
  {
    return;
  }
  s_last_event_ms = now;

  if (!s_ready)
  {
    return;
  }
  (void)store_write_rec(APP_STORE_T_EVENT, text, (uint32_t)((n < (int)sizeof(text)) ? n : (int)sizeof(text)));
}

void APP_STORE_LogShow(uint32_t count)
{
  uint32_t sector;
  uint32_t shown = 0U;
  uint32_t best_seq = 0U;
  static uint8_t payload[APP_STORE_MAX_PAYLOAD];

  if (!s_ready)
  {
    APP_LOG_Write("store: log unavailable\r\n");
    return;
  }

  /* Second pass over the journal, newest first: the seq numbers are monotonic,
   * so scan for the highest seq below the current ceiling. */
  for (uint32_t want = 0U; want < count; want++)
  {
    uint32_t found_seq = 0U;
    uint8_t found = 0U;

    for (sector = 0U; sector < (STORE_DATA_LEN / EXT_NOR_SECTOR_SIZE); sector++)
    {
      uint32_t off = STORE_DATA_OFF + (sector * EXT_NOR_SECTOR_SIZE);
      uint32_t limit = off + EXT_NOR_SECTOR_SIZE;

      while ((off + REC_HDR_SIZE) <= limit)
      {
        store_rec_t r;

        if (store_read(off, &r, sizeof(r)) != EXT_NOR_OK) { break; }
        if (!store_rec_valid(&r)) { break; }
        if ((r.type == APP_STORE_T_EVENT) &&
            ((best_seq == 0U) ? (r.seq > found_seq) : ((r.seq > found_seq) && (r.seq < best_seq))))
        {
          uint32_t len = (r.len < APP_STORE_MAX_PAYLOAD) ? r.len : APP_STORE_MAX_PAYLOAD;
          if (store_read(off + REC_HDR_SIZE, payload, len) == EXT_NOR_OK)
          {
            found_seq = r.seq;
            found = 1U;
            payload[len] = 0U;
          }
        }
        off += REC_HDR_SIZE + ((r.len + 3U) & ~3U);
      }
    }

    if (!found)
    {
      break;
    }
    best_seq = found_seq;
    APP_LOG_Printf("  [%lu] %s\r\n", (unsigned long)found_seq, (const char *)payload);
    shown++;
  }

  if (shown == 0U)
  {
    APP_LOG_Write("store: no events recorded yet\r\n");
  }
}

void APP_STORE_PrintStatus(void)
{
  if (s_records_known)
  {
    APP_LOG_Printf("store: %s%s, head=0x%06lX sector=%lu records=%lu writes=%lu\r\n",
                   s_ready ? "ready" : "unavailable",
                   s_formatted ? " (formatted now)" : "",
                   (unsigned long)s_head, (unsigned long)s_sector,
                   (unsigned long)s_records, (unsigned long)s_writes);
  }
  else
  {
    APP_LOG_Printf("store: %s%s, head=0x%06lX sector=%lu records=? writes=%lu"
                   "   (? = mounted from the CMOS hint, no scan this boot)\r\n",
                   s_ready ? "ready" : "unavailable",
                   s_formatted ? " (formatted now)" : "",
                   (unsigned long)s_head, (unsigned long)s_sector,
                   (unsigned long)s_writes);
  }
  APP_LOG_Printf("       window 0x%06lX..0x%06lX (%lu KB), nor: %s, id=0x%06lX errors=%lu\r\n",
                 (unsigned long)EXT_NOR_STORE_OFF,
                 (unsigned long)(EXT_NOR_STORE_OFF + EXT_NOR_STORE_SIZE),
                 (unsigned long)(EXT_NOR_STORE_SIZE / 1024U),
                 EXT_NOR_Status(),
                 (unsigned long)EXT_NOR_JedecId(),
                 (unsigned long)EXT_NOR_Errors());
}
