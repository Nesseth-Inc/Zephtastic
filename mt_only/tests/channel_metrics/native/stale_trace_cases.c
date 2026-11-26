int main(void){
  for(unsigned enabled=0;enabled<2;++enabled)for(unsigned i=0;i<stale_case_count();++i){
    const struct stale_case *c=stale_prepare(i,enabled);uint16_t detail=0;
    int ret=sx126x_rx_activity_detail(&dev,&detail);
    printf("stale name=%s observer=%u ret=%d detail=%u clear=%u/%u submits=%u active=%u stamps=%d/%d/%d releases=%d trace=%s\n",
      c->name,enabled,ret,detail,clear_calls,last_clear_mask,submits,data.rx_packet_active,
      atomic_get(&data.preamble_seen_at_ms),atomic_get(&data.raw_header_seen_at_ms),
      atomic_get(&data.header_seen_at_ms),atomic_get(&data.rx_stale_releases),trace);
  }
  if(data.observation_queue.init)assert(!pthread_mutex_destroy(&data.observation_queue.mutex));
  return 0;
}
