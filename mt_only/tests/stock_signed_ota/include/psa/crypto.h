#pragma once
#include <openssl/sha.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>
#include <cstring>
using psa_hash_operation_t=SHA256_CTX;
using psa_status_t=int;
using mbedtls_svc_key_id_t=EC_KEY*;
using psa_key_attributes_t=int;
#define PSA_KEY_ATTRIBUTES_INIT 0
#define PSA_HASH_OPERATION_INIT {}
#define PSA_SUCCESS 0
#define PSA_ALG_SHA_256 1
#define PSA_ALG_ECDSA(x) 2
#define PSA_KEY_TYPE_ECC_PUBLIC_KEY(x) 3
#define PSA_ECC_FAMILY_SECP_R1 4
#define PSA_KEY_USAGE_VERIFY_HASH 5
inline void psa_set_key_type(int*,int){}
inline void psa_set_key_bits(int*,int){}
inline void psa_set_key_usage_flags(int*,int){}
inline void psa_set_key_algorithm(int*,int){}
inline void psa_reset_key_attributes(int*){}
inline int psa_hash_setup(SHA256_CTX *h,int){return SHA256_Init(h)==1?0:-1;}
inline int psa_hash_update(SHA256_CTX *h,const void *p,size_t n){return SHA256_Update(h,p,n)==1?0:-1;}
inline int psa_hash_finish(SHA256_CTX *h,uint8_t *p,size_t n,size_t *len){*len=32;return n>=32&&SHA256_Final(p,h)==1?0:-1;}
inline void psa_hash_abort(SHA256_CTX*){}
inline int psa_import_key(int*,const uint8_t *p,size_t n,EC_KEY **out){
    *out=EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    return *out&&o2i_ECPublicKey(out,&p,n)?0:-1;
}
inline int psa_verify_hash(EC_KEY *key,int,const uint8_t *hash,size_t n,const uint8_t *raw,size_t bytes){
    if(bytes!=64)return -1;
    ECDSA_SIG *sig=ECDSA_SIG_new();
    ECDSA_SIG_set0(sig,BN_bin2bn(raw,32,nullptr),BN_bin2bn(raw+32,32,nullptr));
    int ok=ECDSA_do_verify(hash,int(n),sig,key);ECDSA_SIG_free(sig);return ok==1?0:-1;
}
inline void psa_destroy_key(EC_KEY*key){EC_KEY_free(key);}
