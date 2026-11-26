/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
/* Pure finite admission policy shared by the native driver and host fixtures.
 * Its caller holds the native spinlock. No allocation, I/O or persistence. */
struct sx126x_bench_gate {
 bool used,active;
 uint8_t attempts,relay,stage;
 uint16_t len;
 uint32_t deadline,packet_id,expires,timeout_ms,budget_ms;
 uintptr_t owner;
 uint8_t frame[255];
};
static inline uint32_t bg_le32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static inline bool bg_due(uint32_t now,uint32_t at) { return (int32_t)(now-at)>=0; }
static inline void bg_revoke(struct sx126x_bench_gate *g) { g->active=false;g->stage=0;g->len=0; }
static inline bool bg_arm(struct sx126x_bench_gate *g,uint8_t relay,uintptr_t owner,uint32_t now,uint32_t deadline) {
 if(g->used || !owner || !relay || bg_due(now,deadline) || (uint32_t)(deadline-now)>90000) return false;
 g->used=g->active=true;g->relay=relay;g->owner=owner;g->deadline=deadline;return true;
}
static inline bool bg_issue(struct sx126x_bench_gate *g,const uint8_t *p,size_t n,uintptr_t owner,uint32_t now,uint32_t air_ms) {
 if(!g->active || g->stage || g->owner!=owner || g->attempts>=3 || !p || n<17 || n>255 || !air_ms || air_ms>1749 ||
    g->budget_ms+air_ms+251>6000 || bg_due(now,g->deadline) || g->deadline-now<air_ms+751) return false;
 uint8_t hop=p[12]&7,start=p[12]>>5;
 if(bg_le32(p)!=0xffffffffu || bg_le32(p+4)!=0x84cb320bu || !bg_le32(p+8) || p[13]!=8 ||
    !start || hop>=start || (p[12]&0x18) || p[14] || p[15]!=g->relay ||
    (g->packet_id && g->packet_id!=bg_le32(p+8))) return false;
 g->packet_id=bg_le32(p+8);g->len=n;memcpy(g->frame,p,n);g->timeout_ms=air_ms+251;
 g->expires=now+250; /* Admission + four-symbol CAD must complete promptly. */
 g->stage=1;return true;
}
static inline bool bg_admit(struct sx126x_bench_gate *g,const uint8_t *p,size_t n,uintptr_t owner,uint32_t now) {
 if(!g->active || g->stage!=1 || g->owner!=owner || bg_due(now,g->expires) || bg_due(now,g->deadline) ||
    !p || n!=g->len || memcmp(p,g->frame,n)) { g->stage=0;return false; }
 ++g->attempts;g->budget_ms+=g->timeout_ms;g->stage=2;return true;
}
static inline bool bg_ready(const struct sx126x_bench_gate *g,uintptr_t owner,uint32_t now) {
 return g->active && g->stage==2 && g->owner==owner && !bg_due(now,g->expires) && !bg_due(now,g->deadline);
}
static inline bool bg_consume(struct sx126x_bench_gate *g,uintptr_t owner,uint32_t now,const uint8_t *p,size_t n) {
 bool ok=bg_ready(g,owner,now) && p && n==3;
 uint32_t ticks=ok ? ((uint32_t)p[0]<<16|(uint32_t)p[1]<<8|p[2]) : 0;
 ok=ok && ticks==g->timeout_ms*64 && ticks>0 && ticks<=2000*64;
 g->stage=0;g->len=0;return ok;
}
