#pragma once
#include "Service.h"
namespace qualify {
struct Clock {uint64_t ms=0;bool valid=true;};
// Parent fills these after the delivered public API is frozen and reviewed.
void bindObservation(mt::Service &,Clock &);
bool observedPercent(mt::Service &,float &);
}
