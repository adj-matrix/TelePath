#include "futex.h"

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <climits>
#include <ctime>

namespace telepath::sync {

static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));

void FutexWait(std::atomic<std::uint32_t> &word, std::uint32_t expected) {
  syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(&word), FUTEX_WAIT, expected, nullptr, nullptr, 0);
}

void FutexWaitFor(std::atomic<std::uint32_t> &word, std::uint32_t expected, std::chrono::nanoseconds timeout) {
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
  timespec relative{static_cast<time_t>(seconds.count()), static_cast<long>((timeout - seconds).count())};
  syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(&word), FUTEX_WAIT, expected, &relative, nullptr, 0);
}

void FutexWakeOne(std::atomic<std::uint32_t> &word) {
  syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(&word), FUTEX_WAKE, 1, nullptr, nullptr, 0);
}

void FutexWakeAll(std::atomic<std::uint32_t> &word) {
  syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(&word), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
}

}  // namespace telepath::sync
