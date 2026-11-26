// SPDX-License-Identifier: GPL-3.0-only
// Actual Zephyr NMEA parser/matcher with synthetic UART publication only.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern "C" {
#include <zephyr/drivers/gnss.h>
#include <gnss_nmea0183.h>
#include <gnss_nmea0183_match.h>
#include <zephyr/drivers/gnss/gnss_publish.h>
bool zt_gnss_nmea0183_match_has_bearing(const struct gnss_data *data);
}

#include "../src/gnss/GnssMeasuredProjection.h"

static struct gnss_data observed{};
static bool observedHasBearing=false;
static unsigned published=0;

extern "C" void gnss_publish_data(const struct device *,const struct gnss_data *data) {
  struct gnss_data wrongPointer=*data;
  assert(!zt_gnss_nmea0183_match_has_bearing(&wrongPointer));
  observed=*data;
  observedHasBearing=zt_gnss_nmea0183_match_has_bearing(data);
  ++published;
}

static void sendSentence(struct gnss_nmea0183_match_data &state,const char *sentence) {
  char body[256];char *argv[32];char checksum[3];char prefix[16];
  assert(strlen(sentence)<sizeof(body));strcpy(body,sentence);
  unsigned sum=0;for(const unsigned char *p=(const unsigned char *)sentence;*p;++p)sum^=*p;
  snprintf(checksum,sizeof(checksum),"%02X",sum&255u);
  char *first=strchr(body,',');assert(first);*first='\0';
  snprintf(prefix,sizeof(prefix),"$%s,",body);argv[0]=prefix;
  unsigned argc=1;char *field=first+1;
  for(char *p=field;;++p)if(*p==','||!*p) {
    const bool last=*p=='\0';*p='\0';argv[argc++]=field;field=p+1;if(last)break;
  }
  argv[argc++]=checksum;
  assert(gnss_nmea0183_validate_message(argv,(uint16_t)argc));
  if(!strcmp(body,"GPGGA"))gnss_nmea0183_match_gga_callback(nullptr,argv,(uint16_t)argc,&state);
  else {assert(!strcmp(body,"GPRMC"));gnss_nmea0183_match_rmc_callback(nullptr,argv,(uint16_t)argc,&state);}
}

static void initialize(struct gnss_nmea0183_match_data &state) {
  const struct gnss_nmea0183_match_config config{};
  assert(gnss_nmea0183_match_init(&state,&config)==0);
  published=0;observed={};observedHasBearing=false;
}

static void projectObserved(uint32_t flags,uint32_t expectedTrack,bool expectedPresent) {
  zt::gnss::PromotionEvent fix{};
  fix.measured=true;fix.hasGroundTrack=observedHasBearing;
  fix.groundTrackE5=observed.nav_data.bearing*100u;
  fix.hdopMilli=(uint16_t)observed.info.hdop;
  meshtastic_Position out{};
  zt::gnss::projectMeasurements(fix,flags,out);
  assert(out.has_ground_track==expectedPresent);
  assert(out.ground_track==(expectedPresent?expectedTrack:0u));
}

int main() {
  struct gnss_nmea0183_match_data state{};
  const char *gga="GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,";

  // An empty optional RMC course stays absent even though the SDK numeric
  // helper would otherwise collapse it to zero.
  initialize(state);sendSentence(state,gga);assert(published==0);
  sendSentence(state,"GPRMC,123519,A,4807.038,N,01131.000,E,22.4,,230324,,");
  assert(published==1&&!observedHasBearing&&observed.nav_data.bearing==0);
  assert(!zt_gnss_nmea0183_match_has_bearing(&observed));
  projectObserved(256u,0,false);
  puts("PASS empty course remains absent in the exact matched RMC/GGA callback");

  // A real zero-degree course is present; zero is not the absence sentinel.
  initialize(state);sendSentence(state,gga);
  sendSentence(state,"GPRMC,123519,A,4807.038,N,01131.000,E,22.4,0.0,230324,,");
  assert(published==1&&observedHasBearing&&observed.nav_data.bearing==0);
  assert(!zt_gnss_nmea0183_match_has_bearing(&observed));
  projectObserved(256u,0,true);
  puts("PASS measured north course preserves zero value and presence");

  // Source millidegrees map to Position ground_track degrees * 1e5.
  initialize(state);sendSentence(state,gga);
  sendSentence(state,"GPRMC,123519,A,4807.038,N,01131.000,E,22.4,84.4,230324,,");
  assert(published==1&&observedHasBearing&&observed.nav_data.bearing==84400u);
  projectObserved(777u,8440000u,true);
  meshtastic_Position reused{};reused.has_ground_track=true;reused.ground_track=17;
  zt::gnss::PromotionEvent fix{};fix.measured=true;fix.hasGroundTrack=true;fix.groundTrackE5=8440000;
  fix.hdopMilli=900;
  zt::gnss::projectMeasurements(fix,777u,reused);
  assert(reused.PDOP==0&&reused.HDOP==0&&reused.VDOP==0);
  zt::gnss::projectMeasurements(fix,0u,reused);
  assert(!reused.has_ground_track&&reused.ground_track==0);
  puts("PASS flag 777 scales heading correctly, leaves unavailable DOP zero, and clears reused heading");

  // Out-of-range/malformed headings invalidate the RMC half. A later GGA
  // cannot promote stale bearing; a fresh valid empty-course pair is absent.
  initialize(state);sendSentence(state,gga);
  sendSentence(state,"GPRMC,123519,A,4807.038,N,01131.000,E,22.4,360.0,230324,,");
  assert(published==0);sendSentence(state,gga);assert(published==0);
  sendSentence(state,"GPRMC,123519,A,4807.038,N,01131.000,E,22.4,84x4,230324,,");
  assert(published==0);
  sendSentence(state,"GPRMC,123519,A,4807.038,N,01131.000,E,22.4,,230324,,");
  assert(published==1&&!observedHasBearing);
  puts("PASS invalid course cannot reuse stale heading or publish malformed range");

  // A mismatched RMC/GGA pair does not publish, and the subsequently matched
  // empty course does not inherit the prior measured course.
  initialize(state);
  sendSentence(state,"GPRMC,123519,A,4807.038,N,01131.000,E,22.4,84.4,230324,,");
  sendSentence(state,"GPGGA,123520,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
  assert(published==0);
  sendSentence(state,"GPRMC,123520,A,4807.038,N,01131.000,E,22.4,,230324,,");
  assert(published==1&&!observedHasBearing&&observed.nav_data.bearing==0);
  puts("PASS course presence is bound to the exact timestamp-matched RMC, not previous fixes");

  // Direct event validation rejects an impossible unit/range value without
  // affecting the rest of a previously valid position projection.
  fix={};fix.measured=true;fix.hasGroundTrack=true;fix.groundTrackE5=35999900u;
  meshtastic_Position out{};zt::gnss::projectMeasurements(fix,256u,out);
  assert(out.has_ground_track&&out.ground_track==35999900u);
  fix.groundTrackE5=36000000u;zt::gnss::projectMeasurements(fix,256u,out);
  assert(!out.has_ground_track&&out.ground_track==0);
  printf("PASS native parser projection and range checks; PromotionEvent bytes=%zu\n",sizeof(fix));
  return 0;
}
