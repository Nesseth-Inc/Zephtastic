// SPDX-License-Identifier: GPL-3.0-only
#include <zephyr/devicetree.h>
#if DT_HAS_COMPAT_STATUS_OKAY(zephcore_external_watchdog)
#include "WatchdogThread.h"
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <errno.h>
#include <zephyr/kernel.h>

namespace {
#define EXT_WDT DT_COMPAT_GET_ANY_STATUS_OKAY(zephcore_external_watchdog)
BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(zephcore_external_watchdog)==1);
constexpr unsigned interval=DT_PROP(EXT_WDT,feed_interval_ms);
BUILD_ASSERT(interval>1);
const gpio_dt_spec wake=GPIO_DT_SPEC_GET(EXT_WDT,wake_gpios);
const gpio_dt_spec done=GPIO_DT_SPEC_GET(EXT_WDT,done_gpios);
struct WatchdogPins {
    bool configure() const {
        return gpio_is_ready_dt(&wake) && gpio_is_ready_dt(&::done) &&
               gpio_pin_configure_dt(&wake,GPIO_INPUT)==0 && gpio_pin_configure_dt(&::done,GPIO_OUTPUT_INACTIVE)==0;
    }
    bool done(bool high) const {return gpio_pin_set_dt(&::done,high)==0;}
    void delayMs(unsigned ms) const {k_msleep(ms);}
} pins;
void tick(k_work *);
K_WORK_DELAYABLE_DEFINE(feed,tick);
void tick(k_work *) {
    if(!mt::stock::watchdog::feedDog(pins))printk("External watchdog GPIO feed failed\n");
    k_work_reschedule(&feed,K_MSEC(interval));
}
int start() {
    if(!mt::stock::watchdog::setup(pins))return -EIO;
    k_work_reschedule(&feed,K_MSEC(interval));
    return 0;
}
SYS_INIT(start,APPLICATION,CONFIG_APPLICATION_INIT_PRIORITY);
}
#endif
