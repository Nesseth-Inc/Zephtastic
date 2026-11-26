// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d KeyVerificationModule::allocReply, processSecurityNumber
// and generateVerificationCode. The embedded donor writes little-endian integers.
#pragma once
#include <stdint.h>
#include <string.h>
#include <meshtastic/mesh.pb.h>

namespace mt::stock::verification {
inline void put32(uint8_t *bytes,uint32_t value) {
    for(unsigned i=0;i<4;++i) bytes[i]=uint8_t(value>>(8*i));
}
inline void putNonce(uint8_t *bytes,uint64_t nonce) {
    for(unsigned i=0;i<8;++i) bytes[i]=uint8_t(nonce>>(8*i));
}

// Keys and node numbers have the initiator first on both ends of the exchange.
// The caller retains ownership of the hash provider and corresponding scratch wipe.
inline void securityNumberTranscript(uint8_t (&bytes)[84], uint32_t number, uint64_t nonce,
                                     uint32_t initiator, uint32_t responder,
                                     const uint8_t *initiatorKey,const uint8_t *responderKey) {
    put32(bytes,number);
    putNonce(bytes+4,nonce);
    put32(bytes+12,initiator);
    put32(bytes+16,responder);
    memcpy(bytes+20,initiatorKey,32);
    memcpy(bytes+52,responderKey,32);
}
inline void challengeTranscript(uint8_t (&bytes)[40], uint64_t nonce,const uint8_t *hash1) {
    putNonce(bytes,nonce);
    memcpy(bytes+8,hash1,32);
}
inline void generateVerificationCode(const uint8_t *hash1,char (&readable)[10]) {
    for(unsigned i=0;i<4;++i) readable[i]=char((hash1[i]>>2)+48);
    readable[4]=' ';
    for(unsigned i=5;i<9;++i) readable[i]=char((hash1[i]>>2)+48);
    readable[9]='\0';
}
inline meshtastic_KeyVerification numberProof(uint64_t nonce,const uint8_t *hash1) {
    meshtastic_KeyVerification result{};
    result.nonce=nonce;
    result.hash1.size=32;
    memcpy(result.hash1.bytes,hash1,32);
    return result;
}
} // namespace mt::stock::verification
