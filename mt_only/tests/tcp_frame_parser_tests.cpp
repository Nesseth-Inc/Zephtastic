// SPDX-License-Identifier: GPL-3.0-only
#include "network/MtTcpFrameParser.h"
#include <cassert>
#include <cstdint>
using mt::tcp::Frame;
using mt::tcp::FrameParser;
using R=FrameParser::Result;
int main() {
 FrameParser p; Frame out{}; out.length=77; out.payload[0]=0x5a; uint32_t t=10;
 // Fragmented one-byte frame; out unchanged until the final body byte.
 const uint8_t a[]={0x94,0xc3,0,1,0x42};
 for(size_t i=0;i<4;i++) assert(p.push(a[i],t++,1,out)==R::NeedMore);
 assert(out.length==77&&out.payload[0]==0x5a);
 assert(p.push(a[4],t++,1,out)==R::FrameReady&&out.length==1&&out.payload[0]==0x42);
 // Coalesced frames and sync-like bytes inside body remain payload.
 const uint8_t b[]={0x94,0xc3,0,3,0x94,0xc3,0,0x94,0xc3,0,1,0x7f};
 bool gotFirst=false,gotSecond=false;
 for(uint8_t v:b){auto r=p.push(v,t++,1,out); if(r==R::FrameReady){if(!gotFirst){gotFirst=true;assert(out.length==3&&out.payload[0]==0x94&&out.payload[1]==0xc3&&out.payload[2]==0);}else{gotSecond=true;assert(out.length==1&&out.payload[0]==0x7f);}}}
 assert(gotFirst&&gotSecond);
 // Maximum accepted, zero and 513 rejected, preserving output on non-ready.
 uint8_t maxf[516]={0x94,0xc3,0x02,0x00}; Frame maxout{}; size_t complete=0;
 for(size_t i=4;i<sizeof(maxf);i++)maxf[i]=uint8_t(i);
 for(uint8_t v:maxf)if(p.push(v,t++,1,maxout)==R::FrameReady)complete++;
 assert(complete==1&&maxout.length==512&&maxout.payload[511]==uint8_t(515));
 Frame unchanged=maxout;
 const uint8_t zero[]={0x94,0xc3,0,0};
 for(size_t i=0;i<3;i++)assert(p.push(zero[i],t++,1,maxout)==R::NeedMore);
 assert(p.push(zero[3],t++,1,maxout)==R::Invalid);
 const uint8_t over[]={0x94,0xc3,0x02,0x01};
 for(size_t i=0;i<3;i++)assert(p.push(over[i],t++,1,maxout)==R::NeedMore);
 assert(p.push(over[3],t++,1,maxout)==R::Invalid);
 assert(maxout.length==unchanged.length&&maxout.payload[511]==unchanged.payload[511]);
 // Epoch change and >2000ms timeout abandon old partial frame; fresh frame succeeds.
 const uint8_t partial[]={0x94,0xc3,0,2,0xaa};
 for(uint8_t v:partial)(void)p.push(v,t++,9,out);
 const uint8_t fresh[]={0x94,0xc3,0,1,0x33};
 auto r=p.push(fresh[0],t++,10,out); assert(r==R::NeedMore);
 for(size_t i=1;i<sizeof(fresh);i++)r=p.push(fresh[i],t++,10,out);
 assert(r==R::FrameReady&&out.length==1&&out.payload[0]==0x33);
 for(uint8_t v:partial)(void)p.push(v,t++,10,out);
 r=p.push(0x94,t+2001,10,out); assert(r==R::NeedMore);
 t+=2002;
 for(size_t i=1;i<sizeof(fresh);i++)r=p.push(fresh[i],t++,10,out);
 // timeout discarded partial bytes; this new sync+tail completes exactly one fresh frame.
 assert(r==R::FrameReady&&out.length==1&&out.payload[0]==0x33);
}
