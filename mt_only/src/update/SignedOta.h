// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace mt::update::signedOta {
// Only one owner exists, on main, after companion and radio admission closes.
class Session {
    uint8_t expected_[32];
    size_t remaining_=0, size_=0;
    bool initialized_=false, complete_=false;
public:
    explicit Session(const uint8_t expected[32]);
    ~Session();
    Session(const Session&)=delete;
    Session& operator=(const Session&)=delete;
    const char *command(const char *line);
    const char *write(const uint8_t *data,size_t length);
    size_t imageSize() const {return size_;}
    bool downloading() const {return remaining_!=0;}
    bool complete() const {return complete_;}
};
bool available();
void activate(const uint8_t hash[32]);
void serve(bool ble,void (*progress)(),uint32_t bootCount);
void confirmHealthyBoot();
void initializeBootVersion();
}
