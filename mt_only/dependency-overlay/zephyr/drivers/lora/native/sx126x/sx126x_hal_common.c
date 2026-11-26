/*
 * Copyright (c) 2026 Carlo Caione <ccaione@baylibre.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/byteorder.h>

#include "sx126x.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sx126x_hal_common, CONFIG_LORA_LOG_LEVEL);

#define SX126X_BUSY_DEFAULT_TIMEOUT (IS_ENABLED(CONFIG_ZEPHCORE_DUAL_RX_BENCH) ? 200 : 1000)

static int spi_transfer(const struct spi_dt_spec *spi,
			const uint8_t *hdr, size_t hdr_len,
			uint8_t *data, size_t data_len, bool read)
{
	uint8_t rx_hdr[4];

	__ASSERT_NO_MSG(hdr_len <= sizeof(rx_hdr));

	struct spi_buf tx_bufs[] = {
		{ .buf = (uint8_t *)hdr, .len = hdr_len },
		{ .buf = read ? NULL : data, .len = data_len },
	};
	struct spi_buf rx_bufs[] = {
		{ .buf = rx_hdr, .len = hdr_len },
		{ .buf = data, .len = data_len },
	};
	int count = (data_len == 0) ? 1 : 2;
	struct spi_buf_set tx_set = { .buffers = tx_bufs, .count = count };
	struct spi_buf_set rx_set = { .buffers = rx_bufs, .count = count };

	return spi_transceive_dt(spi, &tx_set, read ? &rx_set : NULL);
}

int sx126x_hal_wait_busy(const struct device *dev, uint32_t timeout_ms)
{
	/* Fast path: most SX126x commands finish BUSY in <650 us.
	 * Busy-wait first to avoid the ~1 ms k_msleep() scheduler
	 * penalty, which would otherwise dominate RX turnaround. */
	if (WAIT_FOR(!sx126x_hal_is_busy(dev), 1000, k_busy_wait(10))) {
		return 0;
	}

	/* The normal always-on companion uses the post-init RadioLib bound.
	 * Initial probe/calibration and duty-cycled wake retain their budget. */
#if defined(CONFIG_MT_ONLY) && defined(CONFIG_MT_NORMAL_ENDPOINT) && !defined(CONFIG_ZEPHCORE_DUAL_RX_BENCH)
	const struct sx126x_data *data = dev->data;

	if (timeout_ms == SX126X_BUSY_DEFAULT_TIMEOUT && data->config_valid &&
	    !data->rx_duty_cycle_enabled) {
		timeout_ms = 50;
	}
#endif
	if (!WAIT_FOR(!sx126x_hal_is_busy(dev),
		      (timeout_ms * 1000) - 1000,
		      k_msleep(1))) {
		/* Always-awake bench chips need no wake recovery. Avoid recursively
		 * calling wakeup -> wait_busy when an electrical BUSY fault persists. */
		if (IS_ENABLED(CONFIG_ZEPHCORE_DUAL_RX_BENCH) || IS_ENABLED(CONFIG_MT_ONLY)) return sx126x_bench_fault(dev,-ETIMEDOUT);
		/* One-shot recovery: wake the chip and retry once. */
		LOG_WRN("Busy timeout after %u ms, wake+retry", timeout_ms);
		sx126x_hal_wakeup(dev);
		if (WAIT_FOR(!sx126x_hal_is_busy(dev), 2000, k_busy_wait(10))) {
			return 0;
		}
		if (!WAIT_FOR(!sx126x_hal_is_busy(dev),
			      timeout_ms * 1000,
			      k_msleep(1))) {
			LOG_WRN("Busy timeout after wake retry (%u ms)", timeout_ms);
			return -ETIMEDOUT;
		}
	}

	return 0;
}

void sx126x_hal_set_antenna_enable(const struct device *dev, bool enable)
{
	const struct sx126x_hal_config *config = dev->config;

	if (config->antenna_enable.port != NULL) {
		gpio_pin_set_dt(&config->antenna_enable, enable);
	}
}

void sx126x_hal_set_rf_switch(const struct device *dev, bool enable, bool tx)
{
	const struct sx126x_hal_config *config = dev->config;

	if (config->tx_enable.port != NULL) {
		gpio_pin_set_dt(&config->tx_enable, enable && tx);
	}

	if (config->rx_enable.port != NULL) {
		gpio_pin_set_dt(&config->rx_enable, enable && !tx);
	}
}

int sx126x_hal_configure_gpio(const struct gpio_dt_spec *gpio,
			      gpio_flags_t flags, const char *name)
{
	int ret;

	if (gpio->port == NULL) {
		return 0;
	}

	if (!gpio_is_ready_dt(gpio)) {
		LOG_ERR("%s GPIO not ready", name);
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(gpio, flags);
	if (ret < 0) {
		LOG_ERR("Failed to configure %s: %d", name, ret);
		return ret;
	}

	return 0;
}

int sx126x_hal_wakeup(const struct device *dev)
{
	const struct sx126x_hal_config *config = dev->config;
	uint8_t buf[2] = { SX126X_CMD_GET_STATUS, 0x00 };
	struct spi_buf tx_buf = {
		.buf = buf,
		.len = sizeof(buf),
	};
	struct spi_buf_set tx_set = {
		.buffers = &tx_buf,
		.count = 1,
	};
	int ret;

	/*
	 * Send a write-only GET_STATUS command. The NSS falling edge wakes
	 * the chip from sleep. Use spi_write_dt() (TX-only) rather than
	 * spi_transceive_dt() because the chip's SDO line is undefined
	 * during sleep mode.
	 */
	ret = spi_write_dt(&config->spi, &tx_set);
	if (ret < 0) {
		LOG_ERR("Wakeup SPI failed: %d", ret);
		return sx126x_bench_fault(dev,ret);
	}

	return sx126x_hal_wait_busy(dev, SX126X_BUSY_DEFAULT_TIMEOUT);
}

int sx126x_hal_write_cmd(const struct device *dev, uint8_t opcode,
			 const uint8_t *data, size_t len)
{
	/* Final SPI opcode barrier, covering async, blocking and CW API paths. */
	if (IS_ENABLED(CONFIG_MT_ONLY) &&
	    (opcode == SX126X_CMD_SET_TX_CONTINUOUS_WAVE || opcode == 0xd2 ||
	     (opcode == SX126X_CMD_SET_TX && !IS_ENABLED(CONFIG_MT_ALLOW_TX)))) return -EPERM;
	if (IS_ENABLED(CONFIG_ZEPHCORE_DUAL_RX_BENCH) &&
	    (opcode==SX126X_CMD_SET_TX_CONTINUOUS_WAVE || opcode==0xd2)) return -EPERM;
	if (IS_ENABLED(CONFIG_ZEPHCORE_DUAL_RX_BENCH) && opcode==SX126X_CMD_SET_TX && !sx126x_bench_tx_ready(dev)) return -EPERM;
	const struct sx126x_hal_config *config = dev->config;
	uint8_t hdr[1] = { opcode };
	int ret;

	ret = sx126x_hal_wait_busy(dev, SX126X_BUSY_DEFAULT_TIMEOUT);
	if (ret < 0) {
		return sx126x_bench_fault(dev,ret);
	}

	/* Recheck and consume AFTER BUSY wait, immediately before SPI. */
	if (IS_ENABLED(CONFIG_ZEPHCORE_DUAL_RX_BENCH) && opcode==SX126X_CMD_SET_TX && !sx126x_bench_tx_consume(dev,data,len)) return -EPERM;
	/* A storage-pause proof is single-use metadata authority. Every path
	 * that can start RF retires it here, including direct native restarts. */
	if (opcode == SX126X_CMD_SET_RX || opcode == SX126X_CMD_SET_RX_DUTY_CYCLE ||
	    opcode == SX126X_CMD_SET_TX || opcode == SX126X_CMD_SET_CAD ||
	    opcode == SX126X_CMD_SET_TX_CONTINUOUS_WAVE || opcode == 0xd2)
		sx126x_airtime_observer_reconfigure_invalidate(dev);
	ret = spi_transfer(&config->spi, hdr, sizeof(hdr), (uint8_t *)data, len, false);
	if (ret < 0) {
		LOG_ERR("SPI write failed: %d", ret);
		return sx126x_bench_fault(dev,ret);
	}

	if (opcode != SX126X_CMD_SET_SLEEP) {
		ret = sx126x_hal_wait_busy(dev, SX126X_BUSY_DEFAULT_TIMEOUT);
	} else {
		/*
		 * The chip needs time to fully enter sleep mode before the
		 * next NSS falling edge can wake it up reliably.
		 */
		k_busy_wait(500);
	}

	return sx126x_bench_fault(dev,ret);
}

int sx126x_hal_read_cmd(const struct device *dev, uint8_t opcode,
			uint8_t *data, size_t len)
{
	/* Read-command framing must never be a TX-command bypass. */
	if ((IS_ENABLED(CONFIG_ZEPHCORE_DUAL_RX_BENCH) || IS_ENABLED(CONFIG_MT_ONLY)) &&
	    (opcode==SX126X_CMD_SET_TX || opcode==SX126X_CMD_SET_TX_CONTINUOUS_WAVE || opcode==0xd2)) return -EPERM;
	const struct sx126x_hal_config *config = dev->config;
	uint8_t hdr[2] = { opcode, 0x00 };
	int ret;

	ret = sx126x_hal_wait_busy(dev, SX126X_BUSY_DEFAULT_TIMEOUT);
	if (ret < 0) {
		return sx126x_bench_fault(dev,ret);
	}

	ret = spi_transfer(&config->spi, hdr, sizeof(hdr), data, len, true);
	if (ret < 0) {
		LOG_ERR("SPI transceive failed: %d", ret);
		return sx126x_bench_fault(dev,ret);
	}

	return 0;
}

int sx126x_hal_write_regs(const struct device *dev, uint16_t address,
			  const uint8_t *data, size_t len)
{
	const struct sx126x_hal_config *config = dev->config;
	uint8_t hdr[3];
	int ret;

	ret = sx126x_hal_wait_busy(dev, SX126X_BUSY_DEFAULT_TIMEOUT);
	if (ret < 0) {
		return sx126x_bench_fault(dev,ret);
	}

	hdr[0] = SX126X_CMD_WRITE_REGISTER;
	sys_put_be16(address, &hdr[1]);

	ret = spi_transfer(&config->spi, hdr, sizeof(hdr), (uint8_t *)data, len, false);
	if (ret < 0) {
		LOG_ERR("SPI write regs failed: %d", ret);
		return sx126x_bench_fault(dev,ret);
	}

	return sx126x_hal_wait_busy(dev, SX126X_BUSY_DEFAULT_TIMEOUT);
}

int sx126x_hal_read_regs(const struct device *dev, uint16_t address,
			 uint8_t *data, size_t len)
{
	const struct sx126x_hal_config *config = dev->config;
	uint8_t hdr[4];
	int ret;

	ret = sx126x_hal_wait_busy(dev, SX126X_BUSY_DEFAULT_TIMEOUT);
	if (ret < 0) {
		return sx126x_bench_fault(dev,ret);
	}

	hdr[0] = SX126X_CMD_READ_REGISTER;
	sys_put_be16(address, &hdr[1]);
	hdr[3] = 0x00;

	ret = spi_transfer(&config->spi, hdr, sizeof(hdr), data, len, true);
	if (ret < 0) {
		LOG_ERR("SPI read regs failed: %d", ret);
		return sx126x_bench_fault(dev,ret);
	}

	return 0;
}

int sx126x_hal_write_buffer(const struct device *dev, uint8_t offset,
			    const uint8_t *data, size_t len)
{
	const struct sx126x_hal_config *config = dev->config;
	uint8_t hdr[2] = { SX126X_CMD_WRITE_BUFFER, offset };
	int ret;

	ret = sx126x_hal_wait_busy(dev, SX126X_BUSY_DEFAULT_TIMEOUT);
	if (ret < 0) {
		return sx126x_bench_fault(dev,ret);
	}

	ret = spi_transfer(&config->spi, hdr, sizeof(hdr), (uint8_t *)data, len, false);
	if (ret < 0) {
		LOG_ERR("SPI write buffer failed: %d", ret);
		return sx126x_bench_fault(dev,ret);
	}

	return sx126x_hal_wait_busy(dev, SX126X_BUSY_DEFAULT_TIMEOUT);
}

int sx126x_hal_read_buffer(const struct device *dev, uint8_t offset,
			   uint8_t *data, size_t len)
{
	const struct sx126x_hal_config *config = dev->config;
	uint8_t hdr[3] = { SX126X_CMD_READ_BUFFER, offset, 0x00 };
	int ret;

	ret = sx126x_hal_wait_busy(dev, SX126X_BUSY_DEFAULT_TIMEOUT);
	if (ret < 0) {
		return sx126x_bench_fault(dev,ret);
	}

	ret = spi_transfer(&config->spi, hdr, sizeof(hdr), data, len, true);
	if (ret < 0) {
		LOG_ERR("SPI read buffer failed: %d", ret);
		return sx126x_bench_fault(dev,ret);
	}

	return 0;
}
