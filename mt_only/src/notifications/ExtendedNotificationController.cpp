// SPDX-License-Identifier: GPL-3.0-only
#include "ExtendedNotificationController.h"
#include <string.h>
#include "meshtastic/portnums.pb.h"
namespace mt::notification {
bool ExtendedNotificationConfig::fromWire(const meshtastic_ModuleConfig &w,ExtendedNotificationConfig &out){
  if(w.which_payload_variant!=meshtastic_ModuleConfig_external_notification_tag)return false;
  const auto &v=w.payload_variant.external_notification;ExtendedNotificationConfig c;
  c.enabled=v.enabled;c.active=v.active;c.usePwm=v.use_pwm;c.useI2s=v.use_i2s_as_buzzer;
  c.alertMessage=v.alert_message;c.alertBell=v.alert_bell;c.alertMessageVibra=v.alert_message_vibra;c.alertBellVibra=v.alert_bell_vibra;
  c.alertMessageBuzzer=v.alert_message_buzzer;c.alertBellBuzzer=v.alert_bell_buzzer;c.led=uint8_t(v.output);c.vibrator=uint8_t(v.output_vibra);c.buzzer=uint8_t(v.output_buzzer);
  c.outputMs=v.output_ms?v.output_ms:1000;c.nagTimeoutSeconds=v.nag_timeout;
  if(v.output>255||v.output_vibra>255||v.output_buzzer>255)return false;
  out=c;return true;
}
bool ExtendedNotificationController::configure(const ExtendedNotificationConfig &c,ExtendedNotificationCapabilities caps,ExtendedNotificationBackend b){
  stop();config_=c;caps_=caps;backend_=b;
  const bool requestedUnsupported=((c.alertMessageVibra||c.alertBellVibra)&&!caps.vibrator)||
    (c.usePwm&&!caps.pwmBuzzer)||(c.useI2s&&!caps.i2sRtttl)||
    ((c.alertMessageBuzzer||c.alertBellBuzzer)&&!c.usePwm&&!c.useI2s&&!caps.gpioBuzzer);
  if(requestedUnsupported)return false;
  available_=c.enabled&&(((c.alertMessageVibra||c.alertBellVibra)&&caps.vibrator)||
    ((c.alertMessageBuzzer||c.alertBellBuzzer)&&(c.usePwm?caps.pwmBuzzer:c.useI2s?caps.i2sRtttl:caps.gpioBuzzer)));
  return true;
}
bool ExtendedNotificationController::setRingtone(const char *s,size_t n){
  if(!s||!n||n>RingtoneSettings::MaxBytes)return false;
  if(n==ringtoneSize_&&!memcmp(ringtone_,s,n))return true;
  RingtoneSettings candidate;
  const auto result=candidate.assign(s,n);
  if(result!=RingtoneSettings::AssignResult::Changed&&result!=RingtoneSettings::AssignResult::Unchanged)return false;
  RingtoneParser::Settings parsed{};if(!RingtoneParser::parse(candidate.ringtone,candidate.length(),parsed))return false;
  memcpy(ringtone_,candidate.ringtone,sizeof(ringtone_));ringtoneSize_=candidate.length();ringSettings_=parsed;ringParsed_=true;ringtoneCursor_=0;noteCutoff_=0;notePending_=false;
  if(buzzer_&&config_.useI2s&&backend_.playRtttl)(void)backend_.playRtttl(backend_.context,ringtone_,ringtoneSize_,true);
  return true;}
bool ExtendedNotificationController::seen(uint32_t from,uint32_t id,uint32_t now)const{for(const auto &s:seen_)if(s.from==from&&s.id==id&&uint32_t(now-s.at)<600000)return true;return false;}
bool ExtendedNotificationController::receive(uint32_t from,uint32_t local,uint32_t id,uint32_t port,const uint8_t *p,size_t n,bool muted,bool range,uint32_t now){
  if(!available_||!config_.enabled||!id||from==local||!p||n>233)return false;
  if (!stock::notification::wantPacket(port, range)) return false;
  if (seen(from, id, now)) return false;
  seen_[next_++ % 4] = {from, id, now};
  const stock::notification::Flags flags{config_.alertBell, config_.alertMessage, config_.alertBellVibra,
    config_.alertMessageVibra, config_.alertBellBuzzer, config_.alertMessageBuzzer};
  const auto [led, vibra, buzzer] = stock::notification::handleReceived(flags, p, n, muted,
    caps_.gpioBuzzer || caps_.pwmBuzzer || caps_.i2sRtttl, eventBuzzer_ && armed_);
  return arm(led,vibra,buzzer,now);
}
bool ExtendedNotificationController::startNotification(uint32_t now){
  if(!available_||!config_.enabled)return false;
  // The ZT DeviceConfig adapter admits the default buzzer mode only.
  const bool buzzer=config_.alertMessageBuzzer && (caps_.gpioBuzzer||caps_.pwmBuzzer||caps_.i2sRtttl);
  return arm(config_.alertMessage,config_.alertMessageVibra,buzzer,now);
}
bool ExtendedNotificationController::arm(bool led,bool vibra,bool buzzer,uint32_t now){
  if (!led && !vibra && !buzzer) return false;
  cutoff_ = stock::notification::armNagCycle(now, config_.nagTimeoutSeconds, config_.outputMs);
  lastToggle_=now;armed_=true;eventLed_=led;eventVibrator_=vibra;eventBuzzer_=buzzer;
  output(led,vibra,buzzer);return true;
}
void ExtendedNotificationController::output(bool led,bool vibra,bool buzzer){
  if(caps_.led&&backend_.setLed&&led_!=led)(void)backend_.setLed(backend_.context,led);led_=led;
  if(caps_.vibrator&&backend_.setVibrator&&vibrator_!=vibra)(void)backend_.setVibrator(backend_.context,config_.vibrator,vibra);vibrator_=vibra;
  if(config_.usePwm&&caps_.pwmBuzzer){
    if(buzzer&&!buzzer_){ringtoneCursor_=0;noteCutoff_=0;notePending_=false;if(!ringParsed_)ringParsed_=RingtoneParser::parse(ringtone_,ringtoneSize_,ringSettings_);}
    else if(!buzzer&&buzzer_&&backend_.setPwmTone)(void)backend_.setPwmTone(backend_.context,0,0);
  }else if(config_.useI2s&&caps_.i2sRtttl){
    if(buzzer&&!buzzer_){if(backend_.playRtttl)(void)backend_.playRtttl(backend_.context,ringtone_,ringtoneSize_,true);}
    else if(!buzzer&&buzzer_&&backend_.stopRtttl)backend_.stopRtttl(backend_.context);
  }else if(caps_.gpioBuzzer&&backend_.setBuzzer&&buzzer_!=buzzer)(void)backend_.setBuzzer(backend_.context,config_.buzzer,buzzer);
  buzzer_=buzzer;
}
bool ExtendedNotificationController::ring(bool begin){return backend_.playRtttl&&backend_.playRtttl(backend_.context,ringtone_,ringtoneSize_,begin);}
void ExtendedNotificationController::poll(uint32_t now){
  if(!armed_)return;
  const bool expired=stock::notification::deadlinePassed(now,cutoff_);
  if(config_.useI2s&&buzzer_&&backend_.playRtttl){
    if(!ring(false)&&(expired||!ring(true))){stop();return;}
  }else if(expired&&!(config_.usePwm&&buzzer_)){stop();return;}
  if(config_.usePwm&&buzzer_&&backend_.setPwmTone&&(!notePending_||stock::notification::deadlinePassed(now,noteCutoff_))){
    if(!ringParsed_)ringParsed_=RingtoneParser::parse(ringtone_,ringtoneSize_,ringSettings_);
    RingtoneParser::Note note{};
    if((expired&&(!notePending_||ringtoneCursor_==0))||!ringParsed_||
       !RingtoneParser::next(ringtone_,ringtoneSize_,ringSettings_,ringtoneCursor_,note)){stop();return;}
    (void)backend_.setPwmTone(backend_.context,note.frequencyHz,note.frequencyHz?50:0);
    notePending_=true;noteCutoff_=now+note.durationMs;
  }
  if(stock::notification::outputDue(now,lastToggle_,config_.outputMs)){lastToggle_=now;output(eventLed_&&!led_,eventVibrator_?!vibrator_:vibrator_,eventBuzzer_&&!config_.usePwm&&!config_.useI2s?!buzzer_:buzzer_);}
}
void ExtendedNotificationController::stop(){
  if(config_.useI2s&&buzzer_&&backend_.stopRtttl)backend_.stopRtttl(backend_.context);
  if(backend_.setPwmTone&&config_.usePwm)backend_.setPwmTone(backend_.context,0,0);
  if(backend_.setLed&&led_)backend_.setLed(backend_.context,false);
  if(backend_.setVibrator&&vibrator_)backend_.setVibrator(backend_.context,config_.vibrator,false);
  if(backend_.setBuzzer&&buzzer_&&!config_.usePwm&&!config_.useI2s)backend_.setBuzzer(backend_.context,config_.buzzer,false);
  armed_=led_=vibrator_=buzzer_=false;eventLed_=eventVibrator_=eventBuzzer_=false;ringtoneCursor_=0;noteCutoff_=0;notePending_=false;
}
}
