// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "pki/PrivateRuntime.h"

// A boot-ready provider can expose a synthetic owner public key; it never transmits.
struct RelayFixtureProvider final:mt::pki::PrivateRuntime {
 explicit RelayFixtureProvider(uint32_t local,const uint8_t *publicKey=nullptr) : local_(local),publicKey_(publicKey) {}
 uint32_t local_;const uint8_t *publicKey_;
 bool available=true;
 uint32_t peer()const override{return 0x87654321;}
 bool ready(uint32_t local)const override{return available&&local==local_;}
 bool publicIdentity(uint8_t out[32])const override{if(!publicKey_)return false;memcpy(out,publicKey_,32);return true;}
 bool bootstrap(bench::MtCodec &,const bench::MtNodeRecord &,uint8_t,uint32_t,uint32_t,bool,uint32_t,bench::Frame &)override{return false;}
 void freeze()override{available=false;}
 bool matchesPeer(uint32_t)const override{return false;}
 bool revoke(uint32_t)override{return false;}
 void clearEvent()override{}
 void sweep(mt::Endpoint &)override{}
 int submit(mt::Endpoint &,uint8_t,uint32_t,uint32_t,const uint8_t *,size_t,unsigned,bool,const uint8_t *,size_t,bool,uint32_t)override{return -ENOTSUP;}
 void receive(mt::Endpoint &,bench::MtCodec &,const uint8_t *,size_t,uint32_t,uint32_t,bool)override{}
 void proof(mt::Endpoint &,const uint8_t *,size_t,const bench::RouteMetadata &,uint32_t)override{}
 bool event(uint8_t,const uint8_t *,size_t,mt::pki::PrivateEvent &)override{return false;}
};
