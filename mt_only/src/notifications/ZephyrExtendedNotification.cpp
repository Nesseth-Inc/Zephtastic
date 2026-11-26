// SPDX-License-Identifier: GPL-3.0-only
#include "ZephyrExtendedNotification.h"
#include "../stock/peripherals/ExternalNotificationModule.h"
#include "../modules/audio/AudioConfig.h"
#include "../modules/audio/ZephyrI2sAudio.h"
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>

#if DT_NODE_EXISTS(DT_ALIAS(notification_vibrator)) && DT_NODE_HAS_STATUS(DT_ALIAS(notification_vibrator),okay)
#define MT_NOTIFY_HAS_VIBRA 1
static const gpio_dt_spec mtNotifyVibra=GPIO_DT_SPEC_GET(DT_ALIAS(notification_vibrator),gpios);
#else
#define MT_NOTIFY_HAS_VIBRA 0
#endif
#if DT_NODE_EXISTS(DT_ALIAS(notification_buzzer)) && DT_NODE_HAS_STATUS(DT_ALIAS(notification_buzzer),okay)
#define MT_NOTIFY_HAS_BUZZER 1
static const gpio_dt_spec mtNotifyBuzzer=GPIO_DT_SPEC_GET(DT_ALIAS(notification_buzzer),gpios);
#else
#define MT_NOTIFY_HAS_BUZZER 0
#endif
#if DT_NODE_EXISTS(DT_ALIAS(notification_buzzer_pwm)) && DT_NODE_HAS_STATUS(DT_ALIAS(notification_buzzer_pwm),okay)
#define MT_NOTIFY_HAS_PWM 1
static const pwm_dt_spec mtNotifyPwm=PWM_DT_SPEC_GET(DT_ALIAS(notification_buzzer_pwm));
#else
#define MT_NOTIFY_HAS_PWM 0
#endif

namespace mt::notification {
ZephyrExtendedNotification::~ZephyrExtendedNotification(){stop();}
ExtendedNotificationCapabilities ZephyrExtendedNotification::availableCapabilities(const meshtastic_ModuleConfig &wire,const ExtendedNotificationCapabilities &allowed){
  ExtendedNotificationConfig c;if(!ExtendedNotificationConfig::fromWire(wire,c))return {};return availableCapabilities(c,allowed);
}
ExtendedNotificationCapabilities ZephyrExtendedNotification::availableCapabilities(const ExtendedNotificationConfig &c,const ExtendedNotificationCapabilities &allowed){
  ExtendedNotificationCapabilities caps{};if(!c.enabled)return caps;
#if MT_NOTIFY_HAS_VIBRA
  caps.vibrator=(!c.vibrator||c.vibrator==mtNotifyVibra.pin)&&gpio_is_ready_dt(&mtNotifyVibra);
#endif
#if MT_NOTIFY_HAS_BUZZER
  caps.gpioBuzzer=(!c.buzzer||c.buzzer==mtNotifyBuzzer.pin)&&gpio_is_ready_dt(&mtNotifyBuzzer);
#endif
#if MT_NOTIFY_HAS_PWM
  caps.pwmBuzzer=c.usePwm&&!c.buzzer&&pwm_is_ready_dt(&mtNotifyPwm);
#endif
  if(c.useI2s)caps.i2sRtttl=audio::ZephyrI2sAudio::availablePlaybackCapabilities().i2sPlayback;
  caps.led=caps.led&&allowed.led;caps.vibrator=caps.vibrator&&allowed.vibrator;caps.gpioBuzzer=caps.gpioBuzzer&&allowed.gpioBuzzer;
  caps.pwmBuzzer=caps.pwmBuzzer&&allowed.pwmBuzzer;caps.i2sRtttl=caps.i2sRtttl&&allowed.i2sRtttl;return caps;
}
ExtendedNotificationCapabilities ZephyrExtendedNotification::configure(const ExtendedNotificationConfig &c,audio::ZephyrI2sAudio *audio){
  return configureInternal(c,{true,true,true,true,true},audio);
}
ExtendedNotificationCapabilities ZephyrExtendedNotification::configure(const meshtastic_ModuleConfig &wire,const ExtendedNotificationCapabilities &allowed,audio::ZephyrI2sAudio *audio){
  ExtendedNotificationConfig c;if(!ExtendedNotificationConfig::fromWire(wire,c)){stop();return {};}
  return configureInternal(c,allowed,audio);
}
ExtendedNotificationCapabilities ZephyrExtendedNotification::configureInternal(const ExtendedNotificationConfig &c,const ExtendedNotificationCapabilities &allowed,audio::ZephyrI2sAudio *audio){
  stop();config_=c;audio_=audio;caps_={};if(!c.enabled)return caps_;
#if MT_NOTIFY_HAS_VIBRA
  if((!c.vibrator||c.vibrator==mtNotifyVibra.pin)&&gpio_is_ready_dt(&mtNotifyVibra)&&gpio_pin_configure_dt(&mtNotifyVibra,GPIO_OUTPUT_INACTIVE)==0)caps_.vibrator=true;
#endif
#if MT_NOTIFY_HAS_BUZZER
  if((!c.buzzer||c.buzzer==mtNotifyBuzzer.pin)&&gpio_is_ready_dt(&mtNotifyBuzzer)&&gpio_pin_configure_dt(&mtNotifyBuzzer,GPIO_OUTPUT_INACTIVE)==0)caps_.gpioBuzzer=true;
#endif
#if MT_NOTIFY_HAS_PWM
  // PWM is alias-owned, not selected through arbitrary output_buzzer GPIO IDs.
  // Reject a nonzero protobuf pin because the PWM controller owns pin muxing.
  if(c.usePwm&&!c.buzzer&&pwm_is_ready_dt(&mtNotifyPwm))caps_.pwmBuzzer=true;
#endif
  if(c.useI2s&&allowed.i2sRtttl&&audio_){audio::AudioConfig audioConfig{};if(audio_->configurePlayback(audioConfig))caps_.i2sRtttl=true;}
  caps_.led=caps_.led&&allowed.led;caps_.vibrator=caps_.vibrator&&allowed.vibrator;
  caps_.gpioBuzzer=caps_.gpioBuzzer&&allowed.gpioBuzzer;caps_.pwmBuzzer=caps_.pwmBuzzer&&allowed.pwmBuzzer;
  caps_.i2sRtttl=caps_.i2sRtttl&&allowed.i2sRtttl;
  if(((c.alertMessageVibra||c.alertBellVibra)&&!caps_.vibrator)||
     ((c.alertMessageBuzzer||c.alertBellBuzzer)&&!c.usePwm&&!c.useI2s&&!caps_.gpioBuzzer)||
     (c.usePwm&&!caps_.pwmBuzzer)||(c.useI2s&&!caps_.i2sRtttl)){
    stop();return {};
  }
  return caps_;
}
ExtendedNotificationBackend ZephyrExtendedNotification::backend(){return {this,ledThunk,vibraThunk,buzzerThunk,pwmThunk,rtttlThunk,stopThunk};}
bool ZephyrExtendedNotification::setLed(bool){return false;}
bool ZephyrExtendedNotification::setVibrator(uint8_t pin,bool on){
#if MT_NOTIFY_HAS_VIBRA
  if(!caps_.vibrator||pin&&pin!=mtNotifyVibra.pin)return false;const bool ok=gpio_pin_set_dt(&mtNotifyVibra,on)==0;if(ok)vibratorOn_=on;return ok;
#else
  (void)pin;(void)on;return false;
#endif
}
bool ZephyrExtendedNotification::setBuzzer(uint8_t pin,bool on){
#if MT_NOTIFY_HAS_BUZZER
  if(!caps_.gpioBuzzer||pin&&pin!=mtNotifyBuzzer.pin)return false;const bool ok=gpio_pin_set_dt(&mtNotifyBuzzer,on)==0;if(ok)buzzerOn_=on;return ok;
#else
  (void)pin;(void)on;return false;
#endif
}
bool ZephyrExtendedNotification::setPwmTone(uint16_t hz,uint8_t duty){
#if MT_NOTIFY_HAS_PWM
  if(!caps_.pwmBuzzer)return false;const uint32_t period=hz?1000000000u/hz:1000000u;return pwm_set_dt(&mtNotifyPwm,period,period*duty/100)==0;
#else
  (void)hz;(void)duty;return false;
#endif
}
bool ZephyrExtendedNotification::playRtttl(const char *ring,size_t len,bool begin){
  if(!caps_.i2sRtttl||!audio_)return false;
  if(begin){
    RingtoneParser::Settings parsed{};if(!ring||!RingtoneParser::parse(ring,len,parsed))return false;
    rtttlText_=ring;rtttlSize_=len;rtttlSettings_=parsed;rtttlCursor_=0;nextToneAt_=0;rtttlActive_=true;
  }
  if(!rtttlActive_||!rtttlText_)return false;
  const uint32_t now=k_uptime_get_32();
  if(!begin){
    if(!stock::notification::deadlinePassed(now,nextToneAt_))return true;
    if(!rtttlCursor_){stopRtttl();return false;}
  }
  RingtoneParser::Note note{};
  if(!RingtoneParser::next(rtttlText_,rtttlSize_,rtttlSettings_,rtttlCursor_,note)){stopRtttl();return false;}
  nextToneAt_=now+note.durationMs;return audio_->playTone(note.frequencyHz,35);
}
void ZephyrExtendedNotification::stopRtttl(){if(caps_.i2sRtttl&&audio_)audio_->playTone(0,0);setPwmTone(0,0);rtttlActive_=false;rtttlText_=nullptr;rtttlSize_=0;rtttlCursor_=0;nextToneAt_=0;}
void ZephyrExtendedNotification::stop(){stopRtttl();setVibrator(config_.vibrator,false);setBuzzer(config_.buzzer,false);setPwmTone(0,0);caps_={};vibratorOn_=buzzerOn_=false;audio_=nullptr;}
bool ZephyrExtendedNotification::ledThunk(void *p,bool on){return static_cast<ZephyrExtendedNotification*>(p)->setLed(on);}
bool ZephyrExtendedNotification::vibraThunk(void *p,uint8_t pin,bool on){return static_cast<ZephyrExtendedNotification*>(p)->setVibrator(pin,on);}
bool ZephyrExtendedNotification::buzzerThunk(void *p,uint8_t pin,bool on){return static_cast<ZephyrExtendedNotification*>(p)->setBuzzer(pin,on);}
bool ZephyrExtendedNotification::pwmThunk(void *p,uint16_t hz,uint8_t duty){return static_cast<ZephyrExtendedNotification*>(p)->setPwmTone(hz,duty);}
bool ZephyrExtendedNotification::rtttlThunk(void *p,const char *s,size_t n,bool begin){return static_cast<ZephyrExtendedNotification*>(p)->playRtttl(s,n,begin);}
void ZephyrExtendedNotification::stopThunk(void *p){static_cast<ZephyrExtendedNotification*>(p)->stopRtttl();}
}
