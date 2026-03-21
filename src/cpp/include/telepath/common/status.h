#ifndef TELEPATH_COMMON_STATUS_H_
#define TELEPATH_COMMON_STATUS_H_

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
  Status(StatusCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  static Status Ok() { return Status(); }

  static Status InvalidArgument(std::string message) {
    return Status(StatusCode::kInvalidArgument, std::move(message));
  }

  static Status NotFound(std::string message) {
    return Status(StatusCode::kNotFound, std::move(message));
  }

  static Status IoError(std::string message) {
    return Status(StatusCode::kIoError, std::move(message));
  }

  static Status Unavailable(std::string message) {
    return Status(StatusCode::kUnavailable, std::move(message));
  }

  static Status ResourceExhausted(std::string message) {
    return Status(StatusCode::kResourceExhausted, std::move(message));
  }

  static Status Internal(std::string message) {
    return Status(StatusCode::kInternal, std::move(message));
  }

  bool ok() const { return code_ == StatusCode::kOk; }
  StatusCode code() const { return code_; }
  const std::string &message() const { return message_; }

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
  const Status &status() const { return status_; }

  const T &value() const { return value_; }
  T &value() { return value_; }

 private:
  Status status_;
  T value_{};
  bool has_value_{false};
};

}  // namespace telepath

#endif  // TELEPATH_COMMON_STATUS_H_
