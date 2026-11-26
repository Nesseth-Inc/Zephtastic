// SPDX-License-Identifier: GPL-3.0-only
#include "PrimaryChannel.hpp"
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>

using Channel=mt::PrimaryChannel;
unsigned checks=0;
void require(bool ok){assert(ok);++checks;}
void poison(Channel::Selection &out){
  for(auto &b:out.bytes)b=0xa5;
  out.length=999;out.hash=123;out.publicDefault=true;
}
void cleared(const Channel::Selection &out){
  for(auto b:out.bytes)require(b==0);
  require(!out.length&&!out.hash&&!out.publicDefault);
}
// Literal stock Channels::generateHash oracle, fixed non-AEAD LongFast profile.
uint8_t oracle(const uint8_t *key,size_t n){
  uint8_t hash=0;
  for(char c:std::array<char,8>{'L','o','n','g','F','a','s','t'})hash^=uint8_t(c);
  for(size_t i=0;i<n;++i)hash^=key[i];
  return hash;
}
void accepted(const uint8_t *input,size_t n,const uint8_t *key,size_t length,bool pub){
  Channel::Selection out;poison(out);
  const uint8_t expected=oracle(key,length);
  require(expected!=0);
  require(Channel::normalize(input,n,out));
  require(out.length==length&&out.hash==expected&&out.publicDefault==pub);
  require(!memcmp(out.bytes,key,length));
  for(size_t i=length;i<32;++i)require(out.bytes[i]==0);
  Channel::clear(out);cleared(out);
}
int main(){
  const uint8_t alias=1;
  require(oracle(bench::MtNodeRecord::PublicKey,16)==bench::MtNodeRecord::publicHash());
  accepted(&alias,1,bench::MtNodeRecord::PublicKey,16,true);
  accepted(bench::MtNodeRecord::PublicKey,16,bench::MtNodeRecord::PublicKey,16,true);
  for(size_t length:{size_t(16),size_t(32)})for(unsigned seed=0;seed<64;++seed){
    std::array<uint8_t,32> key{};
    for(size_t i=0;i<length;++i)key[i]=uint8_t(seed*17+i*13);
    if(!oracle(key.data(),length))key[0]^=1;
    accepted(key.data(),length,key.data(),length,false);
    for(size_t offset=0;offset+length<=32;++offset){
      Channel::Selection out;poison(out);memcpy(out.bytes+offset,key.data(),length);
      require(Channel::normalize(out.bytes+offset,length,out));
      require(out.length==length&&out.hash==oracle(key.data(),length)&&!out.publicDefault);
      require(!memcmp(out.bytes,key.data(),length));
      for(size_t i=length;i<32;++i)require(out.bytes[i]==0);
    }
    key.fill(0);key[0]=oracle(key.data(),length); // Valid key width, zero RF hash.
    require(oracle(key.data(),length)==0);
    Channel::Selection out;poison(out);
    require(!Channel::normalize(key.data(),length,out));cleared(out);
  }
  for(size_t offset=0;offset<32;++offset){
    Channel::Selection out;poison(out);out.bytes[offset]=1;
    require(Channel::normalize(out.bytes+offset,1,out));
    require(out.length==16&&out.publicDefault&&!memcmp(out.bytes,bench::MtNodeRecord::PublicKey,16));
  }
  std::array<uint8_t,32> default32{};memcpy(default32.data(),bench::MtNodeRecord::PublicKey,16);
  accepted(default32.data(),32,default32.data(),32,false);
  for(unsigned value=0;value<256;++value)if(value!=1){
    uint8_t byte=uint8_t(value);Channel::Selection out;poison(out);
    require(!Channel::normalize(&byte,1,out));cleared(out);
  }
  for(size_t size:{size_t(0),size_t(1),size_t(2),size_t(15),size_t(16),size_t(17),size_t(31),size_t(32),size_t(33),std::numeric_limits<size_t>::max()}){
    Channel::Selection out;poison(out);
    require(!Channel::normalize(nullptr,size,out));cleared(out);
    if(size!=1&&size!=16&&size!=32){
      poison(out);require(!Channel::normalize(reinterpret_cast<const uint8_t *>(uintptr_t(1)),size,out));cleared(out);
    }
  }
  {
    bench::MtNodeRecord disabled;
    require(disabled.valid()&&!Channel::configured(disabled));
    disabled.setPublic();require(disabled.valid()&&Channel::configured(disabled));
    require(disabled.setName("Primary owner",false)&&disabled.setName("KEY",true));
    require(Channel::configured(disabled));
  }
  for(size_t length:{size_t(16),size_t(32)}) {
    std::array<uint8_t,32> key{};
    for(size_t i=0;i<length;++i)key[i]=uint8_t(i*19+7);
    if(!oracle(key.data(),length))key[0]^=1;
    bench::MtNodeRecord record;
    record.setKey(oracle(key.data(),length),key.data(),length);
    std::array<uint8_t,72> before{};memcpy(before.data(),record.bytes,72);
    require(record.valid()&&Channel::configured(record));
    require(!memcmp(before.data(),record.bytes,72));
    record.bytes[70]^=1;require(!record.valid()&&!Channel::configured(record));
    memcpy(record.bytes,before.data(),72);
    record.bytes[4]^=1;record.seal();require(!Channel::configured(record));
    memcpy(record.bytes,before.data(),72);
    record.bytes[bench::MtNodeRecord::Name]=0;record.seal();
    require(!record.valid()&&!Channel::configured(record));
    memcpy(record.bytes,before.data(),72);
    for(unsigned width:{0u,1u,15u,17u,31u,33u,255u}) {
      record.bytes[3]=uint8_t(width);record.seal();require(!Channel::configured(record));
    }
    key.fill(0);key[0]=oracle(key.data(),length);
    record=bench::MtNodeRecord();record.setKey(0,key.data(),length);
    require(record.valid()&&!Channel::configured(record));
  }
  std::cout<<"PASS primary helper contract checks="<<checks<<" hardware=false production_integration=false\n";
}
