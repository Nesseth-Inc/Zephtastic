static int sx126x_rx_activity(const struct device *dev)
{
	struct sx126x_data *data;
	uint32_t now, seen, bound;
	atomic_t *stamp;
	uint16_t irq = 0, latest = 0, stale_mask;
	int busy, ret = 1;

	if (!device_is_ready(dev)) {
		return -ENODEV;
	}
	data = dev->data;
	busy = sx126x_hal_is_busy(dev);
	if (busy != 0) {
		return busy < 0 ? busy : 1;
	}
	if (k_mutex_lock(&data->lock, K_NO_WAIT) != 0) {
		return 1;
	}

	/* The worker copies RX buffers and resets latches outside this mutex.
	 * Never declare clear while it is pending, running or cancelling.
	 */
	if (k_work_busy_get(&data->irq_work) != 0) {
		goto out;
	}
	ret = sx126x_get_irq_status(dev, &irq);
	if (ret < 0) {
		goto out;
	}

	/* Preserve completed packets awaiting service, even past a deadline.
	 * The per-device worker can recover a missed terminal interrupt edge.
	 */
	if ((irq & SX126X_IRQ_RX_DONE) != 0) {
		ret = k_work_submit_to_queue(&data->dio1_wq, &data->irq_work);
		if (ret >= 0) {
			ret = 1;
		}
		goto out;
	}

	now = k_uptime_get_32();
	if (data->rx_packet_active) {
		stamp = &data->header_seen_at_ms;
		bound = sx126x_max_payload_ms(data);
		stale_mask = SX126X_IRQ_PREAMBLE_DETECTED | SX126X_IRQ_SYNC_WORD_VALID |
			     SX126X_IRQ_HEADER_VALID | SX126X_IRQ_HEADER_ERR;
	} else if ((irq & SX126X_IRQ_HEADER_VALID) != 0) {
		stamp = &data->raw_header_seen_at_ms;
		bound = sx126x_max_payload_ms(data);
		stale_mask = SX126X_IRQ_PREAMBLE_DETECTED | SX126X_IRQ_SYNC_WORD_VALID |
			     SX126X_IRQ_HEADER_VALID | SX126X_IRQ_HEADER_ERR;
	} else if ((irq & SX126X_IRQ_PREAMBLE_DETECTED) != 0) {
		stamp = &data->preamble_seen_at_ms;
		bound = sx126x_preamble_grace_ms(data);
		stale_mask = SX126X_IRQ_PREAMBLE_DETECTED;
	} else {
		atomic_set(&data->preamble_seen_at_ms, 0);
		atomic_set(&data->raw_header_seen_at_ms, 0);
		ret = 0;
		goto out;
	}

	seen = (uint32_t)atomic_get(stamp);
	if (seen == 0) {
		atomic_set(stamp, (atomic_val_t)(now == 0 ? 1U : now));
		ret = 1;
		goto out;
	}
	/* Worker timestamps use 1 as the time-zero sentinel. */
	if ((now == 0 && seen == 1) || (uint32_t)(now - seen) < bound) {
		ret = 1;
		goto out;
	}

	/* Re-snapshot before clearing: a header, completion or worker may have
	 * arrived since the first read. Never clear RX_DONE or the FIFO.
	 */
	if (k_work_busy_get(&data->irq_work) != 0) {
		ret = 1;
		goto out;
	}
	ret = sx126x_get_irq_status(dev, &latest);
	if (ret < 0) {
		goto out;
	}
	if ((latest & SX126X_IRQ_RX_DONE) != 0 ||
	    (stale_mask == SX126X_IRQ_PREAMBLE_DETECTED &&
	     ((latest & SX126X_IRQ_HEADER_VALID) != 0 || data->rx_packet_active))) {
		ret = 1;
		goto out;
	}
	ret = sx126x_clear_irq_status(dev, stale_mask);
	if (ret < 0) {
		goto out;
	}
	if (stale_mask == SX126X_IRQ_PREAMBLE_DETECTED) {
		atomic_set(&data->preamble_seen_at_ms, 0);
	} else {
		sx126x_reset_rx_busy_signals(data);
	}
	atomic_inc(&data->rx_stale_releases);
	ret = 0;

out:
	k_mutex_unlock(&data->lock);
	return ret;
}
