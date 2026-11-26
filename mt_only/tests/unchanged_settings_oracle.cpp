// SPDX-License-Identifier: GPL-3.0-only
// Line adapter for actual stock Python admin vectors; memory only.
#include "device_config_fixture.h"
#include <sstream>
static std::string hex(const uint8_t *p,size_t n) {
  static const char digits[]="0123456789abcdef";std::string out;
  for(size_t i=0;i<n;++i){out+=digits[p[i]>>4];out+=digits[p[i]&15];}return out;
}
int main(int argc,char **argv) {
  assert(argc==2);const unsigned model=unsigned(std::stoul(argv[1]));assert(model==81||model==127);
  auto f=std::make_unique<Fixture>(uint8_t(model));std::string line;unsigned count=0;
  while(std::getline(std::cin,line)) {
    assert(++count<=128&&line.size()<=1100);std::istringstream in(line);std::string mode,raw;in>>mode>>raw;
    const bool ble=mode=="ble";assert(ble||mode=="usb");assert(!raw.empty()&&raw.size()%2==0&&raw.size()<=1024);
    std::vector<uint8_t> bytes;for(size_t i=0;i<raw.size();i+=2) {
      unsigned value=0;for(unsigned j=0;j<2;++j){const char c=raw[i+j];assert((c>='0'&&c<='9')||(c>='a'&&c<='f'));value=value*16+unsigned(c<='9'?c-'0':c-'a'+10);}bytes.push_back(uint8_t(value));
    }
    meshtastic_ToRadio request{};auto requestStream=pb_istream_from_buffer(bytes.data(),bytes.size());assert(pb_decode(&requestStream,meshtastic_ToRadio_fields,&request));
    meshtastic_AdminMessage decodedAdmin{};auto adminStream=pb_istream_from_buffer(request.packet.decoded.payload.bytes,request.packet.decoded.payload.size);
    assert(pb_decode(&adminStream,meshtastic_AdminMessage_fields,&decodedAdmin));
    uint8_t canonical[512]{};auto canonicalStream=pb_ostream_from_buffer(canonical,sizeof(canonical));assert(pb_encode(&canonicalStream,meshtastic_AdminMessage_fields,&decodedAdmin));
    const bool nanopbRoundtrip=canonicalStream.bytes_written==request.packet.decoded.payload.size&&
      !memcmp(canonical,request.packet.decoded.payload.bytes,canonicalStream.bytes_written);
    f->usb.records.clear();f->ble.records.clear();f->raw(bytes.data(),bytes.size(),ble,1000*count);
    const auto &host=ble?f->ble:f->usb;const auto node=f->service.config.node();
    std::cout<<"{\"records_hex\":[";bool first=true;for(const auto &record:host.records) {
      if(!first)std::cout<<',';
      first=false;std::cout<<'\"'<<hex(record.data(),record.size())<<'\"';
    }
    std::cout<<"],\"writes\":"<<f->backend.writes<<",\"saves\":"<<(f->usb.saves+f->ble.saves)
      <<",\"starts\":"<<f->radio.starts<<",\"stops\":"<<f->radio.stops<<",\"sends\":"<<f->radio.sends
      <<",\"receiving\":"<<f->service.receiving()<<",\"armed\":"<<f->service.endpointArmed()
      <<",\"endpoint_free\":"<<f->service.endpointFree()<<",\"region_us\":"<<unsigned(f->service.config.bytes[4])
      <<",\"tx_intent\":"<<unsigned(f->service.config.bytes[83])<<",\"public\":"<<node.isPublic()<<",\"nanopb_roundtrip\":"<<nanopbRoundtrip
      <<",\"decoded_admin_hex\":\""<<hex(canonical,canonicalStream.bytes_written)<<"\"";
    if(decodedAdmin.which_payload_variant==meshtastic_AdminMessage_set_owner_tag) {
      const auto &owner=decodedAdmin.set_owner;std::cout<<",\"owner_hw\":"<<mt::detail::enumNumber(owner.hw_model)
        <<",\"owner_role\":"<<mt::detail::enumNumber(owner.role)<<",\"owner_has_unmessagable\":"<<owner.has_is_unmessagable;
    }
    std::cout<<"}"<<std::endl;
    assert(!f->radio.sends);
  }
}
