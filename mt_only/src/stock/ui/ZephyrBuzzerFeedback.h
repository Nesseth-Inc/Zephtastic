// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <zephyr/devicetree.h>
#include "BuzzerFeedback.h"
#if defined(CONFIG_PWM) && DT_NODE_HAS_STATUS(DT_ALIAS(ui_buzzer),okay)
#include <zephyr/drivers/pwm.h>
namespace mt::stock::ui::buzzer {
inline const pwm_dt_spec output=PWM_DT_SPEC_GET(DT_ALIAS(ui_buzzer));
inline BuzzerFeedback sequencer;
inline bool muted=false;
inline bool tone(uint16_t hz){
    if(!pwm_is_ready_dt(&output))return false;
    const uint32_t period=hz?1000000000u/hz:1000000u;
    return pwm_set_dt(&output,period,hz?period/2:0)==0;
}
inline void input(unsigned event,uint32_t now){sequencer.input(event,muted?1:0,false,now,tone);}
inline void privacy(bool silent){muted=silent;if(muted)sequencer.stop(tone);}
inline void poll(uint32_t now){sequencer.poll(now,tone);}
}
#else
namespace mt::stock::ui::buzzer {
inline void input(unsigned,uint32_t){}
inline void privacy(bool){}
inline void poll(uint32_t){}
}
#endif
