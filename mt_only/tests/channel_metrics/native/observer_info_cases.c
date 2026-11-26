/* Actual production getter is extracted by run.py; no duplicate getter model. */
static void zero_config(const struct sx126x_airtime_observer_info *info)
{
  assert(info->sf==0 && info->cr==0 && info->bandwidth_hz==0);
  assert(info->preamble_len==0 && info->ldro==0 && info->crc==0);
}

static struct sx126x_airtime_observer_info passive_snapshot(void)
{
  unsigned char before[sizeof(data)];
  memcpy(before,&data,sizeof(data));
  size_t prior_used=used;
  unsigned prior_callbacks=callbacks,prior_submits=submits,prior_msgs=msgs;
  unsigned prior_spin_locks=atomic_load(&observation_spin_locks);
  unsigned prior_spin_unlocks=atomic_load(&observation_spin_unlocks);
  struct sx126x_airtime_observer_info info;
  memset(&info,0xa5,sizeof(info));
  assert(sx126x_airtime_observer_info(&dev,&info)==0);
  assert(memcmp(before,&data,sizeof(data))==0);
  assert(used==prior_used && callbacks==prior_callbacks && submits==prior_submits && msgs==prior_msgs);
  assert(observation_spin_held==0);
  assert(atomic_load(&observation_spin_locks)-prior_spin_locks==
         atomic_load(&observation_spin_unlocks)-prior_spin_unlocks);
  if(info.config_state==1)assert(atomic_load(&observation_spin_locks)>prior_spin_locks);
  return info;
}

static void invalid_arguments(void)
{
  boot(); struct sx126x_airtime_observer_info info;
  memset(&info,0xa5,sizeof(info));
  assert(sx126x_airtime_observer_info(NULL,NULL)==-EINVAL);
  assert(sx126x_airtime_observer_info(&dev,NULL)==-EINVAL);
  assert(sx126x_airtime_observer_info(NULL,&info)==-EINVAL);
  assert(info.enabled==0 && info.config_state==0); zero_config(&info);
  struct device missing={0}; memset(&info,0xa5,sizeof(info));
  assert(sx126x_airtime_observer_info(&missing,&info)==-EINVAL);
  assert(info.enabled==0 && info.config_state==0); zero_config(&info);
  assert(used==0 && callbacks==0 && submits==0 && msgs==0);
  puts("PASS info NULL arguments refuse and zero valid output without HAL/activity");
}

static void unsupported_snapshots(void)
{
  boot(); memset(&data.observation_config,0xa5,sizeof(data.observation_config));
  struct sx126x_airtime_observer_info info=passive_snapshot();
  assert(info.enabled==0 && info.config_state==0); zero_config(&info);
  atomic_set(&data.state,SX126X_REST_STATE);data.rx_cb=NULL;
  assert(sx126x_airtime_observer_start(&dev)==0);
  info=passive_snapshot(); assert(info.enabled==1 && info.config_state==0); zero_config(&info);
  atomic_set(&data.observation_config_state,2);
  info=passive_snapshot(); assert(info.enabled==1 && info.config_state==2); zero_config(&info);
  puts("PASS info disabled/uncommitted/unsupported config fields remain zero");
}

static void committed_and_unproven_updates(void)
{
  boot(); enable();
  struct sx126x_airtime_input committed=data.observation_config;
  struct sx126x_airtime_observer_info info=passive_snapshot();
  assert(info.enabled==1 && info.config_state==1);
  assert(info.sf==11 && info.cr==1 && info.bandwidth_hz==250000 && info.preamble_len==16);
  assert(info.ldro==0 && info.crc==1);
  /* Mutable live config is deliberately unrelated to committed observer data. */
  data.config.datarate=SF_5; data.config.coding_rate=4;
  data.config.bandwidth=BW_125_KHZ; data.config.preamble_len=99; data.config.packet_crc_disable=true;
  dev.ready=false;
  struct sx126x_airtime_observer_info after=passive_snapshot();
  assert(memcmp(&info,&after,sizeof(info))==0);
  assert(memcmp(&data.observation_config,&committed,sizeof(committed))==0);
  dev.ready=true;
  struct lora_modem_config changed=data.config;
  sx126x_observe_config(&dev,&changed,true,false);
  after=passive_snapshot(); assert(after.enabled==1 && after.config_state==2); zero_config(&after);
  assert(memcmp(&data.observation_config,&committed,sizeof(committed))==0);
  sx126x_observe_config(&dev,&changed,true,false);
  after=passive_snapshot(); assert(after.config_state==2); zero_config(&after);
  sx126x_observe_config(&dev,&changed,true,true);
  after=passive_snapshot(); assert(after.enabled==1 && after.config_state==2);zero_config(&after);
  assert(memcmp(&data.observation_config,&committed,sizeof(committed))==0);
  puts("PASS info snapshots last complete tuple; unproved config precheck/commit never heals unsupported state");
}

static void preserves_queue_loss_and_activity(void)
{
  boot(); enable();
  sx126x_observe_rx(&data,SX126X_IRQ_RX_DONE,16,true);
  atomic_set(&data.observation_loss,1);
  atomic_set(&data.preamble_seen_at_ms,17);atomic_set(&data.header_seen_at_ms,23);
  data.rx_packet_active=true; data.irq_work.busy=1;
  uint32_t head=data.observation_queue.head,count=data.observation_queue.count;
  struct sx126x_airtime_observer_info first=passive_snapshot(),second=passive_snapshot();
  assert(memcmp(&first,&second,sizeof(first))==0);
  assert(data.observation_queue.head==head && data.observation_queue.count==count);
  assert(atomic_get(&data.observation_loss)==1);
  unsigned known;assert(drain(&known)==1 && known==1);
  assert(sx126x_airtime_observer_take_loss(&dev));
  assert(!sx126x_airtime_observer_take_loss(&dev));
  puts("PASS spin snapshot does not drain metadata, clear loss, change activity or call HAL/application mutex");
}

static void effective_preamble_and_boolean_fields(void)
{
  boot();atomic_set(&data.state,SX126X_REST_STATE);data.rx_cb=NULL;
  assert(!sx126x_airtime_observer_start(&dev));hal.force_ldro=true;
  struct lora_modem_config cfg={.frequency=906875000,.datarate=SF_5,.bandwidth=BW_125_KHZ,
      .preamble_len=4,.coding_rate=4,.packet_crc_disable=true,.sync_word=0x2b};
  sx126x_observe_config(&dev,&cfg,true,true);
  struct sx126x_airtime_observer_info info=passive_snapshot();
  assert(info.enabled==1 && info.config_state==1 && info.sf==5 && info.cr==4);
  assert(info.bandwidth_hz==125000 && info.preamble_len==12 && info.ldro==1 && info.crc==0);
  puts("PASS info reports effective preamble and both boolean polarities");
}

int main(void)
{
  invalid_arguments();unsupported_snapshots();committed_and_unproven_updates();preserves_queue_loss_and_activity();effective_preamble_and_boolean_fields();
  if(data.observation_queue.init)assert(!pthread_mutex_destroy(&data.observation_queue.mutex));
  return 0;
}
