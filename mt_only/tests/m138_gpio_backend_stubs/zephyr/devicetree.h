// SPDX-License-Identifier: MIT
#pragma once

#define MT_DT_CAT_INNER(a,b) a##b
#define MT_DT_CAT(a,b) MT_DT_CAT_INNER(a,b)

#if defined(FAKE_GPIO_NO_NODE)
#define DT_HAS_COMPAT_STATUS_OKAY_zephcore_gpio_module_exports 0
#else
#define DT_HAS_COMPAT_STATUS_OKAY_zephcore_gpio_module_exports 1
#endif
#define DT_HAS_COMPAT_STATUS_OKAY(compat) \
  MT_DT_CAT(DT_HAS_COMPAT_STATUS_OKAY_,compat)
#define DT_NUM_INST_STATUS_OKAY(compat) \
  MT_DT_CAT(DT_NUM_INST_STATUS_OKAY_,compat)

#if defined(FAKE_GPIO_MULTIPLE_NODES)
#define DT_NUM_INST_STATUS_OKAY_zephcore_gpio_module_exports 2
#else
#define DT_NUM_INST_STATUS_OKAY_zephcore_gpio_module_exports \
  DT_HAS_COMPAT_STATUS_OKAY_zephcore_gpio_module_exports
#endif

#define DT_INST(inst,compat) MT_DT_CAT(DT_INST_,inst)
#define DT_INST_0 ROOT

#define DT_REG_ADDR(node) MT_DT_CAT(DT_REG_ADDR_,node)
#define DT_PROP_OR(node,prop,default_value) MT_DT_CAT(MT_DT_CAT(DT_PROP_,node),MT_DT_CAT(_,prop))

#define DT_PROP_PIN0_zephcore_readable 1
#define DT_PROP_PIN0_zephcore_writable 1
#define DT_PROP_PIN0_zephcore_pullup 1
#define DT_PROP_PIN1_zephcore_readable 0
#define DT_PROP_PIN1_zephcore_writable 1
#if defined(FAKE_GPIO_BAD_PULLUP)
#define DT_PROP_PIN1_zephcore_pullup 1
#else
#define DT_PROP_PIN1_zephcore_pullup 0
#endif
#define DT_PROP_PIN2_zephcore_readable 1
#define DT_PROP_PIN2_zephcore_writable 0
#define DT_PROP_PIN2_zephcore_pullup 0

#if defined(FAKE_GPIO_BAD_WIRE_ID)
#define DT_REG_ADDR_PIN0 64
#define DT_REG_ADDR_PIN1 63
#define DT_REG_ADDR_PIN2 4
#elif defined(FAKE_GPIO_DUPLICATE_WIRE_ID)
#define DT_REG_ADDR_PIN0 0
#define DT_REG_ADDR_PIN1 0
#define DT_REG_ADDR_PIN2 4
#else
#define DT_REG_ADDR_PIN0 0
#define DT_REG_ADDR_PIN1 63
#define DT_REG_ADDR_PIN2 4
#endif

#define DT_FOREACH_CHILD_STATUS_OKAY(node,fn) \
  fn(PIN0) fn(PIN1) fn(PIN2)

#if defined(FAKE_GPIO_DUPLICATE_PHYSICAL_PIN)
#define MT_FAKE_PIN1 2
#else
#define MT_FAKE_PIN1 5
#endif
