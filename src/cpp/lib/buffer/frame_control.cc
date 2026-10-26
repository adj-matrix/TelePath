#include "frame_control.h"

#include <algorithm>

namespace telepath::frame_control {

auto PackWritebackError(const Status &status) -> std::uint32_t {
  if (status.has_value()) return 0;
  const auto code = static_cast<std::uint32_t>(status.error().code()) & 0xFFu;
  const auto system_error = std::min<std::uint32_t>(static_cast<std::uint32_t>(std::max(status.error().system_error(), 0)), 0xFFFFFFu);
  return code | (system_error << 8);
}

auto UnpackWritebackError(std::uint32_t packed) -> Status {
  if (packed == 0) return Ok();
  const auto code = static_cast<ErrorCode>(packed & 0xFFu);
  const auto system_error = static_cast<int>(packed >> 8);
  return std::unexpected<Error>(Error(code, "writeback failed", system_error));
}

}  // namespace telepath::frame_control
