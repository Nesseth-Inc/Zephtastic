// SPDX-License-Identifier: GPL-3.0-only
#include "Accessories.h"
#include "AirAndSoil.h"
#include "ParticleAliases.h"
#include "Hm330x.h"
#include "Pmsa003i.h"
#if defined(CONFIG_MT_RAK12035) || defined(CONFIG_MT_PMSA003I)
#include <zephyr/drivers/gpio.h>
#endif
#include "Scd30.h"
#include "RangeAndRadiation.h"
#include "Ltr390.h"
#if defined(CONFIG_MT_OPT3001)
#include "Opt3001.h"
#endif
#if defined(CONFIG_MT_BH1750)
#include "Bh1750.h"
#endif
#if defined(CONFIG_MT_TSL_LIGHT)
#include "TslLight.h"
#endif
#if defined(CONFIG_MT_MLX90614)
#include "Mlx90614.h"
#endif
#include "Rainfall.h"
#if defined(CONFIG_MT_LARK_WEATHER)
#include "stock/optional/LarkWeather.h"
#endif
#if defined(CONFIG_MT_SCD4X_ADMIN)
#include "../ZephyrScd4xAdmin.h"
#endif
#include "Senxx.h"
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <pb_encode.h>
#include <pb_decode.h>

namespace mt::stock::telemetry::accessory {
namespace {
atomic_t sensorUtc=ATOMIC_INIT(0);
struct ZephyrBus : Bus {
    const i2c_dt_spec i2c;
    explicit ZephyrBus(i2c_dt_spec b):i2c(b){}
    int write(const uint8_t *p,size_t n)override{return i2c_write_dt(&i2c,p,n);}
    int read(uint8_t *p,size_t n)override{return i2c_read_dt(&i2c,p,n);}
    int readReg(uint8_t r,uint8_t *p,size_t n)override{return i2c_write_read_dt(&i2c,&r,1,p,n);}
#if DT_NODE_HAS_STATUS(DT_ALIAS(zt_ltr390),okay)
    int recover()override{return i2c_recover_bus(i2c.bus);}
#endif
    void wait(unsigned ms)override{k_msleep(ms);}
    uint64_t now()override{return k_uptime_get();}
    uint32_t utc()override{return uint32_t(atomic_get(&sensorUtc));}
    int load(const char *key,void *p,size_t n)override{
        ssize_t size=settings_get_val_len(key);
        if(size<=0)return size<0?int(size):-ENOENT;
        if(size_t(size)!=n)return -EINVAL;
        ssize_t bytes=settings_load_one(key,p,n);return bytes==size?0:bytes<0?int(bytes):-EIO;
    }
    int save(const char *key,const void *p,size_t n)override{
        uint8_t verify[64];
        if(n>sizeof(verify))return -E2BIG;
        int e=settings_save_one(key,p,n);
        if(!e){e=load(key,verify,n);if(!e&&memcmp(verify,p,n))e=-EIO;}
        return e;
    }
};
#define ACCESSORY_BUS(alias,bus) static ZephyrBus bus(I2C_DT_SPEC_GET(DT_ALIAS(alias)))
#define HAS_ACCESSORY(alias) DT_NODE_HAS_STATUS(DT_ALIAS(alias),okay)
#if HAS_ACCESSORY(zt_scd30)
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_scd30),sensirion_scd30),"unexpected compatible for zt_scd30");
ACCESSORY_BUS(zt_scd30,scdBus);Scd30 scd(scdBus);
#endif
#if HAS_ACCESSORY(zt_sen5x)
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_sen5x),sensirion_sen5x),"unexpected compatible for zt_sen5x");
ACCESSORY_BUS(zt_sen5x,sen5Bus);Senxx sen5(sen5Bus,false);
#endif
#if HAS_ACCESSORY(zt_sen6x)
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_sen6x),sensirion_sen6x),"unexpected compatible for zt_sen6x");
ACCESSORY_BUS(zt_sen6x,sen6Bus);Senxx sen6(sen6Bus,true);
#endif
#if HAS_ACCESSORY(zt_as3935)
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_as3935),ams_as3935),"unexpected compatible for zt_as3935");
ACCESSORY_BUS(zt_as3935,lightningBus);Lightning lightning(lightningBus);
#endif
#if HAS_ACCESSORY(zt_nau7802)
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_nau7802),nuvoton_nau7802),"unexpected compatible for zt_nau7802");
ACCESSORY_BUS(zt_nau7802,scaleBus);Scale scale(scaleBus);
#endif
#if HAS_ACCESSORY(zt_radsens)
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_radsens),climateguard_radsens));
ACCESSORY_BUS(zt_radsens,radiationBus);
#endif
#if HAS_ACCESSORY(zt_rcwl9620)
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_rcwl9620),rcwl_rcwl9620));
ACCESSORY_BUS(zt_rcwl9620,rangeBus);
#endif
#if HAS_ACCESSORY(zt_ltr390)
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_ltr390),liteon_ltr390));
ACCESSORY_BUS(zt_ltr390,lightBus);Ltr390 light(lightBus);
#endif
#if defined(CONFIG_MT_OPT3001)
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_opt3001),zephtastic_opt3001));
static_assert(DT_REG_ADDR(DT_ALIAS(zt_opt3001))==0x45 || DT_REG_ADDR(DT_ALIAS(zt_opt3001))==0x44);
#define OPT_DISTINCT(node) static_assert(!DT_SAME_NODE(DT_BUS(node),DT_BUS(DT_ALIAS(zt_opt3001))) || DT_REG_ADDR(node)!=DT_REG_ADDR(DT_ALIAS(zt_opt3001)), "two devices assigned one OPT3001 address");
DT_FOREACH_STATUS_OKAY(ti_opt3001,OPT_DISTINCT)
DT_FOREACH_STATUS_OKAY(sensirion_sht4x,OPT_DISTINCT)
DT_FOREACH_STATUS_OKAY(sensirion_sht3xd,OPT_DISTINCT)
#undef OPT_DISTINCT
ACCESSORY_BUS(zt_opt3001,optBus);Opt3001 opt(optBus);
int optGeneration=-1;
#endif
#if defined(CONFIG_MT_BH1750)
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_bh1750),zephtastic_bh1750) && DT_REG_ADDR(DT_ALIAS(zt_bh1750))==0x23);
#define BH_DISTINCT(node) static_assert(!DT_SAME_NODE(DT_BUS(node),DT_BUS(DT_ALIAS(zt_bh1750))) || DT_REG_ADDR(node)!=0x23,"two BH1750 owners at one address");
DT_FOREACH_STATUS_OKAY(rohm_bh1750,BH_DISTINCT)
#undef BH_DISTINCT
ACCESSORY_BUS(zt_bh1750,bhBus);Bh1750 bh(bhBus);
int bhGeneration=-1;
#endif
#if defined(CONFIG_MT_TSL_LIGHT)
#define TSL_DISTINCT(node, owned) static_assert(!DT_SAME_NODE(DT_BUS(node),DT_BUS(DT_ALIAS(owned))) || \
    DT_REG_ADDR(node)!=DT_REG_ADDR(DT_ALIAS(owned)), "two light owners share the same physical address");
#if HAS_ACCESSORY(zt_tsl2591)
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_tsl2591),zephtastic_tsl2591) && DT_REG_ADDR(DT_ALIAS(zt_tsl2591))==0x29);
#define TSL2591_DISTINCT(node) TSL_DISTINCT(node,zt_tsl2591)
DT_FOREACH_STATUS_OKAY(ams_tsl2591,TSL2591_DISTINCT)
DT_FOREACH_STATUS_OKAY(ams_tsl2561,TSL2591_DISTINCT)
#undef TSL2591_DISTINCT
ACCESSORY_BUS(zt_tsl2591,wideLightBus);TslLight<true> wideLight(wideLightBus);
#endif
#if HAS_ACCESSORY(zt_tsl2561)
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_tsl2561),zephtastic_tsl2561) && DT_REG_ADDR(DT_ALIAS(zt_tsl2561))==0x29);
#define TSL2561_DISTINCT(node) TSL_DISTINCT(node,zt_tsl2561)
DT_FOREACH_STATUS_OKAY(ams_tsl2591,TSL2561_DISTINCT)
DT_FOREACH_STATUS_OKAY(ams_tsl2561,TSL2561_DISTINCT)
#undef TSL2561_DISTINCT
#if HAS_ACCESSORY(zt_tsl2591)
TSL_DISTINCT(DT_ALIAS(zt_tsl2591),zt_tsl2561)
#endif
ACCESSORY_BUS(zt_tsl2561,flatLightBus);TslLight<false> flatLight(flatLightBus);
#endif
#undef TSL_DISTINCT
int tslGeneration=-1;
#endif
#if defined(CONFIG_MT_MLX90614)
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_mlx90614), zephtastic_mlx90614));
static_assert(DT_REG_ADDR(DT_ALIAS(zt_mlx90614)) == 0x5a);
static_assert(DT_PROP(DT_BUS(DT_ALIAS(zt_mlx90614)), clock_frequency) == 100000, "MLX90614 requires standard-mode SMBus");
ACCESSORY_BUS(zt_mlx90614, mlxBus);
Mlx90614 mlx(mlxBus);
atomic_t healthEnabled=ATOMIC_INIT(0), healthEpoch=ATOMIC_INIT(0);
int mlxGeneration=-1;
#endif
#if defined(CONFIG_MT_VEML7700)
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_veml7700), zephtastic_veml7700));
static_assert(DT_REG_ADDR(DT_ALIAS(zt_veml7700)) == 0x10);
#define VEML_DISTINCT(node) static_assert(!DT_SAME_NODE(DT_BUS(node), DT_BUS(DT_ALIAS(zt_veml7700))) || \
    DT_REG_ADDR(node) != 0x10, "VEML7700 must have one physical I2C owner");
DT_FOREACH_STATUS_OKAY(vishay_veml7700, VEML_DISTINCT)
#if HAS_ACCESSORY(zt_environment) && DT_ON_BUS(DT_ALIAS(zt_environment), i2c)
VEML_DISTINCT(DT_ALIAS(zt_environment))
#endif
#if HAS_ACCESSORY(zt_environment_2) && DT_ON_BUS(DT_ALIAS(zt_environment_2), i2c)
VEML_DISTINCT(DT_ALIAS(zt_environment_2))
#endif
#undef VEML_DISTINCT
ACCESSORY_BUS(zt_veml7700, opticalBus);
Veml7700 optical(opticalBus);
int opticalGeneration = -1;
#endif
#if defined(CONFIG_MT_ADS1X15)
#define ADS_DEVICE(alias, name, alt) \
    static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(alias), zephtastic_ads1015) || \
                  DT_NODE_HAS_COMPAT(DT_ALIAS(alias), zephtastic_ads1115), "expected ADS1015 or ADS1115"); \
    static_assert((alt) ? (DT_REG_ADDR(DT_ALIAS(alias)) >= 0x49 && DT_REG_ADDR(DT_ALIAS(alias)) <= 0x4b) : \
                         DT_REG_ADDR(DT_ALIAS(alias)) == 0x48, "unexpected stock ADS1X15 address group"); \
    static_assert(DT_PROP(DT_BUS(DT_ALIAS(alias)), clock_frequency) == 100000, "stock ADS requires a 100 kHz bus"); \
    ACCESSORY_BUS(alias, name##Bus); \
    Ads1x15 name(name##Bus, DT_NODE_HAS_COMPAT(DT_ALIAS(alias), zephtastic_ads1015))
#if HAS_ACCESSORY(zt_ads1x15)
ADS_DEVICE(zt_ads1x15, primaryAdc, false);
#endif
#if HAS_ACCESSORY(zt_ads1x15_alt)
ADS_DEVICE(zt_ads1x15_alt, alternateAdc, true);
#endif
#undef ADS_DEVICE
#endif
#if HAS_ACCESSORY(zt_rainfall)
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_rainfall),dfrobot_sen0575));
ACCESSORY_BUS(zt_rainfall,rainBus);Rainfall rain(rainBus);
#endif
#if HAS_ACCESSORY(zt_sfa30)
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_sfa30), sensirion_sfa30));
static_assert(DT_REG_ADDR(DT_ALIAS(zt_sfa30)) == 0x5d && DT_PROP(DT_BUS(DT_ALIAS(zt_sfa30)), clock_frequency) == 100000);
ACCESSORY_BUS(zt_sfa30, sfaBus);
Sfa30 sfa(sfaBus);
#endif
#if defined(CONFIG_MT_RAK12035)
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_rak12035), rakwireless_rak12035));
static_assert(DT_REG_ADDR(DT_ALIAS(zt_rak12035)) == 0x20);
ACCESSORY_BUS(zt_rak12035, soilBus);
constexpr gpio_dt_spec soilResetPin = GPIO_DT_SPEC_GET(DT_ALIAS(zt_rak12035), reset_gpios);
static_assert(soilResetPin.dt_flags & GPIO_ACTIVE_LOW, "RAK12035 reset must be active-low");
int soilReset(bool asserted) {
    if (!gpio_is_ready_dt(&soilResetPin)) return -ENODEV;
    return gpio_pin_configure_dt(&soilResetPin, asserted ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE);
}
Rak12035 soil(soilBus, soilReset);
int soilGeneration = -1;
#endif
#if defined(CONFIG_MT_LARK_WEATHER)
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_lark),dfrobot_lark));
static_assert(DT_PROP(DT_BUS(DT_ALIAS(zt_lark)),clock_frequency)==100000,"Lark requires a 100 kHz bus");
ACCESSORY_BUS(zt_lark,larkBus);
#endif
#if defined(MT_HM_ALIAS)
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(MT_HM_ALIAS), seeed_hm330x) ||
              DT_NODE_HAS_COMPAT(DT_ALIAS(MT_HM_ALIAS), zephtastic_hm330x));
#if HAS_ACCESSORY(zt_hm330x)
static_assert(DT_PROP(DT_BUS(DT_ALIAS(zt_hm330x)), clock_frequency) == 400000);
#endif
static_assert(DT_REG_ADDR(DT_ALIAS(MT_HM_ALIAS)) == 0x40);
ACCESSORY_BUS(MT_HM_ALIAS, hmBus);
Hm330x hm(hmBus);
#endif
#if defined(MT_PMS_ALIAS)
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(MT_PMS_ALIAS), plantower_pmsa003i) ||
              DT_NODE_HAS_COMPAT(DT_ALIAS(MT_PMS_ALIAS), zephtastic_pmsa003i));
#if HAS_ACCESSORY(zt_pmsa003i)
static_assert(DT_PROP(DT_BUS(DT_ALIAS(zt_pmsa003i)), clock_frequency) == 100000);
#endif
static_assert(DT_REG_ADDR(DT_ALIAS(MT_PMS_ALIAS)) == 0x12);
ACCESSORY_BUS(MT_PMS_ALIAS, pmsBus);
#if DT_NODE_HAS_PROP(DT_ALIAS(MT_PMS_ALIAS), set_gpios)
constexpr gpio_dt_spec pmsEnable=GPIO_DT_SPEC_GET(DT_ALIAS(MT_PMS_ALIAS), set_gpios);
static_assert(!(pmsEnable.dt_flags & GPIO_ACTIVE_LOW), "PMSA003I SET is active high");
int pmsSet(bool on) {
    if (!gpio_is_ready_dt(&pmsEnable)) return -ENODEV;
    return gpio_pin_configure_dt(&pmsEnable,on?GPIO_OUTPUT_ACTIVE:GPIO_OUTPUT_INACTIVE);
}
Pmsa003i pms(pmsBus,pmsSet);
#else
Pmsa003i pms(pmsBus,nullptr);
#endif
atomic_t airPollAt=ATOMIC_INIT(0),hasAirPoll=ATOMIC_INIT(0);
int pmsGeneration=-1;
#endif
K_SEM_DEFINE(wake,0,1);
K_MUTEX_DEFINE(cacheMutex);
atomic_t enabledAir=ATOMIC_INIT(0),enabledEnv=ATOMIC_INIT(0),interval=ATOMIC_INIT(900);
atomic_t airEpoch=ATOMIC_INIT(0),envEpoch=ATOMIC_INIT(0);
enum WorkState {Idle,Pending,Finished};
atomic_t workState=ATOMIC_INIT(Idle);
struct Request {
    uint32_t session=0,id=0,kg=0;bool scale=false;
    // Only one nested config can be present. SEN6X is the largest accepted one.
    uint8_t wire[2+meshtastic_SEN6X_config_size]{};uint8_t size=0;
    int result=0;
    bool sameId(const Request &r)const{return id&&session==r.session&&id==r.id;}
    bool sameBody(const Request &r)const{return scale==r.scale&&kg==r.kg&&size==r.size&&!memcmp(wire,r.wire,size);}
};
Request current{},last{};
int execute(const Request &r){
#if HAS_ACCESSORY(zt_nau7802)
    if(r.scale)return scale.set(r.kg);
#endif
    meshtastic_SensorConfig q{};auto stream=pb_istream_from_buffer(r.wire,r.size);
    if(!pb_decode(&stream,meshtastic_SensorConfig_fields,&q))return -EINVAL;
#if defined(CONFIG_MT_SCD4X_ADMIN)
    if(q.has_scd4x_config)return scd4x::applySync(q);
#endif
#if HAS_ACCESSORY(zt_scd30)
    if(q.has_scd30_config)return scd.apply(q.scd30_config);
#endif
#if HAS_ACCESSORY(zt_sen5x)
    if(q.has_sen5x_config)return sen5.apply(q);
#endif
#if HAS_ACCESSORY(zt_sen6x)
    if(q.has_sen6x_config)return sen6.apply(q);
#endif
#if HAS_ACCESSORY(zt_as3935)
    if(q.has_as3935_config&&q.as3935_config.has_set_tuning_cap_pf)return lightning.tune(q.as3935_config.set_tuning_cap_pf);
#endif
    return -ENOTSUP;
}
template<class T> struct Cache {
    T value{};uint64_t at=0;bool valid=false;
    void publish(const T &v,int result,const atomic_t &epoch,int generation){
        if(result==-EAGAIN||result==-EBUSY)return;
        k_mutex_lock(&cacheMutex,K_FOREVER);
        if(atomic_get(&epoch)==generation){valid=result==0;if(valid){value=v;at=k_uptime_get();}}
        k_mutex_unlock(&cacheMutex);
    }
    bool fresh(uint64_t now)const{return valid&&now>=at&&now-at<=90000;}
};
#if HAS_ACCESSORY(zt_scd30)
Cache<meshtastic_AirQualityMetrics> co2Cache;
#endif
#if HAS_ACCESSORY(zt_sen5x)
Cache<meshtastic_AirQualityMetrics> sen5Cache;
#endif
#if HAS_ACCESSORY(zt_sen6x)
Cache<meshtastic_AirQualityMetrics> sen6Cache;
#endif
#if HAS_ACCESSORY(zt_sfa30)
Cache<Formaldehyde> sfaCache;
#endif
#if defined(MT_HM_ALIAS)
Cache<StandardPm> hmCache;
#endif
#if defined(MT_PMS_ALIAS)
Cache<PlantowerPm> pmsCache;
#endif
// Each accessory retains its own freshness and bus failure state.
#if defined(CONFIG_MT_STOCK_BAROMETERS)
template<class Sensor> struct Barometer {
    Sensor sensor;
    Cache<BarometerSample> cache;
    explicit Barometer(Bus &bus) : sensor(bus) {}
    void collect(int generation) {
        BarometerSample value;
        const int result = sensor.measure(value);
        cache.publish(value, result, envEpoch, generation);
    }
};
// Refuse a second driver on the same physical I2C address, including the T096
// generic probes and other mutually exclusive 0x76/0x77 accessory options.
#define BAROMETER_DISTINCT(node) static_assert(DT_SAME_NODE(node, DT_ALIAS(BAROMETER_ALIAS)) || \
    !DT_SAME_NODE(DT_BUS(node), DT_BUS(DT_ALIAS(BAROMETER_ALIAS))) || \
    DT_REG_ADDR(node) != DT_REG_ADDR(DT_ALIAS(BAROMETER_ALIAS)), "Disable the colliding environment sensor node");
#define CHECK_BAROMETER_OWNERS() DT_FOREACH_STATUS_OKAY(aosong_aht20, BAROMETER_DISTINCT) \
    DT_FOREACH_STATUS_OKAY(aosong_dht20, BAROMETER_DISTINCT) \
    DT_FOREACH_STATUS_OKAY(aosong_am2301b, BAROMETER_DISTINCT) \
    DT_FOREACH_STATUS_OKAY(bosch_bmp180, BAROMETER_DISTINCT) \
    DT_FOREACH_STATUS_OKAY(bosch_bme280, BAROMETER_DISTINCT) \
    DT_FOREACH_STATUS_OKAY(bosch_bmp388, BAROMETER_DISTINCT) \
    DT_FOREACH_STATUS_OKAY(bosch_bmp390, BAROMETER_DISTINCT) \
    DT_FOREACH_STATUS_OKAY(bosch_bme680, BAROMETER_DISTINCT) \
    DT_FOREACH_STATUS_OKAY(infineon_dps310, BAROMETER_DISTINCT) \
    DT_FOREACH_STATUS_OKAY(goertek_spa06, BAROMETER_DISTINCT) \
    DT_FOREACH_STATUS_OKAY(zephtastic_aht, BAROMETER_DISTINCT) \
    DT_FOREACH_STATUS_OKAY(zephtastic_bmp085, BAROMETER_DISTINCT) \
    DT_FOREACH_STATUS_OKAY(zephtastic_dps310, BAROMETER_DISTINCT) \
    DT_FOREACH_STATUS_OKAY(zephtastic_bmp3xx, BAROMETER_DISTINCT)
#if HAS_ACCESSORY(zt_aht)
#define BAROMETER_ALIAS zt_aht
CHECK_BAROMETER_OWNERS()
#undef BAROMETER_ALIAS
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_aht), zephtastic_aht), "Unexpected aht compatible");
static_assert(DT_REG_ADDR(DT_ALIAS(zt_aht)) == 0x38, "Unexpected aht address");
ACCESSORY_BUS(zt_aht, baro_ahtBus);
Barometer<AhtSensor> baro_aht(baro_ahtBus);
#endif
#if HAS_ACCESSORY(zt_bmp085)
#define BAROMETER_ALIAS zt_bmp085
CHECK_BAROMETER_OWNERS()
#undef BAROMETER_ALIAS
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_bmp085), zephtastic_bmp085), "Unexpected bmp085 compatible");
static_assert(DT_REG_ADDR(DT_ALIAS(zt_bmp085)) == 0x77, "Unexpected bmp085 address");
ACCESSORY_BUS(zt_bmp085, baro_bmp085Bus);
Barometer<Bmp085> baro_bmp085(baro_bmp085Bus);
#endif
#if HAS_ACCESSORY(zt_dps310)
#define BAROMETER_ALIAS zt_dps310
CHECK_BAROMETER_OWNERS()
#undef BAROMETER_ALIAS
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_dps310), zephtastic_dps310), "Unexpected dps310 compatible");
static_assert(DT_REG_ADDR(DT_ALIAS(zt_dps310)) == 0x76 || DT_REG_ADDR(DT_ALIAS(zt_dps310)) == 0x77, "Unexpected dps310 address");
ACCESSORY_BUS(zt_dps310, baro_dps310Bus);
Barometer<DpsSensor> baro_dps310(baro_dps310Bus);
#endif
#if HAS_ACCESSORY(zt_bmp3xx)
#define BAROMETER_ALIAS zt_bmp3xx
CHECK_BAROMETER_OWNERS()
#undef BAROMETER_ALIAS
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_bmp3xx), zephtastic_bmp3xx), "Unexpected bmp3xx compatible");
static_assert(DT_REG_ADDR(DT_ALIAS(zt_bmp3xx)) == 0x76 || DT_REG_ADDR(DT_ALIAS(zt_bmp3xx)) == 0x77, "Unexpected bmp3xx address");
ACCESSORY_BUS(zt_bmp3xx, baro_bmp3xxBus);
Barometer<Bmp3> baro_bmp3xx(baro_bmp3xxBus);
#endif
#undef CHECK_BAROMETER_OWNERS
#undef BAROMETER_DISTINCT
template<class F> void eachBarometer(F f) {
#if HAS_ACCESSORY(zt_aht)
    f(baro_aht);
#endif
#if HAS_ACCESSORY(zt_bmp085)
    f(baro_bmp085);
#endif
#if HAS_ACCESSORY(zt_dps310)
    f(baro_dps310);
#endif
#if HAS_ACCESSORY(zt_bmp3xx)
    f(baro_bmp3xx);
#endif
}
int barometerGeneration = -1;
#endif
#if defined(CONFIG_MT_STOCK_SHT)
#if HAS_ACCESSORY(zt_sht44)
#define SHT_FIRST zt_sht44
#elif HAS_ACCESSORY(zt_sht40)
#define SHT_FIRST zt_sht40
#elif HAS_ACCESSORY(zt_sht45)
#define SHT_FIRST zt_sht45
#else
#define SHT_FIRST zt_sht70
#endif
#define SHT_BUS_CHECK() static_assert(DT_SAME_NODE(DT_BUS(DT_ALIAS(SHT_FIRST)),DT_BUS(DT_ALIAS(SHT_SLOT))), \
    "Stock SHTXX selects one sensor on one bus");
#define SHT_NATIVE_ALIAS(n) (DT_NODE_HAS_COMPAT(DT_ALIAS(n),sensirion_sht3xd) || \
    DT_NODE_HAS_COMPAT(DT_ALIAS(n),sensirion_sht4x) || DT_NODE_HAS_COMPAT(DT_ALIAS(n),sensirion_shtc1) || \
    DT_NODE_HAS_COMPAT(DT_ALIAS(n),sensirion_shtc3))
#if HAS_ACCESSORY(zt_environment)
static_assert(!SHT_NATIVE_ALIAS(zt_environment), "Use one stock SHTXX producer");
#endif
#if HAS_ACCESSORY(zt_environment_2)
static_assert(!SHT_NATIVE_ALIAS(zt_environment_2), "Use one stock SHTXX producer");
#endif
#undef SHT_NATIVE_ALIAS
// Reject two enabled drivers for one electrical accessory.
#define SHT_DISTINCT(node) static_assert(DT_SAME_NODE(node,DT_ALIAS(SHT_SLOT)) || \
    !DT_SAME_NODE(DT_BUS(node),DT_BUS(DT_ALIAS(SHT_SLOT))) || \
    DT_REG_ADDR(node)!=DT_REG_ADDR(DT_ALIAS(SHT_SLOT)), "Disable the other owner of this SHT address");
#define SHT_CHECK() DT_FOREACH_STATUS_OKAY(sensirion_shtc1,SHT_DISTINCT) \
  DT_FOREACH_STATUS_OKAY(sensirion_shtc3,SHT_DISTINCT) \
  DT_FOREACH_STATUS_OKAY(sensirion_sht3xd,SHT_DISTINCT) \
  DT_FOREACH_STATUS_OKAY(sensirion_sht4x,SHT_DISTINCT) \
  DT_FOREACH_STATUS_OKAY(ti_ina219,SHT_DISTINCT) \
  DT_FOREACH_STATUS_OKAY(ti_ina226,SHT_DISTINCT) \
  DT_FOREACH_STATUS_OKAY(ti_ina260,SHT_DISTINCT) \
  DT_FOREACH_STATUS_OKAY(ti_opt3001,SHT_DISTINCT) \
  DT_FOREACH_STATUS_OKAY(zephtastic_opt3001,SHT_DISTINCT) \
  DT_FOREACH_STATUS_OKAY(seeed_hm330x,SHT_DISTINCT)
#if HAS_ACCESSORY(zt_sht40)
#define SHT_SLOT zt_sht40
SHT_CHECK()
SHT_BUS_CHECK()
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(SHT_SLOT),zephtastic_sht_family) && DT_REG_ADDR(DT_ALIAS(SHT_SLOT))==0x40);
ACCESSORY_BUS(zt_sht40, shtBus40);
#define SHT_BUS40 &shtBus40
#undef SHT_SLOT
#else
#define SHT_BUS40 nullptr
#endif
#if HAS_ACCESSORY(zt_sht44)
#define SHT_SLOT zt_sht44
SHT_CHECK()
SHT_BUS_CHECK()
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(SHT_SLOT),zephtastic_sht_family) && DT_REG_ADDR(DT_ALIAS(SHT_SLOT))==0x44);
ACCESSORY_BUS(zt_sht44, shtBus44);
#define SHT_BUS44 &shtBus44
#undef SHT_SLOT
#else
#define SHT_BUS44 nullptr
#endif
#if HAS_ACCESSORY(zt_sht45)
#define SHT_SLOT zt_sht45
SHT_CHECK()
SHT_BUS_CHECK()
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(SHT_SLOT),zephtastic_sht_family) && DT_REG_ADDR(DT_ALIAS(SHT_SLOT))==0x45);
ACCESSORY_BUS(zt_sht45, shtBus45);
#define SHT_BUS45 &shtBus45
#undef SHT_SLOT
#else
#define SHT_BUS45 nullptr
#endif
#if HAS_ACCESSORY(zt_sht70)
#define SHT_SLOT zt_sht70
SHT_CHECK()
SHT_BUS_CHECK()
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(SHT_SLOT),zephtastic_sht_family) && DT_REG_ADDR(DT_ALIAS(SHT_SLOT))==0x70);
ACCESSORY_BUS(zt_sht70, shtBus70);
#define SHT_BUS70 &shtBus70
#undef SHT_SLOT
#else
#define SHT_BUS70 nullptr
#endif
#undef SHT_DISTINCT
#undef SHT_CHECK
#undef SHT_FIRST
#undef SHT_BUS_CHECK
StockSht sht(SHT_BUS44,SHT_BUS40,SHT_BUS45,SHT_BUS70);
Cache<ShtSample> shtCache;
atomic_t shtModel=ATOMIC_INIT(0),shtAccuracy=ATOMIC_INIT(2);
int shtGeneration=-1;
#endif
Cache<Environment> envCache;
#if defined(CONFIG_MT_MLX90614)
Cache<float> objectCache;
#endif
#if defined(CONFIG_MT_VEML7700)
Cache<VemlLight> opticalCache;
#endif
#if defined(CONFIG_MT_LARK_WEATHER)
using mt::stock::optional::LarkWeather;
using mt::stock::optional::StationWeather;
LarkWeather larkCycle;
StationWeather larkNext{},larkLatest{};
mt::stock::optional::Conditions dropLatest{};
uint64_t larkTime=0,dropTime=0;
bool dropWantsWeather=false,dropComplete=false;int larkGeneration=-1;
void collectWeather(){
    k_mutex_lock(&cacheMutex,K_FOREVER);
    const bool env=atomic_get(&enabledEnv);
    const bool needed=env||dropWantsWeather;
    const int generation=atomic_get(&envEpoch);
    k_mutex_unlock(&cacheMutex);
    if(!needed||generation!=larkGeneration){larkCycle={};larkNext={};larkGeneration=generation;}
    if(!needed)return;
    const auto write=[](const uint8_t *p,size_t n){return !larkBus.write(p,n);};
    const auto read=[](uint8_t *p,size_t n){return !larkBus.read(p,n);};
    const uint64_t now=k_uptime_get();
    const bool hadFour=larkNext.hasWeather;
    const bool done=env?larkCycle.pollEnvironment(uint32_t(now),larkNext,write,read):
                        larkCycle.poll(uint32_t(now),larkNext,write,read);
    if (done || larkNext.hasWeather) {
        k_mutex_lock(&cacheMutex,K_FOREVER);
        if(dropWantsWeather && !hadFour){
            dropLatest=larkNext;dropTime=now;dropComplete=true;dropWantsWeather=false;
        }
        if(done && env && generation==atomic_get(&envEpoch)){
            larkLatest=larkNext;larkTime=now;
        }
        k_mutex_unlock(&cacheMutex);
    }
    if(done){larkCycle={};larkNext={};}
}
void copyWeather(Environment &out,const mt::stock::optional::Conditions &weather){
    out.hasWeather=weather.hasWeather;
    if(!weather.hasWeather)return;
    out.temperature=weather.celsius;out.pressure=weather.pressureHpa;
    out.wind=weather.windMps;out.bearing=weather.bearing;
}
#endif
void collect(){
#if defined(CONFIG_MT_MLX90614)
    const int generation = atomic_get(&healthEpoch);
    if (generation != mlxGeneration) { mlx.stop(); mlxGeneration = generation; }
    if (atomic_get(&healthEnabled)) {
        float celsius = 0;
        int result = mlx.measure(celsius);
        objectCache.publish(celsius, result, healthEpoch, generation);
    }
#endif
#if defined(CONFIG_MT_LARK_WEATHER)
    collectWeather();
#endif
#if HAS_ACCESSORY(zt_sen5x)
    sen5.maintenance();
#endif
#if HAS_ACCESSORY(zt_sen6x)
    sen6.maintenance();
#endif
    const int airGeneration=atomic_get(&airEpoch);
#if defined(MT_PMS_ALIAS)
    if (pmsGeneration!=airGeneration) { pms.stop(); pmsGeneration=airGeneration; }
#endif
    if(atomic_get(&enabledAir)){
#if defined(MT_PMS_ALIAS)
        PlantowerPm dust{};
        const int pmsStatus=pms.measure(dust,atomic_get(&interval),uint32_t(atomic_get(&airPollAt)),atomic_get(&hasAirPoll));
        pmsCache.publish(dust,pmsStatus,airEpoch,airGeneration);
#endif
#if defined(MT_HM_ALIAS)
        StandardPm fine{};
        int status = hm.measure(fine);
        hmCache.publish(fine, status, airEpoch, airGeneration);
#endif
#if HAS_ACCESSORY(zt_sfa30)
        Formaldehyde sample;
        int result = sfa.measure(sample);
        sfaCache.publish(sample, result, airEpoch, airGeneration);
#endif
#if HAS_ACCESSORY(zt_scd30)
        meshtastic_AirQualityMetrics c{};int ec=scd.measure(c);co2Cache.publish(c,ec,airEpoch,airGeneration);
#endif
#if HAS_ACCESSORY(zt_sen5x)
        meshtastic_AirQualityMetrics f{};int ef=sen5.measure(f,atomic_get(&interval));sen5Cache.publish(f,ef,airEpoch,airGeneration);
#endif
#if HAS_ACCESSORY(zt_sen6x)
        meshtastic_AirQualityMetrics s{};int es=sen6.measure(s,atomic_get(&interval));sen6Cache.publish(s,es,airEpoch,airGeneration);
#endif
    }
    else {
#if defined(MT_PMS_ALIAS)
        pms.stop();
#endif
#if defined(MT_HM_ALIAS)
        hm.stop();
#endif
#if HAS_ACCESSORY(zt_sfa30)
        sfa.stop();
#endif
#if HAS_ACCESSORY(zt_scd30)
        scd.stop();
#endif
#if HAS_ACCESSORY(zt_sen5x)
        sen5.stop();
#endif
#if HAS_ACCESSORY(zt_sen6x)
        sen6.stop();
#endif
    }
    const int envGeneration=atomic_get(&envEpoch);
#if defined(CONFIG_MT_STOCK_SHT)
    if (envGeneration!=shtGeneration) {
        sht.stop();
        shtGeneration=envGeneration;
    }
    if (atomic_get(&enabledEnv)) {
        ShtSample value;
        const int rc=sht.measure(atomic_get(&shtAccuracy),value);
        shtCache.publish(value,rc,envEpoch,envGeneration);
        k_mutex_lock(&cacheMutex,K_FOREVER);
        if (envGeneration==atomic_get(&envEpoch)) atomic_set(&shtModel,int(sht.model()));
        k_mutex_unlock(&cacheMutex);
    }
#endif
#if defined(CONFIG_MT_RAK12035)
    if (envGeneration != soilGeneration) {
        soil.stop();
        soilGeneration = envGeneration;
    }
#endif
#if defined(CONFIG_MT_VEML7700)
    if (envGeneration != opticalGeneration || !atomic_get(&enabledEnv)) {
        optical.stop();
        opticalGeneration = envGeneration;
    }
#endif
#if defined(CONFIG_MT_TSL_LIGHT)
    if (envGeneration != tslGeneration || !atomic_get(&enabledEnv)) {
#if HAS_ACCESSORY(zt_tsl2591)
        wideLight.stop();
#endif
#if HAS_ACCESSORY(zt_tsl2561)
        flatLight.stop();
#endif
        tslGeneration=envGeneration;
    }
#endif
#if defined(CONFIG_MT_STOCK_BAROMETERS)
    if (envGeneration != barometerGeneration || !atomic_get(&enabledEnv)) {
        eachBarometer([](auto &part) { part.sensor.stop(); });
        barometerGeneration = envGeneration;
    }
#endif
#if defined(CONFIG_MT_BH1750)
    if (envGeneration!=bhGeneration || !atomic_get(&enabledEnv)) { bh.stop();bhGeneration=envGeneration; }
#endif
#if defined(CONFIG_MT_OPT3001)
    if (envGeneration!=optGeneration || !atomic_get(&enabledEnv)) { opt.stop();optGeneration=envGeneration; }
#endif
    if(atomic_get(&enabledEnv)){
        Environment e;
#if defined(CONFIG_MT_TSL_LIGHT) && HAS_ACCESSORY(zt_tsl2591)
        e.hasTslLux=wideLight.sample(e.tslLux);
#endif
#if defined(CONFIG_MT_OPT3001)
        e.hasOptLux=opt.sample(e.optLux);
#endif
#if defined(CONFIG_MT_TSL_LIGHT) && HAS_ACCESSORY(zt_tsl2561)
        if(flatLight.sample(e.tslLux)) {
            e.hasTslLux=true;
            e.hasTsl2561Lux=true;
        }
#endif
#if defined(CONFIG_MT_BH1750)
        e.hasBhLux=bh.sample(e.bhLux);
#endif
#if defined(CONFIG_MT_STOCK_BAROMETERS)
        eachBarometer([&](auto &part) { part.collect(envGeneration); });
#endif
#if defined(CONFIG_MT_VEML7700)
        VemlLight sample;
        int result = optical.poll(sample);
        opticalCache.publish(sample, result, envEpoch, envGeneration);
#endif
#if defined(CONFIG_MT_RAK12035)
        e.soil = soil.measure();
#endif
#if HAS_ACCESSORY(zt_as3935)
        (void)lightning.measure(e);
#endif
#if HAS_ACCESSORY(zt_nau7802)
        (void)scale.measure(e);
#endif
#if HAS_ACCESSORY(zt_radsens)
        e.hasRadiation=staticRadiation(radiationBus,e.radiation);
#endif
#if HAS_ACCESSORY(zt_rcwl9620)
        e.hasRange=ultrasonicRange(rangeBus,e.rangeMm);
#endif
#if HAS_ACCESSORY(zt_ltr390)
        e.hasLight=light.sample(e.lux,e.uvIndex);
#endif
#if defined(CONFIG_MT_ADS1X15) && HAS_ACCESSORY(zt_ads1x15)
        primaryAdc.sample(false, e.adc);
#endif
#if defined(CONFIG_MT_ADS1X15) && HAS_ACCESSORY(zt_ads1x15_alt)
        alternateAdc.sample(true, e.adc);
#endif
#if HAS_ACCESSORY(zt_rainfall)
        e.hasRain=rain.sample(e.rainHour,e.rainDay);
#endif
        const bool measured=e.hasRain||e.hasStrikes||e.hasWeight||e.hasRadiation||e.hasRange||e.hasLight
#if defined(CONFIG_MT_ADS1X15)
            || e.adc.mask
#endif
#if defined(CONFIG_MT_TSL_LIGHT)
            || e.hasTslLux
#endif
#if defined(CONFIG_MT_OPT3001)
            || e.hasOptLux
#endif
#if defined(CONFIG_MT_BH1750)
            || e.hasBhLux
#endif
#if defined(CONFIG_MT_RAK12035)
            || e.soil.valid
#endif
            ;
        envCache.publish(e,measured?0:-ENODATA,envEpoch,envGeneration);
    }
#if HAS_ACCESSORY(zt_ltr390)
    else light.stop();
#endif
}
void worker(void*,void*,void*){
    uint64_t next=0;
    for(;;){
        k_sem_take(&wake,K_SECONDS(1));
        if(atomic_get(&workState)==Pending){
            current.result=execute(current);atomic_set(&workState,Finished);
        }
        uint64_t now=k_uptime_get();
        if(now>=next){collect();next=now+1000;}
    }
}
K_THREAD_DEFINE(accessoryThread,2304,worker,nullptr,nullptr,nullptr,10,0,0);
int enqueue(const Request &r){
    const auto state=atomic_get(&workState);
    if(state!=Idle)return current.sameId(r)?(current.sameBody(r)?0:-EINVAL):-EBUSY;
    if(last.sameId(r)){
        if(!last.sameBody(r))return -EINVAL;
        current=last;atomic_set(&workState,Finished);return 0;
    }
    current=r;atomic_set(&workState,Pending);k_sem_give(&wake);return 0;
}
void mergeAir(meshtastic_AirQualityMetrics &out,const meshtastic_AirQualityMetrics &v){
#define COPY_FIELD(name) if(v.has_##name){out.has_##name=true;out.name=v.name;}
    COPY_FIELD(co2) COPY_FIELD(co2_temperature) COPY_FIELD(co2_humidity)
    COPY_FIELD(pm10_standard) COPY_FIELD(pm25_standard) COPY_FIELD(pm40_standard) COPY_FIELD(pm100_standard)
    COPY_FIELD(particles_05um) COPY_FIELD(particles_10um) COPY_FIELD(particles_25um) COPY_FIELD(particles_40um)
    COPY_FIELD(particles_100um) COPY_FIELD(particles_tps) COPY_FIELD(pm_humidity) COPY_FIELD(pm_temperature)
    COPY_FIELD(pm_voc_idx) COPY_FIELD(pm_nox_idx) COPY_FIELD(form_formaldehyde) COPY_FIELD(pm_status_flags)
#undef COPY_FIELD
}
}
int submit(const meshtastic_SensorConfig &q,uint32_t session,uint32_t id){
    if(!id)return -EINVAL;
    if(int(q.has_scd4x_config)+q.has_scd30_config+q.has_sen5x_config+q.has_sen6x_config+q.has_as3935_config!=1||
       q.has_ds248x_config||q.has_shtxx_config)return -EINVAL;
#if !defined(CONFIG_MT_SCD4X_ADMIN)
    if(q.has_scd4x_config)return -ENOTSUP;
#endif
    if((q.has_scd30_config&&!HAS_ACCESSORY(zt_scd30))||(q.has_sen5x_config&&!HAS_ACCESSORY(zt_sen5x))||
       (q.has_sen6x_config&&!HAS_ACCESSORY(zt_sen6x))||(q.has_as3935_config&&!HAS_ACCESSORY(zt_as3935)))return -ENOTSUP;
    Request r;r.id=id;r.session=session;auto stream=pb_ostream_from_buffer(r.wire,sizeof(r.wire));
    if(!pb_encode(&stream,meshtastic_SensorConfig_fields,&q))return -EINVAL;
    r.size=stream.bytes_written;return enqueue(r);
}
int submitScale(uint32_t kg,uint32_t session,uint32_t id){
    if(!id)return -EINVAL;
    if(!HAS_ACCESSORY(zt_nau7802))return -ENOTSUP;
    Request r;r.id=id;r.session=session;r.scale=true;r.kg=kg;return enqueue(r);
}
bool pending(){return atomic_get(&workState)==Pending;}
bool take(uint32_t session,uint32_t &id,int &result){
    if(atomic_get(&workState)!=Finished)return false;
    last=current;current={};atomic_set(&workState,Idle);
    id=last.id;result=last.result;return last.session==session;
}
void clock(uint32_t utc){atomic_set(&sensorUtc,static_cast<atomic_val_t>(utc));}
#if defined(CONFIG_MT_MLX90614)
bool mlxAvailable() { return i2c_is_ready_dt(&mlxBus.i2c); }
void enableMlx(bool enabled) {
    k_mutex_lock(&cacheMutex, K_FOREVER);
    if (atomic_set(&healthEnabled, enabled) != enabled) {
        atomic_inc(&healthEpoch);
        objectCache.valid = false;
        k_sem_give(&wake);
    }
    k_mutex_unlock(&cacheMutex);
}
bool objectTemperature(uint64_t now, meshtastic_HealthMetrics &out) {
    k_mutex_lock(&cacheMutex, K_FOREVER);
    const bool fresh = atomic_get(&healthEnabled) && objectCache.fresh(now);
    if (fresh) { out.has_temperature = true; out.temperature = objectCache.value; }
    k_mutex_unlock(&cacheMutex);
    return fresh;
}
#endif
bool airAvailable(){
    bool ok=false;
#if defined(MT_PMS_ALIAS)
    ok |= i2c_is_ready_dt(&pmsBus.i2c);
#endif
#if defined(MT_HM_ALIAS)
    ok |= i2c_is_ready_dt(&hmBus.i2c);
#endif
#if HAS_ACCESSORY(zt_sfa30)
    ok |= i2c_is_ready_dt(&sfaBus.i2c);
#endif
#if HAS_ACCESSORY(zt_scd30)
    ok|=i2c_is_ready_dt(&scdBus.i2c);
#endif
#if HAS_ACCESSORY(zt_sen5x)
    ok|=i2c_is_ready_dt(&sen5Bus.i2c);
#endif
#if HAS_ACCESSORY(zt_sen6x)
    ok|=i2c_is_ready_dt(&sen6Bus.i2c);
#endif
    return ok;
}
#if defined(MT_PMS_ALIAS)
void airPollDeadline(uint64_t deadline) {
    atomic_set(&airPollAt,static_cast<atomic_val_t>(uint32_t(deadline)));
    atomic_set(&hasAirPoll,1);
}
#endif
void enableAir(bool enabled,uint32_t seconds){
    atomic_set(&interval,seconds?seconds:900);
    if(atomic_set(&enabledAir,enabled)==enabled)return;
    k_mutex_lock(&cacheMutex,K_FOREVER);atomic_inc(&airEpoch);
#if HAS_ACCESSORY(zt_scd30)
    co2Cache.valid=false;
#endif
#if HAS_ACCESSORY(zt_sfa30)
    sfaCache.valid=false;
#endif
#if defined(MT_HM_ALIAS)
    hmCache.valid=false;
#endif
#if defined(MT_PMS_ALIAS)
    pmsCache.valid=false;
#endif
#if HAS_ACCESSORY(zt_sen5x)
    sen5Cache.valid=false;
#endif
#if HAS_ACCESSORY(zt_sen6x)
    sen6Cache.valid=false;
#endif
    k_mutex_unlock(&cacheMutex);k_sem_give(&wake);
}
bool air(uint64_t now,meshtastic_AirQualityMetrics &out){
    bool found=false;k_mutex_lock(&cacheMutex,K_FOREVER);
#if defined(MT_PMS_ALIAS)
    if (pmsCache.fresh(now)) { pmsCache.value.apply(out); found=true; }
#endif
    auto include=[&](const Cache<meshtastic_AirQualityMetrics> &cache){if(cache.fresh(now)){mergeAir(out,cache.value);found=true;}};
#if HAS_ACCESSORY(zt_sen5x)
    include(sen5Cache);
#endif
#if HAS_ACCESSORY(zt_sen6x)
    include(sen6Cache);
#endif
#if HAS_ACCESSORY(zt_scd30)
    include(co2Cache);
#endif
#if HAS_ACCESSORY(zt_sfa30)
    if (sfaCache.fresh(now)) { sfaCache.value.apply(out); found = true; }
#endif
#if defined(MT_HM_ALIAS)
    // HM330X is last in the pinned AirQualityTelemetry sensor order.
    if (hmCache.fresh(now)) { hmCache.value.apply(out); found = true; }
#endif
    k_mutex_unlock(&cacheMutex);return found;
}
#if defined(CONFIG_MT_LARK_WEATHER)
void requestWeather(){
    k_mutex_lock(&cacheMutex,K_FOREVER);dropWantsWeather=true;dropComplete=false;
    k_mutex_unlock(&cacheMutex);k_sem_give(&wake);
}
bool dropzoneWeather(Environment &out){
    k_mutex_lock(&cacheMutex,K_FOREVER);
    const bool done=dropComplete;
    if(done){
        out.hasWeather=false;const uint64_t now=k_uptime_get();
        if(now>=dropTime&&now-dropTime<=90000)copyWeather(out,dropLatest);
        dropComplete=false;
    }
    k_mutex_unlock(&cacheMutex);return done;
}
#endif
#if defined(CONFIG_MT_STOCK_SHT)
int selectShtAccuracy(unsigned level) {
    if (level>2) return -EINVAL;
    k_mutex_lock(&cacheMutex,K_FOREVER);
    const auto model=ShtModel(atomic_get(&shtModel));
    const int rc=model==ShtModel::None?-ENODEV:StockSht::adjustable(model)?0:-ENOTSUP;
    if (!rc) atomic_set(&shtAccuracy,level);
    k_mutex_unlock(&cacheMutex);
    return rc;
}
#endif
void enableEnvironment(bool enabled){
    if(atomic_set(&enabledEnv,enabled)==enabled)return;
    k_mutex_lock(&cacheMutex,K_FOREVER);atomic_inc(&envEpoch);envCache.valid=false;
#if defined(CONFIG_MT_STOCK_SHT)
    shtCache.valid=false;
    atomic_set(&shtModel,0);
    // Requested accuracy survives a measurement pause within this boot.
#endif
#if defined(CONFIG_MT_STOCK_BAROMETERS)
    eachBarometer([](auto &part) { part.cache.valid = false; });
#endif
#if defined(CONFIG_MT_VEML7700)
    opticalCache.valid = false;
#endif
#if defined(CONFIG_MT_LARK_WEATHER)
    larkLatest={};
#endif
    k_mutex_unlock(&cacheMutex);k_sem_give(&wake);
}
Environment environment(uint64_t now){
    k_mutex_lock(&cacheMutex,K_FOREVER);auto e=envCache.fresh(now)?envCache.value:Environment{};
#if defined(CONFIG_MT_STOCK_SHT)
    if (atomic_get(&enabledEnv) && shtCache.fresh(now)) e.sht=shtCache.value;
#endif
#if defined(CONFIG_MT_STOCK_BAROMETERS)
    if (atomic_get(&enabledEnv)) eachBarometer([&](auto &part) {
        if (part.cache.fresh(now)) e.barometer.merge(part.cache.value);
    });
#endif
#if defined(CONFIG_MT_VEML7700)
    if (atomic_get(&enabledEnv) && opticalCache.fresh(now)) e.optical = opticalCache.value;
#endif
#if defined(CONFIG_MT_LARK_WEATHER)
    if(atomic_get(&enabledEnv)&&now>=larkTime&&now-larkTime<=90000){
        copyWeather(e,larkLatest);
        e.hasWeatherHumidity=larkLatest.hasHumidity;
        e.humidity=larkLatest.humidity;
    }
#endif
    k_mutex_unlock(&cacheMutex);
#if defined(CONFIG_MT_RAK12035)
    if (e.soil.valid && (now < e.soil.at || now-e.soil.at > 90000)) e.soil = {};
#endif
    return e;
}
}
