#include "v14/SettingsJournal.h"
#include <cassert>
#include <fstream>
#include <map>
#include <string>
#include <vector>
struct Backend:mt::SettingsBackend {
 std::map<std::string,std::vector<uint8_t>> records;
 int read(const char *k,uint8_t *out,size_t cap)override{auto i=records.find(k);if(i==records.end())return -ENOENT;if(i->second.size()>cap)return -E2BIG;memcpy(out,i->second.data(),i->second.size());return int(i->second.size());}
 int write(const char *k,const uint8_t *data,size_t n)override{records[k]={data,data+n};return 0;}
};
int main(){
 mt::Config c;c.bytes[4]=1;c.seal();assert(c.setFixedPosition(123456789,-123456789,true,-35));c.setPositionSharing(true);
 mt::ChannelRecord secondary;assert(secondary.set(2,"Old",nullptr,0));assert(c.setSecondaryChannel(3,secondary));
 assert(c.valid()&&c.bytes[2]==14);std::ofstream config("v14-config.bin",std::ios::binary);config.write(reinterpret_cast<const char*>(c.bytes),sizeof(c.bytes));config.close();assert(config);
 Backend b;mt::SettingsJournal journal;assert(!journal.load(b)&&!journal.save(b,c));const auto &raw=b.records.at("mtbase/state0");assert(raw.size()==624&&raw[3]==5);
 std::ofstream record("v5-journal.bin",std::ios::binary);record.write(reinterpret_cast<const char*>(raw.data()),raw.size());record.close();assert(record);
}
