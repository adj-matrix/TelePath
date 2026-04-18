#ifndef TELEPATH_COMMON_STATUS_H_
#define TELEPATH_COMMON_STATUS_H_

#include <cassert>
#include <string>
#include <utility>

namespace telepath {

enum class StatusCode {
  kOk = 0,
  kInvalidArgument,
  kNotFound,
  kIoError,
  kUnavailable,
  kResourceExhausted,
  kInternal,
};

class Status {
 public:
  Status() = default;
  Status(
    StatusCode code,
    std::string message
  ) : code_(code), message_(std::move(message)) {}

  static auto Ok() -> Status { return Status(); }

  static auto InvalidArgument(std::string message) -> Status {
    return Status(StatusCode::kInvalidArgument, std::move(message));
  }

  static auto NotFound(std::string message) -> Status {
    return Status(StatusCode::kNotFound, std::move(message));
  }

  static auto IoError(std::string message) -> Status {
    return Status(StatusCode::kIoError, std::move(message));
  }

  static auto Unavailable(std::string message) -> Status {
    return Status(StatusCode::kUnavailable, std::move(message));
  }

  static auto ResourceExhausted(std::string message) -> Status {
    return Status(StatusCode::kResourceExhausted, std::move(message));
  }

  static auto Internal(std::string message) -> Status {
    return Status(StatusCode::kInternal, std::move(message));
  }

  bool ok() const { return code_ == StatusCode::kOk; }
  auto code() const -> StatusCode { return code_; }
  auto message() const -> const std::string & { return message_; }

 private:
  StatusCode code_{StatusCode::kOk};
  std::string message_;
};

template <typename T>
class Result {
 public:
  Result(const Status &status) : status_(status) {}
  Result(Status &&status) : status_(std::move(status)) {}
  Result(const T &value) : status_(Status::Ok()), value_(value), has_value_(true) {}
  Result(T &&value) : status_(Status::Ok()), value_(std::move(value)), has_value_(true) {}

  bool ok() const { return status_.ok(); }
  auto status() const -> const Status & { return status_; }

  auto value() const -> const T & {
    assert(has_value_);
    return value_;
  }
  auto value() -> T & {
    assert(has_value_);
    return value_;
  }

 private:
  Status status_;
  T value_{};
  bool has_value_{false};
};

}  // namespace telepath

#endif  // TELEPATH_COMMON_STATUS_H_
