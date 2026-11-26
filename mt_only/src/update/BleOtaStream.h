// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt::update {
// The stock Android transport fragments command lines at ATT_MTU-3, then
// waits for exactly one ACK or terminal OK for each binary GATT write.
class BleOtaStream {
  char line_[96]{};
  size_t used_=0;

  template<class Notify> static bool reply(const char *lines,Notify &notify){
    if(!lines||!*lines)return false;
    while(*lines){
      const char *end=strchr(lines,'\n');
      if(!end||end==lines||size_t(end-lines)>=127||!notify(lines,size_t(end-lines)+1))return false;
      lines=end+1;
    }
    return true;
  }
public:
  template<class Sink,class Notify> bool write(Sink &sink,const uint8_t *bytes,size_t size,Notify notify){
    if(!bytes||!size||size>512)return false;
    if(sink.downloading())return reply(sink.write(bytes,size),notify);
    for(size_t i=0;i<size;++i){
      const auto c=bytes[i];
      if(c=='\n'||c=='\r'){
        if(!used_)continue;
        line_[used_]=0;used_=0;
        if(!reply(sink.command(line_),notify))return false;
        // OTA command and binary data are separated by the client's OK wait.
        if(sink.downloading()){
          for(++i;i<size;++i)if(bytes[i]!='\r'&&bytes[i]!='\n')return false;
          break;
        }
      }else{
        if(c<0x20||c>0x7e||used_==sizeof(line_)-1){
          (void)reply("ERR Invalid Format\n",notify);return false;
        }
        line_[used_++]=char(c);
      }
    }
    return true;
  }
};
}
