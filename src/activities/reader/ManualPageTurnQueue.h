#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

class ManualPageTurnQueue {
 public:
  bool hasPending() const { return steps.load(std::memory_order_acquire) != 0; }

  bool enqueue(const int delta) {
    if (delta == 0) return false;
    if (dispatchedDirection != 0 && (dispatchedDirection > 0) != (delta > 0)) {
      store(delta);
      finishDispatched();
      return true;
    }
    const int pending = steps.load(std::memory_order_relaxed);
    if (pending != 0 && (pending > 0) != (delta > 0)) {
      clear();
      return true;
    }
    store(pending + delta);
    return false;
  }

  int take() { return steps.exchange(0, std::memory_order_acq_rel); }
  void markDispatched(const int delta) { dispatchedDirection = delta > 0 ? 1 : -1; }
  void finishDispatched() { dispatchedDirection = 0; }
  void clear() {
    store(0);
    finishDispatched();
  }

 private:
  static constexpr int MAX_PENDING_TURNS = 5;
  std::atomic<int8_t> steps{0};
  int8_t dispatchedDirection = 0;

  void store(const int delta) {
    steps.store(static_cast<int8_t>(std::clamp(delta, -MAX_PENDING_TURNS, MAX_PENDING_TURNS)),
                std::memory_order_release);
  }
};
