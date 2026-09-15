#include "apie_learn.h"
#include "apie.h"
#include "apie_decode.h"
#include "apie_unknown.h"
#include "app_config.h"
#include "app_learn_store.h"
#include "app_store.h"
#include "app_log.h"
#include "app_pd.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

extern uint32_t HAL_GetTick(void);

#define FNV_OFFSET_BASIS 2166136261u
#define FNV_PRIME        16777619u

static APIE_LearnState_t s_learn;
static uint16_t s_active_vid = 0, s_active_pid = 0;
static uint8_t  s_pending_learn_q = 0xFFu;
static uint32_t s_pending_learn_since = 0;
static char     s_pending_learn_reply[256];

static const char *s_cat_names[] = {
  "UNKNOWN","GET_STATUS","GET_PPS","GET_SRC_CAP","GET_SNK_CAP","GET_SRC_CAP_EXT","VDM_IDENTITY","VDM_SVIDS","VDM_MODES","VDM_ENTER","VDM_EXIT","VDM_ATTENTION","MANU_INFO","BATTERY","COUNTRY","ALERT","REQUEST","VENDOR_DEFINED","CABLE_IDENTITY","CABLE_VDM","EPR_AVS","EPR_MODE","PERIODIC_TELEM","STATE_DEPENDENT","RESET_RELATED",
};

/* No filesystem any more: the learned table lives in the external NOR flash
   (app_learn_store.c) and in the backup SRAM (apie_bkp.c). */

static uint32_t fnv1a_hash(const uint8_t *data, uint32_t len){
  uint32_t h=FNV_OFFSET_BASIS;
  for(uint32_t i=0;i<len;i++){ h ^= data[i]; h*=FNV_PRIME; }
  return h;
}
static uint32_t compute_sig(uint8_t sop,uint8_t msg_type,uint8_t extended,const uint8_t *payload,uint16_t len){
  uint8_t buf[8]; uint8_t p=0;
  buf[p++]=sop; buf[p++]=msg_type; buf[p++]=extended;
  uint16_t c=(len>4U)?4U:len;
  for(uint16_t i=0;i<c;i++) buf[p++]=payload[i];
  return fnv1a_hash(buf,p);
}

/* Proper PD type names via decoder + purpose inference */
static uint8_t classify_signature(uint8_t sop,uint8_t msg_type,uint8_t extended,uint8_t n_objects,const uint8_t *payload,uint16_t len){
  char tname[48]={0};
  APIE_Decode_TypeNameN(msg_type, extended, n_objects, tname, sizeof(tname));
  if (!extended){
    /* Control (n_objects==0) vs Data (n_objects>0) use tname disambiguation */
    if (n_objects==0){
      if (msg_type==0x07) return APIE_LEARN_CAT_GET_SRC_CAP; /* Get_Source_Cap (control) */
      if (msg_type==0x08) return APIE_LEARN_CAT_GET_SNK_CAP;
      if (msg_type==0x03) return APIE_LEARN_CAT_GET_STATUS; /* Accept -> treat as status flow */
      if (msg_type==0x11) return APIE_LEARN_CAT_GET_SRC_CAP_EXT; /* Get_Source_Cap_Extended (ctrl) */
      if (msg_type==0x12) return APIE_LEARN_CAT_GET_STATUS; /* Get_Status */
      if (msg_type==0x14) return APIE_LEARN_CAT_GET_PPS;
      if (msg_type==0x15) return APIE_LEARN_CAT_COUNTRY; /* Get_Country_Codes */
      if (msg_type==0x16) return APIE_LEARN_CAT_GET_SNK_CAP; /* Get_Sink_Cap_Extended */
    } else {
      if (msg_type==0x01) return APIE_LEARN_CAT_GET_SRC_CAP; /* Source_Capabilities */
      if (msg_type==0x02) return APIE_LEARN_CAT_REQUEST; /* Request */
      if (msg_type==0x04) return APIE_LEARN_CAT_GET_SNK_CAP; /* Sink_Capabilities */
      if (msg_type==0x06) return APIE_LEARN_CAT_ALERT;
      if (msg_type==0x0A) return APIE_LEARN_CAT_EPR_MODE; /* EPR_Mode */
      if (msg_type==0x0F) return APIE_LEARN_CAT_VENDOR_DEFINED; /* Vendor_Defined (data) */
    }
  } else {
    if (len>=1){
      switch(payload[0]){ /* extended message type byte */
        case 0x01: return APIE_LEARN_CAT_GET_SRC_CAP_EXT;
        case 0x02: return APIE_LEARN_CAT_GET_STATUS;
        case 0x03: return APIE_LEARN_CAT_GET_PPS;
        case 0x05: return APIE_LEARN_CAT_BATTERY;
        case 0x06: return APIE_LEARN_CAT_BATTERY;
        case 0x07: return APIE_LEARN_CAT_MANU_INFO;
        case 0x08: return APIE_LEARN_CAT_COUNTRY;
        case 0x09: return APIE_LEARN_CAT_COUNTRY;
        case 0x0A: return APIE_LEARN_CAT_EPR_AVS;
        case 0x0C: return APIE_LEARN_CAT_GET_PPS;
        case 0x0E: return APIE_LEARN_CAT_COUNTRY;
        default: break;
      }
    }
    /* unstructured/extended control falls through to VDM check */
  }
  if (len>=4){
    uint32_t vdo0 = payload[0] | (payload[1]<<8) | (payload[2]<<16) | (payload[3]<<24);
    uint8_t vtype = (vdo0>>13)&0x03;
    uint8_t vcmd = (vdo0>>8)&0x1F;
    if (vtype==0x01){
      switch(vcmd){
        case 0x01: return APIE_LEARN_CAT_VDM_IDENTITY;
        case 0x02: return APIE_LEARN_CAT_VDM_SVIDS;
        case 0x03: return APIE_LEARN_CAT_VDM_MODES;
        case 0x04: return APIE_LEARN_CAT_VDM_ENTER;
        case 0x05: return APIE_LEARN_CAT_VDM_EXIT;
        case 0x06: return APIE_LEARN_CAT_VDM_ATTENTION;
        default: return APIE_LEARN_CAT_CABLE_VDM;
      }
    } else if (vtype==0x00){ return APIE_LEARN_CAT_VENDOR_DEFINED; }
  }
  if (sop==1 || sop==2) return APIE_LEARN_CAT_CABLE_IDENTITY;
  /* periodic telemetry heuristic will be refined by interval tracking */
  return APIE_LEARN_CAT_VENDOR_DEFINED;
}

/* Human purpose string: heavy inference with decoding */
static void infer_human_purpose(const APIE_LearnSig_t *sig, char *out, uint32_t outsz){
  if(!sig||!out||outsz==0) return;
  char tname[48]={0}; APIE_Decode_TypeNameN(sig->msg_type, sig->extended, sig->n_objects, tname, sizeof(tname));
  const char *cat = (sig->category < 25) ? s_cat_names[sig->category] : "UNKNOWN";
  float freq=0; if(sig->interval_count>0) freq=1000.0f/((float)sig->interval_sum_ms/(float)sig->interval_count);
  /* vendor payload hex */
  char phex[65]={0}; for(uint8_t i=0;i<sig->payload_len && i<8;i++) snprintf(&phex[i*2], sizeof(phex)-i*2, "%02X", sig->payload[i]);
  /* purpose sentence */
  if(sig->category==APIE_LEARN_CAT_VENDOR_DEFINED){
    snprintf(out,outsz,"Vendor-specific %s (SOP%u %s, %u objs, payload %s) — appears %lu time(s)%.0f freq=%.1f/s; vbus_corr=%u cur_corr=%u temp_corr=%u. Use `learn run %u` to query the source for its human-readable reply. Learned as %s.",
      tname, (unsigned)sig->sop, sig->extended?"extended":"", (unsigned)sig->n_objects, phex[0]?phex:"<empty>", (unsigned long)sig->count, (double)freq, freq, (unsigned)sig->vbus_corr, (unsigned)sig->current_corr, (unsigned)sig->temp_corr, (unsigned)(sig - s_learn.sigs), cat);
  } else if(sig->interval_count>=3 && freq>=1.0f){
    snprintf(out,outsz,"%s %s — periodic %.1f/s (interval %lums), count %lu, payload %s. Likely telemetry/state broadcast. Try `learn run %u`.",
      cat, tname, (double)freq, (unsigned long)(sig->interval_sum_ms / (sig->interval_count?sig->interval_count:1)), (unsigned long)sig->count, phex, (unsigned)(sig - s_learn.sigs));
  } else if(sig->reset_corr>0){
    snprintf(out,outsz,"%s %s — correlates with hard resets (%u). Payload %s. Caution: querying may disturb link. Use `learn run %u` carefully.",
      cat, tname, (unsigned)sig->reset_corr, phex, (unsigned)(sig - s_learn.sigs));
  } else {
    snprintf(out,outsz,"%s %s — %u payload bytes %s, seen %lu time(s) from VID:PID %04X:%04X. Confidence %u%%. Query via `learn run %u` for source's human reply.",
      cat, tname, (unsigned)sig->payload_len, phex, (unsigned long)sig->count, (unsigned)s_active_vid, (unsigned)s_active_pid, (unsigned)sig->confidence, (unsigned)(sig - s_learn.sigs));
  }
  out[outsz-1]='\0';
}

static void auto_assign_name(APIE_LearnSig_t *sig){
  if(!s_learn.auto_name || sig->has_cli) return;
  const char *base = (sig->category < 25) ? s_cat_names[sig->category] : "UNKNOWN";
  uint8_t suffix=0;
  for(uint16_t i=0;i<s_learn.count;i++) if(s_learn.sigs[i].category==sig->category && s_learn.sigs[i].has_cli) suffix++;
  if(suffix==0){ snprintf(sig->cli_name, APIE_LEARN_NAME_MAX, "%s", base); snprintf(sig->cli_cmd, APIE_LEARN_CMD_MAX, "%s", base); }
  else { snprintf(sig->cli_name, APIE_LEARN_NAME_MAX, "%s_%u", base, suffix); snprintf(sig->cli_cmd, APIE_LEARN_CMD_MAX, "%s%u", base, suffix); }
  for(char *p=sig->cli_cmd; *p; p++) *p=tolower(*p);
  sig->has_cli=1; s_learn.dirty=1U;
  /* Auto-announce on serial CLI */
  char human[256]; infer_human_purpose(sig, human, sizeof(human));
  APP_LOG_Printf("\r\n[learn] new %s discovered (idx %u): %s\r\n", base, (unsigned)(sig - s_learn.sigs), human);
  APP_LOG_Printf("[learn]  type `learn run %u` to ask the source and get its human reply\r\n", (unsigned)(sig - s_learn.sigs));
}

static void update_confidence(APIE_LearnSig_t *sig){
  uint32_t conf=10;
  if(sig->count>=10) conf+=20; else if(sig->count>=5) conf+=15; else if(sig->count>=3) conf+=10; else if(sig->count>=2) conf+=5;
  if(sig->interval_count>=3) conf+=15;
  if(sig->vbus_corr>0 || sig->current_corr>0 || sig->temp_corr>0) conf+=10;
  if(sig->reset_corr>0) conf+=15;
  if(sig->has_response) conf+=10;
  if(sig->category!=APIE_LEARN_CAT_UNKNOWN && sig->category!=APIE_LEARN_CAT_VENDOR_DEFINED) conf+=10;
  if(conf>100) conf=100;
  sig->confidence=(uint8_t)conf;
}

void APIE_Learn_Init(void){
  memset(&s_learn,0,sizeof(s_learn));
  s_learn.enabled=1U; s_learn.auto_name=1U; s_learn.save_interval_ms=30000U;
  s_active_vid=0; s_active_pid=0;
  s_pending_learn_q=0xFFu;
}

void APIE_Learn_ResetSession(uint16_t vid, uint16_t pid){
  s_active_vid=vid; s_active_pid=pid;
  for(uint16_t i=0;i<s_learn.count;i++){ s_learn.sigs[i].interval_sum_ms=0; s_learn.sigs[i].interval_count=0; }
  /* Reload the stored image when a source is identified: the blob is keyed by
     source vid/pid per signature, so the whole image is re-imported from the
     external NOR store (app_learn_store.c). */
  if((vid!=0) || (pid!=0)){
    (void)APIE_Learn_LoadStore();
  }
  s_learn.last_save_ms=HAL_GetTick(); s_learn.dirty=1U;
}

void APIE_Learn_FeedPacket(uint8_t sop,uint8_t msg_type,uint8_t extended,uint8_t n_objects,const uint8_t *payload,uint16_t len,uint32_t vbus_mv,uint32_t current_ma,uint32_t temp_c,uint8_t reset_occurred,uint8_t voltchg_occurred,uint8_t attach_occurred){
  if(!s_learn.enabled || payload==NULL) return;
  if(len==0) len=s_learn.sigs[0].payload_len; /* keep sig if len 0? but need guard */
  if(s_learn.count>=APIE_LEARN_MAX_SIGNATURES && len>APIE_LEARN_MAX_PAYLOAD) return;
  uint32_t now=HAL_GetTick();
  uint32_t sig_hash=compute_sig(sop,msg_type,extended,payload,len);
  int16_t idx=-1;
  for(uint16_t i=0;i<s_learn.count;i++) if(s_learn.sigs[i].signature==sig_hash && s_learn.sigs[i].sop==sop && s_learn.sigs[i].msg_type==msg_type && s_learn.sigs[i].extended==extended){ idx=(int16_t)i; break; }
  APIE_LearnSig_t *sig;
  if(idx>=0){
    sig=&s_learn.sigs[idx];
    sig->count++;
    if(sig->last_seen_ms!=0){ uint32_t iv=now - sig->last_seen_ms; sig->interval_sum_ms+=iv; sig->interval_count++; }
    sig->last_seen_ms=now;
    if(reset_occurred) sig->reset_corr++;
    if(voltchg_occurred) sig->voltchg_corr++;
    if(attach_occurred) sig->attach_corr++;
    for(uint8_t j=0;j<sig->payload_len && j<len;j++){ if(sig->payload[j]==(vbus_mv&0xFF)) sig->vbus_corr++; if(sig->payload[j]==(current_ma&0xFF)) sig->current_corr++; if(sig->payload[j]==(temp_c&0xFF)) sig->temp_corr++; }
  } else {
    if(s_learn.count>=APIE_LEARN_MAX_SIGNATURES) return;
    idx=s_learn.count++;
    sig=&s_learn.sigs[idx]; memset(sig,0,sizeof(*sig));
    sig->signature=sig_hash; sig->sop=sop; sig->msg_type=msg_type; sig->extended=extended; sig->n_objects=n_objects;
    sig->payload_len=(len>APIE_LEARN_MAX_PAYLOAD)?APIE_LEARN_MAX_PAYLOAD:len;
    if(len>0) memcpy(sig->payload,payload,sig->payload_len);
    sig->count=1; sig->first_seen_ms=now; sig->last_seen_ms=now;
    sig->category=classify_signature(sop,msg_type,extended,n_objects,payload,len);
    sig->source_vid=s_active_vid; sig->source_pid=s_active_pid; sig->version=1;
  }
  update_confidence(sig);
  if(!sig->has_cli) auto_assign_name(sig);
  s_learn.dirty=1U;
}
void APIE_Learn_FeedEvent(uint8_t et,uint32_t v){ (void)et;(void)v; }
void APIE_Learn_SetAutoName(uint8_t on){ s_learn.auto_name=on?1U:0U; }
uint8_t APIE_Learn_AssignName(uint16_t idx,const char *name,const char *cmd){
  if(idx>=s_learn.count||!name||!cmd) return 0U;
  APIE_LearnSig_t *sig=&s_learn.sigs[idx];
  strncpy(sig->cli_name,name,APIE_LEARN_NAME_MAX-1); sig->cli_name[APIE_LEARN_NAME_MAX-1]='\0';
  strncpy(sig->cli_cmd,cmd,APIE_LEARN_CMD_MAX-1); sig->cli_cmd[APIE_LEARN_CMD_MAX-1]='\0';
  sig->has_cli=1U; s_learn.dirty=1U; return 1U;
}
const APIE_LearnSig_t *APIE_Learn_Get(uint16_t idx){ if(idx>=s_learn.count) return NULL; return &s_learn.sigs[idx]; }
uint16_t APIE_Learn_Count(void){ return s_learn.count; }
int16_t APIE_Learn_Find(uint8_t sop,uint8_t msg_type,uint8_t extended,const uint8_t *payload,uint16_t len){
  if(!payload||len==0) return -1;
  uint32_t h=compute_sig(sop,msg_type,extended,payload,len);
  for(uint16_t i=0;i<s_learn.count;i++) if(s_learn.sigs[i].signature==h && s_learn.sigs[i].sop==sop && s_learn.sigs[i].msg_type==msg_type && s_learn.sigs[i].extended==extended) return (int16_t)i;
  return -1;
}
void APIE_Learn_Dump(void){
  if(s_learn.count==0){ APP_LOG_Write("learn: no signatures captured\r\n"); return; }
  APP_LOG_Printf("learn: %u signature(s) captured (VID:PID %04X:%04X)\r\n",(unsigned)s_learn.count,(unsigned)s_active_vid,(unsigned)s_active_pid);
  for(uint16_t i=0;i<s_learn.count;i++){
    const APIE_LearnSig_t *sig=&s_learn.sigs[i];
    float freq=0; if(sig->interval_count>0) freq=1000.0f/((float)sig->interval_sum_ms/(float)sig->interval_count);
    char human[280]; infer_human_purpose(sig,human,sizeof(human));
    APP_LOG_Printf("  [%02u] sig=0x%08lX sop=%u type=0x%02X ext=%u cat=%s conf=%u%% n=%lu freq=%.1f/s\r\n",(unsigned)i,(unsigned long)sig->signature,(unsigned)sig->sop,(unsigned)sig->msg_type,(unsigned)sig->extended,s_cat_names[sig->category],(unsigned)sig->confidence,(unsigned long)sig->count,(double)freq);
    if(sig->has_cli) APP_LOG_Printf("       CLI: `%s`  (run with `learn run %u`)\r\n",sig->cli_cmd,(unsigned)i);
    APP_LOG_Printf("       %s\r\n",human);
  }
}

/* The SD card is gone; these two keep their names because the CLI, the periodic
   task and the backup-SRAM image refer to them, but they persist to the external
   NOR flash through app_learn_store.c.  Both return 0 when no NOR device is
   present, and the engine then simply keeps running from the backup SRAM. */
uint8_t APIE_Learn_SaveToSD(void){
  uint8_t ok = APIE_Learn_SaveStore();
  if(ok != 0U){ s_learn.last_save_ms=HAL_GetTick(); s_learn.dirty=0U; }
  return ok;
}
uint8_t APIE_Learn_LoadFromSD(void){
  return APIE_Learn_LoadStore();
}
const char *APIE_Learn_CatName(uint8_t cat){ if(cat<sizeof(s_cat_names)/sizeof(s_cat_names[0])) return s_cat_names[cat]; return "INVALID"; }

/* Query-back: send the learned packet's type as a harmless Get_* / VDM and print human reply when response arrives.
   For safety: only structured Get_* / VDM Discover are actually transmitted; pure vendor blob is re-decoded locally and
   shown as hex+classification without bus transmit (never sends arbitrary vendor payload that could confuse source). */
uint8_t APIE_Learn_Exec(uint16_t idx){
  if(idx>=s_learn.count) { APP_LOG_Write("learn: bad index\r\n"); return 0U; }
  const APIE_LearnSig_t *sig=&s_learn.sigs[idx];
  char tname[48]={0}; APIE_Decode_TypeNameN(sig->msg_type, sig->extended, sig->n_objects, tname, sizeof(tname));
  APP_LOG_Printf("learn: running [%u] %s (%s) — asking source for its reply...\r\n",(unsigned)idx, sig->cli_name[0]?sig->cli_name:s_cat_names[sig->category], tname);
  /* Decode what we know locally first */
  char human[280]; infer_human_purpose(sig, human, sizeof(human));
  APP_LOG_Printf("  known local decode: %s\r\n", human);
  /* Mark pending so next feed_unknown response is attributed */
  s_pending_learn_q = (uint8_t)idx;
  s_pending_learn_since = HAL_GetTick();
  /* Dispatch known safe queries */
  int rc=-1;
  switch(sig->category){
    case APIE_LEARN_CAT_GET_STATUS: rc=APIE_IssueQuery(0, APIE_QUERY_GET_STATUS); break;
    case APIE_LEARN_CAT_GET_PPS: rc=APIE_IssueQuery(0, APIE_QUERY_GET_PPS); break;
    case APIE_LEARN_CAT_GET_SRC_CAP: rc=APIE_IssueQuery(0, APIE_QUERY_GET_STATUS); /* fallback */ break;
    case APIE_LEARN_CAT_VDM_IDENTITY: rc=APIE_IssueQuery(0, APIE_QUERY_IDENTITY); break;
    case APIE_LEARN_CAT_VDM_SVIDS: rc=APIE_IssueQuery(0, APIE_QUERY_SVIDS); break;
    case APIE_LEARN_CAT_VDM_MODES: rc=APIE_IssueQuery(0, APIE_QUERY_MODES); break;
    case APIE_LEARN_CAT_MANU_INFO: rc=APIE_IssueQuery(0, APIE_QUERY_MANU_INFO); break;
    case APIE_LEARN_CAT_BATTERY: rc=APIE_IssueQuery(0, APIE_QUERY_BATTERY); break;
    case APIE_LEARN_CAT_COUNTRY: rc=APIE_IssueQuery(0, APIE_QUERY_COUNTRY); break;
    case APIE_LEARN_CAT_VENDOR_DEFINED:
    default: {
      /* Do NOT blindly replay vendor bytes. Show human hex + guidance. */
      char phex[65]={0}; for(uint8_t i=0;i<sig->payload_len && i<8;i++) snprintf(&phex[i*2],32,"%02X", sig->payload[i]);
      APP_LOG_Printf("  vendor/unknown payload %s — no standard query maps 1:1.\r\n", phex);
      APP_LOG_Write("  The engine keeps watching: next time the source emits this pattern its fresh payload will be auto-decoded below.\r\n");
      /* Still record as “asked” so anomaly tracks */
      rc=0;
      break;
    }
  }
  if(rc==0) { APP_LOG_Write("  waiting for source reply (watch `learn dump` / next auto-decode)...\r\n"); return 1U; }
  else { APP_LOG_Write("  could not issue query now (no contract / rate-limited). Will retry when link ready.\r\n"); s_pending_learn_q=0xFFu; return 0U; }
}

void APIE_Learn_OnResponse(uint8_t sop, const uint8_t *payload, uint16_t len){
  if(s_pending_learn_q==0xFFu) return;
  if(HAL_GetTick() - s_pending_learn_since > 2000) { s_pending_learn_q=0xFFu; return; }
  const APIE_LearnSig_t *sig=&s_learn.sigs[s_pending_learn_q];
  char tname[48]={0}; APIE_Decode_TypeNameN(sig->msg_type, sig->extended, sig->n_objects, tname, sizeof(tname));
  char phex[65]={0}; for(uint16_t i=0;i<len && i<8;i++) snprintf(&phex[i*2],32,"%02X", payload[i]);
  APP_LOG_Printf("\r\n[learn] reply for [%u] %s (%s): %u byte(s) %s\r\n",(unsigned)s_pending_learn_q, sig->cli_name[0]?sig->cli_name:s_cat_names[sig->category], tname, (unsigned)len, phex);
  /* Try decode as VDO/text */
  if(len>=4){
    char vdo_txt[64]={0}; uint32_t vdo = payload[0]|(payload[1]<<8)|(payload[2]<<16)|(payload[3]<<24);
    APIE_Decode_VDO(vdo, vdo_txt, sizeof(vdo_txt));
    APP_LOG_Printf("  decoded VDO: %s\r\n", vdo_txt);
    if(len>=8){
      char txt2[64]={0}; uint32_t vdo2 = payload[4]|(payload[5]<<8)|(payload[6]<<16)|(payload[7]<<24);
      APIE_Decode_VDO(vdo2, txt2, sizeof(txt2));
      APP_LOG_Printf("           + %s\r\n", txt2);
    }
  }
  if(len>0 && payload[0] < 0x20){
    /* looks like extended status bytes */
    APP_LOG_Printf("  raw extended status byte 0x%02X — see `unknown` for UNKNOWN_SIGNATURE analysis\r\n", (unsigned)payload[0]);
  }
  snprintf(s_pending_learn_reply, sizeof(s_pending_learn_reply), "last reply %uB %s", (unsigned)len, phex);
  s_pending_learn_q=0xFFu;
}

const char *APIE_Learn_LastReply(void){ return s_pending_learn_reply; }

void APIE_Learn_CliLearn(int argc,char *argv[]){
  if(argc<2){ APP_LOG_Write("usage: learn on|off|dump|save|load|auto on|off|assign <idx> <name> <cmd>|run <idx>|export\r\n"); return; }
  if(strcmp(argv[1],"on")==0){ s_learn.enabled=1U; APP_LOG_Write("learn: enabled\r\n"); }
  else if(strcmp(argv[1],"off")==0){ s_learn.enabled=0U; APP_LOG_Write("learn: disabled\r\n"); }
  else if(strcmp(argv[1],"dump")==0){ APIE_Learn_Dump(); }
  else if(strcmp(argv[1],"save")==0){ APIE_Learn_SaveToSD(); }
  else if(strcmp(argv[1],"load")==0){ APIE_Learn_LoadFromSD(); }
  else if(strcmp(argv[1],"run")==0){
    if(argc<3){ APP_LOG_Write("usage: learn run <idx>\r\n"); return; }
    unsigned idx; if(sscanf(argv[2],"%u",&idx)!=1){ APP_LOG_Write("learn: bad idx\r\n"); return; }
    APIE_Learn_Exec((uint16_t)idx);
  }
  else if(strcmp(argv[1],"auto")==0){
    if(argc>=3) s_learn.auto_name=(strcmp(argv[2],"on")==0)?1U:0U;
    APP_LOG_Printf("learn: auto-name %s\r\n",s_learn.auto_name?"on":"off");
  }
  else if(strcmp(argv[1],"assign")==0){
    if(argc<5){ APP_LOG_Write("usage: learn assign <idx> <name> <cmd>\r\n"); return; }
    unsigned idx; if(sscanf(argv[2],"%u",&idx)!=1||idx>=s_learn.count){ APP_LOG_Write("learn: invalid index\r\n"); return; }
    if(APIE_Learn_AssignName((uint16_t)idx,argv[3],argv[4])) APP_LOG_Printf("learn: assigned '%s' -> '%s' to %u\r\n",argv[3],argv[4],idx);
  }
  else APP_LOG_Write("usage: learn on|off|dump|save|load|auto on|off|assign <idx> <name> <cmd>|run <idx>\r\n");
}
void APIE_Learn_CliSignatures(int argc,char *argv[]){ (void)argc;(void)argv; APIE_Learn_Dump(); }
void APIE_Learn_CliAssign(int argc,char *argv[]){ APIE_Learn_CliLearn(argc,argv); }
void APIE_Learn_CliExport(int argc,char *argv[]){
  static char csv[APP_STORE_MAX_PAYLOAD];
  uint32_t used;
  (void)argc;(void)argv;

  /* No filesystem: the table goes to the console (capture it there) and a copy
     is kept in the external NOR store, which is what 'store' hands back. */
  APP_LOG_Write("idx,sop,msg_type,cat,conf,count,cli_name,cli_cmd,human\r\n");
  used=(uint32_t)snprintf(csv,sizeof(csv),"idx,sop,type,cat,conf,count,cli\r\n");
  for(uint16_t i=0;i<s_learn.count;i++){
    const APIE_LearnSig_t *sig=&s_learn.sigs[i];
    char human[160];
    infer_human_purpose(sig,human,sizeof(human));
    APP_LOG_Printf("%u,%u,0x%02X,%s,%u,%lu,%s,%s,%s\r\n",(unsigned)i,
                   (unsigned)sig->sop,(unsigned)sig->msg_type,
                   s_cat_names[sig->category],(unsigned)sig->confidence,
                   (unsigned long)sig->count,sig->has_cli?sig->cli_name:"",
                   sig->has_cli?sig->cli_cmd:"",human);
    if((used>0U)&&(used<(sizeof(csv)-120U))){
      int n=snprintf(&csv[used],sizeof(csv)-used,"%u,%u,0x%02X,%s,%u,%lu,%s\r\n",
                     (unsigned)i,(unsigned)sig->sop,(unsigned)sig->msg_type,
                     s_cat_names[sig->category],(unsigned)sig->confidence,
                     (unsigned long)sig->count,sig->has_cli?sig->cli_name:"");
      if(n>0){ used+=(uint32_t)n; }
    }
  }
  if(APP_STORE_Ready()!=0U){
    if(APP_STORE_Write(APP_STORE_T_LEARN_CSV,csv,used)==0){
      APP_LOG_Printf("learn: CSV copy kept in the NOR store (%lu bytes)\r\n",(unsigned long)used);
    }
  }
}
void APIE_Learn_PeriodicTask(void){
  if(!s_learn.enabled||!s_learn.dirty) return;
  uint32_t now=HAL_GetTick();
  if((now - s_learn.last_save_ms) >= s_learn.save_interval_ms) APIE_Learn_SaveToSD();
  if(s_pending_learn_q!=0xFFu && (now - s_pending_learn_since) > 2000) s_pending_learn_q=0xFFu;
}

const APIE_LearnState_t *APIE_Learn_GetState(void){ return &s_learn; }

uint16_t APIE_Learn_Export(uint8_t *out, uint16_t outsz)
{
  if (out == NULL || outsz < sizeof(APIE_LearnState_t))
  {
    return 0U;
  }
  memcpy(out, &s_learn, sizeof(APIE_LearnState_t));
  return (uint16_t)sizeof(APIE_LearnState_t);
}

uint8_t APIE_Learn_Import(const uint8_t *in, uint16_t len)
{
  if (in == NULL || len != sizeof(APIE_LearnState_t))
  {
    return 0U;
  }
  /* Validate: count must not exceed max */
  const APIE_LearnState_t *src = (const APIE_LearnState_t *)in;
  if (src->count > APIE_LEARN_MAX_SIGNATURES)
  {
    return 0U;
  }
  memcpy(&s_learn, in, sizeof(APIE_LearnState_t));
  return 1U;
}

/* Map a learned signature category to its canonical APIE query ID.
 * Returns 0xFF if no standard query maps to this category. */
uint8_t APIE_Learn_CategoryToQuery(uint8_t category)
{
  switch (category)
  {
    case APIE_LEARN_CAT_GET_STATUS:       return APIE_QUERY_GET_STATUS;
    case APIE_LEARN_CAT_GET_PPS:          return APIE_QUERY_GET_PPS;
    case APIE_LEARN_CAT_GET_SRC_CAP:      return APIE_QUERY_SRC_EXT;
    case APIE_LEARN_CAT_GET_SRC_CAP_EXT:  return APIE_QUERY_SRC_EXT;
    case APIE_LEARN_CAT_GET_SNK_CAP:      return 0xFF;  /* No standard query */
    case APIE_LEARN_CAT_VDM_IDENTITY:     return APIE_QUERY_IDENTITY;
    case APIE_LEARN_CAT_VDM_SVIDS:        return APIE_QUERY_SVIDS;
    case APIE_LEARN_CAT_VDM_MODES:        return APIE_QUERY_MODES;
    case APIE_LEARN_CAT_MANU_INFO:        return APIE_QUERY_MANU_INFO;
    case APIE_LEARN_CAT_BATTERY:          return APIE_QUERY_BATTERY;
    case APIE_LEARN_CAT_COUNTRY:          return APIE_QUERY_COUNTRY;
    case APIE_LEARN_CAT_CABLE_IDENTITY:   return 0xFF;  /* SOP' identity via different path */
    default:                              return 0xFF;
  }
}

/* Check if a signature should trigger auto-query (confidence≥30%, count≥3) */
uint8_t APIE_Learn_ShouldAutoQuery(const APIE_LearnSig_t *sig)
{
  if (sig == NULL) return 0U;
  if (sig->confidence < 30U) return 0U;
  if (sig->count < 3U) return 0U;
  if (sig->category == APIE_LEARN_CAT_UNKNOWN || sig->category == APIE_LEARN_CAT_VENDOR_DEFINED)
    return 0U;
  return 1U;
}

/* Execute auto-query for a signature index */
uint8_t APIE_Learn_ExecAutoQuery(uint16_t sig_idx)
{
  if (sig_idx >= s_learn.count) return 0U;
  const APIE_LearnSig_t *sig = &s_learn.sigs[sig_idx];
  if (!APIE_Learn_ShouldAutoQuery(sig)) return 0U;
  uint8_t query = APIE_Learn_CategoryToQuery(sig->category);
  if (query == 0xFF) return 0U;
  return (APIE_IssueQuery(0U, (APIE_QueryId_t)query) == 0) ? 1U : 0U;
}

/* Update all signatures with the now-known source VID/PID */
void APIE_Learn_UpdateSourceID(uint16_t vid, uint16_t pid)
{
  for (uint16_t i = 0; i < s_learn.count; i++)
  {
    s_learn.sigs[i].source_vid = vid;
    s_learn.sigs[i].source_pid = pid;
  }
  s_learn.dirty = 1U;
}
