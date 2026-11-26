// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "NotificationController.h"
namespace mt::notification {
void init();
bool available();
uint32_t pin();
// GPIO only from main outside Service serialization; radio hold uses ZephCore's
// existing atomic gate and owns a shared LED ahead of the notification pulse.
void render(NotificationController::Output output,bool heartbeatEnabled=false,uint32_t now=0);
void radioActive(bool active);
void radioReceived(uint32_t now);
}
