#ifndef TELEPATH_COMMON_STATUS_H_
#define TELEPATH_COMMON_STATUS_H_

#include <cstdint>
#include <expected>
#include <string>

// Outcome types for the TelePath API. Status (success or failure) and
// Result<T> (value or failure) are the types every call returns; Error is the
// failure detail they carry. Success is represented by the outcome itself, so
// ErrorCode only enumerates failures.

namespace telepath {

// Machine-readable error kind carried by every TelePath error.
enum class ErrorCode : std::uint8_t {
  kInvalidArgument = 1,
  kNotFound,
  kIoError,
  kUnavailable,
  kResourceExhausted,
  kInternal,
  kFailed,
};

// How a caller should react to an error, derived from its ErrorCode.
enum class ErrorClass : std::uint8_t {
  // Affects only the failed call, for example one page's I/O.
  kPage,
  // Transient shortage; the same call may succeed when retried.
  kResource,
  // Invalid call, missing object, or a component that is unavailable or
  // shutting down.
  kUsage,
  // The instance is unusable; every later call fails fast.
  kFatal,
};

// Error value used by Result and Status. It never allocates: the message is a
// static string, and an optional OS error number stands in for formatted
// details. Use ToString() to render a human-readable description.
class Error {
 public:
  constexpr Error(ErrorCode code, const char *message, int system_error = 0) noexcept
    : code_(code), system_error_(system_error), message_(message != nullptr ? message : "") {}

  constexpr auto code() const noexcept -> ErrorCode { return code_; }
  // Static description; never null.
  constexpr auto message() const noexcept -> const char * { return message_; }
  // errno-style OS error number, or 0 when the error has no OS cause.
  constexpr auto system_error() const noexcept -> int { return system_error_; }

  constexpr auto error_class() const noexcept -> ErrorClass {
    switch (code_) {
    case ErrorCode::kIoError:
      return ErrorClass::kPage;
    case ErrorCode::kResourceExhausted:
      return ErrorClass::kResource;
    case ErrorCode::kInvalidArgument:
    case ErrorCode::kNotFound:
    case ErrorCode::kUnavailable:
      return ErrorClass::kUsage;
    case ErrorCode::kInternal:
    case ErrorCode::kFailed:
      return ErrorClass::kFatal;
    }
    return ErrorClass::kFatal;
  }

  constexpr bool retryable() const noexcept { return error_class() == ErrorClass::kResource; }

  // Renders "message" or "message: <strerror(system_error)>". Allocates.
  auto ToString() const -> std::string;

 private:
  ErrorCode code_;
  int system_error_;
  const char *message_;
};

// Value-or-error return type used across the TelePath API.
template <typename T>
using Result = std::expected<T, Error>;

// Success-or-error return type for operations without a value.
using Status = std::expected<void, Error>;

// Successful Status.
constexpr auto Ok() noexcept -> Status { return {}; }

// Error builders. Each returns std::unexpected<Error>, which converts to any
// Result<T> or Status.
constexpr auto InvalidArgument(const char *message) noexcept -> std::unexpected<Error> {
  return std::unexpected<Error>(Error(ErrorCode::kInvalidArgument, message));
}

constexpr auto NotFound(const char *message, int system_error = 0) noexcept -> std::unexpected<Error> {
  return std::unexpected<Error>(Error(ErrorCode::kNotFound, message, system_error));
}

constexpr auto IoError(const char *message, int system_error = 0) noexcept -> std::unexpected<Error> {
  return std::unexpected<Error>(Error(ErrorCode::kIoError, message, system_error));
}

constexpr auto Unavailable(const char *message, int system_error = 0) noexcept -> std::unexpected<Error> {
  return std::unexpected<Error>(Error(ErrorCode::kUnavailable, message, system_error));
}

constexpr auto ResourceExhausted(const char *message) noexcept -> std::unexpected<Error> {
  return std::unexpected<Error>(Error(ErrorCode::kResourceExhausted, message));
}

constexpr auto Internal(const char *message) noexcept -> std::unexpected<Error> {
  return std::unexpected<Error>(Error(ErrorCode::kInternal, message));
}

constexpr auto Failed(const char *message) noexcept -> std::unexpected<Error> {
  return std::unexpected<Error>(Error(ErrorCode::kFailed, message));
}

// Propagates the error of `result` into a differently typed Result or Status.
template <typename T>
constexpr auto PropagateError(const Result<T> &result) noexcept -> std::unexpected<Error> {
  return std::unexpected<Error>(result.error());
}

}  // namespace telepath

#endif  // TELEPATH_COMMON_STATUS_H_
