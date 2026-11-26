/* Extracted production pause/config/observer/IRQ bodies, synthetic HAL/RTOS.
 * Positive permits are minted only by the actual successful storage pause.
 * This is not a Zephyr scheduler, SPI, RF or interrupt-race proof. */
static struct lora_modem_config lf(void)
{
  return (struct lora_modem_config){.frequency=906875000,.datarate=SF_11,
      .bandwidth=BW_250_KHZ,.preamble_len=16,.coding_rate=1,.sync_word=0x2b};
}
static struct lora_modem_config slow(void)
{
  struct lora_modem_config c=lf();c.datarate=SF_12;c.bandwidth=BW_125_KHZ;return c;
}
static void cold(void)
{
  boot();atomic_set(&data.state,SX126X_REST_STATE);data.rx_cb=NULL;
  assert(!sx126x_airtime_observer_start(&dev));
  struct lora_modem_config c=lf();assert(!sx126x_lora_config(&dev,&c));
  assert(sx126x_airtime_observer_supported(&dev));
}
static void expect_info(const struct lora_modem_config *c,bool supported)
{
  struct sx126x_airtime_observer_info info;
  assert(!sx126x_airtime_observer_info(&dev,&info));assert(info.enabled==1);
  assert((info.config_state==1)==supported);
  assert(sx126x_airtime_observer_supported(&dev)==supported);
  if(!supported){assert(info.config_state==2&&info.sf==0&&info.bandwidth_hz==0&&info.cr==0&&
      info.preamble_len==0&&info.ldro==0&&info.crc==0);return;}
  assert(info.sf==(unsigned)c->datarate&&info.bandwidth_hz==(unsigned)c->bandwidth*1000&&
      info.cr==c->coding_rate&&info.preamble_len==((c->datarate<=SF_6&&c->preamble_len<12)?12:c->preamble_len)&&
      info.ldro==(unsigned)should_enable_ldro(c->datarate,c->bandwidth,&hal)&&info.crc==!c->packet_crc_disable);
  assert(data.observation_frequency==c->frequency&&data.observation_iq==c->iq_inverted&&
      data.observation_sync_word==(c->sync_word?c->sync_word:(c->public_network?0x34:0x12)));
  assert(observation_spin_held==0);
}
static void receive_live(void)
{
  assert(!sx126x_lora_recv_async(&dev,receive,NULL));
  assert(atomic_get(&data.state)==SX126X_STATE_RX&&data.rx_cb==receive);
}
static void pause_ok(void)
{
  unsigned prior_irq=irq_joins,prior_watchdog=watchdog_joins,prior_retired=callback_retires;
  receive_live();input_irq=0;irq_reads=0;
  assert(!sx126x_pause_for_storage(&dev));
  assert(irq_joins==prior_irq+1&&watchdog_joins==prior_watchdog+1&&callback_retires==prior_retired+1);
  assert(atomic_get(&data.state)==SX126X_REST_STATE&&data.rx_cb==NULL);
}
static void generic_and_proved_epochs(void)
{
  struct lora_modem_config a=lf(),b=slow();cold();
  assert(!sx126x_lora_config(&dev,&a));expect_info(&a,true); /* unchanged needs no new permit */
  assert(!sx126x_lora_config(&dev,&b));expect_info(&b,false);
  assert(!sx126x_lora_config(&dev,&a));expect_info(&a,false);
  pause_ok();assert(!sx126x_lora_config(&dev,&b));expect_info(&b,true);
  assert(!sx126x_lora_config(&dev,&a));expect_info(&a,false); /* single-use */
  pause_ok();assert(!sx126x_lora_config(&dev,&a));expect_info(&a,true);
  puts("PASS generic LF/SF12/restoration stays unsupported; joined empty-IRQ pause heals exact tuple once");
}
static void queued_credits_and_loss_survive(void)
{
  struct lora_modem_config a=lf(),b=slow();cold();
  now_ms=101;sx126x_observe_rx(&data,SX126X_IRQ_RX_DONE,16,true);
  sx126x_observe_loss(&data);
  now_ms=202;pause_ok();assert(!sx126x_lora_config(&dev,&b));expect_info(&b,true);
  now_ms=303;sx126x_observe_rx(&data,SX126X_IRQ_RX_DONE,96,true);
  now_ms=404;pause_ok();assert(!sx126x_lora_config(&dev,&a));expect_info(&a,true);
  now_ms=505;sx126x_observe_rx(&data,SX126X_IRQ_RX_DONE,16,true);
  assert(atomic_get(&data.observation_enabled)==1&&atomic_get(&data.observation_loss)==1);
  const uint32_t air[]={354,4202,354};const uint64_t stamp[]={101,303,505};
  const uint8_t len[]={16,96,16};unsigned known=0,unknown=0;uint64_t credit=0;
  struct sx126x_airtime_observation e;
  while(!sx126x_airtime_observer_take(&dev,&e)){
    if(e.known){assert(known<3&&e.irq==SX126X_IRQ_RX_DONE&&e.length==len[known]&&
        e.airtime_ms==air[known]&&e.observed_at_ms==stamp[known]);credit+=e.airtime_ms;++known;}
    else{assert(e.airtime_ms==0&&e.length==0);++unknown;}
  }
  assert(known==3&&unknown==2&&credit==4910);
  assert(sx126x_airtime_observer_take_loss(&dev));assert(!sx126x_airtime_observer_take_loss(&dev));
  puts("PASS LF16 354ms / SF12 BW125 96B 4202ms / restored LF16 354ms retain queued credits, pause gaps and sticky loss");
}
static void tuple_changes(void)
{
  for(unsigned field=0;field<10;++field){
    cold();struct lora_modem_config c=lf();
    switch(field){case 0:++c.frequency;break;case 1:c.bandwidth=BW_125_KHZ;break;
      case 2:c.datarate=SF_12;break;case 3:c.coding_rate=4;break;case 4:++c.preamble_len;break;
      case 5:c.packet_crc_disable=true;break;case 6:c.iq_inverted=true;break;
      case 7:++c.sync_word;break;case 8:c.sync_word=0;c.public_network=true;break;
      case 9:hal.force_ldro=true;break;}
    /* Precheck must revoke before any successful commit, while retaining old cache. */
    struct sx126x_airtime_input old=data.observation_config;
    sx126x_observe_config(&dev,&c,true,false);expect_info(&c,false);
    assert(!memcmp(&old,&data.observation_config,sizeof(old)));
    pause_ok();assert(!sx126x_lora_config(&dev,&c));expect_info(&c,true);
  }
  cold();struct lora_modem_config c=lf();c.public_network=true;
  assert(!sx126x_lora_config(&dev,&c));expect_info(&c,true); /* explicit 0x2b makes toggle equivalent */
  c.tx_power=22;assert(!sx126x_lora_config(&dev,&c));expect_info(&c,true);
  puts("PASS frequency/BW/SF/CR/preamble/CRC/IQ/sync/default-network/LDRO changes need proof; effective-equivalent toggle and TX power keep support");
}
static void irq_in_config(void){sx126x_dio1_callback(&dev);}
static void intervening_activity(void)
{
  for(unsigned activity=0;activity<6;++activity){
    cold();pause_ok();
    switch(activity){case 0:sx126x_observe_rx(&data,SX126X_IRQ_RX_DONE,16,true);break;
      case 1:sx126x_dio1_callback(&dev);break;
      case 2:input_irq=0;sx126x_irq_work_handler(&data.irq_work);break;
      case 3:assert(!sx126x_lora_recv_async(&dev,NULL,NULL));break;
      case 4:receive_live();assert(!sx126x_lora_recv_async(&dev,NULL,NULL));break;
      case 5:config_step_hook=irq_in_config;break;}
    struct lora_modem_config b=slow();assert(!sx126x_lora_config(&dev,&b));
    expect_info(&b,false);config_step_hook=NULL;
    pause_ok();assert(!sx126x_lora_config(&dev,&b));expect_info(&b,true);
  }
  /* IRQ before first config must revoke the cold-config permission too. */
  boot();atomic_set(&data.state,SX126X_REST_STATE);data.rx_cb=NULL;
  assert(!sx126x_airtime_observer_start(&dev));sx126x_dio1_callback(&dev);
  struct lora_modem_config a=lf();assert(!sx126x_lora_config(&dev,&a));expect_info(&a,false);
  pause_ok();assert(!sx126x_lora_config(&dev,&a));expect_info(&a,true);
  puts("PASS RX observation, DIO admission, worker entry, generic stop/start and mid-config IRQ retire proof; cold IRQ cannot manufacture initial support");
}
static void rf_opcode_boundary(void)
{
  const uint8_t rf[]={SX126X_CMD_SET_RX,SX126X_CMD_SET_RX_DUTY_CYCLE,SX126X_CMD_SET_TX,
      SX126X_CMD_SET_CAD,SX126X_CMD_SET_TX_CONTINUOUS_WAVE,0xd2};
  const uint8_t metadata[]={0x8b,0x86,SX126X_CMD_SET_SLEEP,0x80};
  struct lora_modem_config b=slow();
  for(unsigned i=0;i<sizeof(rf);++i){
    cold();pause_ok();unsigned before=hal_spi_calls;
    assert(!sx126x_hal_write_cmd(&dev,rf[i],NULL,0));
    assert(hal_spi_calls==before+1&&hal_permit_at_spi==0);
    assert(!sx126x_lora_config(&dev,&b));expect_info(&b,false);
  }
  for(unsigned i=0;i<sizeof(metadata);++i){
    cold();pause_ok();unsigned before=hal_spi_calls;
    assert(!sx126x_hal_write_cmd(&dev,metadata[i],NULL,0));
    assert(hal_spi_calls==before+1&&hal_permit_at_spi==1);
    assert(!sx126x_lora_config(&dev,&b));expect_info(&b,true);
  }
  cold();pause_ok();hal_spi_error=-EIO;
  assert(sx126x_hal_write_cmd(&dev,SX126X_CMD_SET_RX,NULL,0)==-EIO&&hal_permit_at_spi==0);
  hal_spi_error=0;assert(!sx126x_lora_config(&dev,&b));expect_info(&b,false);
  puts("PASS actual HAL write body retires pause proof before SPI for all 6 RF opcodes including SPI failure; config/sleep/standby opcodes retain proof (synthetic SPI, MT/bench policy off)");
}
static void failed_pause_and_config(void)
{
  for(unsigned failure=0;failure<7;++failure){
    cold();receive_live();
    switch(failure){case 0:callback_error=-EIO;break;case 1:standby_error=-EIO;break;
      case 2:input_irq=SX126X_IRQ_PREAMBLE_DETECTED;break;case 3:input_irq=SX126X_IRQ_RX_DONE;break;
      case 4:irq_error=-EIO;break;case 5:clear_error=-EIO;break;case 6:sleep_error=-EIO;break;}
    assert(sx126x_pause_for_storage(&dev)<0);
    callback_error=standby_error=irq_error=clear_error=sleep_error=0;input_irq=0;
    /* Generic async stop is explicitly not a joined storage barrier. */
    assert(!sx126x_lora_recv_async(&dev,NULL,NULL));
    struct lora_modem_config b=slow();assert(!sx126x_lora_config(&dev,&b));expect_info(&b,false);
    pause_ok();assert(!sx126x_lora_config(&dev,&b));expect_info(&b,true);
  }
  for(unsigned failure=1;failure<=9;++failure){
    cold();pause_ok();struct lora_modem_config b=slow();
    config_fail_step=failure;config_step=0;
    assert(sx126x_lora_config(&dev,&b)==-EIO);expect_info(&b,false);
    config_fail_step=config_step=0;
    assert(!sx126x_lora_config(&dev,&b));expect_info(&b,false);
    pause_ok();assert(!sx126x_lora_config(&dev,&b));expect_info(&b,true);
  }
  puts("PASS callback/standby/pending IRQ/status/clear/sleep pause failures never heal; all 9 config failures consume proof and need a fresh successful pause");
}
int main(void)
{
  generic_and_proved_epochs();queued_credits_and_loss_survive();tuple_changes();intervening_activity();rf_opcode_boundary();failed_pause_and_config();
  if(data.observation_queue.init)assert(!pthread_mutex_destroy(&data.observation_queue.mutex));
  return 0;
}
