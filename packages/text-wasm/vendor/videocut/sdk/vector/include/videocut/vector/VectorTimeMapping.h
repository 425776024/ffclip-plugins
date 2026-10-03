#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace videocut::vector {

enum class VectorLoopMode : std::uint8_t {
  Clamp = 0,
  Repeat,
  PingPong,
};

enum class VectorDirection : std::uint8_t {
  Forward = 0,
  Reverse,
};

struct VectorTimeMapping final {
  std::int64_t sourceInUs{0};
  std::int64_t sourceOutUs{0};
  std::uint32_t rateNumerator{1};
  std::uint32_t rateDenominator{1};
  VectorLoopMode loopMode{VectorLoopMode::Clamp};
  VectorDirection direction{VectorDirection::Forward};
  bool holdBefore{true};
  bool holdAfter{true};
};

struct VectorTimeSample final {
  bool valid{false};
  std::int64_t sourceUs{0};
  std::string error;
};

/// Typed owner-local sticker phases learned from the Qt/Ghidra reference.
/// The phase remains attached to the existing Vector/Clip identity; it never
/// creates a panel-owned animation object or rewrites intrinsic source time.
enum class VectorAnimationPhase : std::uint8_t {
  Intrinsic = 0,
  Enter,
  Loop,
  Exit,
};

struct VectorAnimationPhaseSegment final {
  std::int64_t sourceInUs{0};
  std::int64_t sourceOutUs{0};
  std::uint32_t rateNumerator{1};
  std::uint32_t rateDenominator{1};
  VectorLoopMode loopMode{VectorLoopMode::Clamp};
  VectorDirection direction{VectorDirection::Forward};
  bool holdAfter{true};
};

/// Enter consumes [0, enterDurationUs), loop owns the middle interval, and an
/// optional exit begins at exitBeginUs. Source ranges are explicit so phase
/// boundaries do not depend on mutable panel playhead state.
struct VectorAnimationPhaseEnvelope final {
  std::optional<VectorAnimationPhaseSegment> enter;
  VectorAnimationPhaseSegment loop;
  std::optional<VectorAnimationPhaseSegment> exit;
  std::int64_t enterDurationUs{0};
  std::optional<std::int64_t> exitBeginUs;
};

struct VectorAnimationPhaseSample final {
  bool valid{false};
  VectorAnimationPhase phase{VectorAnimationPhase::Intrinsic};
  std::int64_t phaseLocalUs{0};
  std::int64_t sourceUs{0};
  std::string error;
};

VectorTimeSample MapVectorTime(const VectorTimeMapping &mapping,
                               std::int64_t clipLocalUs) noexcept;

VectorAnimationPhaseSample
MapVectorAnimationPhaseTime(const VectorAnimationPhaseEnvelope &envelope,
                            std::int64_t clipLocalUs) noexcept;

} // namespace videocut::vector
