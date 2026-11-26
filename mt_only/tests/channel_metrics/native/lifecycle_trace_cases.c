int main(void){
  for(unsigned dc=0;dc<2;++dc)for(unsigned fail=0;fail<4;++fail){boot();data.rx_duty_cycle_enabled=dc;clear_error=fail==1?-EIO:0;restart_error=fail==2?-EIO:0;gain_error=fail==3?-EIO:0;int r=sx126x_restart_rx(&dev,&data);printf("restart dc=%u fail=%u ret=%d trace=%s\n",dc,fail,r,trace);}
  for(unsigned fail=0;fail<7;++fail){boot();data.config_valid=true;atomic_set(&data.state,SX126X_REST_STATE);if(fail==1)ready_error=-EIO;if(fail==2)clear_error=-EIO;if(fail==3)params_error=-EIO;if(fail==4)set_rx_error=-EIO;if(fail==5)gain_error=-EIO;if(fail==6)atomic_set(&data.state,SX126X_STATE_TX);int r=sx126x_lora_recv_async(&dev,receive,NULL);printf("start fail=%u ret=%d state=%d trace=%s\n",fail,r,atomic_get(&data.state),trace);}
  boot();assert(sx126x_lora_recv_async(&dev,NULL,NULL)==0);printf("stop state=%d trace=%s\n",atomic_get(&data.state),trace);
  for(unsigned fail=0;fail<=9;++fail){boot();atomic_set(&data.state,SX126X_REST_STATE);data.rx_cb=NULL;struct lora_modem_config cfg={.frequency=906875000,.datarate=SF_11,.bandwidth=BW_250_KHZ,.preamble_len=16,.coding_rate=1,.sync_word=0x2b};config_fail_step=fail;int r=sx126x_lora_config(&dev,&cfg);printf("config fail=%u ret=%d state=%d valid=%d trace=%s\n",fail,r,atomic_get(&data.state),data.config_valid,trace);}
  return 0;
}
