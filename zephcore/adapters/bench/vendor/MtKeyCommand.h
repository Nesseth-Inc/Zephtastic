// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
namespace bench {
inline void wipeKeyBuffer(void *buffer,size_t len) {
  volatile uint8_t *p=static_cast<volatile uint8_t *>(buffer);
  while(len--) *p++=0;
}
inline bool isNodeKeyCommandPrefix(const char *line,size_t len) {
  return len>=12 && memcmp(line,"mt node-key ",12)==0;
}
inline bool isSensitiveNodeKeyPrefix(const char *line,size_t len) {
  size_t i=0;while(i<len&&(line[i]==' '||line[i]=='\t'))++i;
  const char *tokens[2]={"mt","node-key"};
  for(unsigned token=0;token<2;++token) {
    const char *word=tokens[token];
    while(*word){if(i>=len)return false;char c=line[i++];if(c>='A'&&c<='Z')c=char(c-'A'+'a');if(c!=*word++)return false;}
    if(!token){if(i>=len||(line[i]!=' '&&line[i]!='\t'))return false;while(i<len&&(line[i]==' '||line[i]=='\t'))++i;}
  }
  return true;
}
inline bool isKeyCommandPrefix(const char *line,size_t len) {
  return len>=7 && memcmp(line,"mt key ",7)==0;
}
inline bool isSensitivePasswordPrefix(const char *line,size_t len) {
  size_t i=0;while(i<len && (line[i]==' ' || line[i]=='\t'))++i;
  const char *word="password";
  while(*word) {
    if(i>=len)return false;
    char c=line[i++];if(c>='A' && c<='Z')c=char(c-'A'+'a');
    if(c!=*word++)return false;
  }
  return true;
}
inline bool isSensitiveUplinkPrefix(const char *line,size_t len) {
 size_t i=0;while(i<len && (line[i]==' ' || line[i]=='\t'))++i;
 const char *prefix="set uplink.";
 while(*prefix) {
  if(i>=len)return false;
  char c=line[i++];if(c>='A' && c<='Z')c=char(c-'A'+'a');
  if(c!=*prefix++)return false;
 }
 return true;
}
inline bool isSensitiveManagementSecretPrefix(const char *line,size_t len) {
  size_t begin=0;while(begin<len && (line[begin]==' ' || line[begin]=='\t'))++begin;
  static const char *prefixes[]={"set guest.password","set prv.key","get prv.key"};
  for(const char *prefix:prefixes) {
    size_t i=begin,j=0;
    while(prefix[j] && i<len) {
      if(prefix[j]==' ') {
        if(line[i]!=' ' && line[i]!='\t')break;
        while(i<len && (line[i]==' ' || line[i]=='\t'))++i;
        ++j;continue;
      }
      char c=line[i++];if(c>='A' && c<='Z')c=char(c-'A'+'a');
      if(c!=prefix[j])break;
      ++j;
    }
    if(prefix[j]==0)return true;
  }
  return false;
}
inline bool isSensitiveKeyPrefix(const char *line,size_t len) {
  if(isSensitiveNodeKeyPrefix(line,len))return true;
  size_t i=0;
  while(i<len && (line[i]==' ' || line[i]=='\t')) ++i;
  const char *tokens[2]={"mt","key"};
  for(unsigned token=0;token<2;++token) {
    const char *word=tokens[token];
    while(*word) {
      if(i>=len) return false;
      char c=line[i++];if(c>='A' && c<='Z') c=char(c-'A'+'a');
      if(c!=*word++) return false;
    }
    if(token==0) {
      if(i>=len || (line[i]!=' ' && line[i]!='\t')) return false;
      while(i<len && (line[i]==' ' || line[i]=='\t')) ++i;
    }
  }
  // Latch as soon as "key" is complete, before a separator, NUL or payload.
  return true;
}
struct ParsedMtKey {
  uint8_t hash=0,bytes[32]{};
  size_t size=0;
  ~ParsedMtKey() { wipeKeyBuffer(bytes,sizeof(bytes)); }
};
inline int keyHexDigit(char c) {
  if(c>='0' && c<='9') return c-'0';
  if(c>='a' && c<='f') return c-'a'+10;
  if(c>='A' && c<='F') return c-'A'+10;
  return -1;
}
inline bool parseMtKeyArgument(const char *argument,ParsedMtKey &out) {
  wipeKeyBuffer(out.bytes,sizeof(out.bytes));out.size=0;out.hash=0;
  if(!argument || *argument<'0' || *argument>'9') return false;
  unsigned hash=0,count=0;
  while(*argument>='0' && *argument<='9') {
    hash=hash*10U+unsigned(*argument++-'0');
    if(++count>3 || hash>255) return false;
  }
  if(*argument++!=' ') return false;
  const size_t n=strlen(argument);
  if(n!=32 && n!=64) return false;
  for(size_t i=0;i<n/2;++i) {
    const int a=keyHexDigit(argument[2*i]),b=keyHexDigit(argument[2*i+1]);
    if(a<0 || b<0) {wipeKeyBuffer(out.bytes,sizeof(out.bytes));return false;}
    out.bytes[i]=uint8_t((a<<4)|b);
  }
  out.hash=uint8_t(hash);out.size=n/2;return true;
}
struct ParsedMtPublicKey {
  uint32_t node=0;uint8_t bytes[32]{};
};
inline bool parseMtTrustArgument(const char *argument,ParsedMtPublicKey &out) {
  out=ParsedMtPublicKey{};
  if(!argument || strlen(argument)!=73 || argument[8]!=' ')return false;
  for(unsigned i=0;i<8;++i) {
    const int digit=keyHexDigit(argument[i]);if(digit<0)return false;
    out.node=(out.node<<4)|uint32_t(digit);
  }
  if(out.node<=1 || out.node>=0xfffffffeu)return false;
  for(unsigned i=0;i<32;++i) {
    const int a=keyHexDigit(argument[9+2*i]),b=keyHexDigit(argument[10+2*i]);
    if(a<0 || b<0)return false;
    out.bytes[i]=uint8_t((a<<4)|b);
  }
  return true;
}
}
