// SPDX-License-Identifier: GPL-3.0-only
void fs_dir_t_init(fs_dir_t *d){d->value=nullptr;}
int fs_opendir(fs_dir_t *d,const char *p){d->value=opendir(local(p).c_str());return d->value?0:-errno;}
int fs_closedir(fs_dir_t *d){return closedir(d->value)?-errno:0;}
int fs_readdir(fs_dir_t *d,fs_dirent *e){
    errno=0;auto *next=readdir(d->value);
    if(!next){e->name[0]=0;return errno?-errno:0;}
    assert(strlen(next->d_name)<sizeof(e->name));strcpy(e->name,next->d_name);
    struct stat st{};if(fstatat(dirfd(d->value),e->name,&st,AT_SYMLINK_NOFOLLOW))return -errno;
    e->type=S_ISDIR(st.st_mode)?FS_DIR_ENTRY_DIR:FS_DIR_ENTRY_FILE;e->size=st.st_size;return 0;
}
void publicTreeCases(){
    mt::modules::RangeTestLog log;RangeFile file(log);XModem protocol;
    assert(mkdir((root+"/docs").c_str(),0700)==0);
    auto packet=[](Control kind,unsigned seq,const char *text){
        Packet p{};p.control=kind;p.seq=seq;p.buffer.size=strlen(text);
        memcpy(p.buffer.bytes,text,p.buffer.size);p.crc16=crc(p.buffer.bytes,p.buffer.size);return p;
    };
    Packet out{};
    auto upload=[&](const char *name,const char *body,bool commit){
        protocol.handle(file,packet(SOH,0,name),out);assert(out.control==ACK);
        protocol.handle(file,packet(SOH,1,body),out);assert(out.control==ACK);
        protocol.handle(file,packet(commit?EOT:CAN,0,""),out);assert(out.control==ACK);
    };
    upload("/static/docs/notes.txt","first",true);upload("/static/docs/notes.txt","aborted",false);
    protocol.handle(file,packet(STX,0,"/static/docs/notes.txt"),out);assert(out.control==SOH&&out.buffer.size==5&&!memcmp(out.buffer.bytes,"first",5));
    protocol.handle(file,packet(ACK,0,""),out);assert(out.control==EOT);
    for(const char *path:{"/static/../secret","/prefs/config","/static//x","/static/.zt-upload","/static/x/../../z","/static/a/b/c/d","/static/rangetest.csv.tmp"}){
        protocol.handle(file,packet(SOH,0,path),out);assert(out.control==NAK&&!protocol.active());
        assert(log.removeFile(path)==-EINVAL);
    }
    auto embedded=packet(SOH,0,"/static/ok");embedded.buffer.bytes[9]=0;embedded.buffer.bytes[10]='x';embedded.buffer.size=11;
    protocol.handle(file,embedded,out);assert(out.control==NAK);
    upload("/static/rangetest.csv","range",true);
    TreeManifest manifest;
    struct Info{char file_name[228];uint32_t size_bytes;} info{};
    auto item=[&](unsigned owner,const char *name,unsigned size){
        assert(manifest.request(owner,info)==-EAGAIN);manifest.poll();assert(manifest.request(owner,info)==2);
        assert(!strcmp(info.file_name,name)&&info.size_bytes==size);
        assert(manifest.request(owner,info)==2&&!strcmp(info.file_name,name));manifest.advance(owner);
    };
    item(0,"/static/docs/notes.txt",5);item(1,"/static/docs/notes.txt",5);
    assert(log.removeFile("/static/docs/notes.txt")==0);
    assert(log.removeFile("/static/docs")==-EISDIR);
    item(0,"/static/rangetest.csv",5);item(1,"/static/rangetest.csv",5);
    assert(manifest.request(0,info)==-EAGAIN);manifest.poll();assert(manifest.request(0,info)==0);
    manifest.reset(1);item(1,"/static/rangetest.csv",5);
    for(unsigned i=0;i<65;++i){char n[40];snprintf(n,sizeof(n),"/static/test-%02u",i);upload(n,"x",true);}
    manifest.reset(0);unsigned count=0;
    while(true){
        int rc=manifest.request(0,info);if(rc==-EAGAIN){manifest.poll();continue;}if(!rc)break;
        assert(rc==2);manifest.advance(0);++count;
    }
    assert(count==64);
    puts("PASS public volume XModem upload, cancel, nested download, deletion, traversal rejection, two manifests and stock 64-file limit");
}
