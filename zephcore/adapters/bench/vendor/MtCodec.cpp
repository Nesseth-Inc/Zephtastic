// SPDX-License-Identifier: GPL-3.0-only
#include "MtCodec.h"
#if defined(CONFIG_MT_GPIO_MODULES)
#include <meshtastic/remote_hardware.pb.h>
#include <pb_decode.h>
#include <pb_encode.h>
#endif
#if defined(__ZEPHYR__)
#include <psa/crypto.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonCryptor.h>
#elif defined(MT_HOST_OPENSSL)
#include <openssl/evp.h>
#endif

namespace bench {
namespace {
void erase(void *ptr,size_t n) {
  volatile uint8_t *p=static_cast<volatile uint8_t *>(ptr);
  while(n--) *p++=0;
}
}
bool mtAesCtr(void *,const uint8_t *key,size_t keyLen,const uint8_t nonce[16],
              uint8_t *bytes,size_t len) {
  if(!key||!nonce||!bytes||(keyLen!=16&&keyLen!=32)||len>239) return false;
  if(!len) return true;
  // AES is provided by the existing system library. CTR uses Meshtastic's
  // 128-bit nonce and a big-endian low 32-bit block counter. At most 15 blocks
  // are processed, so this never carries into the source/packet ID bytes.
  uint8_t counters[240]{},stream[240]{},counter[16];
  memcpy(counter,nonce,16);
  const size_t blocks=(len+15)/16, padded=blocks*16;
  for(size_t i=0;i<blocks;++i) {
    memcpy(counters+i*16,counter,16);
    for(unsigned b=16;b>12;) {if(++counter[--b]) break;}
  }
  bool ok=false;
#if defined(__ZEPHYR__)
  psa_key_attributes_t attr=PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attr,PSA_KEY_TYPE_AES);
  psa_set_key_bits(&attr,keyLen*8);
  psa_set_key_usage_flags(&attr,PSA_KEY_USAGE_ENCRYPT);
  psa_set_key_algorithm(&attr,PSA_ALG_ECB_NO_PADDING);
  psa_key_id_t keyId=0; size_t result=0;
  if(psa_crypto_init()==PSA_SUCCESS && psa_import_key(&attr,key,keyLen,&keyId)==PSA_SUCCESS) {
    ok=psa_cipher_encrypt(keyId,PSA_ALG_ECB_NO_PADDING,counters,padded,
                          stream,sizeof(stream),&result)==PSA_SUCCESS&&result==padded;
    if(psa_destroy_key(keyId)!=PSA_SUCCESS) ok=false;
  }
  psa_reset_key_attributes(&attr);
#elif defined(__APPLE__)
  size_t result=0;
  ok=CCCrypt(kCCEncrypt,kCCAlgorithmAES,kCCOptionECBMode,key,keyLen,nullptr,
              counters,padded,stream,sizeof(stream),&result)==kCCSuccess&&result==padded;
#elif defined(MT_HOST_OPENSSL)
  // Native host-test adapter only. Firmware continues to use the PSA branch.
  EVP_CIPHER_CTX *ctx=EVP_CIPHER_CTX_new();int produced=0,final=0;
  const EVP_CIPHER *cipher=keyLen==16?EVP_aes_128_ecb():EVP_aes_256_ecb();
  if(ctx&&EVP_EncryptInit_ex(ctx,cipher,nullptr,key,nullptr)==1&&
     EVP_CIPHER_CTX_set_padding(ctx,0)==1&&
     EVP_EncryptUpdate(ctx,stream,&produced,counters,int(padded))==1&&
     EVP_EncryptFinal_ex(ctx,stream+produced,&final)==1)
    ok=size_t(produced+final)==padded;
  EVP_CIPHER_CTX_free(ctx);
#else
  // A host outside macOS must inject a tested backend, never a cipher stub.
  (void)padded;
#endif
  if(ok) for(size_t i=0;i<len;++i) bytes[i]^=stream[i];
  erase(counters,sizeof(counters)); erase(stream,sizeof(stream)); erase(counter,sizeof(counter));
  return ok;
}
#if defined(CONFIG_MT_GPIO_MODULES)
bool MtCodec::encodeGpioRequest(uint32_t ours,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
                                uint8_t *out,size_t cap,size_t &len) const {
  len=0;RouteMetadata metadata{};
  if(!configured()||!out||!data||n>239||cap<n+16||ours<=1||ours>=0xfffffffeu||
     to<=1||to>=0xfffffffeu||!id||!decodeData(data,n,metadata)||metadata.portnum!=2||!metadata.hasBitfield||
     metadata.bitfield>3||metadata.wantResponse!=bool(metadata.bitfield&2)||metadata.request_id||
     metadata.reply_id||metadata.emoji||metadata.signatureLength||metadata.payloadLength>24)return false;
  meshtastic_HardwareMessage message{};auto input=pb_istream_from_buffer(metadata.payload,metadata.payloadLength);
  if(!metadata.payloadLength||!pb_decode(&input,meshtastic_HardwareMessage_fields,&message)||input.bytes_left)return false;
  uint8_t canonical[24]{};auto writer=pb_ostream_from_buffer(canonical,sizeof(canonical));
  if(!pb_encode(&writer,meshtastic_HardwareMessage_fields,&message)||writer.bytes_written!=metadata.payloadLength||
     memcmp(canonical,metadata.payload,metadata.payloadLength))return false;
  using EnumBase=__underlying_type(meshtastic_HardwareMessage_Type);
  EnumBase raw{};memcpy(&raw,&message.type,sizeof(message.type));
  if(raw!=meshtastic_HardwareMessage_Type_WRITE_GPIOS&&
     raw!=meshtastic_HardwareMessage_Type_READ_GPIOS&&
     raw!=meshtastic_HardwareMessage_Type_WATCH_GPIOS)return false;
  // The generic public-data encoder deliberately rejects want_response. The
  // stock RemoteHardware client needs that bit on READ while WRITE/WATCH may
  // omit it, so apply the same zero-hop channel cipher envelope here after
  // the stricter GPIO Data validation above.
  uint8_t frame[255]{};write32(frame,to);write32(frame+4,ours);write32(frame+8,id);
  frame[13]=hash_;frame[15]=uint8_t(ours);memcpy(frame+16,data,n);
  size_t wireSize=0;const bool ok=sealFrame(frame,n,sizeof(frame),cap,wireSize);
  if(ok){memcpy(out,frame,wireSize);len=wireSize;}
  wipe(frame,sizeof(frame));wipe(&metadata,sizeof(metadata));
  return ok;
}
#endif
} // namespace bench
