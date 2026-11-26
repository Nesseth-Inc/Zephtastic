/* Actual timeout/IRQ/SetTx/airtime/info functions are extracted by run.py. */
static unsigned tx_groups;
static void tx_group(const char *name){++tx_groups;printf("PASS %s\n",name);}
static struct sx126x_airtime_observer_info tx_info(void)
{
  unsigned char before[sizeof(data)];memcpy(before,&data,sizeof(data));
  size_t prior_used=used;unsigned prior_signals=signal_calls,prior_msgs=msgs;
  struct sx126x_airtime_observer_info info;
  assert(!sx126x_airtime_observer_info(&dev,&info));
  assert(!memcmp(before,&data,sizeof(data))&&used==prior_used);
  assert(signal_calls==prior_signals&&msgs==prior_msgs&&observation_spin_held==0);
  return info;
}
static void tx_profile(void)
{
  data.config=(struct lora_modem_config){.frequency=906875000,.datarate=SF_12,
    .bandwidth=BW_125_KHZ,.preamble_len=16,.coding_rate=1,.sync_word=0x2b};
  data.config_valid=true;
}
static void accepted(void)
{
  tx_profile();atomic_set(&data.state,SX126X_STATE_TX);
  data.tx_async_signal=(void*)(uintptr_t)0x1234;
  assert(!sx126x_set_tx(&dev,10000));
  assert(tx_timeout_ticks==640000u);
}
static void accepted_and_failed_commands(void)
{
  boot();tx_profile();atomic_set(&data.state,SX126X_STATE_TX);
  tx_cmd_error=-EIO;assert(sx126x_set_tx(&dev,10000)==-EIO);
  struct sx126x_airtime_observer_info info=tx_info();
  assert(info.tx_started==0&&info.tx_done==0&&info.tx_timeouts==0&&info.last_tx_irq==0);
  tx_cmd_error=0;assert(!sx126x_set_tx(&dev,10000));
  assert(tx_commands==2&&tx_info().tx_started==1);
  atomic_set(&data.observation_config_state,2);atomic_set(&data.observation_loss,1);
  info=tx_info();assert(info.config_state==2&&info.tx_started==1&&info.sf==0);
  assert(atomic_get(&data.observation_loss)==1);
  tx_group("TX started counts accepted SetTx only; passive unsupported snapshots preserve RAM counters and loss");
}
static void tx_timeout_signal(bool duty)
{
  boot();accepted();data.rx_duty_cycle_enabled=duty;
  assert(!sx126x_handle_irq_status(&dev,SX126X_IRQ_RX_TX_TIMEOUT));
  assert(signal_calls==1 && signal_result==-ETIMEDOUT);
  assert(last_signal==(void*)(uintptr_t)0x1234&&data.tx_async_signal==NULL);
  assert(atomic_get(&data.state)==SX126X_REST_STATE&&msgs==1&&msg_status==-ETIMEDOUT);
  assert(strchr(trace,'r')==NULL); /* TX timeout cannot become RX rearm. */
  struct sx126x_airtime_observer_info info=tx_info();
  assert(info.tx_started==1&&info.tx_done==0&&info.tx_timeouts==1);
  assert(info.last_tx_irq==SX126X_IRQ_RX_TX_TIMEOUT);
}
static void rx_timeout_preserved(void)
{
  for(unsigned duty=0;duty<2;++duty){
    boot();data.rx_duty_cycle_enabled=duty!=0;
    assert(!sx126x_handle_irq_status(&dev,SX126X_IRQ_RX_TX_TIMEOUT));
    assert(strchr(trace,'r')!=NULL&&signal_calls==0&&msgs==0);
    assert(atomic_get(&data.state)==SX126X_STATE_RX);
    struct sx126x_airtime_observer_info info=tx_info();
    assert(info.tx_started==0&&info.tx_done==0&&info.tx_timeouts==0&&info.last_tx_irq==0);
  }
  tx_group("continuous and duty-cycle RX timeouts retain RX rearm without invented TX facts");
}
static void long_frame_done(void)
{
  boot();tx_profile();assert(sx126x_lora_airtime(&dev,184)==6988);
  accepted();now_ms=5001;
  assert(signal_calls==0&&tx_info().tx_started==1); /* No synthetic 5s event. */
  now_ms=6988;assert(!sx126x_handle_irq_status(&dev,SX126X_IRQ_TX_DONE));
  assert(signal_calls==1&&signal_result==0&&msg_status==0&&data.tx_async_signal==NULL);
  assert(atomic_get(&data.state)==SX126X_REST_STATE);
  struct sx126x_airtime_observer_info info=tx_info();
  assert(info.tx_started==1&&info.tx_done==1&&info.tx_timeouts==0&&info.last_tx_irq==SX126X_IRQ_TX_DONE);
  /* Starting/configuring the observer is not a new driver boot. */
  data.config_valid=false;data.rx_cb=NULL;
  assert(!sx126x_airtime_observer_start(&dev));
  info=tx_info();assert(info.tx_started==1&&info.tx_done==1&&info.tx_timeouts==0);
  sx126x_observe_config(&dev,&data.config,true,true);
  info=tx_info();assert(info.config_state==1&&info.tx_done==1);
  tx_group("actual184B SF12 airtime6988ms and SetTx09c400; nominal delayed TX_DONE unchanged without 5s cutoff");
}
static void state_capture_and_saturation(void)
{
  boot();accepted();
  assert(!sx126x_handle_irq_status(&dev,SX126X_IRQ_PREAMBLE_DETECTED));
  struct sx126x_airtime_observer_info info=tx_info();
  assert(info.tx_done==0&&info.tx_timeouts==0&&info.last_tx_irq==SX126X_IRQ_PREAMBLE_DETECTED);
  assert(!sx126x_handle_irq_status(&dev,SX126X_IRQ_TX_DONE|SX126X_IRQ_RX_TX_TIMEOUT));
  info=tx_info();assert(info.tx_done==1&&info.tx_timeouts==1);
  assert(info.last_tx_irq==(SX126X_IRQ_TX_DONE|SX126X_IRQ_RX_TX_TIMEOUT));
  assert(signal_calls==1&&signal_result==0); /* Existing TX_DONE-first co-fire semantics. */
  assert(atomic_get(&data.state)==SX126X_REST_STATE);
  /* The original captured TX word survived terminal state clearing and an RX IRQ. */
  data.rx_cb=NULL;assert(!sx126x_handle_irq_status(&dev,SX126X_IRQ_HEADER_VALID));
  assert(tx_info().last_tx_irq==info.last_tx_irq);
  boot();data.observation_tx_started=UINT32_MAX-1;
  accepted();assert(!sx126x_set_tx(&dev,10000));assert(tx_info().tx_started==UINT32_MAX);
  data.observation_tx_done=UINT32_MAX-1;data.observation_tx_timeouts=UINT32_MAX-1;
  for(unsigned i=0;i<2;++i){
    atomic_set(&data.state,SX126X_STATE_TX);data.tx_async_signal=(void*)(uintptr_t)0x1234;
    assert(!sx126x_handle_irq_status(&dev,SX126X_IRQ_TX_DONE|SX126X_IRQ_RX_TX_TIMEOUT));
  }
  info=tx_info();assert(info.tx_done==UINT32_MAX&&info.tx_timeouts==UINT32_MAX);
  tx_group("TX-state IRQ capture precedes terminal state clearing; co-fired bits are facts and counters saturate");
}
int main(void)
{
  accepted_and_failed_commands();tx_timeout_signal(false);
  tx_group("continuous RX callback retained during TX still receives exact -ETIMEDOUT signal and terminal state");
  tx_timeout_signal(true);tx_group("duty-cycle TX timeout signal and state-exit behavior remain unchanged");
  rx_timeout_preserved();long_frame_done();state_capture_and_saturation();
  assert(tx_groups==6);
  if(data.observation_queue.init)assert(!pthread_mutex_destroy(&data.observation_queue.mutex));
  return 0;
}
