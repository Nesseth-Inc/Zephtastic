static settings_nvs store{};
static const settings_store_itf itf={settings_nvs_load,nullptr,nullptr,settings_nvs_save};
static int failures,total;
static void check(const char *name,bool ok){++total;if(!ok)++failures;printf("%s %s\n",ok?"PASS":"FAIL",name);}
static void reset(){records.clear();readErrors.clear();writes=deletes=mountError=writeError=deleteError=0;shortCount=-1;writeAfterCommit=false;store={};store.cf_store.cs_itf=&itf;store.flash_dev=(void*)1;store.cf_nvs.flash_device=(void*)1;store.last_name_id=NVS_NAMECNT_ID;source=&store.cf_store;settings_save_dst=source;}
static void entry(uint16_t id,const char *name,const uint8_t *p,size_t n){records[id]={(const uint8_t*)name,(const uint8_t*)name+strlen(name)};records[id+NVS_NAME_ID_OFFSET]={p,p+n};store.last_name_id=std::max(store.last_name_id,id);}
int main(){
 mt::ZephyrSettings adapter;uint8_t b[mt::SettingsJournal::RecordSize]{};mt::Config c;
 reset();check("adapter absent maps to ENOENT without mutation",adapter.read("mtbase/state0",b,sizeof(b))==-ENOENT&&!writes&&!deletes);
 reset();entry(0x8001,"mtbase/state0",c.bytes,sizeof(c.bytes));check("adapter exact value reads bytes",adapter.read("mtbase/state0",b,sizeof(b))==sizeof(c.bytes)&&!memcmp(b,c.bytes,sizeof(c.bytes)));
 check("adapter oversize fails before truncated read",adapter.read("mtbase/state0",b,16)==-E2BIG);
 reset();entry(0x8001,"mtbase/state0",c.bytes,sizeof(c.bytes));readErrors[0xc001]=-EIO;
 int r=adapter.read("mtbase/state0",b,sizeof(b));check("value EIO propagates without deleting valid name",r==-EIO&&!writes&&!deletes);
 reset();entry(0x8001,"mtbase/state0",c.bytes,sizeof(c.bytes));readErrors[0x8001]=-EIO;
 r=adapter.read("mtbase/state0",b,sizeof(b));check("name EIO propagates without deleting valid value",r==-EIO&&!writes&&!deletes);
 reset();entry(0x8001,"mtbase/state0",c.bytes,sizeof(c.bytes));readErrors[0x8001]=-EIO;readErrors[0xc001]=-EIO;
 r=adapter.read("mtbase/state0",b,sizeof(b));check("both EIO propagate without counter mutation",r==-EIO&&!writes&&!deletes);
 reset();entry(0x8001,"mtbase/state0",c.bytes,sizeof(c.bytes));readErrors[0xc001]=-EIO;
 mt::SettingsJournal journal;r=journal.load(adapter);check("journal cannot become fresh on hidden backend EIO",r==-EIO&&!journal.ready()&&!writes&&!deletes);
 reset();readErrors[NVS_NAMECNT_ID]=-EIO;r=settings_nvs_backend_init(&store);check("init counter EIO propagates",r==-EIO&&!writes&&!deletes);
 reset();records[NVS_NAMECNT_ID]={1};r=settings_nvs_backend_init(&store);check("init one-byte counter rejected",r==-EIO&&!writes&&!deletes);
 reset();r=settings_nvs_backend_init(&store);check("init genuinely absent counter remains fresh",r==0&&store.last_name_id==NVS_NAMECNT_ID&&!writes&&!deletes);
 reset();mountError=-EIO;r=settings_nvs_backend_init(&store);check("mount failure propagates",r==-EIO);
 reset();entry(0x8001,"mtbase/state0",c.bytes,sizeof(c.bytes));readErrors[0x8001]=-EIO;
 r=adapter.write("mtbase/state0",c.bytes,sizeof(c.bytes));check("save name scan EIO does not create duplicate IDs",r==-EIO&&!writes&&!deletes);
 reset();entry(0x8001,"mtbase/state0",c.bytes,sizeof(c.bytes));settings_nvs_cache_add(&store,"mtbase/state0",0x8001);readErrors[0x8001]=-EIO;
 r=adapter.write("mtbase/state0",c.bytes,sizeof(c.bytes));check("cache/scan read EIO propagates without writes",r==-EIO&&!writes&&!deletes);
 reset();entry(0x8001,"mtbase/state0",c.bytes,sizeof(c.bytes));records.erase(0xc001);r=adapter.read("mtbase/state0",b,sizeof(b));check("interrupted missing value pair still cleans up",r==-ENOENT&&deletes==2);
 reset();entry(0x8001,"mtbase/state0",c.bytes,sizeof(c.bytes));records.erase(0xc001);deleteError=-EIO;r=adapter.read("mtbase/state0",b,sizeof(b));check("cleanup deletion EIO propagates",r==-EIO);
 reset();store.last_name_id=0x8001;writeError=-EIO;r=adapter.read("mtbase/state0",b,sizeof(b));check("cleanup counter update EIO propagates",r==-EIO);
 for(const auto &bytes:std::vector<std::vector<uint8_t>>{{},{0xff,0x7f},{0x00,0xc0},{0x01,0x80,0x00}}){reset();records[NVS_NAMECNT_ID]=bytes;r=settings_nvs_backend_init(&store);check("init invalid counter length/range rejected",r==-EIO);}
 reset();mt::SettingsJournal uncertain;check("journal setup writes two valid slots",!uncertain.load(adapter)&&!uncertain.save(adapter,c));
 mt::Config older=c;older.setDisplay(60,false);check("journal setup selects second generation",!uncertain.save(adapter,older));
 mt::Config newer=c;newer.setDisplay(120,true);writeError=-EIO;writeAfterCommit=true;
 r=uncertain.save(adapter,newer);check("write EIO with completed flash commit latches uncertainty",r==-EIO&&!uncertain.ready());
 writeError=0;writeAfterCommit=false;r=uncertain.save(adapter,older);check("unchanged save cannot ACK after uncertain write",r==-EACCES);
 mt::SettingsJournal reboot;check("uncertain commit can become newest valid record after reboot",!reboot.load(adapter)&&reboot.config().same(newer));
 printf("%d/%d contract checks passed; cache=%d; no hardware/RF\n",total-failures,total,CONFIG_SETTINGS_NVS_NAME_CACHE);return failures?1:0;
}
