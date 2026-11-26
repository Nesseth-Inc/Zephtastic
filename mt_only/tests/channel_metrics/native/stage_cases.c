/* Actual native send/config/restart/getter bodies; synthetic HAL/RTOS seams.
 * CAD outcomes are injected, not an execution of native CAD/SPI or RF proof. */
static unsigned stage_groups;
static struct k_poll_signal selected_signal;
static uint8_t selected_payload[96];
static void stage_group(const char *name){++stage_groups;printf("PASS %s\n",name);}
static struct lora_modem_config profile(void)
{
  return (struct lora_modem_config){.frequency=906875000,.datarate=SF_12,
    .bandwidth=BW_125_KHZ,.preamble_len=16,.coding_rate=1,.sync_word=0x2b};
}
static void prepared(bool cad)
{
  boot();data.config=profile();data.config_valid=true;data.config.cad.mode=cad?LORA_CAD_MODE_LBT:0;
  atomic_set(&data.state,SX126X_REST_STATE);
}
static int send_selected(void)
{
  return sx126x_lora_send_async(&dev,selected_payload,sizeof(selected_payload),&selected_signal);
}
static struct sx126x_airtime_observer_info snapshot_stage(void)
{
  unsigned char before[sizeof(data)];memcpy(before,&data,sizeof(data));
  size_t prior_used=used;unsigned prior_ready=ready_calls,prior_cad=cad_calls;
  unsigned prior_write=write_buffer_calls,prior_read=read_regs_calls,prior_tx=tx_commands;
  unsigned prior_msgs=msgs,prior_signals=signal_calls;
  struct sx126x_airtime_observer_info info;
  assert(!sx126x_airtime_observer_info(&dev,&info));
  assert(!memcmp(before,&data,sizeof(data))&&used==prior_used&&observation_spin_held==0);
  assert(ready_calls==prior_ready&&cad_calls==prior_cad&&write_buffer_calls==prior_write&&
    read_regs_calls==prior_read&&tx_commands==prior_tx&&msgs==prior_msgs&&signal_calls==prior_signals);
  return info;
}
static void send_failed(unsigned stage,int result)
{
  assert(send_selected()==result);struct sx126x_airtime_observer_info info=snapshot_stage();
  assert(info.send_stage.calls==1&&info.send_stage.failed_call==1);
  assert(info.send_stage.stage==stage&&info.send_stage.result==result);
  assert(info.tx_started==0&&signal_calls==0&&observation_spin_held==0);
}
static void early_and_policy(void)
{
  prepared(false);policy_mt=true;policy_tx=false;send_failed(1,-EPERM);assert(!ready_calls&&!queue_purges);
  prepared(false);policy_bench=true;bench_admitted=false;send_failed(1,-EPERM);
  prepared(false);data.config_valid=false;send_failed(2,-EINVAL);assert(!ready_calls&&!queue_purges);
  prepared(false);assert(sx126x_lora_send_async(&dev,selected_payload,256,&selected_signal)==-EINVAL);
  struct sx126x_airtime_observer_info info=snapshot_stage();assert(info.send_stage.stage==3&&info.send_stage.result==-EINVAL);
  assert(!ready_calls&&!queue_purges);
  prepared(false);atomic_set(&data.state,SX126X_STATE_IDLE);send_failed(4,-EBUSY);
  assert(!ready_calls&&!queue_purges&&atomic_get(&data.state)==SX126X_STATE_IDLE);
  stage_group("actual send policy config size and state refusals record distinct stage without HAL admission");
}
static void ready_and_cad(void)
{
  prepared(false);ready_error=-ENODEV;send_failed(5,-ENODEV);assert(!queue_purges&&atomic_get(&data.state)==SX126X_REST_STATE);
  prepared(true);cad_result=1;send_failed(6,-EBUSY);assert(cad_calls==1&&!queue_purges);
  prepared(true);cad_result=-ETIMEDOUT;send_failed(7,-ETIMEDOUT);
  prepared(true);cad_post_busy=true;send_failed(8,-EBUSY);assert(!queue_purges);
  prepared(true);ready_fail_at=2;ready_fail_result=-EIO;send_failed(9,-EIO);assert(ready_calls==2&&!queue_purges);
  /* Actual cleanup rearm must not overwrite the earlier send failure tuple. */
  prepared(true);atomic_set(&data.state,SX126X_STATE_RX);cad_result=1;clear_error=-EACCES;
  send_failed(6,-EBUSY);struct sx126x_airtime_observer_info info=snapshot_stage();
  assert(info.restart_stage.calls==1&&info.restart_stage.stage==1&&info.restart_stage.result==-EACCES);
  assert(info.send_stage.stage==6&&info.send_stage.result==-EBUSY);
  stage_group("actual send ready CAD activity error and post-CAD busy remain distinct and failing cleanup cannot erase send boundary");
}
static void tx_boundaries(void)
{
  for(unsigned stage=10;stage<=16;++stage){
    prepared(false);
    switch(stage){case 10:params_error=-EIO;break;case 11:write_buffer_error=-EIO;break;
      case 12:workaround_error=-EIO;break;case 13:pa_error=-EIO;break;
      case 14:read_regs_error=-EIO;break;case 15:clamp_write_error=-EIO;break;case 16:tx_cmd_error=-EIO;break;}
    sleep_error=-EACCES;send_failed(stage,-EIO);
    assert(data.tx_async_signal==NULL&&queue_purges==1&&atomic_get(&data.state)==SX126X_REST_STATE);
    assert(strchr(trace,'s')!=NULL&&trace[used-1]=='U');
    assert(tx_commands==(stage==16?1u:0u));
  }
  stage_group("actual packet buffer workaround PA clamp read write and SetTx failures preserve original return through failing sleep cleanup");
}
static void successful_send(void)
{
  prepared(true);cad_result=-ENOSYS;assert(!send_selected());
  struct sx126x_airtime_observer_info info=snapshot_stage();
  assert(info.send_stage.calls==1&&!info.send_stage.failed_call&&!info.send_stage.stage&&!info.send_stage.result);
  assert(info.tx_started==1&&tx_commands==1&&cad_calls==1&&data.tx_async_signal==&selected_signal);
  assert(atomic_get(&data.state)==SX126X_STATE_TX&&queue_purges==1&&write_buffer_calls==1);
  assert(tx_timeout_ticks==640000u); /* Existing 10s floor; no new deadline. */
  stage_group("actual successful send including ENOSYS CAD fallback retains signal TX state and existing timeout with no invented failure");
}
static void configure_boundaries(void)
{
  struct lora_modem_config cfg=profile();prepared(false);validate_error=-EINVAL;
  assert(sx126x_lora_config(&dev,&cfg)==-EINVAL);assert(snapshot_stage().config_stage.stage==1);
  prepared(false);atomic_set(&data.state,SX126X_STATE_RX);
  assert(sx126x_lora_config(&dev,&cfg)==-EBUSY);assert(snapshot_stage().config_stage.stage==2);
  const unsigned expected[]={3,4,5,6,7,8,9,10,12};
  for(unsigned failure=1;failure<=9;++failure){
    prepared(false);config_fail_step=(int)failure;
    assert(sx126x_lora_config(&dev,&cfg)==-EIO);struct sx126x_airtime_observer_info info=snapshot_stage();
    assert(info.config_stage.calls==1&&info.config_stage.failed_call==1&&info.config_stage.stage==expected[failure-1]&&info.config_stage.result==-EIO);
  }
  for(unsigned mismatch=0;mismatch<2;++mismatch){
    prepared(false);policy_bench=true;read_regs_error=mismatch?0:-EACCES;sync_mismatch=mismatch!=0;
    assert(sx126x_lora_config(&dev,&cfg)==(mismatch?-EIO:-EACCES));
    assert(snapshot_stage().config_stage.stage==11);
  }
  stage_group("actual config validation state and every checked primitive plus bench sync readback record exact stage");
}
static void restart_boundaries(void)
{
  prepared(false);clear_error=-EACCES;assert(sx126x_restart_rx(&dev,&data)==-EACCES);
  assert(snapshot_stage().restart_stage.stage==1);
  prepared(false);data.rx_duty_cycle_enabled=true;duty_error=-EACCES;gain_error=-EIO;
  assert(sx126x_restart_rx(&dev,&data)==-EIO);struct sx126x_airtime_observer_info info=snapshot_stage();
  assert(info.restart_stage.stage==2&&info.restart_stage.result==-EACCES&&info.restart_stage.failed_call==1);
  prepared(false);data.rx_duty_cycle_enabled=true;duty_error=-EACCES;
  assert(!sx126x_restart_rx(&dev,&data));info=snapshot_stage();assert(info.restart_stage.stage==2&&info.restart_stage.result==-EACCES);
  prepared(false);restart_error=-EIO;assert(sx126x_restart_rx(&dev,&data)==-EIO);assert(snapshot_stage().restart_stage.stage==3);
  prepared(false);gain_error=-EIO;assert(sx126x_restart_rx(&dev,&data)==-EIO);assert(snapshot_stage().restart_stage.stage==4);
  stage_group("actual restart records first negative even when duty error was historically ignored and later gain error determines return");
}
static void saturation_and_success_retention(void)
{
  prepared(false);data.send_stage.calls=UINT32_MAX-1;data.config_valid=false;
  assert(send_selected()==-EINVAL);assert(send_selected()==-EINVAL);
  struct sx126x_airtime_observer_info info=snapshot_stage();assert(info.send_stage.calls==UINT32_MAX&&info.send_stage.failed_call==UINT32_MAX);
  data.config_valid=true;assert(!send_selected());info=snapshot_stage();
  assert(info.send_stage.calls==UINT32_MAX&&info.send_stage.stage==2&&info.send_stage.result==-EINVAL);
  prepared(false);struct lora_modem_config cfg=profile();data.config_stage.calls=UINT32_MAX-1;validate_error=-EINVAL;
  assert(sx126x_lora_config(&dev,&cfg)==-EINVAL);assert(sx126x_lora_config(&dev,&cfg)==-EINVAL);
  validate_error=0;assert(!sx126x_lora_config(&dev,&cfg));info=snapshot_stage();
  assert(info.config_stage.calls==UINT32_MAX&&info.config_stage.failed_call==UINT32_MAX&&info.config_stage.stage==1&&info.config_stage.result==-EINVAL);
  prepared(false);data.restart_stage.calls=UINT32_MAX-1;clear_error=-EIO;
  assert(sx126x_restart_rx(&dev,&data)==-EIO);assert(sx126x_restart_rx(&dev,&data)==-EIO);
  clear_error=0;assert(!sx126x_restart_rx(&dev,&data));info=snapshot_stage();
  assert(info.restart_stage.calls==UINT32_MAX&&info.restart_stage.failed_call==UINT32_MAX&&info.restart_stage.stage==1&&info.restart_stage.result==-EIO);
  stage_group("all three actual operation call ordinals saturate and later success preserves last negative tuple");
}
static void passive_unsupported_and_observer_enable(void)
{
  prepared(false);data.config_valid=false;assert(send_selected()==-EINVAL);
  struct lora_modem_config cfg=profile();validate_error=-EACCES;assert(sx126x_lora_config(&dev,&cfg)==-EACCES);
  clear_error=-EIO;assert(sx126x_restart_rx(&dev,&data)==-EIO);
  atomic_set(&data.observation_config_state,2);struct sx126x_airtime_observer_info before=snapshot_stage();
  assert(before.send_stage.stage==2&&before.config_stage.stage==1&&before.restart_stage.stage==1&&before.sf==0&&before.cr==0);
  data.rx_cb=NULL;atomic_set(&data.state,SX126X_REST_STATE);assert(!sx126x_airtime_observer_start(&dev));
  struct sx126x_airtime_observer_info after=snapshot_stage();
  assert(!memcmp(&before.send_stage,&after.send_stage,sizeof(before.send_stage))&&
    !memcmp(&before.config_stage,&after.config_stage,sizeof(before.config_stage))&&
    !memcmp(&before.restart_stage,&after.restart_stage,sizeof(before.restart_stage)));
  assert(after.config_state==2&&after.enabled==1&&after.sf==0&&after.bandwidth_hz==0);
  stage_group("passive unsupported snapshot retains all slots without I/O and observer enable cannot reset boot diagnostics");
}
int main(void)
{
  early_and_policy();ready_and_cad();tx_boundaries();successful_send();configure_boundaries();
  restart_boundaries();saturation_and_success_retention();passive_unsupported_and_observer_enable();
  assert(stage_groups==8);
  if(data.observation_queue.init)assert(!pthread_mutex_destroy(&data.observation_queue.mutex));
  return 0;
}
