#pragma once

#include <string>
#include <utility>
#include <variant>

namespace videocut::frame {

enum class FrameErrorCode {
  None = 0,
  InvalidArgument,
  InvalidDesc,
  InvalidPlaneIndex,
  Overflow,
  InsufficientCapacity,
  StorageMismatch,
  NotCpuMappable,
  DeviceGenerationMismatch,
  SurfaceRoleMismatch,
  UnsupportedFormat,
  ResourceClosed,
  OutOfBudget,
  AllocationFailed,
  UnknownHandle,
  AlreadyReleased,
  RetainOverflow,
  PinOverflow,
  NotPinned,
};

class FrameError final {
public:
  FrameError() = default;
  FrameError(FrameErrorCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  [[nodiscard]] FrameErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string &message() const noexcept { return message_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return code_ != FrameErrorCode::None;
  }

private:
  FrameErrorCode code_{FrameErrorCode::None};
  std::string message_;
};

template <typename T> class Result final {
public:
  [[nodiscard]] static Result Success(T value) {
    return Result(std::in_place_index<0>, std::move(value));
  }

  [[nodiscard]] static Result Failure(FrameError error) {
    return Result(std::in_place_index<1>, std::move(error));
  }

  [[nodiscard]] bool hasValue() const noexcept { return value_.index() == 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }

  [[nodiscard]] T &value() & { return std::get<0>(value_); }
  [[nodiscard]] const T &value() const & { return std::get<0>(value_); }
  [[nodiscard]] T &&value() && { return std::get<0>(std::move(value_)); }

  [[nodiscard]] FrameError &error() & { return std::get<1>(value_); }
  [[nodiscard]] const FrameError &error() const & {
    return std::get<1>(value_);
  }

private:
  template <typename... Args>
  explicit Result(std::in_place_index_t<0> index, Args &&...args)
      : value_(index, std::forward<Args>(args)...) {}

  template <typename... Args>
  explicit Result(std::in_place_index_t<1> index, Args &&...args)
      : value_(index, std::forward<Args>(args)...) {}

  std::variant<T, FrameError> value_;
};

template <> class Result<void> final {
public:
  [[nodiscard]] static Result Success() { return Result(FrameError{}); }

  [[nodiscard]] static Result Failure(FrameError error) {
    return Result(std::move(error));
  }

  [[nodiscard]] bool hasValue() const noexcept { return !error_; }
  [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }
  [[nodiscard]] const FrameError &error() const noexcept { return error_; }

private:
  explicit Result(FrameError error) : error_(std::move(error)) {}

  FrameError error_;
};

[[nodiscard]] inline FrameError MakeFrameError(FrameErrorCode code,
                                               std::string message) {
  return FrameError(code, std::move(message));
}

} // namespace videocut::frame
