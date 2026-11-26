/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SX126X_CONTINUOUS_GATE_H
#define SX126X_CONTINUOUS_GATE_H
#include "sx126x_bench_gate.h"

/* Optional strict trailing-window reservations, not a replenishing burst bucket.
 * Minimum charge is 252 ms: 256 slots cover every possible 60 s window.
 * Restarting a session never clears this ledger. All callers hold the lock. */
#define CD_WINDOW_MS 600000U
#define CD_SHARED_MS 60000U
#define CD_NETWORK_MS 30000U
#define CD_MAX_AIR_MS 3749U
#if defined(CONFIG_ZEPHCORE_UNLIMITED_DUAL_AIRTIME)
/* Diagnostic totals only: no window, slot cap, per-network quota or shared quota.
 * Saturate each total at half UINT32_MAX so the existing shared sum cannot wrap.
 * Native one-shot grants, timeout bounds and the single TX owner stay enforced. */
struct cd_ledger { uint32_t used[2]; };
static inline void cd_expire(struct cd_ledger *l,uint32_t now) {(void)l;(void)now;}
static inline bool cd_allows(struct cd_ledger *l,uint8_t net,uint32_t now,uint32_t ms) {
 (void)l;(void)now;
 return net<2U && ms>251U && ms<=4000U;
}
static inline bool cd_charge(struct cd_ledger *l,uint8_t net,uint32_t now,uint32_t ms) {
 if(!cd_allows(l,net,now,ms))return false;
 const uint32_t cap=UINT32_MAX/2U;
 l->used[net]=l->used[net]>cap-ms ? cap : l->used[net]+ms;
 return true;
}
#else
struct cd_charge { uint32_t at, ms; uint8_t network; };
struct cd_ledger { struct cd_charge entries[256]; uint16_t head, count; uint32_t used[2]; };
static inline void cd_expire(struct cd_ledger *l, uint32_t now) {
 while(l->count && (uint32_t)(now-l->entries[l->head].at)>=CD_WINDOW_MS) {
  struct cd_charge *c=&l->entries[l->head]; l->used[c->network]-=c->ms;
  l->head=(l->head+1U)%256U; --l->count;
 }
}
static inline bool cd_allows(struct cd_ledger *l,uint8_t net,uint32_t now,uint32_t ms) {
 cd_expire(l,now);
 return net<2U && ms>251U && ms<=4000U && l->count<256U &&
  l->used[net]+ms<=CD_NETWORK_MS && l->used[0]+l->used[1]+ms<=CD_SHARED_MS;
}
static inline bool cd_charge(struct cd_ledger *l,uint8_t net,uint32_t now,uint32_t ms) {
 if(!cd_allows(l,net,now,ms)) return false;
 uint16_t at=(l->head+l->count)%256U;
 l->entries[at]=(struct cd_charge){now,ms,net};++l->count;l->used[net]+=ms;return true;
}
#endif
static inline bool cd_mc_frame(const uint8_t *p,size_t n) {
 if(!p || n<3U || n>255U || (p[0]>>6)!=0U) return false;
 size_t offset=(p[0]&3U)==0U || (p[0]&3U)==3U ? 5U : 1U;
 if(offset>=n) return false;
 uint8_t path=p[offset++],width=1U+(path>>6);
 size_t bytes=(path&63U)*width;
 return width<=3U && bytes<=64U && offset+bytes<n && n-offset-bytes<=184U;
}
static inline bool cd_mt_frame(const uint8_t *p,size_t n,uint8_t relay) {
 if(!p || n<17U || n>255U) return false;
 uint32_t src=bg_le32(p+4);
 uint8_t hop=p[12]&7U,start=p[12]>>5;
#if defined(CONFIG_ZEPHCORE_MT_ROUTING)
 uint32_t dest=bg_le32(p);
 /* Admission still binds exact bytes, owner, device, modem and timer.
  * The transit core owns channel/destination/next-hop eligibility; this
  * boundary validates legal outgoing transit framing and our relay stamp.
  */
 return dest!=0U && dest!=0xfffffffeU && src!=0U && src!=0xffffffffU &&
  bg_le32(p+8)!=0U && (!start || hop<start) && p[15]==relay &&
  (dest!=0xffffffffU || !(p[12]&8U));
#else
 return bg_le32(p)==0xffffffffU && src!=0U && src!=0xffffffffU &&
  bg_le32(p+8)!=0U && p[13]==8U && (!start || hop<start) &&
  !(p[12]&8U) && p[14]==0U && p[15]==relay;
#endif
}
struct sx126x_dual_gate {
 struct { uint8_t stage; } mt;
 bool active,in_flight,admitted;
 uint8_t network,stage,relay,identity[3],network_mask;
 uint16_t len;
 uint32_t expires,timeout_ms,deadline;
 uintptr_t owner;
 uint8_t frame[255];
 struct cd_ledger ledger;
};
static inline void dg_revoke(struct sx126x_dual_gate *g) {g->active=false;g->stage=g->mt.stage=0U;}
static inline bool dg_arm(struct sx126x_dual_gate *g,uint8_t relay,const uint8_t identity[3],uintptr_t owner,uint32_t now,uint32_t deadline) {
 if(!relay || !identity || !owner || (deadline && (bg_due(now,deadline) || deadline-now>60000U)) || g->active || g->in_flight || g->admitted || (g->owner && g->owner!=owner)) return false;
 cd_expire(&g->ledger,now);g->owner=owner;g->relay=relay;g->deadline=deadline;memcpy(g->identity,identity,3U);g->network_mask=3;g->active=true;return true;
}
static inline bool dg_issue(struct sx126x_dual_gate *g,uint8_t net,const uint8_t *p,size_t n,uintptr_t owner,uint32_t now,uint32_t air) {
 if(!g->active || net>1U || !(g->network_mask&(1U<<net)) || g->in_flight || g->admitted || g->stage || g->owner!=owner || !air || air>CD_MAX_AIR_MS ||
  (g->deadline && (bg_due(now,g->deadline) || g->deadline-now<air+751U)) ||
  !cd_allows(&g->ledger,net,now,air+251U) || !(net==0U ? cd_mc_frame(p,n) : net==1U && cd_mt_frame(p,n,g->relay))) return false;
 g->network=net;g->len=n;memcpy(g->frame,p,n);g->timeout_ms=air+251U;g->expires=now+250U;g->stage=1U;return true;
}
static inline bool dg_admit(struct sx126x_dual_gate *g,uint8_t net,const uint8_t *p,size_t n,uintptr_t owner,uint32_t now) {
 bool ok=g->active && !g->in_flight && g->stage==1U && g->network==net && g->owner==owner &&
  !bg_due(now,g->expires) && (!g->deadline || (!bg_due(now,g->deadline) && g->deadline-now>=g->timeout_ms+500U)) &&
  p && n==g->len && !memcmp(p,g->frame,n) && cd_charge(&g->ledger,net,now,g->timeout_ms);
 g->stage=ok ? 2U : 0U;g->admitted=ok;return ok;
}
static inline bool dg_ready(const struct sx126x_dual_gate *g,uint8_t net,uintptr_t owner,uint32_t now) {
 return g->active && !g->in_flight && g->stage==2U && g->network==net && g->owner==owner && !bg_due(now,g->expires) &&
  (!g->deadline || (!bg_due(now,g->deadline) && g->deadline-now>=g->timeout_ms+100U));
}
static inline bool dg_consume(struct sx126x_dual_gate *g,uint8_t net,uintptr_t owner,uint32_t now,const uint8_t *p,size_t n) {
 bool ok=dg_ready(g,net,owner,now) && p && n==3U;
 uint32_t ticks=ok ? ((uint32_t)p[0]<<16 | (uint32_t)p[1]<<8 | p[2]) : 0U;
 ok=ok && ticks==g->timeout_ms*64U && ticks>0U && ticks<=4000U*64U;
 g->stage=g->mt.stage=0U;if(ok)g->in_flight=true;return ok;
}
static inline bool dg_complete(struct sx126x_dual_gate *g,uint8_t net,bool success) {
 (void)success;
 if(g->network!=net || !g->admitted)return false;
 g->admitted=false;g->in_flight=false;g->stage=g->mt.stage=0U;return true;
}
#endif
