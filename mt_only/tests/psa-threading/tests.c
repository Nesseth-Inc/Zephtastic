// SPDX-License-Identifier: GPL-3.0-only
#include "ZephyrPsaThreading.h"
#include <zephyr/kernel.h>
#include <mbedtls/threading.h>
#include <psa/crypto.h>
#include <psa/crypto_extra.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sched.h>
extern int (*host_install)(void);
extern const char *host_init_level;
extern const unsigned host_init_priority;
static unsigned long rng_calls;
psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t *ctx,
    uint8_t *out, size_t n, size_t *got) {
    (void)ctx; (void)out; (void)n; *got = 0;
    __atomic_fetch_add(&rng_calls, 1, __ATOMIC_RELAXED);
    return PSA_ERROR_INSUFFICIENT_ENTROPY;
}
static void ok(psa_status_t status) {
    if (status) fprintf(stderr, "PSA error %d\n", (int)status);
    assert(status == PSA_SUCCESS);
}
static pthread_mutex_t overlap_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t overlap_cond=PTHREAD_COND_INITIALIZER;
static unsigned overlap_count;
static _Thread_local bool first_import;
static void held_key_overlap(void) {
    if (!first_import) return;
    first_import=false;
    assert(!pthread_mutex_lock(&overlap_mutex));++overlap_count;
    if(overlap_count==6)assert(!pthread_cond_broadcast(&overlap_cond));
    while(overlap_count<6)assert(!pthread_cond_wait(&overlap_cond,&overlap_mutex));
    assert(!pthread_mutex_unlock(&overlap_mutex));
}
static psa_key_id_t key(psa_key_type_t type, psa_algorithm_t alg, psa_key_usage_t usage,
                        const uint8_t *p, size_t n) {
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, type); psa_set_key_algorithm(&a, alg); psa_set_key_usage_flags(&a, usage);
    psa_set_key_bits(&a, type == PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1) ? 256 : n * 8);
    psa_key_id_t id = 0; ok(psa_import_key(&a, p, n, &id)); assert(id); held_key_overlap(); return id;
}
static void roundtrip(unsigned worker, unsigned i) {
    uint8_t aes[32], plain[227], nonce[13] = {0}, enc[235], out[227]; size_t n, got;
    for (unsigned j=0;j<sizeof aes;++j) aes[j]=(uint8_t)(worker*17+j);
    for (unsigned j=0;j<sizeof plain;++j) plain[j]=(uint8_t)(i+j*3);
    memcpy(nonce,&i,sizeof i); nonce[4]=(uint8_t)worker; // synthetic unique domain only
    psa_algorithm_t alg=PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM,8);
    psa_key_id_t id=key(PSA_KEY_TYPE_AES,alg,PSA_KEY_USAGE_ENCRYPT|PSA_KEY_USAGE_DECRYPT,aes,32);
    ok(psa_aead_encrypt(id,alg,nonce,13,NULL,0,plain,227,enc,sizeof enc,&n));assert(n==235);
    ok(psa_aead_decrypt(id,alg,nonce,13,NULL,0,enc,n,out,sizeof out,&got));assert(got==227&&!memcmp(out,plain,227));
    enc[0]^=1; assert(psa_aead_decrypt(id,alg,nonce,13,NULL,0,enc,n,out,sizeof out,&got)==PSA_ERROR_INVALID_SIGNATURE);
    ok(psa_destroy_key(id));
}
static void cmac(void) {
    // NIST SP800-38B appendix D.1 zero-length message known-answer vector.
    const uint8_t k[16]={0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
    const uint8_t expected[16]={0xbb,0x1d,0x69,0x29,0xe9,0x59,0x37,0x28,0x7f,0xa3,0x7d,0x12,0x9b,0x75,0x67,0x46};
    uint8_t mac[16]; size_t n; psa_key_id_t id=key(PSA_KEY_TYPE_AES,PSA_ALG_CMAC,PSA_KEY_USAGE_SIGN_MESSAGE,k,16);
    ok(psa_mac_compute(id,PSA_ALG_CMAC,NULL,0,mac,sizeof mac,&n));assert(n==16&&!memcmp(mac,expected,16));ok(psa_destroy_key(id));
}
static void p256(void) {
    // Public deterministic scalar 1: neither generated nor retained credentials.
    uint8_t scalar[32]={0},pub[65],secret[32]; scalar[31]=1;size_t n,s;
    psa_key_id_t id=key(PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1),PSA_ALG_ECDH,PSA_KEY_USAGE_DERIVE,scalar,32);
    ok(psa_export_public_key(id,pub,sizeof pub,&n));assert(n==65&&pub[0]==4);
    ok(psa_raw_key_agreement(PSA_ALG_ECDH,id,pub,n,secret,sizeof secret,&s));
    assert(s==32&&!memcmp(secret,pub+1,32));ok(psa_destroy_key(id));
}
static void *crypto_worker(void *arg) {
    unsigned n=(unsigned)(uintptr_t)arg; first_import=true;
    for(unsigned i=0;i<200;++i) {ok(psa_crypto_init());if(n%3==0)roundtrip(n,i);else if(n%3==1)cmac();else p256();}
    return NULL;
}
static mbedtls_threading_mutex_t cm;
static mbedtls_threading_condition_variable_t cv;
static unsigned waiting,done;
static bool proceed;
static void *waiter(void *ignored) {
    (void)ignored;assert(!mbedtls_mutex_lock(&cm));++waiting;
    while(!proceed)assert(!mbedtls_condition_variable_wait(&cv,&cm));
    assert(cm.MBEDTLS_PRIVATE(mutex).owner==k_current_get());++done;assert(!mbedtls_mutex_unlock(&cm));return NULL;
}
static void conditions(void) {
    mbedtls_mutex_init(&cm);assert(!mbedtls_condition_variable_init(&cv));
    pthread_t t[3];for(unsigned i=0;i<3;++i)assert(!pthread_create(t+i,NULL,waiter,NULL));
    // Synchronize with each real atomic wait. This yield loop is test scheduling,
    // never a production wait implementation.
    for(;;){assert(!mbedtls_mutex_lock(&cm));if(waiting==3)break;assert(!mbedtls_mutex_unlock(&cm));sched_yield();}
    assert(!done);proceed=true;assert(!mbedtls_condition_variable_signal(&cv));
    assert(!mbedtls_condition_variable_broadcast(&cv));assert(!mbedtls_mutex_unlock(&cm));
    for(unsigned i=0;i<3;++i)assert(!pthread_join(t[i],NULL));
    assert(done==3&&shim_waits>=3);
    mbedtls_condition_variable_free(&cv);mbedtls_mutex_free(&cm);
}
static void fatal(unsigned scenario) {
    shim_panic_mode=1;
    mbedtls_threading_mutex_t m={0};mbedtls_threading_condition_variable_t c;
    if(scenario==0){host_install();return;}
    if(scenario==1){shim_fault=F_MUTEX_INIT;mbedtls_mutex_init(&m);return;}
    mbedtls_mutex_init(&m);assert(!mbedtls_condition_variable_init(&c));
    if(scenario==2){shim_isr=true;mbedtls_mutex_lock(&m);return;}
    if(scenario==3){shim_fault=F_MUTEX_LOCK;psa_crypto_init();return;} // ignored-return getter must never continue
    if(scenario==4){shim_fault=F_MUTEX_LOCK;mbedtls_mutex_lock(&m);return;}
    assert(!mbedtls_mutex_lock(&m));
    switch(scenario){
    case 5:shim_fault=F_MUTEX_UNLOCK;mbedtls_mutex_unlock(&m);break;
    case 6:shim_fault=F_COND_INIT;mbedtls_condition_variable_init(&c);break;
    case 7:shim_fault=F_COND_SIGNAL;mbedtls_condition_variable_signal(&c);break;
    case 8:shim_fault=F_COND_BROADCAST;mbedtls_condition_variable_broadcast(&c);break;
    case 9:shim_fault=F_COND_WAIT;mbedtls_condition_variable_wait(&c,&m);break;
    case 10:mbedtls_mutex_lock(&m);break; // recursive use violates library contract
    case 11:mbedtls_mutex_free(&m);break; // live owner destroy forbidden
    case 12:assert(!mbedtls_mutex_unlock(&m));mbedtls_condition_variable_wait(&c,&m);break;
    }
}
int main(void) {
    assert(!mt_psa_threading_ready());assert(host_init_priority==39&&!strcmp(host_init_level,"POST_KERNEL"));
    assert(!host_install()&&mt_psa_threading_ready());ok(psa_crypto_init());
    // Fork only while single-threaded. Returning past any fatal callback fails.
    for(unsigned i=0;i<13;++i){pid_t pid=fork();assert(pid>=0);if(!pid){fatal(i);_Exit(91);}int st;assert(waitpid(pid,&st,0)==pid);assert(WIFEXITED(st)&&WEXITSTATUS(st)==73);}
    conditions();
    pthread_t t[6];for(unsigned i=0;i<6;++i)assert(!pthread_create(t+i,NULL,crypto_worker,(void*)(uintptr_t)i));
    for(unsigned i=0;i<6;++i)assert(!pthread_join(t[i],NULL));
    // All 16 slots available after overlap and auth failures; 17th rejected.
    uint8_t k[16]={0};psa_key_id_t ids[16];for(unsigned i=0;i<16;++i)ids[i]=key(PSA_KEY_TYPE_AES,PSA_ALG_CMAC,PSA_KEY_USAGE_SIGN_MESSAGE,k,16);
    psa_key_attributes_t a=PSA_KEY_ATTRIBUTES_INIT;psa_set_key_type(&a,PSA_KEY_TYPE_AES);psa_set_key_bits(&a,128);psa_key_id_t extra=0;
    assert(psa_import_key(&a,k,16,&extra)==PSA_ERROR_INSUFFICIENT_MEMORY&&extra==0);
    for(unsigned i=0;i<16;++i)ok(psa_destroy_key(ids[i]));
    assert(!rng_calls&&shim_locks==shim_unlocks&&overlap_count==6);
    printf("PASS: 13 terminal faults; native condition signal/broadcast/relock; 1200 overlapping CCM/CMAC/P256 operations; 16-slot reclamation; %lu shared lock acquisitions; no RNG or persistent keys\n",shim_locks);
}
