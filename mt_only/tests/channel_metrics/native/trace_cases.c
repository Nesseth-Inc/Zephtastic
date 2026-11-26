int main(void){
  const uint16_t bits[]={SX126X_IRQ_RX_DONE,SX126X_IRQ_RX_DONE|SX126X_IRQ_CRC_ERR,SX126X_IRQ_RX_DONE|SX126X_IRQ_HEADER_ERR,SX126X_IRQ_RX_DONE|SX126X_IRQ_HEADER_ERR|SX126X_IRQ_HEADER_VALID,SX126X_IRQ_CRC_ERR,SX126X_IRQ_HEADER_ERR,SX126X_IRQ_RX_TX_TIMEOUT,SX126X_IRQ_RX_DONE|SX126X_IRQ_RX_TX_TIMEOUT,SX126X_IRQ_TX_DONE,SX126X_IRQ_HEADER_VALID,SX126X_IRQ_CAD_DONE,SX126X_IRQ_CAD_DONE|SX126X_IRQ_CAD_ACTIVITY_DETECTED};
  for(unsigned c=0;c<sizeof(bits)/sizeof(bits[0]);++c)for(unsigned fault=0;fault<9;++fault){boot();switch(fault){case 1:buffer_error=-EIO;break;case 2:fifo_error=-EIO;break;case 3:restart_error=-EIO;break;case 4:clear_error=-EIO;break;case 5:input_length=0;break;case 6:mutate_generation=true;break;case 7:data.rx_cb=NULL;break;case 8:data.rx_duty_cycle_enabled=true;break;}int r=sx126x_handle_irq_status(&dev,bits[c]);printf("case=%u fault=%u ret=%d state=%d callbacks=%u len=%u msgs=%u msg_status=%d active=%d trace=%s\n",c,fault,r,atomic_get(&data.state),callbacks,callback_length,msgs,msg_status,data.rx_packet_active,trace);}
  for(unsigned f=0;f<2;++f){boot();input_irq=SX126X_IRQ_RX_DONE;irq_error=f?-EIO:0;sx126x_irq_work_handler(&data.irq_work);printf("work=%u callbacks=%u trace=%s\n",f,callbacks,trace);}
  if(data.observation_queue.init)assert(pthread_mutex_destroy(&data.observation_queue.mutex)==0);
  return 0;
}
