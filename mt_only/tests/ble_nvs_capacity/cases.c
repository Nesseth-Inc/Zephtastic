// Offline synthetic records only. Actual NVS, settings_nvs_save and backend init above.
struct item {const char *name;size_t length;uint8_t data[2048];};
static struct item items[]={
 {"mtbase/config",80,{0}}, {"mtbase/state0",624,{0}}, {"mtbase/state1",624,{0}},
 {"mtbase/nodes0",1492,{0}}, {"mtbase/nodes1",1492,{0}},
 {"mtidentity/state0",84,{0}}, {"mtidentity/state1",84,{0}},
 {"mtpki/ledger0",1944,{0}}, {"mtpki/ledger1",1944,{0}},
 {"mtmod/state0",128,{0}},{"mtmod/state1",128,{0}},
 {"mtble/state0",32,{0}},{"mtble/state1",32,{0}},
 {"bt/id",7,{0}},{"bt/irk",16,{0}},{"bt/name",18,{0}},{"bt/hash",16,{0}},
 {"bt/keys/0000000000010",56,{0}},
 {"bt/ccc/0000000000010",8,{0}},
 {"bt/sc/0000000000010",4,{0}},
 {"bt/cf/0000000000010",2,{0}},
 {"bt/keys/0000000000020",56,{0}},
 {"bt/ccc/0000000000020",8,{0}},
 {"bt/sc/0000000000020",4,{0}},
 {"bt/cf/0000000000020",2,{0}},
 {"bt/keys/0000000000030",56,{0}},
 {"bt/ccc/0000000000030",8,{0}},
 {"bt/sc/0000000000030",4,{0}},
 {"bt/cf/0000000000030",2,{0}},
 {"bt/keys/0000000000040",56,{0}},
 {"bt/ccc/0000000000040",8,{0}},
 {"bt/sc/0000000000040",4,{0}},
 {"bt/cf/0000000000040",2,{0}},
 {"bt/keys/0000000000050",56,{0}},
 {"bt/ccc/0000000000050",8,{0}},
 {"bt/sc/0000000000050",4,{0}},
 {"bt/cf/0000000000050",2,{0}}
};
static struct settings_nvs backend;
static void mount_fresh(void){backend=(struct settings_nvs){.cf_nvs={.sector_size=4096,.sector_count=sectors},.flash_dev=&dev};int rc=settings_nvs_backend_init(&backend);if(rc){fprintf(stderr,"mount failed %d events%u phase%d\n",rc,cut_at,cut_phase);abort();}}
static void pattern(struct item *p,unsigned generation){for(size_t i=0;i<p->length;i++)p->data[i]=(uint8_t)(i*29+generation*17+(generation>>8));}
static int save(unsigned index){return settings_nvs_save(&backend.cf_store,items[index].name,(const char *)items[index].data,items[index].length);}
static int get(unsigned index,uint8_t *out){char name[97];for(uint16_t id=0x8001;id<=backend.last_name_id;id++){ssize_t n=nvs_read(&backend.cf_nvs,id,name,96);if(n==-ENOENT)continue;assert(n>=0&&n<97);name[n]=0;if(!strcmp(name,items[index].name))return (int)nvs_read(&backend.cf_nvs,id+0x4000,out,2048);}return -ENOENT;}
static void verify_all(int uncertain,const uint8_t *old){uint8_t got[2048];for(unsigned i=0;i<ARRAY_SIZE(items);i++){int n=get(i,got);assert(n==(int)items[i].length);if(memcmp(got,items[i].data,items[i].length)){assert((int)i==uncertain&&old&&!memcmp(got,old,items[i].length));}}}
static void exercise(unsigned count){sectors=count;memset(flash_bytes,255,sizeof(flash_bytes));events=reads=writes=erases=0;cuts=false;mount_fresh();
 for(unsigned i=0;i<ARRAY_SIZE(items);i++){pattern(&items[i],i+1);assert(save(i)==0);}verify_all(-1,NULL);
 ssize_t first_free=nvs_calc_free_space(&backend.cf_nvs);unsigned erases_before=erases;
 for(unsigned i=0;i<1200;i++){
  unsigned id=i%ARRAY_SIZE(items);pattern(&items[id],i+30);assert(save(id)==0);verify_all(-1,NULL);
  if(i%19==0){mount_fresh();verify_all(-1,NULL);}
 }
 assert(erases>erases_before);printf("%usector mixed workload PASS:1200 updates, all37 synthetic MT/BT records checked each write, remount64, erases%u, free%zd ->%zd\n",count,erases,first_free,nvs_calc_free_space(&backend.cf_nvs));
 // Find an update that actually collects/erases, then reproduce every hardware mutation boundary.
 static uint8_t snapshot[32768],old[2048];unsigned target=7,total=0;
 for(unsigned attempt=0;attempt<20;attempt++){
  memcpy(snapshot,flash_bytes,sizeof(snapshot));memcpy(old,items[target].data,items[target].length);pattern(&items[target],2000+attempt);
  unsigned before=erases;events=0;assert(save(target)==0);if(erases>before){total=events;break;}
 }
 assert(total>0);unsigned cases=0;
 for(unsigned event=1;event<=total;event++)for(int phase=0;phase<3;phase++){
  memcpy(flash_bytes,snapshot,sizeof(snapshot));cuts=false;mount_fresh();events=0;cut_at=event;cut_phase=phase;
  if(!setjmp(power_loss)){cuts=true;(void)save(target);assert(!"cut not reached");}
  cuts=false;mount_fresh();verify_all((int)target,old);assert(save(target)==0);verify_all(-1,NULL);++cases;
 }
 printf("%usector GC interruption PASS:%u cuts (before/partial/after every%u mutation boundary), all unrelated records preserved, uncertain record old-or-new, fresh remount/readback+retry verified\n",count,cases,total);
}
static void full_store_refuses_without_losing_existing(unsigned count){
 sectors=count;memset(flash_bytes,255,sizeof(flash_bytes));cuts=false;mount_fresh();
 for(unsigned i=0;i<ARRAY_SIZE(items);++i){pattern(&items[i],i+4000);assert(save(i)==0);}
 bool full=false;uint8_t filler[2048];memset(filler,0x5a,sizeof(filler));
 for(unsigned i=0;i<24;++i){char name[32];snprintf(name,sizeof(name),"synthetic/filler%u",i);int rc=settings_nvs_save(&backend.cf_store,name,(const char*)filler,sizeof(filler));if(rc){assert(rc==-ENOSPC);full=true;break;}}
 assert(full);verify_all(-1,NULL);mount_fresh();verify_all(-1,NULL);
 printf("%usector capacity PASS: full-store returns ENOSPC; all37 baseline records byte-exact before/after remount\n",count);
}
int main(void){assert(ARRAY_SIZE(items)==37);exercise(4);exercise(8);full_store_refuses_without_losing_existing(4);full_store_refuses_without_losing_existing(8);return 0;}
