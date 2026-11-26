int main(void){
  const int results[]={-EIO,0,1};
  for(unsigned enabled=0;enabled<2;++enabled)for(unsigned i=0;i<3;++i){
    boot();atomic_set(&data.observation_enabled,enabled);submit_result=results[i];
    sx126x_dio1_callback(&dev);
    printf("dio enabled=%u submit_result=%d submits=%u trace=%s\n",enabled,results[i],submits,trace);
    boot();atomic_set(&data.observation_enabled,enabled);submit_result=results[i];input_irq=SX126X_IRQ_RX_DONE;uint16_t detail=0;
    int r=sx126x_rx_activity_detail(&dev,&detail);
    printf("activity enabled=%u submit_result=%d ret=%d detail=%u submits=%u trace=%s\n",enabled,results[i],r,detail,submits,trace);
  }
  return 0;
}
