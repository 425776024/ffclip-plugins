#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace videocut::text {

/// Runtime-only clock events for stateful Effect Program nodes. These events
/// are execution input, not authored template/project data.
enum class EffectRuntimeClockEvent : std::uint8_t {
  /// Start a caller-owned non-zero lifecycle epoch at elapsedUs == 0. A newer
  /// epoch supersedes prior state; duplicate and stale epochs fail closed.
  Begin = 1,
  /// Add the explicit positive advanceUs to the active epoch. Timeline motion
  /// is not used to derive elapsed time and may move forwards or backwards.
  Advance,
  /// Move only the timeline anchor while preserving instance elapsed time.
  SeekHold,
  /// Clear instance elapsed time within the active epoch at a new anchor.
  Reset,
  /// Install the exact validated checkpoint carried by the event.
  RestoreCheckpoint,
};

/// Stable identity for one runtime node instance. All three components are
/// required so cloned render groups never share elapsed state accidentally.
struct EffectRuntimeClockIdentity final {
  std::uint64_t graphInstanceId{0U};
  std::uint64_t effectNodeInstanceId{0U};
  std::uint64_t renderGroupInstanceId{0U};
};

[[nodiscard]] bool operator==(const EffectRuntimeClockIdentity &left,
                              const EffectRuntimeClockIdentity &right) noexcept;
[[nodiscard]] bool operator!=(const EffectRuntimeClockIdentity &left,
                              const EffectRuntimeClockIdentity &right) noexcept;
[[nodiscard]] bool operator<(const EffectRuntimeClockIdentity &left,
                             const EffectRuntimeClockIdentity &right) noexcept;

/// Full collision-free cache identity contributed by the runtime clock. The
/// ordinary authored/timeline render key remains separate and can combine
/// this value only for nodes that consume an instance-elapsed clock.
struct EffectRuntimeClockCacheKey final {
  EffectRuntimeClockIdentity identity;
  std::uint64_t lifecycleEpoch{0U};
  std::int64_t timelineTimeUs{0};
  std::int64_t elapsedUs{0};
  std::uint64_t stateRevision{0U};
};

[[nodiscard]] bool operator==(const EffectRuntimeClockCacheKey &left,
                              const EffectRuntimeClockCacheKey &right) noexcept;
[[nodiscard]] bool operator!=(const EffectRuntimeClockCacheKey &left,
                              const EffectRuntimeClockCacheKey &right) noexcept;

/// Stable non-cryptographic digest for hash-based caches. Callers that require
/// collision-free equality must retain and compare EffectRuntimeClockCacheKey.
[[nodiscard]] std::uint64_t BuildEffectRuntimeClockCacheRevision(
    const EffectRuntimeClockCacheKey &key) noexcept;

/// An exact transient execution checkpoint. No JSON/project codec is exposed:
/// authored documents persist Effect Program parameters, never this state.
struct EffectRuntimeClockCheckpoint final {
  EffectRuntimeClockIdentity identity;
  std::uint64_t lifecycleEpoch{0U};
  std::int64_t timelineTimeUs{0};
  std::int64_t elapsedUs{0};
  std::uint64_t stateRevision{0U};
  std::uint64_t cacheRevision{0U};
};

/// One caller-ordered playback/export event. advanceUs is explicit and is
/// never inferred from timelineTimeUs; therefore reverse playback and random
/// seeks cannot accidentally alter instance elapsed time.
struct EffectRuntimeClockFrame final {
  EffectRuntimeClockEvent event{EffectRuntimeClockEvent::Begin};
  std::uint64_t lifecycleEpoch{0U};
  std::int64_t timelineTimeUs{0};
  std::int64_t advanceUs{0};
  std::optional<EffectRuntimeClockCheckpoint> checkpoint;
};

struct EffectRuntimeClockSample final {
  EffectRuntimeClockIdentity identity;
  std::uint64_t lifecycleEpoch{0U};
  std::int64_t timelineTimeUs{0};
  std::int64_t elapsedUs{0};
  /// Starts at one on Begin, advances for every accepted non-restore event,
  /// and is restored exactly from a checkpoint.
  std::uint64_t stateRevision{0U};
  EffectRuntimeClockCacheKey cacheKey;
  std::uint64_t cacheRevision{0U};
};

[[nodiscard]] EffectRuntimeClockCheckpoint MakeEffectRuntimeClockCheckpoint(
    const EffectRuntimeClockSample &sample) noexcept;

enum class EffectRuntimeClockStatus : std::uint8_t {
  Applied = 0,
  InvalidEvent,
  InvalidIdentity,
  InvalidLifecycleEpoch,
  InvalidTimelineTime,
  InvalidAdvance,
  UnexpectedCheckpoint,
  MissingCheckpoint,
  MissingState,
  DuplicateBegin,
  StaleLifecycleEpoch,
  LifecycleEpochMismatch,
  CheckpointIdentityMismatch,
  CheckpointEpochMismatch,
  CheckpointTimelineMismatch,
  InvalidCheckpoint,
  ArithmeticOverflow,
  RevisionOverflow,
};

[[nodiscard]] const char *
EffectRuntimeClockStatusName(EffectRuntimeClockStatus status) noexcept;

struct EffectRuntimeClockResult final {
  EffectRuntimeClockStatus status{EffectRuntimeClockStatus::InvalidEvent};
  EffectRuntimeClockSample sample;
  std::string error;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == EffectRuntimeClockStatus::Applied && error.empty();
  }
};

/// Transient, thread-safe state owner. Determinism is defined for the explicit
/// event order supplied per identity; no wall clock or template/package ID is
/// read. Failed transitions leave the prior state unchanged.
class EffectRuntimeClockStore final {
public:
  EffectRuntimeClockStore() = default;
  EffectRuntimeClockStore(const EffectRuntimeClockStore &) = delete;
  EffectRuntimeClockStore &operator=(const EffectRuntimeClockStore &) = delete;
  EffectRuntimeClockStore(EffectRuntimeClockStore &&) = delete;
  EffectRuntimeClockStore &operator=(EffectRuntimeClockStore &&) = delete;

  [[nodiscard]] EffectRuntimeClockResult
  Apply(const EffectRuntimeClockIdentity &identity,
        const EffectRuntimeClockFrame &frame);

  [[nodiscard]] std::optional<EffectRuntimeClockSample>
  Find(const EffectRuntimeClockIdentity &identity,
       std::uint64_t lifecycleEpoch) const;

  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::size_t EraseGraph(std::uint64_t graphInstanceId);
  void Clear();

private:
  struct State final {
    std::uint64_t lifecycleEpoch{0U};
    std::int64_t timelineTimeUs{0};
    std::int64_t elapsedUs{0};
    std::uint64_t stateRevision{0U};
  };

  [[nodiscard]] static EffectRuntimeClockSample
  MakeSample(const EffectRuntimeClockIdentity &identity,
             const State &state) noexcept;

  mutable std::mutex mutex_;
  std::map<EffectRuntimeClockIdentity, State> states_;
};

} // namespace videocut::text
