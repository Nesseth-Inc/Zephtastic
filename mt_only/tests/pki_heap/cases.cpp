#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "PsaPkiBackend.h"
#include <mbedtls/private/aes.h>
#include <mbedtls/private/cmac.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
static unsigned calls,frees,failures;static bool deny;static unsigned failAt;static unsigned live,peak;static void *blocks[8];static size_t sizes[8],liveBytes,peakBytes,allocation;
extern "C" void *zt_probe_calloc(size_t n,size_t s){++calls;allocation=n*s;if(deny||calls==failAt){++failures;return nullptr;}void *p=std::calloc(n,s);assert(p);unsigned i=0;while(i<8&&blocks[i])++i;assert(i<8);blocks[i]=p;sizes[i]=n*s;++live;liveBytes+=n*s;if(live>peak)peak=live;if(liveBytes>peakBytes)peakBytes=liveBytes;return p;}
extern "C" void zt_probe_free(void *p){if(p){unsigned i=0;while(i<8&&blocks[i]!=p)++i;assert(i<8);blocks[i]=nullptr;liveBytes-=sizes[i];--live;++frees;}std::free(p);}
extern "C" psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t *,uint8_t *,size_t,size_t *n){*n=0;return PSA_ERROR_INSUFFICIENT_ENTROPY;}
int main(){
 mt::pki::PsaPkiBackend backend;assert(backend.begin());
 uint8_t key[32]{},nonce[13]{},plain[227]{},cipher[227],tag[8],output[227];
 for(unsigned i=0;i<sizeof(plain);++i)plain[i]=uint8_t(i); // deterministic public synthetic fixture only
 memset(cipher,0x5a,sizeof(cipher));memset(tag,0x5a,sizeof(tag));
 assert(backend.ccm(true,key,nonce,plain,sizeof(plain),cipher,tag));printf("first calls=%u frees=%u live=%u allocation=%zu expected=%zu\n",calls,frees,live,allocation,sizeof(mbedtls_aes_context));fflush(stdout);assert(calls==1&&frees==1&&!live&&allocation==sizeof(mbedtls_aes_context));
 assert(backend.ccm(false,key,nonce,cipher,sizeof(cipher),output,tag));assert(calls==2&&frees==2&&!live&&!memcmp(plain,output,sizeof(plain)));
 deny=true;
 for(unsigned i=0;i<512;++i){
  uint8_t failed[227],failedTag[8];memset(failed,0xa5,sizeof(failed));memset(failedTag,0x5a,sizeof(failedTag));
  assert(!backend.ccm(true,key,nonce,plain,sizeof(plain),failed,failedTag));
  for(auto x:failed)assert(x==0xa5);for(auto x:failedTag)assert(x==0x5a);
  assert(backend.healthy()&&!backend.hasQuarantinedKey()&&!live);
 }
 memset(output,0xa5,sizeof(output));assert(!backend.ccm(false,key,nonce,cipher,sizeof(cipher),output,tag));for(auto x:output)assert(x==0xa5);
 assert(failures==513&&calls==515&&frees==2&&!live);
 deny=false;assert(backend.ccm(false,key,nonce,cipher,sizeof(cipher),output,tag)&&!memcmp(output,plain,sizeof(plain)));assert(calls==516&&frees==3&&!live);
 uint8_t digest[32];unsigned before=calls;assert(backend.sha256(plain,227,digest));assert(backend.hmac256(key,plain,227,digest));assert(calls==before);
 psa_key_attributes_t a=PSA_KEY_ATTRIBUTES_INIT;psa_set_key_type(&a,PSA_KEY_TYPE_AES);psa_set_key_bits(&a,128);psa_set_key_usage_flags(&a,PSA_KEY_USAGE_SIGN_MESSAGE);psa_set_key_algorithm(&a,PSA_ALG_CMAC);
 psa_key_id_t k;assert(psa_import_key(&a,key,16,&k)==PSA_SUCCESS);
 psa_mac_operation_t op=PSA_MAC_OPERATION_INIT;before=calls;assert(psa_mac_sign_setup(&op,k,PSA_ALG_CMAC)==PSA_SUCCESS);assert(calls==before+2&&live==2&&liveBytes==sizeof(mbedtls_aes_context)+sizeof(mbedtls_cmac_context_t));
 assert(backend.ccm(true,key,nonce,plain,sizeof(plain),cipher,tag));assert(live==2&&peak==3&&peakBytes==2*sizeof(mbedtls_aes_context)+sizeof(mbedtls_cmac_context_t));
 size_t macn;assert(psa_mac_update(&op,plain,sizeof(plain))==PSA_SUCCESS);assert(psa_mac_sign_finish(&op,digest,sizeof(digest),&macn)==PSA_SUCCESS&&macn==16);assert(!live&&!liveBytes);
 op=psa_mac_operation_t PSA_MAC_OPERATION_INIT;failAt=calls+2;assert(psa_mac_sign_setup(&op,k,PSA_ALG_CMAC)==PSA_ERROR_INSUFFICIENT_MEMORY);assert(psa_mac_abort(&op)==PSA_SUCCESS&&!live&&!liveBytes);failAt=0;assert(psa_destroy_key(k)==PSA_SUCCESS);
 printf("PASS overlapping synthetic BLE CMAC+private CCM: peak3 allocations, host peak%zuB (AES%zu CMAC%zu); CMAC second allocation failure frees first\n",peakBytes,sizeof(mbedtls_aes_context),sizeof(mbedtls_cmac_context_t));assert(backend.close());
 printf("PASS AES wrapper: host context=%zu; one allocation/free per CCM; 513 allocation failures, unchanged output/tag, no quarantined key, later recovery; SHA/HMAC zero cipher-wrapper allocations\n",sizeof(mbedtls_aes_context));
}
