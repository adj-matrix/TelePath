#include "frame_control.h"

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <climits>
#include <ctime>

namespace telepath::frame_control {

static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));

void FutexWait(std::atomic<std::uint32_t> &control, std::uint32_t expected) {
  syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(&control), FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0);
}

void FutexWaitFor(std::atomic<std::uint32_t> &control, std::uint32_t expected, std::chrono::nanoseconds timeout) {
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
  timespec relative{static_cast<time_t>(seconds.count()), static_cast<long>((timeout - seconds).count())};
  syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(&control), FUTEX_WAIT_PRIVATE, expected, &relative, nullptr, 0);
}

void FutexWakeAll(std::atomic<std::uint32_t> &control) {
  syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(&control), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
}

auto PackWritebackError(const Status &status) -> std::uint64_t {
  if (status.has_value()) return 0;
  const auto code = static_cast<std::uint64_t>(status.error().code());
  const auto system_error = static_cast<std::uint64_t>(static_cast<std::uint32_t>(status.error().system_error()));
  return code | (system_error << 32);
}

auto UnpackWritebackError(std::uint64_t packed) -> Status {
  if (packed == 0) return Ok();
  const auto code = static_cast<ErrorCode>(packed & 0xFFu);
  const auto system_error = static_cast<int>(static_cast<std::uint32_t>(packed >> 32));
  return std::unexpected<Error>(Error(code, "writeback failed", system_error));
}

}  // namespace telepath::frame_control
