#ifndef TELEPATH_LIB_SYNC_FUTEX_H_
#define TELEPATH_LIB_SYNC_FUTEX_H_

#include <atomic>
#include <chrono>
#include <cstdint>

namespace telepath::sync {

// Futex operations on a 32-bit word. They use process-shared futexes, so the
// word may live in memory mapped by several processes; for private memory they
// behave like private futexes at a slightly higher cost on the slow path.
void FutexWait(std::atomic<std::uint32_t> &word, std::uint32_t expected);
void FutexWaitFor(std::atomic<std::uint32_t> &word, std::uint32_t expected, std::chrono::nanoseconds timeout);
void FutexWakeOne(std::atomic<std::uint32_t> &word);
void FutexWakeAll(std::atomic<std::uint32_t> &word);

// A mutex in one 32-bit word (0 unlocked, 1 locked, 2 locked with waiters)
// that works across processes. It has no pointers or kernel handles, so it can
// be placed in shared memory and zero-initialized. Satisfies Lockable.
class FutexMutex {
 public:
  FutexMutex() = default;
  FutexMutex(const FutexMutex &) = delete;
  FutexMutex &operator=(const FutexMutex &) = delete;

  bool try_lock() {
    std::uint32_t unlocked = 0;
    return word_.compare_exchange_strong(unlocked, 1, std::memory_order_acquire, std::memory_order_relaxed);
  }

  void lock() {
    std::uint32_t state = 0;
    if (word_.compare_exchange_strong(state, 1, std::memory_order_acquire, std::memory_order_relaxed)) return;
    if (state != 2) state = word_.exchange(2, std::memory_order_acquire);
    while (state != 0) {
      FutexWait(word_, 2);
      state = word_.exchange(2, std::memory_order_acquire);
    }
  }

  void unlock() {
    if (word_.exchange(0, std::memory_order_release) == 2) FutexWakeOne(word_);
  }

 private:
  std::atomic<std::uint32_t> word_{0};
};

static_assert(sizeof(FutexMutex) == sizeof(std::uint32_t));

}  // namespace telepath::sync

#endif  // TELEPATH_LIB_SYNC_FUTEX_H_
