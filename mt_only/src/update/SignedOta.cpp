// SPDX-License-Identifier: GPL-3.0-only
// Stock wire commands from meshtastic/esp32-unified-ota 6258997041a3.
// Write only the inactive slot and select a trial after signature and digest verification.
#include "SignedOta.h"
#include "SignedImagePolicy.h"
#include "../Version.h"
#include "../BoardIdentity.h"
#include "MtReleaseKey.h"
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>
#include <psa/crypto.h>
#include <string.h>
#if defined(CONFIG_MT_SIGNED_BLE_OTA)
#include "SignedBleOta.h"
#endif

namespace mt {
namespace {
SignedVersionText bootVersion{};
bool signedVersionValid;
}
const SignedVersionText &firmwareVersion() { return bootVersion; }
}

namespace mt::update::signedOta {
namespace {
constexpr uint8_t Primary=PARTITION_ID(slot0_partition),Secondary=PARTITION_ID(slot1_partition);
static_assert(PARTITION_OFFSET(slot1_partition)==0x10000 && PARTITION_SIZE(slot1_partition)==0x1c0000);
static_assert(PARTITION_OFFSET(slot0_partition)==0x1d0000 && PARTITION_SIZE(slot0_partition)==0x1c0000);
static_assert(PARTITION_OFFSET(scratch_partition)==0x3e0000 && PARTITION_SIZE(scratch_partition)==0x10000);
static_assert(DT_REG_ADDR(DT_CHOSEN(zephyr_settings_partition))==0x7f0000);
constexpr size_t Maximum=PARTITION_SIZE(slot1_partition)-0x2000; // reserves all 448 swap status entries and trailer
flash_img_context writer;
uint8_t armedHash[32];bool armed=false;
char versionResponse[85];uint32_t boots;
void (*yieldToOwner)()=nullptr;
bool activeHeader(uint8_t header[32]){
    const flash_area *f=nullptr;if(flash_area_open(Primary,&f))return false;
    int rc=flash_area_read(f,0,header,32);flash_area_close(f);
    return !rc&&le32(header)==0x96f3b83du&&le16(header+8)==32;
}
bool digest(const flash_area *f,size_t bytes,uint8_t out[32]){
    psa_hash_operation_t op=PSA_HASH_OPERATION_INIT;size_t used=0;
    psa_status_t rc=psa_hash_setup(&op,PSA_ALG_SHA_256);
    static uint8_t buf[1024]; // Single owner; keep it off the nested socket/main stacks.
    for(size_t pos=0;rc==PSA_SUCCESS&&pos<bytes;){
        const size_t n=bytes-pos<sizeof(buf)?bytes-pos:sizeof(buf);
        if(flash_area_read(f,pos,buf,n)){psa_hash_abort(&op);return false;}
        rc=psa_hash_update(&op,buf,n);pos+=n;
        if(yieldToOwner&&(pos&0x3fffu)==0)yieldToOwner();
    }
    if(rc==PSA_SUCCESS)rc=psa_hash_finish(&op,out,32,&used);
    psa_hash_abort(&op);return rc==PSA_SUCCESS&&used==32;
}
bool verifySignature(const uint8_t hash[32],const uint8_t *der,size_t bytes){
    uint8_t raw[64];if(!rawP256Signature(der,bytes,raw))return false;
    psa_key_attributes_t attr=PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr,PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));psa_set_key_bits(&attr,256);
    psa_set_key_usage_flags(&attr,PSA_KEY_USAGE_VERIFY_HASH);psa_set_key_algorithm(&attr,PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t id{};const auto imported=psa_import_key(&attr,releasePoint,sizeof(releasePoint),&id);
    psa_reset_key_attributes(&attr);if(imported!=PSA_SUCCESS)return false;
    const auto rc=psa_verify_hash(id,PSA_ALG_ECDSA(PSA_ALG_SHA_256),hash,32,raw,sizeof(raw));psa_destroy_key(id);
    return rc==PSA_SUCCESS;
}
bool verifySlot(size_t bytes,const uint8_t expected[32]){
    uint8_t active[32],got[32];const flash_area *f=nullptr;
    if(!activeHeader(active)||flash_area_open(Secondary,&f))return false;
    const auto read=[&](size_t pos,void *out,size_t n){return flash_area_read(f,pos,out,n)==0;};
    const auto verify=[&](size_t body,const uint8_t *hash,const uint8_t *key,const uint8_t *sig,size_t n){
        return !memcmp(key,releaseKeyHash,32)&&digest(f,body,got)&&!memcmp(hash,got,32)&&verifySignature(got,sig,n);
    };
    const bool good=digest(f,bytes,got)&&!memcmp(got,expected,32)&&imageAllowed(bytes,active+20,read,verify);
    flash_area_close(f);return good;
}
int nibble(char c){if(c>='0'&&c<='9')return c-'0';if(c>='A'&&c<='F')return c-'A'+10;return c>='a'&&c<='f'?c-'a'+10:-1;}
}
bool available(){uint8_t h[32];return activeHeader(h)&&boot_is_img_confirmed()&&mcuboot_swap_type()==BOOT_SWAP_TYPE_NONE;}
// Called on main before any companion or network transport is opened. Both
// metadata consumers can subsequently read the immutable string without flash I/O.
void initializeBootVersion() {
    uint8_t header[32];
    if (activeHeader(header)) {
        const int size = snprintk(bootVersion, sizeof(bootVersion), "%u.%u.%u+%u",
                                 unsigned(header[20]), unsigned(header[21]), unsigned(le16(header + 22)),
                                 unsigned(le32(header + 24)));
        if (size > 0 && size_t(size) < sizeof(bootVersion)) {
            signedVersionValid = true;
            return;
        }
    }
    static_assert(sizeof(FirmwareVersion) <= sizeof(bootVersion));
    memset(bootVersion, 0, sizeof(bootVersion));
    memcpy(bootVersion, FirmwareVersion, sizeof(FirmwareVersion));
    signedVersionValid = false;
}
void confirmHealthyBoot(){if(!boot_is_img_confirmed())(void)boot_write_img_confirmed();}
void activate(const uint8_t hash[32]){memcpy(armedHash,hash,32);armed=true;}
Session::Session(const uint8_t expected[32]){memcpy(expected_,expected,32);}
Session::~Session(){if(initialized_&&writer.flash_area){flash_area_close(writer.flash_area);writer.flash_area=nullptr;}}
const char *Session::command(const char *line){
    if(!strcmp(line,"VERSION")){
        if(!signedVersionValid)return "ERR Image\n";
        snprintk(versionResponse,sizeof(versionResponse),"OK %u %s %u v1.0.1\n",unsigned(board::Model),firmwareVersion(),boots);return versionResponse;
    }
    if(strncmp(line,"OTA ",4)||initialized_)return "ERR Unknown Command\n";
    const char *p=line+4;size_t n=0;
    if(*p<'1'||*p>'9')return "ERR Invalid Format\n";
    do {const unsigned digit=*p++-'0';if(n>(Maximum-digit)/10)return "ERR Size\n";n=n*10+digit;} while(*p>='0'&&*p<='9');
    if(n<48||*p++!=' '||strlen(p)!=64)return "ERR Invalid Format\n";
    uint8_t hash[32];
    for(size_t i=0;i<32;++i){const int a=nibble(*p++),b=nibble(*p++);if(a<0||b<0)return "ERR Invalid Hash\n";hash[i]=uint8_t(a*16+b);}
    if(memcmp(expected_,hash,32))return "ERR Hash Rejected (NVS Mismatch)\n";
    if(!available()||flash_img_init_id(&writer,Secondary))return "ERR OTA Begin Failed\n";
    initialized_=true;remaining_=size_=n;return "ERASING\nOK\n";
}
const char *Session::write(const uint8_t *data,size_t n){
    if(!n||!remaining_||n>remaining_){remaining_=0;return "ERR Size Mismatch\n";}
    remaining_-=n;
    if(flash_img_buffered_write(&writer,data,n,remaining_==0)){remaining_=0;return "ERR Flash Write\n";}
    if(remaining_)return "ACK\n";
    if(!verifySlot(size_,expected_))return "ERR Hash Mismatch or Invalid Signed Image\n";
    if(!available()||boot_request_upgrade(BOOT_UPGRADE_TEST)||mcuboot_swap_type()!=BOOT_SWAP_TYPE_TEST)return "ERR Selection\n";
    complete_=true;return "OK\n";
}
void serveWifi(const uint8_t expected[32],void (*progress)());
void serve(bool ble,void (*progress)(),uint32_t bootCount){
    if(!armed)return;
    armed=false;yieldToOwner=progress;boots=bootCount;
#if defined(CONFIG_MT_SIGNED_BLE_OTA)
    if(ble)mt::update::serveSignedBle(armedHash,progress);else
#endif
    if(!ble)serveWifi(armedHash,progress);
    memset(armedHash,0,sizeof(armedHash));yieldToOwner=nullptr;
}
}
