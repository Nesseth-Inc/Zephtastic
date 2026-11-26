// SPDX-License-Identifier: GPL-3.0-only
#include "gnss/ZephyrGnss.h"
#include <cassert>
int main() {
#if defined(CONFIG_MT_GNSS_T096) || defined(CONFIG_MT_GNSS_XIAO_L76K)
  static_assert(mt::gnss::present(),"fitted capability cannot depend on worker startup");
#else
  assert(!mt::gnss::present());const auto state=mt::gnss::diagnostics();
  assert(!state.initialized&&!state.ready&&!state.fault&&!state.acquiring);
  assert(!state.sentences&&!state.callbacks&&!state.promotions&&!state.satellites);
#endif
}
