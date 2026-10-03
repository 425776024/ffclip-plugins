#pragma once

#include "videocut/gpu_execution/AppleNativeCompletionBudget.h"
#include "videocut/gpu_execution/internal/AppleMetalExternalCompletion.h"

#include <condition_variable>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>

namespace videocut::skia_runtime::internal {

// One lane-owned receipt timeline for Ganesh and native passes on its queue.
// The shared event advances only over a contiguous completed prefix, including
// failed commands; consumers inherit the failure instead of waiting forever.
class SkiaMetalCompletionTimeline final {
public:
  explicit SkiaMetalCompletionTimeline(id<MTLDevice> device)
      : event_([device newSharedEvent]) {}
  ~SkiaMetalCompletionTimeline() { [event_ release]; }

  std::uint64_t Append() {
    std::lock_guard lock(mutex_);
    const auto serial = ++submitted_;
    pending_.emplace(serial, false);
    return serial;
  }
  void Complete(std::uint64_t serial, bool success,
                const std::string &error) noexcept {
    std::lock_guard lock(mutex_);
    auto found = pending_.find(serial);
    if (found == pending_.end())
      return;
    if (!success && (failed_ == 0 || serial < failed_)) {
      failed_ = serial;
      error_ = error.empty() ? "Skia Metal command failed" : error;
    }
    found->second = true;
    while (!pending_.empty() && pending_.begin()->second) {
      completed_ = pending_.begin()->first;
      pending_.erase(pending_.begin());
    }
    // Signal terminal failure as well as success. Logical dependency checking
    // belongs to the common GPU submission consumer.
    event_.signaledValue = completed_;
    ready_.notify_all();
  }
  gpu_execution::CompletionTerminalStatus Status(
      std::uint64_t serial, std::string &error) const noexcept {
    std::lock_guard lock(mutex_);
    return StatusLocked(serial, error);
  }
  bool Wait(std::uint64_t serial, std::string &error,
            std::chrono::milliseconds timeout = std::chrono::seconds(30)) const noexcept {
    try {
      std::unique_lock lock(mutex_);
      if (!ready_.wait_for(lock, timeout, [&] { return completed_ >= serial; })) {
        poisoned_ = true;
        error = "Skia Metal exceeded its completion deadline";
        return false;
      }
      return StatusLocked(serial, error) ==
          gpu_execution::CompletionTerminalStatus::Succeeded;
    } catch (...) {
      poisoned_ = true;
      error = "Skia Metal completion wait failed";
      return false;
    }
  }
  bool healthy() const noexcept { return !poisoned_; }
  id<MTLSharedEvent> event() const noexcept { return event_; }

private:
  gpu_execution::CompletionTerminalStatus StatusLocked(
      std::uint64_t serial, std::string &error) const noexcept {
    error.clear();
    if (completed_ < serial)
      return gpu_execution::CompletionTerminalStatus::Pending;
    if (failed_ != 0 && failed_ <= serial) {
      error = error_;
      return gpu_execution::CompletionTerminalStatus::Failed;
    }
    return gpu_execution::CompletionTerminalStatus::Succeeded;
  }
  mutable std::mutex mutex_;
  mutable std::condition_variable ready_;
  mutable std::atomic_bool poisoned_{false};
  std::map<std::uint64_t, bool> pending_;
  std::uint64_t submitted_{0}, completed_{0}, failed_{0};
  std::string error_;
  id<MTLSharedEvent> event_{nil};
};

class SkiaMetalFrameCompletion final
    : public gpu_execution::internal::AppleMetalExternalCompletion {
public:
  SkiaMetalFrameCompletion(
      std::shared_ptr<SkiaMetalCompletionTimeline> timeline,
      std::shared_ptr<gpu_execution::Device> device, std::uint64_t serial)
      : timeline_(std::move(timeline)), device_(std::move(device)), serial_(serial) {}
  bool completed() const noexcept override {
    std::string error;
    return terminalStatus(error) ==
        gpu_execution::CompletionTerminalStatus::Succeeded;
  }
  std::uint64_t deviceGeneration() const noexcept override {
    return device_->generation();
  }
  std::uint64_t serial() const noexcept override { return serial_; }
  gpu_execution::Backend backend() const noexcept override {
    return gpu_execution::Backend::Metal;
  }
  std::int32_t deviceIndex() const noexcept override { return device_->deviceIndex(); }
  std::uintptr_t nativeDeviceHandle() const noexcept override {
    return device_->nativeDeviceHandle();
  }
  std::uintptr_t nativeSharedEventHandle() const noexcept override {
    return reinterpret_cast<std::uintptr_t>((void *)timeline_->event());
  }
  std::uint64_t sharedEventValue() const noexcept override { return serial_; }
  gpu_execution::CompletionTerminalStatus terminalStatus(
      std::string &error) const noexcept override {
    return timeline_->Status(serial_, error);
  }
  bool Wait(std::string &error) const noexcept override {
    return timeline_->Wait(serial_, error);
  }
private:
  std::shared_ptr<SkiaMetalCompletionTimeline> timeline_;
  std::shared_ptr<gpu_execution::Device> device_;
  std::uint64_t serial_;
};

} // namespace videocut::skia_runtime::internal
