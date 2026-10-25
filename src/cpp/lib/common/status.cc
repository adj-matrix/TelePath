#include "telepath/common/status.h"

#include <cstring>

namespace telepath {

auto Error::ToString() const -> std::string {
  std::string text(message_);
  if (system_error_ == 0) return text;
  text += ": ";
  text += std::strerror(system_error_);
  return text;
}

}  // namespace telepath
