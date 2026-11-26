static void classifier_domain_cases(void){
  uint64_t checks=0;
  /* Exhaust each 16-bit dimension, not a claim of all 2^32 ordered pairs. */
  for(uint32_t latest=0;latest<=UINT16_MAX;++latest){
    bool quiet=latest==0||latest==P;
    assert(sx126x_channel_stale_uncertain(P,(uint16_t)latest)==!quiet);++checks;
    const uint16_t masks[]={0,H,FULL_STALE,0xffff,P|C,P|0x8000};
    for(unsigned j=0;j<sizeof(masks)/sizeof(masks[0]);++j){
      assert(sx126x_channel_stale_uncertain(masks[j],(uint16_t)latest));++checks;
    }
  }
  const uint16_t latest_cases[]={0,P,C,E,H,R,S,0xffff,0x8000,P|C};
  for(uint32_t mask=0;mask<=UINT16_MAX;++mask){
    for(unsigned j=0;j<sizeof(latest_cases)/sizeof(latest_cases[0]);++j){
      uint16_t latest=latest_cases[j];bool quiet=mask==P&&(latest==0||latest==P);
      assert(sx126x_channel_stale_uncertain((uint16_t)mask,latest)==!quiet);++checks;
    }
  }
  assert(checks==UINT64_C(1114112));
  printf("PASS actual stale classifier %llu checks; exhaustive uint16 domains, critical cross-input cases, not full Cartesian pairs\n",(unsigned long long)checks);
}
static void activity_queue_cases(void){
  for(unsigned enabled=0;enabled<2;++enabled)for(unsigned i=0;i<stale_case_count();++i){
    const struct stale_case *c=stale_prepare(i,enabled);uint16_t detail=0;
    int ret=sx126x_rx_activity_detail(&dev,&detail);
    assert(ret==c->result);assert((detail&31U)==c->reason);
    assert((detail>>5)==(ret<0?(unsigned)-ret:0U));
    assert(clear_calls==(c->clear_mask?1U:0U));assert(last_clear_mask==c->clear_mask);
    assert(submits==c->dispatches);assert(!callbacks&&!msgs);
    assert(sx126x_airtime_observer_take_loss(&dev)==(enabled&&c->loss));
    assert(!sx126x_airtime_observer_take_loss(&dev));
    unsigned n=0,expected_events=enabled?c->events:0;
    if(enabled){struct sx126x_airtime_observation event;
      while(sx126x_airtime_observer_take(&dev,&event)==0){
        assert(n<2);assert(!event.known&&!event.length&&!event.airtime_ms);
        assert(event.observed_at_ms==(uint64_t)now_ms);
        assert(event.irq==(n==0?c->event0:c->event1));++n;
      }
    }
    assert(n==expected_events);
    if(c->mode==LATE_ACTIVE)assert(data.rx_packet_active);
    if(ret>=0&&c->clear_mask){assert(atomic_get(&data.rx_stale_releases)==1);
      assert(atomic_get(&data.preamble_seen_at_ms)==0);}
    else assert(atomic_get(&data.rx_stale_releases)==0);
  }
  if(data.observation_queue.init)assert(!pthread_mutex_destroy(&data.observation_queue.mutex));
  printf("PASS actual stale-release queue/error/gate cases %u with observer off/on; late RX_DONE/header/work preserved\n",stale_case_count()*2);
}
int main(void){classifier_domain_cases();activity_queue_cases();return 0;}
