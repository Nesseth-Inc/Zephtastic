// SPDX-License-Identifier: GPL-3.0-only
#include "pki_runtime_fixture.h"
#include "stock/optional/ReplyWire.h"

static Bytes request(Fixture &f,bool example,unsigned id) {
    meshtastic_Data d{};
    d.portnum=example?meshtastic_PortNum_REPLY_APP:meshtastic_PortNum_TEXT_MESSAGE_APP;
    d.has_bitfield=true;d.want_response=example;d.bitfield=example?2:0;
    static const char body[]="/ping";
    d.payload.size=sizeof(body)-1;memcpy(d.payload.bytes,body,d.payload.size);
    uint8_t plain[96],cipher[128];size_t size=0;
    auto w=pb_ostream_from_buffer(plain,sizeof(plain));assert(pb_encode(&w,meshtastic_Data_fields,&d));
    assert(f.crypto.encrypt(f.peer.scalar,f.local.binding.publicKey,f.peer.binding.node,id,0x12345678,
                            plain,w.bytes_written,cipher,sizeof(cipher),size));
    Bytes frame(16+size);put(frame.data(),f.local.binding.node);put(frame.data()+4,f.peer.binding.node);
    put(frame.data()+8,id);frame[12]=0x61|(example?8:0);frame[15]=uint8_t(f.peer.binding.node);
    memcpy(frame.data()+16,cipher,size);return frame;
}

int main(){
    for(bool example:{false,true}){
        Fixture f(true);
        const unsigned id=190;
        auto frame=request(f,example,id);
        auto radioOnly=[&](const Bytes &frame,uint32_t now,bool radio=true){
            f.s.rx(frame.data(),frame.size(),3,now,81,radio,nullptr,false,1792660000u,0,-80,3.f);
        };
        auto corrupted=frame;corrupted.back()^=1;
        radioOnly(corrupted,70);f.s.flushStockReply(70,432);f.poll(90);assert(f.radio.frames.empty());
        radioOnly(frame,100);f.s.flushStockReply(101,832);f.poll(700);
        assert(f.radio.frames.size()==1);
        const auto &response=f.radio.frames.front();
        const auto replyId=bench::le32(response.data()+8);
        assert(replyId!=id&&bench::le32(response.data())==f.peer.binding.node&&response[13]==0);
        assert(bool(response[12]&8)==example);
        uint8_t plain[160];size_t size=0;
        assert(f.crypto.decrypt(f.peer.scalar,f.local.binding.publicKey,f.local.binding.node,replyId,
                                response.data()+16,response.size()-16,plain,sizeof(plain),size));
        assert(mt::stock::optional::exactWire(plain,size,false));
        meshtastic_Data d{};auto in=pb_istream_from_buffer(plain,size);assert(pb_decode(&in,meshtastic_Data_fields,&d));
        assert(d.request_id==(example?id:0));
        f.complete(750);radioOnly(frame,850);f.s.flushStockReply(851,432);f.poll(1300);
        assert(f.radio.frames.size()==1);
    }
    for(bool example:{false,true}){
        Fixture f(true);f.s.stop();auto cfg=f.s.config;mt::ChannelRecord secondary;
        assert(secondary.set(2,"Airfield",nullptr,0,0,false,false,false,false)&&cfg.setSecondaryChannel(1,secondary));
        assert(cfg.enableLicensedOperation()&&f.s.apply(cfg)&&f.s.start(false,0)&&f.s.armEndpoint(true));
        const char *message=example?mt::stock::optional::ExampleAnswer:"🎙️ Mic Check : 0 Hops away | RSSI -81 | SNR 3.0";
        mt::stock::optional::Reply reply{example?unsigned(meshtastic_PortNum_REPLY_APP):unsigned(meshtastic_PortNum_TEXT_MESSAGE_APP),
            1,2,f.peer.binding.node,example?139u:0u,false,false,message};
        assert(!f.s.submitStockReply(reply,100,829));
        f.poll(700);assert(f.radio.frames.size()==1);
        const auto &wire=f.radio.frames[0];bench::RouteMetadata meta{};unsigned slot=9;
        assert(f.s.decodeChannel(wire.data(),wire.size(),meta,slot)&&slot==1&&meta.signatureLength==64);
        bench::MtSignature signature;
        assert(signature.trustPublicKey(f.local.binding.node,f.local.binding.publicKey,600));
        assert(signature.apply(wire.data(),wire.size(),meta,700,true)==bench::MtSignature::Result::Verified);
        assert(meta.portnum==reply.port&&meta.request_id==reply.requestId&&std::string(reinterpret_cast<char*>(meta.payload),meta.payloadLength)==message);
    }
    puts("PASS signed licensed replies from both enabled handlers on the selected secondary channel");
    puts("PASS both optional responders over authenticated PSA-encrypted RF, rejected tamper, replay, destination and request correlation");
}
