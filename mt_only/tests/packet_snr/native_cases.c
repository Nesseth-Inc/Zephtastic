/* SPDX-License-Identifier: GPL-3.0-only */
struct Captured {
  uint8_t tag, raw;
  bool known;
  uint16_t length;
  int8_t integer;
};
static struct Captured captured[512];
static unsigned captured_count;
static void packet_callback(const struct device *d, uint8_t *p, uint16_t n, int16_t r, int8_t s,
                            const struct sx126x_packet_status *status, void *user) {
  (void) d;
  (void) r;
  assert(user == &captured_count && status);
  assert((p != NULL) == (n != 0));
  assert(captured_count < 512);
  captured[captured_count++] = (struct Captured) { p ? p[0] : 0, status->snr_raw, status->snr_known, n, s };
}
static void start_packet(void) {
  boot();
  captured_count = 0;
  packet_reads = 0;
  data.config_valid = true;
  assert(sx126x_recv_async_packet(&dev, packet_callback, &captured_count) == 0);
}
int main(void) {
  for (unsigned raw = 0; raw < 256; ++raw) {
    start_packet();
    packet_raw = raw;
    payload_tag = (uint8_t) (raw ^ 0x55);
    assert(sx126x_handle_irq_rx_done(&dev, SX126X_IRQ_RX_DONE) == 0);
    assert(packet_reads == 1 && captured_count == 1 && captured[0].known && captured[0].raw == raw);
    assert(captured[0].tag == payload_tag && captured[0].length == 16 && captured[0].integer == ((int8_t) raw >> 2));
  }
  puts("PASS all256 raw octets bound to same successful packet-status read and payload; legacy signed integer unchanged");
  for (unsigned mode = 0; mode < 8; ++mode) {
    start_packet();
    packet_raw = 255;
    uint16_t irq = SX126X_IRQ_RX_DONE;
    if (mode == 0)
      packet_error = -EIO;
    if (mode == 1)
      buffer_error = -EIO;
    if (mode == 2)
      fifo_error = -EIO;
    if (mode == 3)
      restart_error = -EIO;
    if (mode == 4)
      irq |= SX126X_IRQ_CRC_ERR;
    if (mode == 5)
      irq |= SX126X_IRQ_HEADER_ERR;
    if (mode == 6)
      input_length = 0;
    if (mode == 7)
      irq |= SX126X_IRQ_HEADER_ERR | SX126X_IRQ_HEADER_VALID;
    assert(sx126x_handle_irq_rx_done(&dev, irq) == (mode == 3 ? -EIO : 0) && captured_count == 1);
    assert(packet_reads == (mode == 1 ? 0 : 1));
    if (mode == 0) {
      assert(captured[0].length == 16 && !captured[0].known && captured[0].raw == 0 && captured[0].integer == 0);
    } else if (mode == 7) {
      assert(captured[0].length == 16 && captured[0].known && captured[0].raw == 255);
    } else {
      assert(captured[0].length == 0 && !captured[0].known && captured[0].raw == 0);
    }
  }
  puts("PASS status failure retains legacy payload fallback; buffer/CRC/header/FIFO/restart/empty rejection never credits metadata");
  start_packet();
  mutate_generation = true;
  assert(sx126x_handle_irq_rx_done(&dev, SX126X_IRQ_RX_DONE) == 0);
  assert(packet_reads == 1 && !captured_count && !callbacks);
  puts("PASS stale generation suppresses both metadata and legacy packet callbacks");
  start_packet();
  packet_raw = 1;
  payload_tag = 0x11;
  sx126x_handle_irq_rx_done(&dev, SX126X_IRQ_RX_DONE);
  packet_raw = 255;
  payload_tag = 0x22;
  sx126x_handle_irq_rx_done(&dev, SX126X_IRQ_RX_DONE);
  assert(captured_count == 2 && captured[0].raw == 1 && captured[0].tag == 0x11 && captured[1].raw == 255 && captured[1].tag == 0x22);
  puts("PASS back-to-back packets retain distinct copied raw metadata without a last-value accessor");
  start_packet();
  assert(data.rx_packet_cb == packet_callback);
  atomic_val_t before = atomic_get(&data.rx_cb_gen);
  assert(sx126x_lora_recv_async(&dev, receive, NULL) == 0 && data.rx_packet_cb == NULL && data.rx_cb == receive);
  assert(atomic_get(&data.rx_cb_gen) == before + 1);
  packet_raw = 129;
  sx126x_handle_irq_rx_done(&dev, SX126X_IRQ_RX_DONE);
  assert(callbacks == 1 && !captured_count);
  assert(sx126x_recv_async_packet(&dev, packet_callback, &captured_count) == 0);
  assert(sx126x_recv_async_packet(&dev, NULL, NULL) == 0 && data.rx_cb == NULL && data.rx_packet_cb == NULL);
  assert(sx126x_recv_async_packet(NULL, packet_callback, NULL) == -EINVAL);
  puts("PASS standard and extended callbacks mutually exclusive; same generation and cancel path");
  for (unsigned fail = 0; fail < 7; ++fail) {
    boot();
    captured_count = 0;
    data.config_valid = fail != 0;
    atomic_set(&data.state, SX126X_REST_STATE);
    if (fail == 1)
      ready_error = -EIO;
    if (fail == 2)
      clear_error = -EIO;
    if (fail == 3)
      params_error = -EIO;
    if (fail == 4)
      set_rx_error = -EIO;
    if (fail == 5)
      gain_error = -EIO;
    if (fail == 6)
      atomic_set(&data.state, SX126X_STATE_TX);
    assert(sx126x_recv_async_packet(&dev, packet_callback, &captured_count) < 0 && !captured_count);
  }
  puts("PASS unchanged configuration/state/SPI registration failure gates do not deliver metadata");
}
