// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
struct device {};
struct k_sem {int count;};
struct k_mutex {bool held=false;};
struct Rng {uint32_t CONFIG=1;};
struct Fake {
  uint32_t tick=0,cycle=0,step=1000,flags=99,cpu=240000000,apb=80000000;
  unsigned reads=0,cycles=0,sleeps=0,locks=0,unlocks=0,irq=0,word=1;
  int chunk=4,error=0,status=1,phy=2;
  bool deviceReady=true,compatible=true,isr=false,lockBusy=false,unlockFail=false,stallTicks=false,loseAfterRead=false;
};
inline Fake fake;
inline device fakeDevice;
inline k_mutex fakeMutex;
inline Rng fakeRng;
#define NRF_RNG (&fakeRng)
#define RNG_CONFIG_DERCEN_Msk 1u
#define K_NO_WAIT 0
#define K_SEM_DEFINE(name,count,max) static k_sem name{count}
#define DT_CHOSEN(x) 0
#define DT_NODE_HAS_COMPAT(node,compat) (fake.compatible)
#define DEVICE_DT_GET_OR_NULL(x) (&fakeDevice)
inline bool k_is_in_isr(){return fake.isr;}
inline int k_sem_take(k_sem *s,int wait){assert(wait==K_NO_WAIT);if(!s->count)return -EAGAIN;--s->count;return 0;}
inline void k_sem_give(k_sem *s){++s->count;assert(s->count==1);}
inline int k_mutex_lock(k_mutex *m,int wait){assert(wait==K_NO_WAIT);++fake.locks;if(fake.lockBusy||m->held)return -EBUSY;m->held=true;return 0;}
inline int k_mutex_unlock(k_mutex *m){++fake.unlocks;assert(m->held);if(fake.unlockFail)return -EIO;m->held=false;return 0;}
inline int64_t k_uptime_ticks(){return fake.tick;}
inline uint64_t k_ticks_to_us_floor64(uint64_t tick){return tick;}
inline void k_usleep(uint32_t us){assert(us==250&&!fake.irq);++fake.sleeps;if(!fake.stallTicks)fake.tick+=us;}
inline bool device_is_ready(const device *){return fake.deviceReady;}
inline int entropy_get_entropy_isr(const device *,uint8_t *out,uint16_t n,uint32_t flags){
  ++fake.reads;fake.flags=flags;assert(!flags&&n<=32);if(fake.error)return fake.error;
  unsigned count=unsigned(fake.chunk)<n?unsigned(fake.chunk):n;memset(out,0x71,count);
  if(fake.loseAfterRead)fake.deviceReady=false;
  return int(count);
}
inline k_mutex *phy_get_lock(){return &fakeMutex;}
inline uint32_t esp_clk_cpu_freq(){return fake.cpu;}
inline uint32_t esp_clk_apb_freq(){return fake.apb;}
inline int esp_bt_controller_get_status(){return fake.status;}
inline int phy_get_modem_flag(){return fake.phy;}
#define ESP_BT_CONTROLLER_STATUS_ENABLED 1
#define PHY_MODEM_BT 2
inline unsigned irq_lock(){assert(!fake.irq);fake.irq=1;return 0;}
inline void irq_unlock(unsigned key){assert(!key&&fake.irq);fake.irq=0;}
inline uint32_t esp_cpu_get_cycle_count(){assert(fake.irq);++fake.cycles;fake.cycle+=fake.step;return fake.cycle;}
inline uint32_t rng_ll_read_data(){assert(fake.irq&&fakeMutex.held&&fake.phy==2);++fake.reads;if(fake.loseAfterRead)fake.phy=0;return fake.word++;}
