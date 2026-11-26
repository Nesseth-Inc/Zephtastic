// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "MtCodec.h"
#include "TakPliPayload.h"
#include "meshtastic/atak.pb.h"
#include "meshtastic/mesh.pb.h"
#include "pb_encode.h"

namespace mt {
namespace tak_pli_reply {

static constexpr size_t ReplyDataLimit = 239;
static constexpr size_t ReplyPayloadLimit = 233;

namespace detail {
struct Buffer {
  uint8_t *data; size_t capacity; size_t size=0;
  bool byte(uint8_t value){if(size>=capacity)return false;data[size++]=value;return true;}
  bool varint(uint64_t value){do{const uint8_t part=uint8_t(value&0x7fu);value>>=7;if(!byte(uint8_t(part|(value?0x80u:0u))))return false;}while(value);return true;}
  bool key(unsigned tag,unsigned wire){return tag&&varint((uint64_t(tag)<<3)|wire);}
  bool scalar(unsigned tag,uint32_t value){return !value||(key(tag,0)&&varint(value));}
  bool bytes(const uint8_t *value,size_t length){if(length>capacity-size)return false;if(length)memcpy(data+size,value,length);size+=length;return true;}
  bool string(unsigned tag,const uint8_t *value,size_t length){return !length||(key(tag,2)&&varint(length)&&bytes(value,length));}
};
struct Reader {
  const uint8_t *data; size_t size; size_t at=0;
  bool varint(uint64_t &value){
    value=0;const size_t begin=at;
    for(unsigned i=0;i<10;++i){if(at>=size)return false;const uint8_t b=data[at++];if(i==9&&b>1)return false;value|=uint64_t(b&0x7fu)<<(7*i);if(!(b&0x80u)){size_t min=1;for(uint64_t n=value;n>=0x80u;n>>=7)++min;return at-begin==min;}}
    return false;
  }
  bool fixed32(uint32_t &value){if(size-at<4)return false;value=uint32_t(data[at])|(uint32_t(data[at+1])<<8)|(uint32_t(data[at+2])<<16)|(uint32_t(data[at+3])<<24);at+=4;return true;}
  bool block(const uint8_t *&value,size_t &length){uint64_t n=0;if(!varint(n)||n>size-at)return false;value=data+at;length=size_t(n);at+=length;return true;}
};
inline int32_t signedBits(uint32_t bits){int32_t value;memcpy(&value,&bits,sizeof(value));return value;}
inline bool validUid(const uint8_t *p,size_t n){if(n!=9||p[0]!='!')return false;for(size_t i=1;i<n;++i)if(!((p[i]>='0'&&p[i]<='9')||(p[i]>='a'&&p[i]<='f')))return false;return true;}
inline bool pliCanonical(const uint8_t *bytes,size_t length){
  if(!bytes||length<2||length>ReplyPayloadLimit||bytes[0]!=tak_pli::RawProtobufMarker)return false;
  const uint8_t *wire=bytes+1;const size_t wireSize=length-1;Reader reader{wire,wireSize};
  uint64_t seen=0;uint32_t values[16]{};const uint8_t *callsign=nullptr,*uid=nullptr,*device=nullptr;size_t callsignSize=0,uidSize=0,deviceSize=0;
  while(reader.at<reader.size){
    uint64_t key=0;if(!reader.varint(key)||key==0)return false;const unsigned tag=unsigned(key>>3),type=unsigned(key&7u);
    if(tag==0||tag>15||(seen&(uint64_t(1)<<tag)))return false;seen|=uint64_t(1)<<tag;
    if(tag==3||tag==14||tag==15){if(type!=2)return false;const uint8_t *p=nullptr;size_t n=0;if(!reader.block(p,n))return false;const size_t max=tag==14?47u:119u;if(n>max||memchr(p,0,n))return false;if(tag==3){callsign=p;callsignSize=n;}else if(tag==14){uid=p;uidSize=n;}else{device=p;deviceSize=n;}continue;}
    if(tag==6||tag==7){if(type!=5)return false;uint32_t raw=0;if(!reader.fixed32(raw))return false;values[tag]=raw;continue;}
    if(type!=0)return false;uint64_t raw=0;if(!reader.varint(raw)||raw>UINT32_MAX)return false;values[tag]=uint32_t(raw);
  }
  const auto cot=static_cast<uint32_t>(meshtastic_CotType_CotType_a_f_G_U_C);
  const uint8_t emptyString=0;
  if(values[1]!=cot||!values[2]||values[2]>2||values[4]<1||values[4]>14||values[5]<1||values[5]>8||
     values[12]<1||values[12]>2||values[13]!=values[12]||!((values[2]==1&&values[12]==2)||(values[2]==2&&values[12]==1))||
     !uid||callsignSize!=deviceSize||memcmp(callsign?callsign:&emptyString,device?device:&emptyString,callsignSize)||!validUid(uid,uidSize)||values[10]>36000||values[11]>100)return false;
  const int32_t latitude=signedBits(values[6]),longitude=signedBits(values[7]);
  const uint32_t altitudeBits=uint32_t((values[8]>>1)^(0u-(values[8]&1u)));const int32_t altitude=signedBits(altitudeBits);
  if(latitude < -900000000||latitude>900000000||longitude < -1800000000||longitude>1800000000)return false;
  uint8_t canonical[ReplyPayloadLimit-1];Buffer out{canonical,sizeof(canonical)};
  if(!out.scalar(1,values[1])||!out.scalar(2,values[2])||!out.string(3,callsign,callsignSize)||!out.scalar(4,values[4])||!out.scalar(5,values[5]))return false;
  auto fixed=[&out](unsigned tag,uint32_t value){return !value||(out.key(tag,5)&&out.byte(uint8_t(value))&&out.byte(uint8_t(value>>8))&&out.byte(uint8_t(value>>16))&&out.byte(uint8_t(value>>24)));};
  auto sint=[&out](unsigned tag,int32_t value){const uint32_t raw=uint32_t(value);return !value||(out.key(tag,0)&&out.varint((raw<<1)^(0u-(raw>>31))));};
  if(!fixed(6,uint32_t(latitude))||!fixed(7,uint32_t(longitude))||!sint(8,altitude)||!out.scalar(9,values[9])||!out.scalar(10,values[10])||!out.scalar(11,values[11])||!out.scalar(12,values[12])||!out.scalar(13,values[13])||!out.string(14,uid,uidSize)||!out.string(15,device,deviceSize))return false;
  return out.size==wireSize&&!memcmp(canonical,wire,wireSize);
}
} // namespace detail

inline bool responseShape(const bench::RouteMetadata &m){return m.portnum==meshtastic_PortNum_ATAK_PLUGIN_V2&&m.request_id&&!m.wantResponse&&!m.reply_id&&!m.emoji&&!m.signatureLength&&m.hasBitfield&&!(m.bitfield&~1u)&&m.payloadLength>1&&m.payloadLength<=ReplyPayloadLimit;}
inline bool canonical(const uint8_t *payload,size_t length){return detail::pliCanonical(payload,length);}
inline bool response(const bench::RouteMetadata &m,uint8_t *output,size_t capacity,size_t &size){
  size=0;if(!responseShape(m)||!detail::pliCanonical(m.payload,m.payloadLength)||!output||!capacity)return false;
  meshtastic_Data data=meshtastic_Data_init_zero;data.portnum=meshtastic_PortNum_ATAK_PLUGIN_V2;data.request_id=m.request_id;data.has_bitfield=true;data.bitfield=m.bitfield;data.payload.size=m.payloadLength;memcpy(data.payload.bytes,m.payload,m.payloadLength);
  uint8_t staged[ReplyDataLimit];pb_ostream_t stream=pb_ostream_from_buffer(staged,sizeof(staged));
  if(!pb_encode(&stream,meshtastic_Data_fields,&data)||!stream.bytes_written||
     stream.bytes_written>ReplyDataLimit||stream.bytes_written>capacity||
     !m.encodedDataSize||stream.bytes_written!=m.encodedDataSize)return false;
  memcpy(output,staged,stream.bytes_written);size=stream.bytes_written;return true;
}

// Whole Data validation is separate from canonical(), whose input is the inner
// raw PLI payload. Transport control paths must prove every original Data byte.
inline bool canonicalData(const uint8_t *bytes,size_t length) {
  if(!bytes||!length||length>ReplyDataLimit)return false;
  bench::RouteMetadata metadata{};uint8_t expected[ReplyDataLimit]{};size_t size=0;
  const bool ok=bench::MtCodec::decodeData(bytes,length,metadata)&&
    response(metadata,expected,sizeof(expected),size)&&size==length&&!memcmp(bytes,expected,length);
  volatile uint8_t *clear=expected;for(size_t i=0;i<sizeof(expected);++i)clear[i]=0;
  clear=reinterpret_cast<volatile uint8_t *>(&metadata);for(size_t i=0;i<sizeof(metadata);++i)clear[i]=0;
  return ok;
}

} // namespace tak_pli_reply
} // namespace mt
