// SPDX-License-Identifier: MIT
#pragma once

struct device {
  bool ready=true;
  mutable int configureError[64]{};
  mutable int getError[64]{};
  mutable int setError[64]{};
  mutable int values[64]{};
  mutable uint32_t lastConfigureFlags[64]{};
  mutable unsigned configureCalls=0;
  mutable unsigned getCalls=0;
  mutable unsigned setCalls=0;
  mutable unsigned operationsAfterUnready=0;
};

inline bool device_is_ready(const device *dev){return dev&&dev->ready;}
