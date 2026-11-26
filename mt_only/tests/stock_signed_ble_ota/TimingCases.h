// SPDX-License-Identifier: GPL-3.0-only
// Included inside the carrier fixture after data() and sendCommand().
namespace timing {
unsigned notifications=0,writes=0;
size_t sent=0;
int64_t firstData=0;
bool enabled(){return scenario.rfind("timed-",0)==0;}
bool full(){return scenario=="timed-mtu23";}
void setSize(){signedOta::expectedSize=full()?1599050:256;}

void pump(){
  if(inFlight||replies.size()==notifications)return;
  notifications=unsigned(replies.size());
  if(scenario=="timed-preamble"){
    assert(replies.back()=="OK 81 test 1 v2\n");
    // Feed valid VERSION commands and blank frames without ever accepting START.
    k_sleep(30000);sendCommand("\r\nVERSION\n");return;
  }
  if(phase==1){
    assert(replies.back()=="OK 81 test 1 v2\n");
    sendCommand("OTA "+std::to_string(signedOta::expectedSize)+" "+std::string(64,'1')+"\n");
    phase=2;return;
  }
  if(phase==2&&replies.back()=="ERASING\n")return;
  if(phase==2){
    assert(replies.back()=="OK\n");phase=3;firstData=k_uptime_get();
  }else if(replies.back()=="ACK\n"){
    // The carrier adds a 5 ms workqueue poll. An ordinary MTU-23 chunk then
    // takes 85 ms end to end, with precisely one outstanding write.
    if(full())k_sleep(80);
    else if(scenario=="timed-idle")k_sleep(45000);
    else k_sleep(20000); // active one-byte trickle still has an absolute limit
  }else {assert(replies.back()=="OK\n"&&sent==signedOta::expectedSize);return;}
  const size_t n=std::min<size_t>(full()?20:1,signedOta::expectedSize-sent);
  uint8_t bytes[20];
  for(size_t i=0;i<n;++i)bytes[i]=uint8_t(sent+i);
  assert(data(update,bytes,n)==ssize_t(n));sent+=n;++writes;
}

void verify(){
  const auto elapsed=k_uptime_get();
  printf("case=%s offered=%zu stored=%zu writes=%u ACKs=%zu selected=%d elapsed_ms=%lld\n",
         scenario.c_str(),sent,signedOta::stored.size(),writes,
         size_t(std::count(replies.begin(),replies.end(),"ACK\n")),signedOta::selected,(long long)elapsed);
  fflush(stdout);
  if(full()){
    assert(mockMtu==23&&writes==79953&&signedOta::stored.size()==1599050&&signedOta::selected);
    assert(replies.back()=="OK\n"&&std::count(replies.begin(),replies.end(),"ACK\n")==79952);
    for(size_t i=0;i<signedOta::stored.size();++i)assert(signedOta::stored[i]==uint8_t(i));
    assert(elapsed-firstData>=6795000&&elapsed-firstData<6810000);
  }else{
    assert(!signedOta::selected);
    if(scenario=="timed-preamble")assert(elapsed>=120000&&elapsed<121000&&signedOta::stored.empty());
    else if(scenario=="timed-idle")assert(elapsed-firstData>=45000&&elapsed-firstData<45100&&signedOta::stored.size()==1);
    else assert(elapsed-firstData>=175000&&elapsed-firstData<200000&&signedOta::stored.size()==9);
  }
}
}
