static void enable(void){
  atomic_set(&data.state,SX126X_REST_STATE);data.rx_cb=NULL;
  assert(sx126x_airtime_observer_start(&dev)==0);
  struct lora_modem_config cfg={.frequency=906875000,.datarate=SF_11,.bandwidth=BW_250_KHZ,.preamble_len=16,.coding_rate=1,.sync_word=0x2b};
  sx126x_observe_config(&dev,&cfg,true,true);assert(sx126x_airtime_observer_supported(&dev));
  data.config=cfg;data.rx_cb=receive;atomic_set(&data.state,SX126X_STATE_RX);used=0;trace[0]=0;
}
static unsigned drain(unsigned *known){struct sx126x_airtime_observation e;unsigned n=0;*known=0;while(!sx126x_airtime_observer_take(&dev,&e)){++n;*known+=e.known;if(e.known){assert(e.length>0);assert(e.airtime_ms>0);}assert(e.observed_at_ms==(uint64_t)now_ms||e.observed_at_ms==UINT64_MAX);}return n;}
static void terminal_cases(void){
  const uint8_t lengths[]={1,15,16,255};unsigned n,k;
  for(unsigned i=0;i<sizeof(lengths);++i){boot();enable();input_length=lengths[i];assert(sx126x_handle_irq_status(&dev,SX126X_IRQ_RX_DONE)==0);n=drain(&k);assert(n==1&&k==1&&callbacks==1&&callback_length==lengths[i]);}
  for(unsigned f=0;f<12;++f){boot();enable();uint16_t bits=SX126X_IRQ_RX_DONE;switch(f){case 0:bits|=SX126X_IRQ_CRC_ERR;break;case 1:bits|=SX126X_IRQ_HEADER_ERR;break;case 2:input_length=0;break;case 3:buffer_error=-EIO;break;case 4:fifo_error=-EIO;break;case 5:restart_error=-EIO;break;case 6:clear_error=-EIO;break;case 7:mutate_generation=true;break;case 8:bits|=SX126X_IRQ_HEADER_ERR|SX126X_IRQ_HEADER_VALID;break;case 9:bits|=SX126X_IRQ_CRC_ERR|SX126X_IRQ_RX_TX_TIMEOUT;break;case 10:bits|=SX126X_IRQ_RX_TX_TIMEOUT;break;case 11:packet_error=-EIO;break;}
    (void)sx126x_handle_irq_status(&dev,bits);n=drain(&k);
    if(f<=4||f==8||f==9){assert(k==0);assert(n>=1);}else if(f==5||f==6){assert(k==1&&n>=2);}else{assert(k==1);}
    if(f==7)assert(callbacks==0);
  }
  const uint16_t unknown[]={SX126X_IRQ_CRC_ERR,SX126X_IRQ_HEADER_ERR,SX126X_IRQ_RX_TX_TIMEOUT};
  for(unsigned i=0;i<3;++i){boot();enable();(void)sx126x_handle_irq_status(&dev,unknown[i]);assert(drain(&k)>=1&&k==0);}
  boot();enable();irq_error=-EIO;sx126x_irq_work_handler(&data.irq_work);assert(drain(&k)==1&&k==0);
  boot();enable();now_ms=-1;sx126x_observe_rx(&data,SX126X_IRQ_RX_DONE,16,true);assert(drain(&k)==1&&k==0);
  puts("PASS terminal clean lengths 1/15/16/255, CRC/header/zero/read/clear/restart errors, generation change and cofires");
}
static void config_cases(void){
  unsigned n,k;boot();atomic_set(&data.state,SX126X_REST_STATE);data.rx_cb=NULL;
  dev.ready=false;assert(sx126x_airtime_observer_start(&dev)==-ENODEV);dev.ready=true;
  data.config_valid=true;assert(sx126x_airtime_observer_start(&dev)==-EBUSY);data.config_valid=false;
  data.irq_work.busy=1;assert(sx126x_airtime_observer_start(&dev)==-EBUSY);data.irq_work.busy=0;
  data.rx_cb=receive;assert(sx126x_airtime_observer_start(&dev)==-EBUSY);data.rx_cb=NULL;
  assert(sx126x_airtime_observer_start(&dev)==0);assert(sx126x_airtime_observer_start(&dev)==-EALREADY);assert(!sx126x_airtime_observer_supported(&dev));
  struct lora_modem_config cfg={.frequency=906875000,.datarate=SF_11,.bandwidth=BW_250_KHZ,.preamble_len=16,.coding_rate=1,.sync_word=0x2b};
  sx126x_observe_config(&dev,&cfg,true,false);assert(!sx126x_airtime_observer_supported(&dev));
  sx126x_observe_config(&dev,&cfg,true,true);assert(sx126x_airtime_observer_supported(&dev));
  sx126x_observe_config(&dev,&cfg,true,false);sx126x_observe_config(&dev,&cfg,true,true);assert(sx126x_airtime_observer_supported(&dev));
  sx126x_observe_rx(&data,SX126X_IRQ_RX_DONE,16,true);
  ++cfg.frequency;sx126x_observe_config(&dev,&cfg,true,false);assert(!sx126x_airtime_observer_supported(&dev));
  --cfg.frequency;sx126x_observe_config(&dev,&cfg,true,true);assert(!sx126x_airtime_observer_supported(&dev));
  sx126x_observe_rx(&data,SX126X_IRQ_RX_DONE,16,true);n=drain(&k);assert(n==2&&k==1);
  boot();enable();cfg=data.config;sx126x_observe_config(&dev,&cfg,false,true);assert(!sx126x_airtime_observer_supported(&dev));sx126x_observe_config(&dev,&cfg,true,true);assert(!sx126x_airtime_observer_supported(&dev));sx126x_observe_rx(&data,SX126X_IRQ_RX_DONE,16,true);assert(drain(&k)==1&&k==0);
  boot();enable();cfg=data.config;cfg.preamble_len=10;cfg.datarate=SF_5;sx126x_observe_config(&dev,&cfg,true,true);assert(!sx126x_airtime_observer_supported(&dev));
  boot();atomic_set(&data.state,SX126X_REST_STATE);data.rx_cb=NULL;assert(!sx126x_airtime_observer_start(&dev));cfg.datarate=SF_5;cfg.preamble_len=4;sx126x_observe_config(&dev,&cfg,true,true);assert(data.observation_config.preamble_len==12);cfg.preamble_len=12;sx126x_observe_config(&dev,&cfg,true,true);assert(sx126x_airtime_observer_supported(&dev));
  boot();atomic_set(&data.state,SX126X_REST_STATE);data.rx_cb=NULL;assert(!sx126x_airtime_observer_start(&dev));cfg.sync_word=0;cfg.public_network=false;sx126x_observe_config(&dev,&cfg,true,true);assert(sx126x_airtime_observer_supported(&dev));cfg.public_network=true;sx126x_observe_config(&dev,&cfg,true,false);assert(!sx126x_airtime_observer_supported(&dev));
  boot();enable();cfg=data.config;cfg.public_network=!cfg.public_network;sx126x_observe_config(&dev,&cfg,true,true);assert(sx126x_airtime_observer_supported(&dev));
  for(unsigned changed=0;changed<8;++changed){boot();enable();cfg=data.config;switch(changed){case 0:cfg.bandwidth=BW_125_KHZ;break;case 1:cfg.datarate=SF_5;break;case 2:++cfg.coding_rate;break;case 3:++cfg.preamble_len;break;case 4:cfg.packet_crc_disable=true;break;case 5:cfg.iq_inverted=true;break;case 6:++cfg.sync_word;break;case 7:hal.force_ldro=true;break;}sx126x_observe_config(&dev,&cfg,true,false);assert(!sx126x_airtime_observer_supported(&dev));sx126x_observe_rx(&data,SX126X_IRQ_RX_DONE,16,true);assert(drain(&k)==1&&k==0);}
  puts("PASS optional registration, no unproved runtime healing, semantic preamble, failed/in-progress config and late event");
}
static void *producer(void *arg){unsigned count=*(unsigned*)arg;for(unsigned i=0;i<count;++i)sx126x_observe_rx(&data,SX126X_IRQ_RX_DONE,16,true);return NULL;}
static void queue_cases(void){
  unsigned k;boot();enable();for(unsigned i=0;i<17;++i)sx126x_observe_rx(&data,SX126X_IRQ_RX_DONE,16,true);
  assert(drain(&k)==16&&k==16);assert(sx126x_airtime_observer_take_loss(&dev));assert(!sx126x_airtime_observer_take_loss(&dev));
  boot();enable();pthread_t threads[4];unsigned count=1000;for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],NULL,producer,&count));for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],NULL));assert(drain(&k)==16&&k==16);assert(sx126x_airtime_observer_take_loss(&dev));assert(!sx126x_airtime_observer_take_loss(&dev));
  assert(sx126x_airtime_observer_take(&dev,NULL)==-EINVAL);
  puts("PASS bounded queue and atomic sticky loss with 4 concurrent host producers; no Zephyr scheduler claim");
}
int main(void){terminal_cases();config_cases();queue_cases();if(data.observation_queue.init)assert(!pthread_mutex_destroy(&data.observation_queue.mutex));return 0;}
