// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

#include <zephyr/device.h>

using gpio_pin_t=uint32_t;
using gpio_flags_t=uint32_t;
using gpio_dt_flags_t=uint32_t;

struct gpio_dt_spec {
  const device *port;
  gpio_pin_t pin;
  gpio_dt_flags_t dt_flags;
};

inline device mt_fake_gpio_device{};
extern gpio_dt_spec mt_fake_gpio_spec_PIN0;
extern gpio_dt_spec mt_fake_gpio_spec_PIN1;
extern gpio_dt_spec mt_fake_gpio_spec_PIN2;

#define GPIO_DT_SPEC_GET(node,property) MT_DT_CAT(mt_fake_gpio_spec_,node)

constexpr gpio_flags_t GPIO_INPUT=1u<<0;
constexpr gpio_flags_t GPIO_OUTPUT=1u<<1;
constexpr gpio_flags_t GPIO_PULL_UP=1u<<2;
constexpr gpio_flags_t GPIO_OUTPUT_ACTIVE=GPIO_OUTPUT|(1u<<3);
constexpr gpio_flags_t GPIO_OUTPUT_INACTIVE=GPIO_OUTPUT|(1u<<4);
constexpr gpio_dt_flags_t GPIO_ACTIVE_LOW=1u<<5;

inline bool gpio_is_ready_dt(const gpio_dt_spec *spec){
  return spec&&device_is_ready(spec->port);
}
inline int gpio_pin_configure_dt(const gpio_dt_spec *spec,gpio_flags_t flags){
  if(!spec||!spec->port)return -19;
  if(!spec->port->ready){++spec->port->operationsAfterUnready;return -19;}
  ++spec->port->configureCalls;
  spec->port->lastConfigureFlags[spec->pin]=flags;
  return spec->port->configureError[spec->pin];
}
inline int gpio_pin_get_dt(const gpio_dt_spec *spec){
  if(!spec||!spec->port)return -19;
  if(!spec->port->ready){++spec->port->operationsAfterUnready;return -19;}
  ++spec->port->getCalls;
  const int error=spec->port->getError[spec->pin];
  if(error)return error;
  return spec->port->values[spec->pin];
}
inline int gpio_pin_set_dt(const gpio_dt_spec *spec,int value){
  if(!spec||!spec->port)return -19;
  if(!spec->port->ready){++spec->port->operationsAfterUnready;return -19;}
  ++spec->port->setCalls;
  const int error=spec->port->setError[spec->pin];
  if(error)return error;
  spec->port->values[spec->pin]=value?1:0;
  return 0;
}
