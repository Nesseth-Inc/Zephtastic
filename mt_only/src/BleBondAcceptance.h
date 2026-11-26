// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
namespace mt::ble {
// Peer matching uses native ID/address in the caller. Only the captured epoch
// and status cross a store; no key or peer address is retained here.
class BleBondAcceptance {
 uint32_t epoch_=0;bool pin_=false,loaded_=false,paired_=false,stored_=false,failed_=false;
public:
 void begin(uint32_t epoch,bool pin,bool loaded){epoch_=epoch;pin_=pin;loaded_=loaded;paired_=stored_=failed_=false;}
 uint32_t storeBegin()const{return epoch_&&pin_&&!loaded_&&!failed_?epoch_:0;}
 void storeEnd(uint32_t epoch,int result){if(!epoch||epoch!=epoch_)return;if(result)failed_=true;else stored_=true;}
 void paired(bool bonded){if(pin_&&!bonded)failed_=true;else paired_=true;}
 void revoke(){loaded_=stored_=paired_=false;failed_=true;}
 void acceptLoaded(){if(epoch_&&pin_&&!failed_)loaded_=true;}
 bool allowed()const{return epoch_&&!failed_&&(!pin_||loaded_||(paired_&&stored_));}
};
}
