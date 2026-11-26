// SPDX-License-Identifier: GPL-3.0-only
// Actual ALT C implementation, independent deterministic kernel/PSA call doubles.
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include MT_REVIEW_THREADING_SOURCE
static jmp_buf trap;
static unsigned expecting_panic,panics,set_calls,kernel_calls,isr;
static int failure,lock_timeout,wait_timeout;
static unsigned thread_token,bad_reacquire;
static int (*mi)(mbedtls_platform_mutex_t *),(*ml)(mbedtls_platform_mutex_t *),(*mu)(mbedtls_platform_mutex_t *);
static void (*md)(mbedtls_platform_mutex_t *);
static int (*ci)(mbedtls_platform_condition_variable_t *),(*cs)(mbedtls_platform_condition_variable_t *),(*cb)(mbedtls_platform_condition_variable_t *);
static void (*cd)(mbedtls_platform_condition_variable_t *);
static int (*cw)(mbedtls_platform_condition_variable_t *,mbedtls_platform_mutex_t *);
void k_panic(void){assert(expecting_panic);++panics;longjmp(trap,1);}
bool k_is_in_isr(void){return isr;}
void *k_current_get(void){return &thread_token;}
int k_mutex_init(struct k_mutex *p){++kernel_calls;p->owner=NULL;p->lock_count=0;return failure;}
int k_mutex_lock(struct k_mutex *p,int t){++kernel_calls;lock_timeout=t;if(!failure){p->owner=k_current_get();++p->lock_count;}return failure;}
int k_mutex_unlock(struct k_mutex *p){++kernel_calls;if(!failure){assert(p->owner==k_current_get()&&p->lock_count);if(!--p->lock_count)p->owner=NULL;}return failure;}
int k_condvar_init(struct k_condvar *p){(void)p;++kernel_calls;return failure;}
int k_condvar_signal(struct k_condvar *p){(void)p;++kernel_calls;return failure;}
int k_condvar_broadcast(struct k_condvar *p){(void)p;++kernel_calls;return failure;}
int k_condvar_wait(struct k_condvar *p,struct k_mutex *m,int t){(void)p;(void)m;++kernel_calls;wait_timeout=t;if(bad_reacquire)m->owner=NULL;return failure;}
void mbedtls_threading_set_alt(int (*a)(mbedtls_platform_mutex_t *),void (*b)(mbedtls_platform_mutex_t *),int (*c)(mbedtls_platform_mutex_t *),int (*d)(mbedtls_platform_mutex_t *),int (*e)(mbedtls_platform_condition_variable_t *),void (*f)(mbedtls_platform_condition_variable_t *),int (*g)(mbedtls_platform_condition_variable_t *),int (*h)(mbedtls_platform_condition_variable_t *),int (*i)(mbedtls_platform_condition_variable_t *,mbedtls_platform_mutex_t *)){
 ++set_calls;assert(!mt_psa_threading_ready());mi=a;md=b;ml=c;mu=d;ci=e;cd=f;cs=g;cb=h;cw=i;
}
#define PANICS(call) do{unsigned old=panics;expecting_panic=1;if(!setjmp(trap)){(void)(call);assert(!"unsafe continuation after primitive failure");}expecting_panic=0;assert(panics==old+1);}while(0)
int main(void){
 assert(!mt_psa_threading_ready()&&!kernel_calls&&!set_calls);assert(review_init_priority==39&&review_init_function==mt_psa_threading_install);
 assert(!mt_psa_threading_install()&&mt_psa_threading_ready()&&set_calls==1);
 assert(mi&&md&&ml&&mu&&ci&&cd&&cs&&cb&&cw);
 PANICS(mt_psa_threading_install());assert(mt_psa_threading_ready()&&set_calls==1);
 puts("PASS one-ordered-install-only-readiness-published-after-all-callbacks");
 struct k_mutex m={0};struct k_condvar c={0};
 assert(!mi(&m)&&!ml(&m)&&lock_timeout==1000&&!mu(&m));md(&m);
 assert(!ci(&c)&&!cs(&c)&&!cb(&c)&&!ml(&m)&&!cw(&c,&m)&&wait_timeout==K_FOREVER&&!mu(&m));cd(&c);
 failure=3;assert(!cb(&c));failure=0;
 puts("PASS native-timeout-arguments-condvar-contract-and-positive-broadcast-count");
 failure=-5;PANICS(mi(&m));PANICS(ml(&m));PANICS(mu(&m));PANICS(ci(&c));PANICS(cs(&c));PANICS(cb(&c));failure=0;assert(!ml(&m));failure=-5;PANICS(cw(&c,&m));failure=0;assert(!mu(&m));
 m.owner=&m;PANICS(md(&m));m.owner=NULL;m.lock_count=1;PANICS(md(&m));m.lock_count=0;
 puts("PASS all-error-returning-primitives-stop-before-unlocked-PSA-continuation");
 assert(!ml(&m));PANICS(ml(&m));m.lock_count=1;bad_reacquire=1;PANICS(cw(&c,&m));bad_reacquire=0;m.owner=k_current_get();assert(!mu(&m));PANICS(cw(&c,&m));
 puts("PASS recursive-lock-and-invalid-condition-ownership-never-continue");
 const unsigned old=kernel_calls;
 PANICS(mi(NULL));PANICS(md(NULL));PANICS(ml(NULL));PANICS(mu(NULL));PANICS(ci(NULL));PANICS(cd(NULL));PANICS(cs(NULL));PANICS(cb(NULL));PANICS(cw(NULL,&m));PANICS(cw(&c,NULL));
 isr=1;PANICS(mi(&m));PANICS(md(&m));PANICS(ml(&m));PANICS(mu(&m));PANICS(ci(&c));PANICS(cd(&c));PANICS(cs(&c));PANICS(cb(&c));PANICS(cw(&c,&m));isr=0;
 assert(kernel_calls==old&&mt_psa_threading_ready());
 puts("PASS null-and-interrupt-context-never-reach-kernel-primitives");
}
