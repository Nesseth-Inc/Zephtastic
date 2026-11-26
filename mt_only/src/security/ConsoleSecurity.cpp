// SPDX-License-Identifier: GPL-3.0-only
#include "ConsoleSecurity.h"

#if defined(__ZEPHYR__)
#include <zephyr/sys/atomic.h>
#else
#include <atomic>
#endif

#if defined(CONFIG_PRINTK) && CONFIG_PRINTK
extern "C" {
#include <zephyr/sys/printk-hooks.h>
}
#endif

namespace mt_security {
namespace {
#if defined(__ZEPHYR__)
atomic_t consoleAllowed=0;
bool allowed(){return atomic_get(&consoleAllowed)!=0;}
void allow(bool value){(void)atomic_set(&consoleAllowed,value?1:0);}
#else
std::atomic<bool> consoleAllowed{false};
bool allowed(){return consoleAllowed.load(std::memory_order_acquire);}
void allow(bool value){consoleAllowed.store(value,std::memory_order_release);}
#endif

#if defined(CONFIG_PRINTK) && CONFIG_PRINTK
#if defined(__ZEPHYR__)
atomic_t hookInitialized=0;
atomic_ptr_t originalPrintkHook=nullptr;
bool claimHook(){return atomic_cas(&hookInitialized,0,1);}
void saveHook(printk_hook_fn_t hook){
    (void)atomic_ptr_set(&originalPrintkHook,reinterpret_cast<void *>(hook));
}
printk_hook_fn_t savedHook(){
    return reinterpret_cast<printk_hook_fn_t>(atomic_ptr_get(&originalPrintkHook));
}
#else
std::atomic<bool> hookInitialized{false};
std::atomic<printk_hook_fn_t> originalPrintkHook{nullptr};
bool claimHook(){
    bool expected=false;
    return hookInitialized.compare_exchange_strong(expected,true,
        std::memory_order_acq_rel,std::memory_order_acquire);
}
void saveHook(printk_hook_fn_t hook){originalPrintkHook.store(hook,std::memory_order_release);}
printk_hook_fn_t savedHook(){return originalPrintkHook.load(std::memory_order_acquire);}
#endif

int gatedPrintkHook(int character)
{
    if (!allowed())return character;
    const auto hook=savedHook();
    return hook?hook(character):character;
}
#endif
} // namespace

void initializeConsoleSecurity()
{
    // Always begin closed; settings ownership must explicitly open the gate.
    allow(false);
#if defined(CONFIG_PRINTK) && CONFIG_PRINTK
    if (claimHook()) {
        saveHook(__printk_get_hook());
        __printk_hook_install(gatedPrintkHook);
    }
#endif
}

void serialConsolePolicy(bool enabled, bool settingsReady)
{
    allow(enabled&&settingsReady);
}

bool serialConsoleAllowed()
{
    return allowed();
}

} // namespace mt_security
