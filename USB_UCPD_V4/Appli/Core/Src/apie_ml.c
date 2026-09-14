/**
  ******************************************************************************
  * @file    apie_ml.c
  * @brief   Embedded ML inference: NaiveBayes + Logistic + Heavy MLP + Per-Q
  *          anomaly.  All online, bounded, failsafe (no NaN/Inf leaks).
  *
  * Heavy mode: 12-in ->16 ReLU ->8 ReLU ->1 sigmoid.  ~352 MACs per forward,
  * double for backward, ~700 FLOPs per Observe.  Three-model ensemble:
  * NB (discrete), logistic (4-feat), MLP (12-feat).  Per-query Welford (9x)
  * plus global.  All NaN/Inf clamped, weights bounded, never gates safety.
  ******************************************************************************
  */
#include "apie_ml.h"
#include "apie_decode.h"
#include "apie_stats.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static APIE_MlModel_t s_ml;

/* Online SGD params */
#define ML_LOGIT_LR     0.05f
#define ML_MLP_LR       0.01f
#define ML_CLAMP        10.0f
#define ML_CLAMP_W1     2.0f

static void bin_get(uint32_t val, uint8_t *bin);
static float clamp_f(float x, float lo, float hi) { if (x < lo) return lo; if (x > hi) return hi; return x; }
static float sigmoid_f(float x) {
  if (x > 10.0f) return 1.0f;
  if (x < -10.0f) return 0.0f;
  return 1.0f / (1.0f + expf(-x));
}
static float relu_f(float x) { return (x > 0.0f) ? x : 0.0f; }

/* Normalize 12 features to ~[0,1] for MLP */
static void mlp_norm(const APIE_FeatureVec_t *fv, float out[APIE_MLP_IN]) {
  if (fv == NULL) { memset(out, 0, APIE_MLP_IN*sizeof(float)); return; }
  /* 0 latency_ms [0,2000], 1 txn_result [0,7], 2 pdo_count [0,12],
     3 voltage_mv [0,21000], 4 current_ma [0,5000], 5 power_mw [0,105000],
     6 soc_temp_c [-10,85]->[0,1], 7 packet_rate [0,100], 8 has_pps [0,1],
     9 msgid_delta [0,7], 10 nobjects [0,7], 11 adv_interval_ms [0,60000] */
  out[0]  = clamp_f(fv->v[0] / 2000.0f, 0, 1);
  out[1]  = clamp_f(fv->v[1] / 7.0f, 0, 1);
  out[2]  = clamp_f(fv->v[2] / 12.0f, 0, 1);
  out[3]  = clamp_f(fv->v[3] / 21000.0f, 0, 1);
  out[4]  = clamp_f(fv->v[4] / 5000.0f, 0, 1);
  out[5]  = clamp_f(fv->v[5] / 105000.0f, 0, 1);
  out[6]  = clamp_f((fv->v[6] + 10.0f) / 95.0f, 0, 1);
  out[7]  = clamp_f(fv->v[7] / 100.0f, 0, 1);
  out[8]  = fv->v[8] > 0.5f ? 1.0f : 0.0f;
  out[9]  = clamp_f(fv->v[9] / 7.0f, 0, 1);
  out[10] = clamp_f(fv->v[10] / 7.0f, 0, 1);
  out[11] = clamp_f(fv->v[11] / 60000.0f, 0, 1);
  for (uint8_t i=0;i<APIE_MLP_IN;i++) if (!isfinite(out[i])) out[i]=0;
}

static float mlp_forward_raw(const float in[APIE_MLP_IN], float h1[APIE_MLP_H1], float h2[APIE_MLP_H2]) {
  for (uint8_t j=0;j<APIE_MLP_H1;j++) {
    float s = s_ml.mlp_b1[j];
    for (uint8_t i=0;i<APIE_MLP_IN;i++) s += in[i] * s_ml.mlp_w1[i][j];
    h1[j] = relu_f(s);
    if (!isfinite(h1[j])) h1[j]=0;
  }
  for (uint8_t j=0;j<APIE_MLP_H2;j++) {
    float s = s_ml.mlp_b2[j];
    for (uint8_t i=0;i<APIE_MLP_H1;i++) s += h1[i] * s_ml.mlp_w2[i][j];
    h2[j] = relu_f(s);
    if (!isfinite(h2[j])) h2[j]=0;
  }
  float out = s_ml.mlp_b3;
  for (uint8_t i=0;i<APIE_MLP_H2;i++) out += h2[i] * s_ml.mlp_w3[i];
  if (!isfinite(out)) out=0;
  return sigmoid_f(out);
}

void APIE_Ml_Init(void)
{
  memset(&s_ml, 0, sizeof(s_ml));
  s_ml.meta.id = 1U;
  s_ml.meta.version = 3U;           /* v3: heavy MLP + per-Q anomaly, fixed logit */
  s_ml.meta.feature_version = APIE_FEATURE_COUNT;
  s_ml.meta.kind = (uint8_t)APIE_MODEL_SMALL_MLP;
  s_ml.meta.accuracy = 0.0f;
  snprintf(s_ml.meta.trained, sizeof(s_ml.meta.trained), "seed-online-v3-heavy");
  /* Small random-ish init for MLP break symmetry: deterministic LCG */
  uint32_t s = 0x9E3779B9u;
  for (uint8_t i=0;i<APIE_MLP_IN;i++) for (uint8_t j=0;j<APIE_MLP_H1;j++) { s = s*1664525u+1013904223u; s_ml.mlp_w1[i][j] = ((float)(s & 0xFFFFu)/65535.0f - 0.5f)*0.2f; }
  for (uint8_t j=0;j<APIE_MLP_H1;j++) s_ml.mlp_b1[j]=0;
  for (uint8_t i=0;i<APIE_MLP_H1;i++) for (uint8_t j=0;j<APIE_MLP_H2;j++) { s = s*1664525u+1013904223u; s_ml.mlp_w1[0][0]+=0; s_ml.mlp_w2[i][j] = ((float)(s & 0xFFFFu)/65535.0f - 0.5f)*0.2f; }
  for (uint8_t j=0;j<APIE_MLP_H2;j++) s_ml.mlp_b2[j]=0;
  for (uint8_t i=0;i<APIE_MLP_H2;i++) { s = s*1664525u+1013904223u; s_ml.mlp_w3[i] = ((float)(s & 0xFFFFu)/65535.0f - 0.5f)*0.2f; }
  s_ml.mlp_b3 = 0;
  for (uint8_t q=0;q<APIE_NB_QUERY_BINS;q++) {
    APIE_Stats_Init(&s_ml.anom_per_q[q]);
    s_ml.anom_inited[q]=0U;
  }
  s_ml.meta.crc32 = APIE_Crc32((const uint8_t *)&s_ml, sizeof(APIE_MlModel_t));
}

void APIE_Ml_Reset(void) { APIE_Ml_Init(); }
const APIE_MlModel_t *APIE_Ml_GetModel(void) { return &s_ml; }

static void bin_get(uint32_t val, uint8_t *bin) {
  if (val >= 7U) *bin = 7U; else *bin = (uint8_t)val;
}

void APIE_Ml_Observe(uint8_t query, uint8_t attempt, uint8_t has_pps, uint8_t hard_known, uint8_t success)
{
  uint8_t cls = (success != 0U) ? 1U : 0U;
  uint8_t abin, pbin, hbin, qbin; uint8_t i;
  if (query >= APIE_NB_QUERY_BINS) query = (uint8_t)(APIE_NB_QUERY_BINS - 1U);
  s_ml.nclass[cls]++;
  bin_get((uint32_t)attempt, &abin);
  bin_get((uint32_t)has_pps, &pbin);
  bin_get((uint32_t)hard_known, &hbin);
  qbin = query;
  s_ml.ncount[cls][APIE_NB_FEAT_QUERY][qbin % 9U]++;
  s_ml.ncount[cls][APIE_NB_FEAT_ATTEMPT][abin]++;
  s_ml.ncount[cls][APIE_NB_FEAT_HASPPS][pbin]++;
  s_ml.ncount[cls][APIE_NB_FEAT_HARD][hbin]++;
  /* Logistic SGD (4 feats) - keep trained weights, do NOT overwrite bias with rate */
  {
    float target = (success != 0U) ? 1.0f : 0.0f;
    float x[4]; x[0]=(float)qbin/8.0f; x[1]=(float)abin/7.0f; x[2]=(float)pbin; x[3]=(float)hbin;
    float wtx = s_ml.logit_b;
    for (i=0;i<4;i++) wtx += s_ml.logit_w[i]*x[i];
    float pred = sigmoid_f(wtx);
    float err = target - pred;
    float lr = ML_LOGIT_LR;
    for (i=0;i<4;i++) {
      float g = err * x[i];
      s_ml.logit_w[i] += lr * g;
      s_ml.logit_w[i] = clamp_f(s_ml.logit_w[i], -ML_CLAMP, ML_CLAMP);
      if (!isfinite(s_ml.logit_w[i])) s_ml.logit_w[i]=0;
    }
    s_ml.logit_b += lr * err;
    s_ml.logit_b = clamp_f(s_ml.logit_b, -ML_CLAMP, ML_CLAMP);
    if (!isfinite(s_ml.logit_b)) s_ml.logit_b=0;
  }
  /* CRC */
  s_ml.meta.crc32 = APIE_Crc32((const uint8_t *)&s_ml, sizeof(APIE_MlModel_t));
}

/* External entry for MLP online observe: called with full fv */
void APIE_Mlp_Observe(const APIE_FeatureVec_t *fv, uint8_t success)
{
  if (fv == NULL) return;
  float in[APIE_MLP_IN]; float h1[APIE_MLP_H1]; float h2[APIE_MLP_H2];
  mlp_norm(fv, in);
  /* forward */
  for (uint8_t j=0;j<APIE_MLP_H1;j++) { float s=s_ml.mlp_b1[j]; for(uint8_t i=0;i<APIE_MLP_IN;i++) s+=in[i]*s_ml.mlp_w1[i][j]; h1[j]=relu_f(s); }
  for (uint8_t j=0;j<APIE_MLP_H2;j++) { float s=s_ml.mlp_b2[j]; for(uint8_t i=0;i<APIE_MLP_H1;i++) s+=h1[i]*s_ml.mlp_w2[i][j]; h2[j]=relu_f(s); }
  float logit = s_ml.mlp_b3; for(uint8_t i=0;i<APIE_MLP_H2;i++) logit+=h2[i]*s_ml.mlp_w3[i];
  float pred = sigmoid_f(logit);
  float target = success?1.0f:0.0f;
  float err = target - pred;
  float d_out = err * pred * (1.0f - pred); /* sig derivative */
  if (!isfinite(d_out)) d_out=0;
  /* grad w3,b3 */
  for(uint8_t i=0;i<APIE_MLP_H2;i++) {
    float g = d_out * h2[i];
    s_ml.mlp_w3[i] += ML_MLP_LR * g;
    s_ml.mlp_w3[i] = clamp_f(s_ml.mlp_w3[i], -ML_CLAMP_W1, ML_CLAMP_W1);
  }
  s_ml.mlp_b3 += ML_MLP_LR * d_out;
  s_ml.mlp_b3 = clamp_f(s_ml.mlp_b3, -ML_CLAMP, ML_CLAMP);
  /* backprop h2 */
  float d_h2[APIE_MLP_H2];
  for(uint8_t j=0;j<APIE_MLP_H2;j++) {
    float d = d_out * s_ml.mlp_w3[j];
    d_h2[j] = (h2[j] > 0.0f) ? d : 0.0f;
    if (!isfinite(d_h2[j])) d_h2[j]=0;
  }
  /* w2,b2 */
  for(uint8_t i=0;i<APIE_MLP_H1;i++) for(uint8_t j=0;j<APIE_MLP_H2;j++) {
    float g = d_h2[j] * h1[i];
    s_ml.mlp_w2[i][j] += ML_MLP_LR * g;
    s_ml.mlp_w2[i][j] = clamp_f(s_ml.mlp_w2[i][j], -ML_CLAMP_W1, ML_CLAMP_W1);
  }
  for(uint8_t j=0;j<APIE_MLP_H2;j++) { s_ml.mlp_b2[j] += ML_MLP_LR * d_h2[j]; s_ml.mlp_b2[j]=clamp_f(s_ml.mlp_b2[j],-ML_CLAMP,ML_CLAMP); }
  /* backprop h1 */
  float d_h1[APIE_MLP_H1];
  for(uint8_t i=0;i<APIE_MLP_H1;i++) { float s=0; for(uint8_t j=0;j<APIE_MLP_H2;j++) s+=d_h2[j]*s_ml.mlp_w2[i][j]; d_h1[i]=(h1[i]>0)?s:0; if(!isfinite(d_h1[i])) d_h1[i]=0; }
  for(uint8_t i=0;i<APIE_MLP_IN;i++) for(uint8_t j=0;j<APIE_MLP_H1;j++) {
    float g = d_h1[j]*in[i];
    s_ml.mlp_w1[i][j] += ML_MLP_LR * g;
    s_ml.mlp_w1[i][j] = clamp_f(s_ml.mlp_w1[i][j], -ML_CLAMP_W1, ML_CLAMP_W1);
  }
  for(uint8_t j=0;j<APIE_MLP_H1;j++) { s_ml.mlp_b1[j]+=ML_MLP_LR*d_h1[j]; s_ml.mlp_b1[j]=clamp_f(s_ml.mlp_b1[j],-ML_CLAMP,ML_CLAMP); }
  s_ml.meta.crc32 = APIE_Crc32((const uint8_t *)&s_ml, sizeof(APIE_MlModel_t));
}

void APIE_Mlp_Reset(void) { /* keep NB counts, only reset MLP */
  uint32_t sc0=s_ml.nclass[0], sc1=s_ml.nclass[1]; uint32_t nc[2][4][8]; memcpy(nc, s_ml.ncount, sizeof(nc));
  float lw[12]; memcpy(lw, s_ml.logit_w, sizeof(lw)); float lb=s_ml.logit_b;
  APIE_Ml_Init();
  s_ml.nclass[0]=sc0; s_ml.nclass[1]=sc1; memcpy(s_ml.ncount,nc,sizeof(nc)); memcpy(s_ml.logit_w,lw,sizeof(lw)); s_ml.logit_b=lb;
  s_ml.meta.crc32 = APIE_Crc32((const uint8_t *)&s_ml, sizeof(APIE_MlModel_t));
}

float APIE_Mlp_Predict(const APIE_FeatureVec_t *fv) {
  if (fv==NULL) return 0.5f;
  float in[APIE_MLP_IN]; float h1[APIE_MLP_H1]; float h2[APIE_MLP_H2];
  mlp_norm(fv,in);
  return mlp_forward_raw(in,h1,h2);
}

float APIE_Ml_PredictUseful(const APIE_FeatureVec_t *fv, uint8_t query)
{
  uint32_t i; float pc1,pc0,p1,p0; uint8_t abin,pbin,hbin,qbin;
  if (fv==NULL) return 0.5f;
  if (query>=APIE_NB_QUERY_BINS) query=(uint8_t)(APIE_NB_QUERY_BINS-1U);
  qbin=query;
  bin_get((uint32_t)(fv->v[1]), &abin);
  bin_get((uint32_t)((fv->v[8]>0.f)?1u:0u), &pbin);
  bin_get((uint32_t)((fv->v[3]>0.f && fv->v[6]>0.f)?1u:0u), &hbin);
  pc1=(float)(s_ml.nclass[1U]+1U)/(float)(s_ml.nclass[0U]+s_ml.nclass[1U]+2U);
  pc0=1.0f-pc1; p1=1.0f; p0=1.0f;
  for(i=0;i<APIE_NB_FEAT_COUNT;i++){
    uint32_t tot1,tot0,c1,c0;
    switch(i){case APIE_NB_FEAT_QUERY: tot1=s_ml.nclass[1U]+9U; tot0=s_ml.nclass[0U]+9U; c1=s_ml.ncount[1][i][qbin%9U]; c0=s_ml.ncount[0][i][qbin%9U]; break;
      case APIE_NB_FEAT_ATTEMPT: tot1=s_ml.nclass[1U]+8U; tot0=s_ml.nclass[0U]+8U; c1=s_ml.ncount[1][i][abin]; c0=s_ml.ncount[0][i][abin]; break;
      case APIE_NB_FEAT_HASPPS: tot1=s_ml.nclass[1U]+2U; tot0=s_ml.nclass[0U]+2U; c1=s_ml.ncount[1][i][pbin]; c0=s_ml.ncount[0][i][pbin]; break;
      default: tot1=s_ml.nclass[1U]+2U; tot0=s_ml.nclass[0U]+2U; c1=s_ml.ncount[1][i][hbin]; c0=s_ml.ncount[0][i][hbin]; break; }
    p1*=((float)c1+1.0f)/(float)tot1; p0*=((float)c0+1.0f)/(float)tot0;
  }
  float nb_prob=(pc1*p1)/(pc1*p1+pc0*p0);
  if(pc1*p1+pc0*p0<=0.0f) nb_prob=(pc1>=pc0)?1.0f:0.0f;
  if(!isfinite(nb_prob)) nb_prob=0.5f;
  /* logistic */
  float x[4]; x[0]=(float)qbin/8.0f; x[1]=(float)abin/7.0f; x[2]=(float)pbin; x[3]=(float)hbin;
  float wtx=s_ml.logit_b; for(i=0;i<4;i++) wtx+=s_ml.logit_w[i]*x[i];
  float logit_prob=sigmoid_f(wtx);
  /* mlp */
  float mlp_prob = APIE_Mlp_Predict(fv);
  /* ensemble: NB 40%, logit 30%, mlp 30% when data rich else favor NB */
  float total_obs=(float)(s_ml.nclass[0U]+s_ml.nclass[1U]);
  float w_nb = (total_obs < 20.0f)?0.7f:0.4f;
  float w_log = (total_obs < 20.0f)?0.2f:0.3f;
  float w_mlp = 1.0f - w_nb - w_log;
  float ens = w_nb*nb_prob + w_log*logit_prob + w_mlp*mlp_prob;
  if(!isfinite(ens)) ens=0.5f;
  return clamp_f(ens,0,1);
}

uint8_t APIE_Ml_Classify(const APIE_FeatureVec_t *fv, uint8_t query){ return (APIE_Ml_PredictUseful(fv,query)>=0.5f)?1U:0U; }

uint8_t APIE_Ml_Validate(void){
  if(s_ml.meta.id==0U || s_ml.meta.feature_version!=APIE_FEATURE_COUNT) return 0U;
  uint32_t calc = APIE_Crc32((const uint8_t *)&s_ml, sizeof(APIE_MlModel_t));
  if(calc != s_ml.meta.crc32) return 0U;
  /* check for NaN in weights */
  for(uint8_t i=0;i<APIE_FEATURE_COUNT;i++) if(!isfinite(s_ml.logit_w[i])) return 0U;
  if(!isfinite(s_ml.logit_b)) return 0U;
  return 1U;
}
uint16_t APIE_Ml_Export(uint8_t *out, uint16_t outsz){ uint32_t sz=(uint32_t)sizeof(APIE_MlModel_t); if(out==NULL||outsz<sz) return 0U; memcpy(out,&s_ml,sz); return (uint16_t)sz; }
uint8_t APIE_Ml_Import(const uint8_t *in, uint16_t len){
  if(in==NULL||len!=sizeof(APIE_MlModel_t)) return 0U;
  APIE_MlModel_t tmp; memcpy(&tmp,in,sizeof(tmp));
  /* quick sanity: feature_version must match */
  if(tmp.meta.feature_version!=APIE_FEATURE_COUNT) return 0U;
  uint32_t calc = APIE_Crc32((const uint8_t *)&tmp, sizeof(APIE_MlModel_t));
  if(calc != tmp.meta.crc32) return 0U;
  memcpy(&s_ml,&tmp,sizeof(s_ml));
  return APIE_Ml_Validate();
}

uint8_t APIE_Tree_ClassifyUseful(const APIE_FeatureVec_t *fv, uint8_t query){
  float has_pps,hard_known; if(fv==NULL) return 0U; if(query>=APIE_NB_QUERY_BINS) query=(uint8_t)(APIE_NB_QUERY_BINS-1U);
  has_pps=(fv->v[8]>0.5f)?1.0f:0.0f; hard_known=(fv->v[3]>0.0f && fv->v[6]>0.0f)?1.0f:0.0f;
  switch((APIE_QueryId_t)query){case APIE_QUERY_GET_PPS: return (has_pps>0.5f)?1U:0U; case APIE_QUERY_BATTERY: return 0U;
    case APIE_QUERY_IDENTITY: case APIE_QUERY_SVIDS: case APIE_QUERY_MODES: return hard_known>0.5f?1U:0U;
    case APIE_QUERY_GET_STATUS: case APIE_QUERY_MANU_INFO: default: return 1U; }
}
#define APIE_ANOMALY_MIN_SAMPLES 8U
static APIE_StatAccum_t s_anom; static uint8_t s_anom_init;
void APIE_Ml_Anomaly_Observe(float x){ if(s_anom_init==0U){APIE_Stats_Init(&s_anom); s_anom_init=1U;} APIE_Stats_Update(&s_anom,x); }
uint8_t APIE_Ml_Anomaly_Flag(float x,float k_sigma){ float m,s,dev; if(s_anom_init==0U||s_anom.n<APIE_ANOMALY_MIN_SAMPLES) return 0U; m=APIE_Stats_Mean(&s_anom); s=APIE_Stats_Stddev(&s_anom); dev=(x>m)?(x-m):(m-x); return (s>0.0f && dev>k_sigma*s)?1U:0U; }
float APIE_Ml_Anomaly_Mean(void){ return s_anom_init?APIE_Stats_Mean(&s_anom):0.0f; }
float APIE_Ml_Anomaly_Std(void){ return s_anom_init?APIE_Stats_Stddev(&s_anom):0.0f; }
uint32_t APIE_Ml_Anomaly_Count(void){ return s_anom.n; }
uint8_t APIE_Ml_Anomaly_Trained(void){ return (s_anom_init!=0U && s_anom.n>=APIE_ANOMALY_MIN_SAMPLES)?1U:0U; }
void APIE_Ml_Anomaly_ObserveQ(uint8_t q,float x){ if(q>=APIE_NB_QUERY_BINS) return; if(s_ml.anom_inited[q]==0U){APIE_Stats_Init(&s_ml.anom_per_q[q]); s_ml.anom_inited[q]=1U;} APIE_Stats_Update(&s_ml.anom_per_q[q],x); if(s_ml.anom_inited[q]){ s_ml.meta.crc32=APIE_Crc32((const uint8_t*)&s_ml,sizeof(s_ml)); } }
uint8_t APIE_Ml_Anomaly_FlagQ(uint8_t q,float x,float k_sigma){ float m,s,dev; if(q>=APIE_NB_QUERY_BINS) return 0U; if(s_ml.anom_inited[q]==0U || s_ml.anom_per_q[q].n < APIE_ANOMALY_MIN_SAMPLES) return 0U; m=APIE_Stats_Mean(&s_ml.anom_per_q[q]); s=APIE_Stats_Stddev(&s_ml.anom_per_q[q]); dev=(x>m)?(x-m):(m-x); return (s>0.0f && dev>k_sigma*s)?1U:0U; }
