#include "frame_control.h"

namespace telepath::frame_control {

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
