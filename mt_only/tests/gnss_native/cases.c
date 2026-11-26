/* SPDX-License-Identifier: GPL-3.0-only */
/* Synthetic NMEA only. Real parser/matcher bodies with a host publication seam. */
#include "gnss_nmea0183.h"
#include "gnss_nmea0183_match.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct gnss_data observed;
static unsigned published;
void gnss_publish_data(const struct device *dev,const struct gnss_data *data) {
  (void)dev;observed=*data;++published;
}
static void send_sentence(struct gnss_nmea0183_match_data *data,const char *sentence,int bad_checksum) {
  char body[256];char *argv[32];char checksum[3];char prefix[16];
  assert(strlen(sentence)<sizeof(body));strcpy(body,sentence);
  const unsigned char *s=(const unsigned char *)sentence;unsigned sum=0;
  while(*s)sum^=*s++;
  snprintf(checksum,sizeof(checksum),"%02X",(sum^(bad_checksum?1u:0u))&255u);
  char *first=strchr(body,',');assert(first);*first=0;
  snprintf(prefix,sizeof(prefix),"$%s,",body);argv[0]=prefix;
  unsigned argc=1;char *field=first+1;
  for(char *p=field;;++p)if(*p==','||!*p) {
    const int last=!*p;*p=0;argv[argc++]=field;field=p+1;if(last)break;
  }
  argv[argc++]=checksum;
  assert(gnss_nmea0183_validate_message(argv,argc)==!bad_checksum);
  if(!strcmp(body,"GPGGA"))gnss_nmea0183_match_gga_callback(NULL,argv,argc,data);
  else gnss_nmea0183_match_rmc_callback(NULL,argv,argc,data);
}
static void initialize(struct gnss_nmea0183_match_data *data) {
  const struct gnss_nmea0183_match_config config={0};
  assert(gnss_nmea0183_match_init(data,&config)==0);published=0;memset(&observed,0,sizeof(observed));
}
#define GGA "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"
#define RMC "GPRMC,123519,A,4807.038,N,01131.000,E,22.4,84.4,230324,,"
int main(void) {
  struct gnss_nmea0183_match_data state;
  assert(gnss_nmea0183_checksum(GGA)==0x47); /* independent public NMEA example */
  initialize(&state);send_sentence(&state,GGA,0);assert(!published);
  send_sentence(&state,RMC,0);assert(published==1&&observed.info.fix_status==GNSS_FIX_STATUS_GNSS_FIX);
  assert(observed.nav_data.latitude>=48117299999LL&&observed.nav_data.latitude<=48117300000LL&&observed.nav_data.altitude==545400);
  assert(observed.info.geoid_separation==46900&&observed.utc.century_year==24&&observed.nav_data.speed>0);
  puts("PASS native matched GGA/RMC navigation and measured extras");
  initialize(&state);send_sentence(&state,RMC,0);
  send_sentence(&state,"GPGGA,123519,4807.038,N,01131.000,E,1,08,65.535,545.4,M,46.9,M,,",0);
  assert(published==1&&observed.info.hdop==UINT16_MAX);
  send_sentence(&state,"GPGGA,123519,4807.038,N,01131.000,E,1,08,65.536,545.4,M,46.9,M,,",0);
  assert(published==1);send_sentence(&state,RMC,0);assert(published==1);
  send_sentence(&state,GGA,0);assert(published==2&&observed.info.hdop==900);
  puts("PASS native HDOP width boundary rejects truncation and cannot reuse the invalid GGA half");
  initialize(&state);send_sentence(&state,GGA,0);send_sentence(&state,RMC,0);
  /* The old parser retained this valid navigation when RMC became void. */
  send_sentence(&state,"GPRMC,123519,V,,,,,,,230324,,",0);
  assert(published==2&&observed.nav_data.latitude==0&&observed.nav_data.longitude==0&&observed.nav_data.speed==0&&observed.utc.month==0);
  send_sentence(&state,GGA,0);assert(observed.nav_data.latitude==0);
  puts("PASS void RMC revokes old navigation even when GGA still reports a fix");
  initialize(&state);send_sentence(&state,GGA,0);send_sentence(&state,RMC,0);
  const struct gnss_data before=state.data;const unsigned count=published;
  send_sentence(&state,"GPRMC,123519,A,4900.000,N,01200.000,E,22.4,999.0,230324,,",0);
  assert(!memcmp(&state.data,&before,sizeof(before))&&published==count);
  send_sentence(&state,GGA,0);assert(published==count);
  send_sentence(&state,RMC,0);assert(published==count+1);
  puts("PASS failed RMC is atomic and cannot reuse its earlier matched UTC");
  send_sentence(&state,GGA,1);assert(published==count+1);
  send_sentence(&state,RMC,0);assert(published==count+1);
  send_sentence(&state,GGA,0);assert(published==count+2);
  puts("PASS bad checksum invalidates one half until a fresh valid pair");
  initialize(&state);
  send_sentence(&state,"GPGGA,000000,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,",0);
  assert(!published);send_sentence(&state,"GPRMC,000000,A,4807.038,N,01131.000,E,0.0,0.0,010100,,",0);
  assert(published==1&&observed.utc.hour==0&&observed.utc.minute==0&&observed.utc.millisecond==0&&observed.utc.month==1&&observed.utc.month_day==1);
  puts("PASS midnight UTC zero is distinct from a missing NMEA half");
  initialize(&state);send_sentence(&state,GGA,0);send_sentence(&state,"GPRMC,123519,A,4807.038,N,01131.000,E,-1.0,0.0,230324,,",0);assert(!published);
  send_sentence(&state,"GPRMC,123519,VA,,,,,,,230324,,",0);assert(!published);
  puts("PASS negative speed and noncanonical status cannot publish a fix");
  initialize(&state);send_sentence(&state,RMC,0);
  send_sentence(&state,"GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,F,46.9,M,,",0);assert(!published);
  send_sentence(&state,GGA,0);assert(published==1);
  send_sentence(&state,"GPRMC,123519,A,999999999999999999999999999999999999.0,N,01131.000,E,0.0,0.0,230324,,",0);assert(published==1);
  send_sentence(&state,"GPRMC,123519,A,123456789.0,N,01131.000,E,0.0,0.0,230324,,",0);assert(published==1);
  send_sentence(&state,"GPRMC,123519,A,4807.038,N,01131.000,E,9999999999.0,0.0,230324,,",0);assert(published==1);
  puts("PASS metre-unit checks and bounded coordinate/numeric fields reject malformed NMEA before conversion");
  const char *missing[]={"","-",".","-."};char sentence[160];
  for(unsigned i=0;i<sizeof(missing)/sizeof(missing[0]);++i) {
    initialize(&state);send_sentence(&state,RMC,0);
    snprintf(sentence,sizeof(sentence),"GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,%s,M,46.9,M,,",missing[i]);
    send_sentence(&state,sentence,0);assert(!published);
    snprintf(sentence,sizeof(sentence),"GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,%s,M,,",missing[i]);
    send_sentence(&state,sentence,0);assert(!published);
    initialize(&state);send_sentence(&state,GGA,0);
    snprintf(sentence,sizeof(sentence),"GPRMC,123519,A,4807.038,N,01131.000,E,%s,0.0,230324,,",missing[i]);
    send_sentence(&state,sentence,0);assert(!published);
  }
  initialize(&state);send_sentence(&state,"GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,0.0,M,0,M,,",0);
  send_sentence(&state,"GPRMC,123519,A,4807.038,N,01131.000,E,0.0,0.0,230324,,",0);
  assert(published==1&&observed.nav_data.altitude==0&&observed.info.geoid_separation==0&&observed.nav_data.speed==0);
  puts("PASS missing/sign-only/dot-only measurements are absent; explicit measured zero stays valid");
}
