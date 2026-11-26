// Thin adapter for the delivered provisional public API. UNRUN until the final
// worker patch and parent corrections are frozen and independently reviewed.
#include "Service.h"
#include "observation_adapter.h"
namespace qualify {
static uint64_t uptime(void *context) {
 auto &clock=*static_cast<Clock *>(context);
 return clock.valid?clock.ms:UINT64_MAX;
}
void bindObservation(mt::Service &service,Clock &clock) {
 (void)service.airtimeMeter().bind(mt::UptimeClock{uptime,&clock});
 service.airtimeMeter().ensureAnchored();
}
bool observedPercent(mt::Service &service,float &out) {
 auto value=service.airUtilTx();
 if(!value)return false;
 out=*value;return true;
}
}
