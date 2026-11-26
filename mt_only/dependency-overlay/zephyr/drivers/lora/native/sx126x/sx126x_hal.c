/*
 * Copyright (c) 2026 Carlo Caione <ccaione@baylibre.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>

#include "sx126x.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sx126x_hal, CONFIG_LORA_LOG_LEVEL);

/* Timing constants */
#define SX126X_RESET_PULSE_MS       5
#define SX126X_RESET_WAIT_MS        5
#define SX126X_BUSY_DEFAULT_TIMEOUT 1000
#define SX126X_FREQ_400MHZ         400000000

static inline struct sx126x_hal_data *get_hal_data(const struct device *dev)
{
	struct sx126x_data *data = dev->data;

	return &data->hal;
}

int sx126x_hal_reset(const struct device *dev)
{
	const struct sx126x_hal_config *config = dev->config;
	int ret;

	if (!gpio_is_ready_dt(&config->reset)) {
		LOG_ERR("Reset GPIO not ready");
		return -ENODEV;
	}

	/* Pull reset low */
	ret = gpio_pin_set_dt(&config->reset, 1);
	if (ret < 0) {
		LOG_ERR("Failed to assert reset: %d", ret);
		return ret;
	}

	k_msleep(SX126X_RESET_PULSE_MS);

	/* Release reset */
	ret = gpio_pin_set_dt(&config->reset, 0);
	if (ret < 0) {
		LOG_ERR("Failed to release reset: %d", ret);
		return ret;
	}

	k_msleep(SX126X_RESET_WAIT_MS);

	/* Wait for chip to be ready */
	ret = sx126x_hal_wait_busy(dev, SX126X_BUSY_DEFAULT_TIMEOUT);
	if (ret < 0) {
		return ret;
	}

	LOG_DBG("Reset complete");
	return 0;
}

bool sx126x_hal_is_busy(const struct device *dev)
{
	const struct sx126x_hal_config *config = dev->config;

	return gpio_pin_get_dt(&config->busy) != 0;
}

static void dio1_isr(const struct device *gpio, struct gpio_callback *cb,
		     uint32_t pins)
{
	struct sx126x_hal_data *data = CONTAINER_OF(cb, struct sx126x_hal_data, dio1_cb);

	if (data->dio1_callback != NULL) {
		data->dio1_callback(data->dev);
	}
}

int sx126x_hal_set_dio1_callback(const struct device *dev,
				 void (*callback)(const struct device *dev))
{
	const struct sx126x_hal_config *config = dev->config;
	struct sx126x_hal_data *data = get_hal_data(dev);
	int ret;

	if (callback != NULL) {
		/* Do not publish a new handler until GPIO admission succeeds.  A
		 * failing/partial configure can invoke the previously published
		 * handler, but can never submit work through an uncommitted one. */
		ret = gpio_pin_interrupt_configure_dt(&config->dio1,
						      GPIO_INT_EDGE_TO_ACTIVE);
		if (ret == 0) {
			data->dio1_callback = callback;
		}
	} else {
		/* Disable admission before retiring the handler.  An edge racing the
		 * GPIO call still reaches the previous callback and is joined by the
		 * caller; publishing NULL first could silently discard that edge. */
		ret = gpio_pin_interrupt_configure_dt(&config->dio1,
						      GPIO_INT_DISABLE);
		if (ret == 0) {
			data->dio1_callback = NULL;
		}
	}
	return ret;
}

void sx126x_hal_retire_dio1_callback(const struct device *dev)
{
	struct sx126x_hal_data *data = get_hal_data(dev);

	data->dio1_callback = NULL;
}

void sx126x_hal_dio1_irq_enable(const struct device *dev)
{
	/* GPIO interrupts auto re-arm after being serviced, no action needed */
	ARG_UNUSED(dev);
}

int sx126x_hal_init(const struct device *dev)
{
	const struct sx126x_hal_config *config = dev->config;
	struct sx126x_hal_data *data = get_hal_data(dev);
	int ret;

	/* Store device reference for callbacks */
	data->dev = dev;
	data->dio1_callback = NULL;

	/* Check SPI bus */
	if (!spi_is_ready_dt(&config->spi)) {
		LOG_ERR("SPI bus not ready");
		return -ENODEV;
	}

	/* Configure reset GPIO */
	if (!gpio_is_ready_dt(&config->reset)) {
		LOG_ERR("Reset GPIO not ready");
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&config->reset, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure reset GPIO: %d", ret);
		return ret;
	}

	/* Configure busy GPIO */
	if (!gpio_is_ready_dt(&config->busy)) {
		LOG_ERR("Busy GPIO not ready");
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&config->busy, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure busy GPIO: %d", ret);
		return ret;
	}

	/* Configure DIO1 GPIO with interrupt */
	if (!gpio_is_ready_dt(&config->dio1)) {
		LOG_ERR("DIO1 GPIO not ready");
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&config->dio1, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure DIO1 GPIO: %d", ret);
		return ret;
	}

	/* Setup DIO1 interrupt callback */
	gpio_init_callback(&data->dio1_cb, dio1_isr, BIT(config->dio1.pin));
	ret = gpio_add_callback(config->dio1.port, &data->dio1_cb);
	if (ret < 0) {
		LOG_ERR("Failed to add DIO1 callback: %d", ret);
		return ret;
	}

	/* Configure optional GPIOs */
	ret = sx126x_hal_configure_gpio(&config->antenna_enable, GPIO_OUTPUT_INACTIVE,
					"antenna enable");
	if (ret < 0) {
		return ret;
	}

	ret = sx126x_hal_configure_gpio(&config->tx_enable, GPIO_OUTPUT_INACTIVE,
					"TX enable");
	if (ret < 0) {
		return ret;
	}

	ret = sx126x_hal_configure_gpio(&config->rx_enable, GPIO_OUTPUT_INACTIVE,
					"RX enable");
	if (ret < 0) {
		return ret;
	}

	/* Inactive = LOW = FEM LNA in the receive path, which is the state
	 * every board using this line wants by default. */
	ret = sx126x_hal_configure_gpio(&config->lna_bypass, GPIO_OUTPUT_INACTIVE,
					"FEM LNA bypass");
	if (ret < 0) {
		return ret;
	}

	LOG_DBG("HAL initialized");
	return 0;
}

static int sx126x_hal_set_pa_config(const struct device *dev, uint8_t pa_duty_cycle,
				    uint8_t hp_max, uint8_t device_sel, uint8_t pa_lut)
{
	uint8_t buf[4] = { pa_duty_cycle, hp_max, device_sel, pa_lut };

	return sx126x_hal_write_cmd(dev, SX126X_CMD_SET_PA_CONFIG, buf, 4);
}

int sx126x_hal_configure_tx_params(const struct device *dev, int8_t power,
				   uint32_t frequency, uint8_t ramp_time)
{
	const struct sx126x_hal_config *config = dev->config;
	uint8_t pa_duty_cycle;
	int8_t tx_power;
	int ret;

	if (config->is_sx1261) {
		/*
		 * SX1261: Low power PA, up to +15 dBm
		 * For +15 dBm at >400 MHz, use higher paDutyCycle.
		 * For lower power, use lower paDutyCycle for efficiency.
		 */
		pa_duty_cycle = (power >= SX1261_MAX_POWER && frequency >= SX126X_FREQ_400MHZ)
				? SX1261_PA_DUTY_CYCLE_HIGH
				: SX1261_PA_DUTY_CYCLE_LOW;
		ret = sx126x_hal_set_pa_config(dev, pa_duty_cycle, SX1261_HP_MAX,
					       SX126X_DEVICE_SEL_SX1261,
					       SX126X_PA_LUT);
		if (ret < 0) {
			return ret;
		}
		tx_power = CLAMP(power, SX1261_MIN_POWER, SX1261_MAX_POWER_TX_PARAM);
	} else {
		/* SX1262: High power PA, up to +22 dBm.
		 *
		 * Measured-optimal PaConfig per requested power instead of the
		 * datasheet's fixed (dutyCycle=4, hpMax=7): same RF output at
		 * lower PA supply current below +22 dBm.  Table from RadioLib
		 * PR #1662 (github.com/radiolib-org/power-tests, SX1262).
		 * Index = clamped power + 9 (range -9..+22); pa_val is the
		 * SetTxParams byte belonging to that PaConfig -- it is NOT
		 * the requested dBm and must be sent verbatim.  At +22 dBm
		 * the entry {4, 7, 22} equals the old fixed config. */
		static const struct {
			uint8_t pa_duty_cycle;
			uint8_t hp_max;
			int8_t pa_val;
		} pa_opt[32] = {
			{ 2, 2, -5 }, { 2, 1,  0 }, { 1, 1,  3 }, { 1, 2,  0 },
			{ 1, 1,  6 }, { 1, 2,  3 }, { 2, 2,  2 }, { 4, 1,  6 },
			{ 1, 1, 11 }, { 2, 1, 11 }, { 1, 1, 14 }, { 2, 1, 14 },
			{ 1, 1, 20 }, { 1, 1, 22 }, { 2, 2, 11 }, { 3, 1, 21 },
			{ 1, 2, 17 }, { 4, 2, 13 }, { 1, 2, 20 }, { 1, 2, 22 },
			{ 2, 2, 21 }, { 3, 2, 21 }, { 1, 4, 19 }, { 1, 4, 20 },
			{ 3, 3, 20 }, { 2, 5, 19 }, { 1, 6, 22 }, { 2, 5, 22 },
			{ 3, 5, 22 }, { 3, 6, 22 }, { 4, 6, 22 }, { 4, 7, 22 },
		};
		int8_t req = CLAMP(power, SX1262_MIN_POWER, SX1262_MAX_POWER);
		uint8_t idx = (uint8_t)(req - SX1262_MIN_POWER);

		ret = sx126x_hal_set_pa_config(dev, pa_opt[idx].pa_duty_cycle,
					       pa_opt[idx].hp_max,
					       SX126X_DEVICE_SEL_SX1262,
					       SX126X_PA_LUT);
		if (ret < 0) {
			return ret;
		}
		tx_power = pa_opt[idx].pa_val;
	}

	/* SetPaConfig does not auto-update OCP on the standalone SX126x
	 * (cold-reset default is 60 mA / 0x18). The SX1262 high-power PA
	 * needs ~118 mA at +22 dBm, so OCP must be raised to 140 mA / 0x38
	 * or the PA is current-limited and output drops to ~14-16 dBm.
	 * Mirrors what the STM32WL HAL does and what RadioLib's
	 * setCurrentLimit() programs on Arduino MeshCore. */
	uint8_t ocp_value = config->is_sx1261 ? 0x18 /* 60 mA  */
					      : 0x38 /* 140 mA */;
	ret = sx126x_hal_write_regs(dev, SX126X_REG_OCP, &ocp_value, 1);
	if (ret < 0) {
		return ret;
	}

	uint8_t buf[2] = { (uint8_t)tx_power, ramp_time };

	return sx126x_hal_write_cmd(dev, SX126X_CMD_SET_TX_PARAMS, buf, 2);
}
