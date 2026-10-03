#include "videocut/text/EffectRuntimeClock.h"

#include <limits>
#include <tuple>
#include <utility>

namespace videocut::text {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void AppendCacheWord(std::uint64_t &hash, const std::uint64_t value) noexcept {
  for (unsigned shift = 0U; shift < 64U; shift += 8U) {
    hash ^= (value >> shift) & 0xffU;
    hash *= kFnvPrime;
  }
}

bool ValidIdentity(const EffectRuntimeClockIdentity &identity) noexcept {
  return identity.graphInstanceId != 0U &&
         identity.effectNodeInstanceId != 0U &&
         identity.renderGroupInstanceId != 0U;
}

EffectRuntimeClockResult Failure(const EffectRuntimeClockStatus status,
                                 std::string error) {
  EffectRuntimeClockResult result;
  result.status = status;
  result.error = std::move(error);
  return result;
}

EffectRuntimeClockStatus CompareEpochs(const std::uint64_t current,
                                       const std::uint64_t requested) noexcept {
  if (requested < current)
    return EffectRuntimeClockStatus::StaleLifecycleEpoch;
  if (requested > current)
    return EffectRuntimeClockStatus::LifecycleEpochMismatch;
  return EffectRuntimeClockStatus::Applied;
}

} // namespace

bool operator==(const EffectRuntimeClockIdentity &left,
                const EffectRuntimeClockIdentity &right) noexcept {
  return left.graphInstanceId == right.graphInstanceId &&
         left.effectNodeInstanceId == right.effectNodeInstanceId &&
         left.renderGroupInstanceId == right.renderGroupInstanceId;
}

bool operator!=(const EffectRuntimeClockIdentity &left,
                const EffectRuntimeClockIdentity &right) noexcept {
  return !(left == right);
}

bool operator<(const EffectRuntimeClockIdentity &left,
               const EffectRuntimeClockIdentity &right) noexcept {
  return std::tie(left.graphInstanceId, left.effectNodeInstanceId,
                  left.renderGroupInstanceId) <
         std::tie(right.graphInstanceId, right.effectNodeInstanceId,
                  right.renderGroupInstanceId);
}

bool operator==(const EffectRuntimeClockCacheKey &left,
                const EffectRuntimeClockCacheKey &right) noexcept {
  return left.identity == right.identity &&
         left.lifecycleEpoch == right.lifecycleEpoch &&
         left.timelineTimeUs == right.timelineTimeUs &&
         left.elapsedUs == right.elapsedUs &&
         left.stateRevision == right.stateRevision;
}

bool operator!=(const EffectRuntimeClockCacheKey &left,
                const EffectRuntimeClockCacheKey &right) noexcept {
  return !(left == right);
}

std::uint64_t BuildEffectRuntimeClockCacheRevision(
    const EffectRuntimeClockCacheKey &key) noexcept {
  std::uint64_t hash = kFnvOffset;
  // Domain separation keeps this digest independent from other FNV-backed
  // render keys while retaining a platform-independent byte order.
  AppendCacheWord(hash, 0x6c63746365666665ULL); // "effectcl"
  AppendCacheWord(hash, key.identity.graphInstanceId);
  AppendCacheWord(hash, key.identity.effectNodeInstanceId);
  AppendCacheWord(hash, key.identity.renderGroupInstanceId);
  AppendCacheWord(hash, key.lifecycleEpoch);
  AppendCacheWord(hash, static_cast<std::uint64_t>(key.timelineTimeUs));
  AppendCacheWord(hash, static_cast<std::uint64_t>(key.elapsedUs));
  AppendCacheWord(hash, key.stateRevision);
  return hash == 0U ? kFnvOffset : hash;
}

EffectRuntimeClockCheckpoint MakeEffectRuntimeClockCheckpoint(
    const EffectRuntimeClockSample &sample) noexcept {
  return {sample.identity,  sample.lifecycleEpoch, sample.timelineTimeUs,
          sample.elapsedUs, sample.stateRevision,  sample.cacheRevision};
}

const char *
EffectRuntimeClockStatusName(const EffectRuntimeClockStatus status) noexcept {
  switch (status) {
  case EffectRuntimeClockStatus::Applied:
    return "applied";
  case EffectRuntimeClockStatus::InvalidEvent:
    return "invalid_event";
  case EffectRuntimeClockStatus::InvalidIdentity:
    return "invalid_identity";
  case EffectRuntimeClockStatus::InvalidLifecycleEpoch:
    return "invalid_lifecycle_epoch";
  case EffectRuntimeClockStatus::InvalidTimelineTime:
    return "invalid_timeline_time";
  case EffectRuntimeClockStatus::InvalidAdvance:
    return "invalid_advance";
  case EffectRuntimeClockStatus::UnexpectedCheckpoint:
    return "unexpected_checkpoint";
  case EffectRuntimeClockStatus::MissingCheckpoint:
    return "missing_checkpoint";
  case EffectRuntimeClockStatus::MissingState:
    return "missing_state";
  case EffectRuntimeClockStatus::DuplicateBegin:
    return "duplicate_begin";
  case EffectRuntimeClockStatus::StaleLifecycleEpoch:
    return "stale_lifecycle_epoch";
  case EffectRuntimeClockStatus::LifecycleEpochMismatch:
    return "lifecycle_epoch_mismatch";
  case EffectRuntimeClockStatus::CheckpointIdentityMismatch:
    return "checkpoint_identity_mismatch";
  case EffectRuntimeClockStatus::CheckpointEpochMismatch:
    return "checkpoint_epoch_mismatch";
  case EffectRuntimeClockStatus::CheckpointTimelineMismatch:
    return "checkpoint_timeline_mismatch";
  case EffectRuntimeClockStatus::InvalidCheckpoint:
    return "invalid_checkpoint";
  case EffectRuntimeClockStatus::ArithmeticOverflow:
    return "arithmetic_overflow";
  case EffectRuntimeClockStatus::RevisionOverflow:
    return "revision_overflow";
  }
  return "invalid_event";
}

EffectRuntimeClockSample
EffectRuntimeClockStore::MakeSample(const EffectRuntimeClockIdentity &identity,
                                    const State &state) noexcept {
  EffectRuntimeClockSample sample;
  sample.identity = identity;
  sample.lifecycleEpoch = state.lifecycleEpoch;
  sample.timelineTimeUs = state.timelineTimeUs;
  sample.elapsedUs = state.elapsedUs;
  sample.stateRevision = state.stateRevision;
  sample.cacheKey = {identity, state.lifecycleEpoch, state.timelineTimeUs,
                     state.elapsedUs, state.stateRevision};
  sample.cacheRevision = BuildEffectRuntimeClockCacheRevision(sample.cacheKey);
  return sample;
}

EffectRuntimeClockResult
EffectRuntimeClockStore::Apply(const EffectRuntimeClockIdentity &identity,
                               const EffectRuntimeClockFrame &frame) {
  if (!ValidIdentity(identity)) {
    return Failure(EffectRuntimeClockStatus::InvalidIdentity,
                   "effect runtime clock identity requires non-zero graph, "
                   "node, and render-group instance ids");
  }
  if (frame.lifecycleEpoch == 0U) {
    return Failure(EffectRuntimeClockStatus::InvalidLifecycleEpoch,
                   "effect runtime clock lifecycleEpoch must be non-zero");
  }
  if (frame.timelineTimeUs < 0) {
    return Failure(EffectRuntimeClockStatus::InvalidTimelineTime,
                   "effect runtime clock timelineTimeUs must be non-negative");
  }
  if (frame.advanceUs < 0) {
    return Failure(EffectRuntimeClockStatus::InvalidAdvance,
                   "effect runtime clock advanceUs must be non-negative");
  }

  switch (frame.event) {
  case EffectRuntimeClockEvent::Begin:
  case EffectRuntimeClockEvent::SeekHold:
  case EffectRuntimeClockEvent::Reset:
    if (frame.advanceUs != 0) {
      return Failure(EffectRuntimeClockStatus::InvalidAdvance,
                     "Begin, SeekHold, and Reset require advanceUs == 0");
    }
    if (frame.checkpoint) {
      return Failure(EffectRuntimeClockStatus::UnexpectedCheckpoint,
                     "only RestoreCheckpoint accepts a checkpoint");
    }
    break;
  case EffectRuntimeClockEvent::Advance:
    if (frame.advanceUs <= 0) {
      return Failure(EffectRuntimeClockStatus::InvalidAdvance,
                     "Advance requires advanceUs > 0");
    }
    if (frame.checkpoint) {
      return Failure(EffectRuntimeClockStatus::UnexpectedCheckpoint,
                     "Advance does not accept a checkpoint");
    }
    break;
  case EffectRuntimeClockEvent::RestoreCheckpoint:
    if (frame.advanceUs != 0) {
      return Failure(EffectRuntimeClockStatus::InvalidAdvance,
                     "RestoreCheckpoint requires advanceUs == 0");
    }
    if (!frame.checkpoint) {
      return Failure(EffectRuntimeClockStatus::MissingCheckpoint,
                     "RestoreCheckpoint requires an exact checkpoint");
    }
    break;
  default:
    return Failure(EffectRuntimeClockStatus::InvalidEvent,
                   "effect runtime clock event is unsupported");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto found = states_.find(identity);

  if (frame.event == EffectRuntimeClockEvent::Begin) {
    if (found != states_.end()) {
      if (frame.lifecycleEpoch < found->second.lifecycleEpoch) {
        return Failure(EffectRuntimeClockStatus::StaleLifecycleEpoch,
                       "Begin lifecycleEpoch is older than current state");
      }
      if (frame.lifecycleEpoch == found->second.lifecycleEpoch) {
        return Failure(EffectRuntimeClockStatus::DuplicateBegin,
                       "Begin cannot replace an active lifecycle epoch");
      }
    }
    const State state{frame.lifecycleEpoch, frame.timelineTimeUs, 0, 1U};
    states_.insert_or_assign(identity, state);
    EffectRuntimeClockResult result;
    result.status = EffectRuntimeClockStatus::Applied;
    result.sample = MakeSample(identity, state);
    return result;
  }

  if (frame.event == EffectRuntimeClockEvent::RestoreCheckpoint) {
    const auto &checkpoint = *frame.checkpoint;
    if (checkpoint.identity != identity) {
      return Failure(EffectRuntimeClockStatus::CheckpointIdentityMismatch,
                     "checkpoint identity does not match the target instance");
    }
    if (checkpoint.lifecycleEpoch == 0U || checkpoint.stateRevision == 0U ||
        checkpoint.timelineTimeUs < 0 || checkpoint.elapsedUs < 0) {
      return Failure(EffectRuntimeClockStatus::InvalidCheckpoint,
                     "checkpoint contains invalid epoch, time, or revision");
    }
    if (checkpoint.lifecycleEpoch != frame.lifecycleEpoch) {
      return Failure(EffectRuntimeClockStatus::CheckpointEpochMismatch,
                     "checkpoint lifecycleEpoch does not match the frame");
    }
    if (checkpoint.timelineTimeUs != frame.timelineTimeUs) {
      return Failure(EffectRuntimeClockStatus::CheckpointTimelineMismatch,
                     "checkpoint timelineTimeUs does not match the frame");
    }
    const EffectRuntimeClockCacheKey checkpointKey{
        checkpoint.identity, checkpoint.lifecycleEpoch,
        checkpoint.timelineTimeUs, checkpoint.elapsedUs,
        checkpoint.stateRevision};
    if (checkpoint.cacheRevision !=
        BuildEffectRuntimeClockCacheRevision(checkpointKey)) {
      return Failure(EffectRuntimeClockStatus::InvalidCheckpoint,
                     "checkpoint cache revision does not match its state");
    }
    if (found != states_.end() &&
        checkpoint.lifecycleEpoch < found->second.lifecycleEpoch) {
      return Failure(EffectRuntimeClockStatus::StaleLifecycleEpoch,
                     "checkpoint lifecycleEpoch is older than current state");
    }
    const State state{checkpoint.lifecycleEpoch, checkpoint.timelineTimeUs,
                      checkpoint.elapsedUs, checkpoint.stateRevision};
    states_.insert_or_assign(identity, state);
    EffectRuntimeClockResult result;
    result.status = EffectRuntimeClockStatus::Applied;
    result.sample = MakeSample(identity, state);
    return result;
  }

  if (found == states_.end()) {
    return Failure(EffectRuntimeClockStatus::MissingState,
                   "effect runtime clock instance has not begun");
  }
  const auto epochStatus =
      CompareEpochs(found->second.lifecycleEpoch, frame.lifecycleEpoch);
  if (epochStatus != EffectRuntimeClockStatus::Applied) {
    return Failure(epochStatus,
                   epochStatus == EffectRuntimeClockStatus::StaleLifecycleEpoch
                       ? "event lifecycleEpoch is older than current state"
                       : "event lifecycleEpoch requires Begin or checkpoint "
                         "restore before use");
  }
  if (found->second.stateRevision ==
      std::numeric_limits<std::uint64_t>::max()) {
    return Failure(EffectRuntimeClockStatus::RevisionOverflow,
                   "effect runtime clock state revision would overflow");
  }

  State next = found->second;
  if (frame.event == EffectRuntimeClockEvent::Advance) {
    if (next.elapsedUs >
        std::numeric_limits<std::int64_t>::max() - frame.advanceUs) {
      return Failure(EffectRuntimeClockStatus::ArithmeticOverflow,
                     "effect runtime clock elapsedUs would overflow");
    }
    next.elapsedUs += frame.advanceUs;
  } else if (frame.event == EffectRuntimeClockEvent::Reset) {
    next.elapsedUs = 0;
  }
  next.timelineTimeUs = frame.timelineTimeUs;
  ++next.stateRevision;
  found->second = next;

  EffectRuntimeClockResult result;
  result.status = EffectRuntimeClockStatus::Applied;
  result.sample = MakeSample(identity, next);
  return result;
}

std::optional<EffectRuntimeClockSample>
EffectRuntimeClockStore::Find(const EffectRuntimeClockIdentity &identity,
                              const std::uint64_t lifecycleEpoch) const {
  if (!ValidIdentity(identity) || lifecycleEpoch == 0U)
    return std::nullopt;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = states_.find(identity);
  if (found == states_.end() ||
      found->second.lifecycleEpoch != lifecycleEpoch) {
    return std::nullopt;
  }
  return MakeSample(identity, found->second);
}

std::size_t EffectRuntimeClockStore::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return states_.size();
}

std::size_t
EffectRuntimeClockStore::EraseGraph(const std::uint64_t graphInstanceId) {
  if (graphInstanceId == 0U)
    return 0U;
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t erased = 0U;
  for (auto iterator = states_.begin(); iterator != states_.end();) {
    if (iterator->first.graphInstanceId == graphInstanceId) {
      iterator = states_.erase(iterator);
      ++erased;
    } else {
      ++iterator;
    }
  }
  return erased;
}

void EffectRuntimeClockStore::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  states_.clear();
}

} // namespace videocut::text
