int main(void){
  const int results[]={-EIO,0,1};
  for(unsigned enabled=0;enabled<2;++enabled)for(unsigned i=0;i<3;++i){
    boot();atomic_set(&data.observation_enabled,enabled);submit_result=results[i];
    sx126x_dio1_callback(&dev);sx126x_dio1_callback(&dev);
    assert(submits==2&&!strcmp(trace,"QQ"));
    assert(sx126x_airtime_observer_take_loss(&dev)==(enabled&&results[i]<0));
    assert(!sx126x_airtime_observer_take_loss(&dev));
    assert(!data.observation_queue.count);
    boot();atomic_set(&data.observation_enabled,enabled);submit_result=results[i];input_irq=SX126X_IRQ_RX_DONE;uint16_t detail=0;
    int r=sx126x_rx_activity_detail(&dev,&detail);
    assert(r==(results[i]<0?results[i]:1));
    assert((detail&31U)==(results[i]<0?SX126X_RX_BUSY_DISPATCH_ERROR:SX126X_RX_BUSY_RX_DONE));
    assert((detail>>5)==(results[i]<0?EIO:0));
    assert(submits==1&&!strcmp(trace,"BLIQU"));
    assert(sx126x_airtime_observer_take_loss(&dev)==(enabled&&results[i]<0));
    assert(!sx126x_airtime_observer_take_loss(&dev));
  }
  boot();atomic_set(&data.observation_enabled,1);submit_result=-EIO;input_irq=SX126X_IRQ_RX_DONE;data.irq_work.busy=1;uint16_t detail=0;
  assert(sx126x_rx_activity_detail(&dev,&detail)==1);assert(detail==SX126X_RX_BUSY_WORK);assert(!submits&&!sx126x_airtime_observer_take_loss(&dev));
  puts("PASS actual DIO and RX_DONE activity dispatch failure loss; negative/0/1, off/on, sticky read-clear and unchanged gates");return 0;
}
