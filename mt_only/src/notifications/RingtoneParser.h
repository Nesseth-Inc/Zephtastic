// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>

namespace mt::notification {
// Small, allocation-free RTTTL grammar shared by settings validation and both
// nonblocking output backends. Accepted subset is deliberately explicit:
// name:d=1..64,o=4..7,b=25..900:notes, with optional header defaults and
// comma-separated notes [duration]pitch[#][octave][.].
struct RingtoneParser {
  struct Settings {uint8_t duration=4,octave=5;uint16_t bpm=140;size_t body=0,bodySize=0;};
  struct Note {uint16_t frequencyHz=0,durationMs=0;};
private:
  static bool number(const char *p,size_t end,size_t &at,uint32_t max,uint32_t &value){
    value=0;const size_t start=at;
    while(at<end&&p[at]>='0'&&p[at]<='9'){
      const uint32_t digit=uint32_t(p[at++]-'0');if(digit>max||value>(max-digit)/10)return false;value=value*10+digit;
    }
    return at>start&&value!=0;
  }
  static bool parseNote(const char *p,const Settings &s,size_t start,Note &out,size_t &next){
    if(start>=s.bodySize)return false;size_t at=start;uint32_t duration=s.duration;
    if(p[at]>='0'&&p[at]<='9')if(!number(p,s.bodySize,at,64,duration)||duration>64)return false;
    if(at>=s.bodySize)return false;const char pitch=p[at++];uint32_t hz=0;
    switch(pitch){case 'c':hz=262;break;case 'd':hz=294;break;case 'e':hz=330;break;case 'f':hz=349;break;
      case 'g':hz=392;break;case 'a':hz=440;break;case 'b':hz=494;break;case 'p':hz=0;break;default:return false;}
    if(at<s.bodySize&&p[at]=='#'){if(!hz)return false;++at;hz=hz*106u/100u;}
    uint32_t octave=s.octave;if(at<s.bodySize&&p[at]>='0'&&p[at]<='9'){
      octave=uint32_t(p[at++]-'0');if(octave<4||octave>7)return false;
    }
    bool dotted=false;if(at<s.bodySize&&p[at]=='.'){dotted=true;++at;}
    if(at<s.bodySize&&(p[at]!=','||at+1==s.bodySize))return false;
    if(octave>4)for(uint32_t i=4;i<octave;++i)hz*=2;
    else for(uint32_t i=octave;i<4;++i)hz/=2;
    uint32_t milliseconds=240000u/s.bpm/duration;if(dotted)milliseconds=(milliseconds*3u+1u)/2u;
    if(!milliseconds||milliseconds>60000u||hz>65535u)return false;
    out={uint16_t(hz),uint16_t(milliseconds)};next=at<s.bodySize?at+1:at;return true;
  }
public:
  static bool parse(const char *p,size_t n,Settings &out){
    if(!p||!n||n>230)return false;for(size_t i=0;i<n;++i)if(p[i]<' '||p[i]>'~')return false;
    size_t first=0;while(first<n&&p[first]!=':')++first;
    if(first==0||first>32||first==n)return false;
    for(size_t i=0;i<first;++i){const char c=p[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return false;}
    size_t second=first+1;while(second<n&&p[second]!=':')++second;if(second==n||second+1>=n)return false;
    Settings s{};bool seenDuration=false,seenOctave=false,seenBpm=false;size_t at=first+1;
    while(at<second){
      if(second-at<3||p[at+1]!='=')return false;const char key=p[at];at+=2;uint32_t value=0;
      const uint32_t max=key=='d'?64:key=='o'?7:key=='b'?900:0;if(!max||!number(p,second,at,max,value))return false;
      if(key=='d'){if(seenDuration||value<1)return false;seenDuration=true;s.duration=uint8_t(value);}
      else if(key=='o'){if(seenOctave||value<4)return false;seenOctave=true;s.octave=uint8_t(value);}
      else {if(seenBpm||value<25)return false;seenBpm=true;s.bpm=uint16_t(value);}
      if(at<second){if(p[at++]!=',')return false;if(at==second)return false;}
    }
    s.body=second+1;s.bodySize=n-s.body;
    size_t cursor=0;unsigned count=0;
    while(cursor<s.bodySize){Note note{};size_t next=0;if(!parseNote(p+s.body,s,cursor,note,next)||next<=cursor)return false;cursor=next;if(++count>230)return false;}
    if(!count)return false;out=s;return true;
  }
  // `cursor` is an offset into the note body. End-of-song wraps to note zero.
  static bool next(const char *p,size_t n,const Settings &s,size_t &cursor,Note &out){
    if(!p||!n||s.body>=n||!s.bodySize||s.bodySize>n-s.body)return false;
    if(cursor>=s.bodySize)cursor=0;
    size_t nextCursor=0;if(!parseNote(p+s.body,s,cursor,out,nextCursor)||nextCursor<=cursor)return false;
    cursor=nextCursor;if(cursor==s.bodySize)cursor=0;return true;
  }
};
}
