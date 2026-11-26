// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "BrowserWire.h"
#include <stdio.h>
#include <string.h>


namespace mt::network::browser {
struct Page {const char *html;size_t size;const char *csp;};
struct Session {
    uint8_t token[16]{};
    uint32_t peer=0,generation=0;
    int64_t until=0;
    void clear(){*this=Session{};}
    bool matches(uint32_t address,const char *hex)const{
        if(!generation || address!=peer || !hex || strlen(hex)!=32)return false;
        static constexpr char digits[]="0123456789abcdef";
        unsigned diff=0;for(unsigned i=0;i<16;i++){
            diff|=unsigned(hex[i*2]^digits[token[i]>>4]);diff|=unsigned(hex[i*2+1]^digits[token[i]&15]);
        }
        return !diff;
    }
    void hex(char (&out)[33])const{
        const char digits[]="0123456789abcdef";
        for(unsigned i=0;i<16;i++){out[2*i]=digits[token[i]>>4];out[2*i+1]=digits[token[i]&15];}out[32]=0;
    }
};
struct Request {
    enum Target { Root, From, All, To, Unknown } target=Unknown;
    enum Method { Get, Put, Options, Other } method=Other;
    char *host=nullptr,*origin=nullptr,*type=nullptr,*cookies=nullptr,*site=nullptr;
    size_t length=0;
    bool hasLength=false;
    static bool once(char *&old,char *value){if(old)return false;old=value;return true;}
    static bool equalHeader(const char *a,const char *b){
        auto lower=[](unsigned char c){return c>='A' && c<='Z'?c+32:c;};
        while(*a && *b && lower(*a)==lower(*b)){++a;++b;}
        return *a==*b;
    }
    int parse(char *header){
        char *end=strstr(header,"\r\n");if(!end)return 400;*end=0;
        char *path=strchr(header,' ');if(!path)return 400;*path++=0;
        char *version=strchr(path,' ');if(!version)return 400;*version++=0;
        if(strcmp(version,"HTTP/1.1"))return 400;
        method=!strcmp(header,"GET")?Get:!strcmp(header,"PUT")?Put:!strcmp(header,"OPTIONS")?Options:Other;
        target=!strcmp(path,"/")?Root:!strcmp(path,"/api/v1/fromradio")||!strcmp(path,"/api/v1/fromradio?all=false")?From:
               !strcmp(path,"/api/v1/fromradio?all=true")?All:!strcmp(path,"/api/v1/toradio")?To:Unknown;
        for(char *line=end+2;*line;line=end+2){
            end=strstr(line,"\r\n");if(!end)return 400;
            if(end==line)break;
            *end=0;char *value=strchr(line,':');if(!value||value==line)return 400;*value++=0;
            for(const char *p=line;*p;p++)if(!((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='-'||*p=='_'))return 400;
            while(*value==' '||*value=='\t')++value;
            char *tail=value+strlen(value);while(tail>value&&(tail[-1]==' '||tail[-1]=='\t'))*--tail=0;
            if(equalHeader(line,"Host")){if(!once(host,value))return 400;}
            else if(equalHeader(line,"Origin")){if(!once(origin,value))return 400;}
            else if(equalHeader(line,"Sec-Fetch-Site")){if(!once(site,value))return 400;}
            else if(equalHeader(line,"Content-Type")){if(!once(type,value))return 400;}
            else if(equalHeader(line,"Cookie")){if(!once(cookies,value))return 400;}
            else if(equalHeader(line,"Transfer-Encoding")||equalHeader(line,"Expect"))return 400;
            else if(equalHeader(line,"Content-Length")){
                if(hasLength||!*value)return 400;
                hasLength=true;
                for(const char *p=value;*p;p++){
                    if(*p<'0'||*p>'9')return 400;
                    length=length*10+size_t(*p-'0');if(length>512)return 413;
                }
            }
        }
        if(!host)return 400;
        if(target==Unknown)return 404;
        if(method==Other || (target==Root&&method!=Get) || (target==To&&method==Get) ||
           (target!=To&&method==Put))return 405;
        if(method==Put && (!hasLength||!length))return 400;
        if(method==Put && (!type||!equalHeader(type,"application/x-protobuf")))return 415;
        if(method!=Put&&length)return 400;
        return 0;
    }
    bool sameOrigin(const char *local)const{
        char withPort[24],url[38];snprintf(withPort,sizeof(withPort),"%s:80",local);
        if(strcmp(host,local)&&strcmp(host,withPort))return false;
        if(site&&strcmp(site,"same-origin")&&strcmp(site,"none"))return false;
        if(!origin)return true;
        snprintf(url,sizeof(url),"http://%s",local);if(!strcmp(origin,url))return true;
        snprintf(url,sizeof(url),"http://%s",withPort);return !strcmp(origin,url);
    }
    bool token(char (&out)[33])const{
        out[0]=0;
        if(!cookies)return true;
        for(char *p=cookies;*p;){
            while(*p==' '||*p=='\t')++p;
            char *end=strchr(p,';');if(end)*end=0;
            if(!strncmp(p,"zt_session=",11)){
                if(out[0]||strlen(p+11)!=32)return false;
                memcpy(out,p+11,33);
            }
            if(!end)break;
            p=end+1;
        }
        return true;
    }
};
struct HttpMessaging {
    static constexpr unsigned HeaderLimit=4096, MaxBody=512, IdleMs=60000;
    char headers[HeaderLimit+1]{};
    uint8_t original[MaxBody]{}, body[MaxBody]{};
    Wire wire;
    Session session;
    template<class Backend> void expire(Backend &b){
        if(session.generation && (b.now()>=session.until || !b.stillLocal())){
            b.close(session.generation);session.clear();
        }
    }
    template<class IO> static bool reply(IO &io,int code,const char *mime,const void *bytes,size_t n,const char *extra=""){
        const char *reason=code==200?"OK":code==204?"No Content":code==400?"Bad Request":
            code==403?"Forbidden":code==404?"Not Found":code==405?"Method Not Allowed":
            code==408?"Request Timeout":code==409?"Conflict":code==413?"Content Too Large":
            code==415?"Unsupported Media Type":code==431?"Request Header Fields Too Large":"Service Unavailable";
        char h[768];const int hn=snprintf(h,sizeof(h),"HTTP/1.1 %d %s\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Type: %s\r\nContent-Length: %u\r\n%s\r\n",code,reason,mime,unsigned(n),extra);
        return hn>0 && size_t(hn)<sizeof(h) && io.write(h,hn) && (!n||io.write(bytes,n));
    }
    template<class IO> static bool fail(IO &io,int code){return reply(io,code,"text/plain",nullptr,0);}
    template<class IO,class Backend> void handle(IO &io,Backend &b,uint32_t peer,const char *local,const Page &page) {
        struct Clear {HttpMessaging &h;~Clear(){
            auto wipe=[](void *p,size_t n){volatile uint8_t *v=static_cast<volatile uint8_t*>(p);while(n--)*v++=0;};
            wipe(h.headers,sizeof(h.headers));wipe(h.original,sizeof(h.original));wipe(h.body,sizeof(h.body));wipe(&h.wire,sizeof(h.wire));
        }} clear{*this};
        expire(b);
        size_t used=0, headerEnd=0;
        const int64_t deadline=b.now()+2500;
        while(!headerEnd && used<HeaderLimit) {
            const int count=io.read(headers+used,HeaderLimit-used,deadline);
            if(count<=0){fail(io,408);return;}
            const size_t previous=used;used+=size_t(count);
            for(size_t i=previous;i<used;i++){
                if(i>=3&&!memcmp(headers+i-3,"\r\n\r\n",4)){headerEnd=i+1;break;}
                unsigned c=uint8_t(headers[i]);
                if((c<32&&c!='\r'&&c!='\n'&&c!='\t')||c==127 ||
                   (c=='\n' && (i==0||headers[i-1]!='\r')) ||
                   (i && headers[i-1]=='\r' && c!='\n')){fail(io,400);return;}
            }
        }
        if(!headerEnd){fail(io,431);return;}
        size_t count=used-headerEnd;
        if(count>MaxBody){fail(io,413);return;}
        memcpy(original,headers+headerEnd,count);headers[headerEnd]=0;
        Request req;const int parse=req.parse(headers);if(parse){fail(io,parse);return;}
        while(count<req.length) {
            int n=io.read(original+count,req.length-count,deadline);if(n<=0){fail(io,408);return;}count+=size_t(n);
        }
        if(!b.stillLocal()||!req.sameOrigin(local)){fail(io,403);return;}
        if(count>req.length){fail(io,400);return;}
        if(req.target==Request::Root){
            const char *prefix="Content-Security-Policy: ";
            char csp[512];int n=snprintf(csp,sizeof(csp),"%s%s\r\nReferrer-Policy: no-referrer\r\n",prefix,page.csp);
            if(n<=0||size_t(n)>=sizeof(csp)){fail(io,503);return;}
            reply(io,200,"text/html; charset=utf-8",page.html,page.size,csp);return;
        }
        if(req.method==Request::Options){reply(io,204,"application/x-protobuf",nullptr,0,
            req.target==Request::To?"Allow: OPTIONS, PUT\r\nAccess-Control-Allow-Methods: PUT, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\n":"Allow: OPTIONS, GET\r\nAccess-Control-Allow-Methods: GET, OPTIONS\r\n");return;}
        char cookie[33];if(!req.token(cookie)){fail(io,403);return;}
        expire(b);
        const bool owner=session.matches(peer,cookie);
        if(session.generation&&!owner){fail(io,409);return;}
        if(!owner && cookie[0] && req.method==Request::Get){fail(io,403);return;}
        if(req.method==Request::Get){
            if(!owner){reply(io,200,"application/x-protobuf",nullptr,0);return;}
            session.until=b.now()+IdleMs;
            if(!readRadio(io,b,req.target==Request::All)){b.close(session.generation);session.clear();}
            return;
        }
        size_t n=0;const auto op=wire.input(original,count,body,sizeof(body),n);
        if(op==Operation::Denied||op==Operation::Invalid){fail(io,op==Operation::Denied?403:400);return;}
        if(!b.stillLocal()){expire(b);fail(io,403);return;}
        if(owner && b.now()>=session.until){expire(b);fail(io,403);return;}
        char extra[120]{};
        if(!owner){
            if(op!=Operation::Start){fail(io,403);return;}
            if(!b.random(session.token,sizeof(session.token))){session.clear();fail(io,503);return;}
            session.generation=b.open();session.peer=peer;session.hex(cookie);
            if(!session.generation){session.clear();fail(io,409);return;}
        }
        if(op==Operation::Disconnect){b.close(session.generation);session.clear();strcpy(extra,"Set-Cookie: zt_session=; Path=/api/v1; HttpOnly; SameSite=Strict; Max-Age=0\r\n");}
        else {
            if(!b.queue(session.generation,body,n)){
                if(!owner){b.close(session.generation);session.clear();}
                fail(io,503);return;
            }
            session.until=b.now()+IdleMs;
            if(!owner)snprintf(extra,sizeof(extra),"Set-Cookie: zt_session=%s; Path=/api/v1; HttpOnly; SameSite=Strict\r\n",cookie);
        }
        if(!reply(io,200,"application/x-protobuf",original,count,extra)&&!owner&&session.generation){b.close(session.generation);session.clear();}
    }
private:
    template<class IO,class Backend> bool readRadio(IO &io,Backend &b,bool all){
        size_t remaining=b.available(session.generation);
        if(all && remaining) {
            const char h[]="HTTP/1.1 200 OK\r\nContent-Type: application/x-protobuf\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nTransfer-Encoding: chunked\r\n\r\n";
            if(!io.write(h,sizeof(h)-1))return false;
            while(remaining-- && b.stillLocal()){
                size_t size=0,n=0;if(!b.pop(session.generation,original,sizeof(original),size))break;
                if(!wire.output(original,size,body,sizeof(body),n))continue;
                char chunk[16];int len=snprintf(chunk,sizeof(chunk),"%x\r\n",unsigned(n));
                if(!io.write(chunk,len)||!io.write(body,n)||!io.write("\r\n",2))return false;
            }
            return b.stillLocal() && io.write("0\r\n\r\n",5);
        }
        while(remaining-- && b.stillLocal()){
            size_t size=0,n=0;if(!b.pop(session.generation,original,sizeof(original),size))break;
            if(wire.output(original,size,body,sizeof(body),n))return b.stillLocal() && reply(io,200,"application/x-protobuf",body,n);
        }
        return b.stillLocal() && reply(io,200,"application/x-protobuf",nullptr,0);
    }
};
}
