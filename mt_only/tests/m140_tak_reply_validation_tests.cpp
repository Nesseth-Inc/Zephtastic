// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include "modules/TakPliReplyPayload.h"
#include "modules/TakPliReplyValidation.h"
#include "pb_decode.h"
#include "pb_encode.h"

namespace {
meshtastic_Position position(int32_t lat,int32_t lon,bool fixed){
  meshtastic_Position p=meshtastic_Position_init_zero;p.latitude_i=lat;p.longitude_i=lon;
  p.altitude_hae=-17;p.ground_speed=340;p.ground_track=35999000;
  p.location_source=fixed?meshtastic_Position_LocSource_LOC_MANUAL:meshtastic_Position_LocSource_LOC_INTERNAL;
  return p;
}
std::vector<uint8_t> encodePli(meshtastic_Position &p,bool fixed=true){
  const mt::tak_pli::Inputs inputs{&p,"Reply Unit",0x12345678,82,fixed,true,
    meshtastic_Team_Green,meshtastic_MemberRole_Medic};
  uint8_t payload[mt::tak_pli_reply::ReplyPayloadLimit]{};size_t n=0;
  assert(mt::tak_pli::encode(inputs,payload,sizeof(payload),n)==mt::tak_pli::Result::Ok);
  return {payload,payload+n};
}
bench::RouteMetadata metadata(const std::vector<uint8_t>&p){
  bench::RouteMetadata m{};m.portnum=meshtastic_PortNum_ATAK_PLUGIN_V2;m.request_id=1234;
  m.hasBitfield=true;m.bitfield=0;m.payloadLength=uint8_t(p.size());memcpy(m.payload,p.data(),p.size());
  meshtastic_Data d=meshtastic_Data_init_zero;d.portnum=meshtastic_PortNum_ATAK_PLUGIN_V2;d.request_id=m.request_id;d.has_bitfield=true;d.payload.size=p.size();memcpy(d.payload.bytes,p.data(),p.size());
  uint8_t wire[mt::tak_pli_reply::ReplyDataLimit];auto stream=pb_ostream_from_buffer(wire,sizeof(wire));assert(pb_encode(&stream,meshtastic_Data_fields,&d));m.encodedDataSize=uint16_t(stream.bytes_written);return m;
}
bool responseFails(bench::RouteMetadata m){uint8_t out[mt::tak_pli_reply::ReplyDataLimit];size_t n=9;memset(out,0x5a,sizeof(out));const bool result=mt::tak_pli_reply::response(m,out,sizeof(out),n);if(!result){assert(n==0);for(auto b:out)assert(b==0x5a);}return !result;}
bool replaceVarint(const std::vector<uint8_t>&input,unsigned wanted,uint32_t value,std::vector<uint8_t>&out){
  size_t at=1;while(at<input.size()){const size_t start=at;uint64_t key=0;unsigned shift=0;do{if(at>=input.size()||shift>=64)return false;const uint8_t b=input[at++];key|=uint64_t(b&127u)<<shift;shift+=7;if(!(b&128u))break;}while(true);
    const unsigned tag=unsigned(key>>3),wire=unsigned(key&7u);const size_t valueAt=at;
    if(wire==0){while(at<input.size()&&(input[at++]&128u)){}if(at>input.size())return false;if(tag==wanted){out.assign(input.begin(),input.begin()+valueAt);uint32_t v=value;do{uint8_t b=uint8_t(v&127u);v>>=7;if(v)b|=128;out.push_back(b);}while(v);out.insert(out.end(),input.begin()+at,input.end());return true;}}
    else if(wire==1){if(input.size()-at<8)return false;at+=8;}
    else if(wire==2){uint64_t n=0;unsigned s=0;do{if(at>=input.size()||s>=64)return false;const uint8_t b=input[at++];n|=uint64_t(b&127u)<<s;s+=7;if(!(b&128u))break;}while(true);if(n>input.size()-at)return false;at+=size_t(n);}
    else if(wire==5){if(input.size()-at<4)return false;at+=4;}else return false;
    (void)start;
  }return false;
}
}
int main(){
  auto zero=position(0,0,true);auto fixed=encodePli(zero,true);assert(mt::tak_pli_reply::canonical(fixed.data(),fixed.size()));
  auto m=metadata(fixed);assert(mt::tak_pli_reply::responseShape(m));uint8_t output[mt::tak_pli_reply::ReplyDataLimit];memset(output,0xa5,sizeof(output));size_t size=0;
  assert(mt::tak_pli_reply::response(m,output,sizeof(output),size)&&size>0&&size<=sizeof(output));
  assert(mt::tak_pli_reply::canonicalData(output,size));
  assert(!mt::tak_pli_reply::canonicalData(fixed.data(),fixed.size()));
  std::vector<uint8_t> envelope(output,output+size);
  auto duplicate=envelope;duplicate.push_back(72);duplicate.push_back(0);
  assert(!mt::tak_pli_reply::canonicalData(duplicate.data(),duplicate.size()));
  auto unknown=envelope;unknown.push_back(0x98);unknown.push_back(0x06);unknown.push_back(1);
  assert(!mt::tak_pli_reply::canonicalData(unknown.data(),unknown.size()));
  auto reordered=envelope;assert(reordered[0]==8&&reordered[1]==78);
  reordered.erase(reordered.begin(),reordered.begin()+2);reordered.push_back(8);reordered.push_back(78);
  assert(!mt::tak_pli_reply::canonicalData(reordered.data(),reordered.size()));
  auto explicitDestination=envelope;explicitDestination.insert(explicitDestination.end(),{37,0,0,0,0});
  assert(!mt::tak_pli_reply::canonicalData(explicitDestination.data(),explicitDestination.size()));
  meshtastic_Data data=meshtastic_Data_init_zero;pb_istream_t dataIn=pb_istream_from_buffer(output,size);assert(pb_decode(&dataIn,meshtastic_Data_fields,&data)&&!dataIn.bytes_left);
  assert(data.portnum==meshtastic_PortNum_ATAK_PLUGIN_V2&&data.request_id==1234&&data.has_bitfield&&data.bitfield==0&&data.payload.size==fixed.size()&&!memcmp(data.payload.bytes,fixed.data(),fixed.size()));
  auto gnssPosition=position(475123456,-1221234567,false);auto gnss=encodePli(gnssPosition,false);assert(mt::tak_pli_reply::canonical(gnss.data(),gnss.size()));
  m=metadata(gnss);m.bitfield=1;assert(mt::tak_pli_reply::responseShape(m));
  assert(mt::tak_pli_reply::response(m,output,sizeof(output),size)&&size>0);
  assert(mt::tak_pli_reply::canonical(fixed.data(),fixed.size())&&mt::tak_pli_reply::canonical(gnss.data(),gnss.size()));

  assert(!mt::tak_pli_reply::canonical(nullptr,0));
  auto bad=fixed;bad[0]=0;assert(!mt::tak_pli_reply::canonical(bad.data(),bad.size()));
  bad=fixed;bad.pop_back();assert(!mt::tak_pli_reply::canonical(bad.data(),bad.size()));
  bad=fixed;bad[1]=0x0a;assert(!mt::tak_pli_reply::canonical(bad.data(),bad.size())); // wrong wire type for field 1
  bad=fixed;bad.push_back(0x08);bad.push_back(0x01);assert(!mt::tak_pli_reply::canonical(bad.data(),bad.size())); // duplicate CoT field
  bad=fixed;bad.push_back(0x98);bad.push_back(0x06);bad.push_back(0x01);assert(!mt::tak_pli_reply::canonical(bad.data(),bad.size())); // unknown field
  bad=fixed;bad.push_back(0xfa);bad.push_back(0x01);bad.push_back(0);assert(!mt::tak_pli_reply::canonical(bad.data(),bad.size())); // Chat oneof
  std::vector<uint8_t> changed;assert(replaceVarint(fixed,1,200,changed));assert(!mt::tak_pli_reply::canonical(changed.data(),changed.size()));
  assert(replaceVarint(fixed,2,3,changed));assert(!mt::tak_pli_reply::canonical(changed.data(),changed.size()));
  assert(replaceVarint(fixed,4,200,changed));assert(!mt::tak_pli_reply::canonical(changed.data(),changed.size()));
  assert(replaceVarint(fixed,5,9,changed));assert(!mt::tak_pli_reply::canonical(changed.data(),changed.size()));
  assert(replaceVarint(fixed,12,3,changed));assert(!mt::tak_pli_reply::canonical(changed.data(),changed.size()));
  assert(replaceVarint(fixed,10,36001,changed));assert(!mt::tak_pli_reply::canonical(changed.data(),changed.size()));
  assert(replaceVarint(fixed,11,101,changed));assert(!mt::tak_pli_reply::canonical(changed.data(),changed.size()));
  auto outOfRange=position(900000001,0,true);auto outOfRangePli=encodePli(outOfRange,true);assert(!mt::tak_pli_reply::canonical(outOfRangePli.data(),outOfRangePli.size()));
  bad=fixed; // Overlong encoding of the first enum value.
  assert(bad.size()>2&&bad[1]==0x08&&bad[2]<0x80);bad.insert(bad.begin()+3,uint8_t(0));bad[2]|=0x80;assert(!mt::tak_pli_reply::canonical(bad.data(),bad.size()));
  uint8_t oversized[mt::tak_pli_reply::ReplyPayloadLimit+1]{};memcpy(oversized,fixed.data(),fixed.size());assert(!mt::tak_pli_reply::canonical(oversized,sizeof(oversized)));

  auto invalid=metadata(fixed);invalid.bitfield=2;assert(responseFails(invalid));
  invalid=metadata(fixed);invalid.wantResponse=true;assert(responseFails(invalid));
  invalid=metadata(fixed);invalid.reply_id=3;assert(responseFails(invalid));
  invalid=metadata(fixed);invalid.emoji=1;assert(responseFails(invalid));
  invalid=metadata(fixed);invalid.signatureLength=1;assert(responseFails(invalid));
  invalid=metadata(fixed);invalid.request_id=0;assert(responseFails(invalid));
  invalid=metadata(fixed);invalid.portnum=meshtastic_PortNum_POSITION_APP;assert(responseFails(invalid));
  invalid=metadata(fixed);invalid.payload[0]=0;assert(responseFails(invalid));
  invalid=metadata(fixed);++invalid.encodedDataSize;assert(responseFails(invalid));
  memset(output,0xa5,sizeof(output));size=88;assert(!mt::tak_pli_reply::response(metadata(fixed),output,1,size)&&size==0);for(auto b:output)assert(b==0xa5);
  return 0;
}
