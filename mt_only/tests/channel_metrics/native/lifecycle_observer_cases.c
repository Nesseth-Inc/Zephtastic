static void startup(void){boot();atomic_set(&data.state,SX126X_REST_STATE);data.rx_cb=NULL;assert(!sx126x_airtime_observer_start(&dev));}
static unsigned drain_unknown(void){struct sx126x_airtime_observation event;unsigned n=0;while(!sx126x_airtime_observer_take(&dev,&event)){assert(!event.known);++n;}return n;}
int main(void){
  for(unsigned fail=1;fail<=3;++fail){startup();clear_error=fail==1?-EIO:0;restart_error=fail==2?-EIO:0;gain_error=fail==3?-EIO:0;assert(sx126x_restart_rx(&dev,&data)==-EIO);assert(drain_unknown()>=1);}
  startup();assert(!sx126x_lora_recv_async(&dev,NULL,NULL));assert(drain_unknown()==1);
  struct lora_modem_config cfg={.frequency=906875000,.datarate=SF_11,.bandwidth=BW_250_KHZ,.preamble_len=16,.coding_rate=1,.sync_word=0x2b};
  for(unsigned fail=1;fail<=5;++fail){startup();assert(!sx126x_lora_config(&dev,&cfg));assert(sx126x_airtime_observer_supported(&dev));if(fail==1)ready_error=-EIO;if(fail==2)clear_error=-EIO;if(fail==3)params_error=-EIO;if(fail==4)set_rx_error=-EIO;if(fail==5)gain_error=-EIO;assert(sx126x_lora_recv_async(&dev,receive,NULL)==-EIO);assert(drain_unknown()>=1);}
  for(unsigned fail=0;fail<=9;++fail){startup();config_fail_step=fail;int r=sx126x_lora_config(&dev,&cfg);assert((r==0)==(fail==0));assert(sx126x_airtime_observer_supported(&dev)==(fail==0));config_fail_step=config_step=0;assert(!sx126x_lora_config(&dev,&cfg));assert(sx126x_airtime_observer_supported(&dev)==(fail==0));}
  startup();assert(!sx126x_lora_config(&dev,&cfg));assert(sx126x_airtime_observer_supported(&dev));++cfg.frequency;assert(!sx126x_lora_config(&dev,&cfg));assert(!sx126x_airtime_observer_supported(&dev));--cfg.frequency;assert(!sx126x_lora_config(&dev,&cfg));assert(!sx126x_airtime_observer_supported(&dev));
  if(data.observation_queue.init)assert(!pthread_mutex_destroy(&data.observation_queue.mutex));
  puts("PASS actual restart checked error loss, all five actual fresh-RX failure gaps, actual async stop gap, unproved config failure/reapply/change remain unsupported");return 0;
}
