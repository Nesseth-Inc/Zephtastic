static void *settings_nvs_storage_get(struct settings_store *cs){return &CONTAINER_OF(cs,struct settings_nvs,cf_store)->cf_nvs;}
static const struct settings_store_itf interface={.csi_load=settings_nvs_load,.csi_save=settings_nvs_save,.csi_storage_get=settings_nvs_storage_get};
void model_no_fault(void){cut_at=0;read_error=0;}
void model_remount(void){
 backend=(struct settings_nvs){.cf_nvs={.sector_size=4096,.sector_count=sectors},.flash_dev=&dev};
 assert(settings_nvs_backend_init(&backend)==0);backend.cf_store.cs_itf=&interface;
 settings_save_dst=source=&backend.cf_store;
}
void model_reset(unsigned count){assert(count==4||count==8);sectors=count;model_no_fault();memset(flash_bytes,255,sizeof(flash_bytes));write_calls=read_calls=flash_read_calls=events=erase_calls=0;model_remount();}
void model_snapshot(void){memcpy(snapshot,flash_bytes,sizeof(snapshot));}
void model_restore(void){memcpy(flash_bytes,snapshot,sizeof(snapshot));model_no_fault();model_remount();}
void model_copy_flash(void *out){memcpy(out,flash_bytes,sizeof(flash_bytes));}
unsigned model_writes(void){return write_calls;}
unsigned model_reads(void){return read_calls;}
unsigned model_flash_reads(void){return flash_read_calls;}
unsigned model_erases(void){return erase_calls;}
unsigned model_events(void){return events;}
int model_ready(void){return backend.cf_nvs.ready;}
void model_fail(unsigned event,int phase){events=0;cut_at=event;cut_phase=phase;}
void model_read_error(uint16_t id,int error){read_id=id;read_error=error;}
void model_busy(int busy){backend.cf_nvs.nvs_lock.depth=busy?1:0;backend.cf_nvs.nvs_lock.owner=busy?2:0;}
void model_counter(uint16_t id){assert(nvs_write(&backend.cf_nvs,NVS_NAMECNT_ID,&id,sizeof(id))==sizeof(id));}
ssize_t model_put(uint16_t id,const void *data,size_t size){return nvs_write(&backend.cf_nvs,id,data,size);}
int model_delete(uint16_t id){return nvs_delete(&backend.cf_nvs,id);}

ssize_t model_get(uint16_t id,void *out,size_t capacity){return nvs_read(&backend.cf_nvs,id,out,capacity);}

// Enumerate mounted numeric records. Report sizes only; the test never dumps settings values.
void model_inventory(const char *label){
 unsigned nameBytes=0, valueBytes=0, directBytes=0, counterBytes=0, entries=0, aligned=0;
 for(unsigned id=0;id<UINT16_MAX;id++){
  uint8_t ignored;
  const ssize_t length=native_nvs_read(&backend.cf_nvs,id,&ignored,0);
  if(length==-ENOENT)continue;
  assert(length>0);
  ++entries;aligned+=((unsigned)length+3u)&~3u;
  if(id==NVS_NAMECNT_ID){counterBytes+=length;continue;}
  if(id>NVS_NAMECNT_ID&&id<=backend.last_name_id){
   char name[81]={0};assert((size_t)length<sizeof(name));
   assert(native_nvs_read(&backend.cf_nvs,id,name,sizeof(name)-1)==length);
   uint8_t dummy;const ssize_t value=native_nvs_read(&backend.cf_nvs,id+NVS_NAME_ID_OFFSET,&dummy,0);
   assert(value>0);printf("  %s value=%zd name=%zd bytes\n",name,value,length);nameBytes+=length;continue;
  }
  if(id>NVS_NAMECNT_ID+NVS_NAME_ID_OFFSET && id<=backend.last_name_id+NVS_NAME_ID_OFFSET){valueBytes+=length;continue;}
  directBytes+=length;printf("  direct ID 0x%04x=%zd bytes\n",id,length);
 }
 printf("%s: %u live records, settings values %u, names %u, direct %u, name-counter %u, aligned payload %u, per-record ATE %zu, NVS free %zd bytes\n",label,entries,valueBytes,nameBytes,directBytes,counterBytes,aligned,sizeof(struct nvs_ate),nvs_calc_free_space(&backend.cf_nvs));
}
