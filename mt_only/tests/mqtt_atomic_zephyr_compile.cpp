#include "network/MqttAtomic.h"

enum class TinyState : unsigned char { Off, On };

int main() {
  mt::mqtt::Atomic<bool> flag{false};
  mt::mqtt::Atomic<unsigned> counter{0};
  mt::mqtt::Atomic<TinyState> state{TinyState::Off};
  flag.store(true, mt::mqtt::AtomicOrder::Release);
  counter.store(counter.load(mt::mqtt::AtomicOrder::Relaxed) + 1,
                mt::mqtt::AtomicOrder::Release);
  state.store(TinyState::On, mt::mqtt::AtomicOrder::Release);
  return flag.load(mt::mqtt::AtomicOrder::Acquire) && counter.load() == 1 &&
                 state.load() == TinyState::On ? 0 : 1;
}
