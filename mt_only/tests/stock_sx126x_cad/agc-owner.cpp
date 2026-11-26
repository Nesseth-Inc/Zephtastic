#include "../../src/AgcMaintenance.h"
#include <cassert>
#include <cerrno>
#include <cstdint>
#define CONFIG_MT_NORMAL_ENDPOINT 1
#define override
static uint32_t now, lastGood;
static int queueSize, afterPause, pauseResult, calResult, restoreResult, calls, ordinary;
static int radioDevice, rxQueue;
static bool completed;
static mt::AgcMaintenance agcSchedule;
static uint32_t &agcLastGood=lastGood;
static uint32_t k_uptime_get_32(){return now;}
static uint32_t atomic_get(uint32_t *x){return *x;}
static void receive(){}
static int k_msgq_num_used_get(int*){return queueSize;}
static int sx126x_pause_for_storage(int){queueSize=afterPause;return pauseResult;}
static int sx126x_recv_async_packet(int,void(*)(),void*){++ordinary;return restoreResult;}
static int sx126x_recv_after_agc(int,void(*)(),void*,bool *done){++calls;*done=completed;return calResult;}
static int storage_resume_outcome(int result){return result?result:-EAGAIN;}
struct Owner {
    bool joinedForAgc_=false;
    uint32_t storageCalls_=0;
    bool ready() const{return true;}
    bool receiveEventPending() const{return queueSize!=0;}
#include "agc-owner.inc"
};
static void init(){
    now=60000;lastGood=0;queueSize=afterPause=pauseResult=calResult=restoreResult=calls=ordinary=0;
    completed=true;agcSchedule=mt::AgcMaintenance{};agcSchedule.poll(now,lastGood,true);
}
int main(){
    init();Owner r;
    assert(r.restore()==0 && ordinary==1 && !calls);
    queueSize=1;assert(r.stopForStorage()==-EAGAIN && !r.joinedForAgc_);
    queueSize=0;afterPause=1;
    assert(r.stopForStorage()==-EAGAIN && ordinary==2 && !calls && !r.joinedForAgc_);
    queueSize=afterPause=0;pauseResult=-EAGAIN;
    assert(r.stopForStorage()==-EAGAIN && !r.joinedForAgc_);
    pauseResult=0;assert(!r.stopForStorage() && r.joinedForAgc_);
    assert(!r.restore() && calls==1 && !r.joinedForAgc_);
    assert(!r.restore() && calls==1 && ordinary==3);
    now=120000;lastGood=119999;agcSchedule.poll(now,lastGood,true);
    assert(!r.stopForStorage() && !r.restore() && calls==1 && ordinary==4);
    now=180000;agcSchedule.poll(now,lastGood,true);assert(!r.stopForStorage());
    lastGood=now;assert(!r.restore() && calls==1 && ordinary==5);
    /* TX/queued work at the donor tick cannot authorize a maintenance arm. */
    now=240000;agcSchedule.poll(now,0,false);assert(!r.stopForStorage()&&!r.restore()&&calls==1);
    now=300000;agcSchedule.poll(now,0,true);assert(!r.stopForStorage());
    calResult=-EIO;assert(r.restore()==-EIO && calls==2 && !r.joinedForAgc_);
    now=360000;agcSchedule.poll(now,0,true);assert(!r.stopForStorage());
    calResult=0;completed=false;assert(!r.restore()&&calls==3);
}
