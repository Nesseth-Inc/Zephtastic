// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

namespace mt::ble::store {
// RAM-only except invoke(), which executes the caller's original operation.
// No key/value or peer address is retained by this coordinator.
void initialize();
bool beginBoot(bool nativeQuiescent);
bool beginDeviceResetErase(bool nativeQuiescent);
// Called only after all bt backend/h_set reads validate, before native commit.
// Does not mark the store loaded or open payload/advertising gates.
bool prepareBootCommit();
void bootLoadComplete(int result);
void endBoot(int result);
void sealBootIfQuiet(bool nativeIdentityWorkPending);
bool payloadAllowed();
bool healthy();
bool failed();
bool waiting();
void fail(int result);
void sessionEntered();
void sessionLeft();
using Operation=int(*)(void *);
int invoke(bool deletion,Operation operation,void *context);
uint32_t pendingToken();
bool prepare(uint32_t token);
bool grant(uint32_t token);
// Retire only a pre-grant token. A Running operation retains its caller/lease.
bool cancel(uint32_t token,int result);
bool active();
bool completed(uint32_t &token,int &result);
void settle(uint32_t token,int restoreResult);
void reject(uint32_t token,int result);
void wake();
}
