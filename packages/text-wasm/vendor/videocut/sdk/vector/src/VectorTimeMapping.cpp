#include "videocut/vector/VectorTimeMapping.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace videocut::vector {
namespace {

bool Scale(std::int64_t value, std::uint32_t numerator,
           std::uint32_t denominator, std::int64_t &output) noexcept {
  if (value < 0 || numerator == 0 || denominator == 0)
    return false;
#if defined(__SIZEOF_INT128__)
  const __int128 scaled = static_cast<__int128>(value) *
                          static_cast<__int128>(numerator) /
                          static_cast<__int128>(denominator);
  if (scaled > std::numeric_limits<std::int64_t>::max())
    return false;
  output = static_cast<std::int64_t>(scaled);
  return true;
#else
  const auto quotient = value / static_cast<std::int64_t>(denominator);
  const auto remainder = value % static_cast<std::int64_t>(denominator);
  if (quotient > std::numeric_limits<std::int64_t>::max() /
                     static_cast<std::int64_t>(numerator) ||
      remainder > std::numeric_limits<std::int64_t>::max() /
                      static_cast<std::int64_t>(numerator))
    return false;
  const auto head = quotient * static_cast<std::int64_t>(numerator);
  const auto tail = remainder * static_cast<std::int64_t>(numerator) /
                    static_cast<std::int64_t>(denominator);
  if (head > std::numeric_limits<std::int64_t>::max() - tail)
    return false;
  output = head + tail;
  return true;
#endif
}

} // namespace

VectorTimeSample MapVectorTime(const VectorTimeMapping &mapping,
                               std::int64_t clipLocalUs) noexcept {
  VectorTimeSample result;
  if (mapping.sourceInUs < 0 || mapping.sourceOutUs <= mapping.sourceInUs ||
      mapping.rateNumerator == 0 || mapping.rateDenominator == 0 ||
      (mapping.loopMode != VectorLoopMode::Clamp &&
       mapping.loopMode != VectorLoopMode::Repeat &&
       mapping.loopMode != VectorLoopMode::PingPong) ||
      (mapping.direction != VectorDirection::Forward &&
       mapping.direction != VectorDirection::Reverse)) {
    result.error = "vector time mapping is invalid";
    return result;
  }
  const std::int64_t span = mapping.sourceOutUs - mapping.sourceInUs;
  if (clipLocalUs < 0) {
    if (!mapping.holdBefore) {
      result.error = "vector sample is before the clip source window";
      return result;
    }
    clipLocalUs = 0;
  }
  std::int64_t scaled = 0;
  if (!Scale(clipLocalUs, mapping.rateNumerator, mapping.rateDenominator,
             scaled)) {
    result.error = "vector time mapping overflowed";
    return result;
  }

  std::int64_t offset = 0;
  switch (mapping.loopMode) {
  case VectorLoopMode::Clamp:
    if (scaled >= span && !mapping.holdAfter) {
      result.error = "vector sample is after the clip source window";
      return result;
    }
    offset = std::min(scaled, span - 1);
    break;
  case VectorLoopMode::Repeat:
    offset = scaled % span;
    break;
  case VectorLoopMode::PingPong:
    if (span > std::numeric_limits<std::int64_t>::max() / 2) {
      result.error = "vector ping-pong span is too large";
      return result;
    }
    {
      const std::int64_t period = span * 2;
      const std::int64_t phase = scaled % period;
      offset = phase < span ? phase : period - 1 - phase;
    }
    break;
  }
  if (mapping.direction == VectorDirection::Reverse)
    offset = span - 1 - offset;
  result.sourceUs = mapping.sourceInUs + offset;
  result.valid = true;
  return result;
}

VectorAnimationPhaseSample
MapVectorAnimationPhaseTime(const VectorAnimationPhaseEnvelope &envelope,
                            std::int64_t clipLocalUs) noexcept {
  VectorAnimationPhaseSample output;
  const auto validSegment = [](const VectorAnimationPhaseSegment &segment) {
    return segment.sourceInUs >= 0 &&
           segment.sourceOutUs > segment.sourceInUs &&
           segment.rateNumerator != 0 && segment.rateDenominator != 0 &&
           (segment.loopMode == VectorLoopMode::Clamp ||
            segment.loopMode == VectorLoopMode::Repeat ||
            segment.loopMode == VectorLoopMode::PingPong) &&
           (segment.direction == VectorDirection::Forward ||
            segment.direction == VectorDirection::Reverse);
  };
  if (clipLocalUs < 0 || envelope.enterDurationUs < 0 ||
      (envelope.enter.has_value() != (envelope.enterDurationUs > 0)) ||
      (envelope.exit.has_value() != envelope.exitBeginUs.has_value()) ||
      (envelope.exitBeginUs &&
       *envelope.exitBeginUs < envelope.enterDurationUs) ||
      !validSegment(envelope.loop) ||
      (envelope.enter && !validSegment(*envelope.enter)) ||
      (envelope.exit && !validSegment(*envelope.exit))) {
    output.error = "vector animation phase envelope is invalid";
    return output;
  }

  const VectorAnimationPhaseSegment *segment = &envelope.loop;
  std::int64_t phaseLocalUs = clipLocalUs - envelope.enterDurationUs;
  output.phase = VectorAnimationPhase::Loop;
  if (envelope.enter && clipLocalUs < envelope.enterDurationUs) {
    segment = &*envelope.enter;
    phaseLocalUs = clipLocalUs;
    output.phase = VectorAnimationPhase::Enter;
  } else if (envelope.exitBeginUs && clipLocalUs >= *envelope.exitBeginUs) {
    segment = &*envelope.exit;
    phaseLocalUs = clipLocalUs - *envelope.exitBeginUs;
    output.phase = VectorAnimationPhase::Exit;
  }
  if (phaseLocalUs < 0) {
    output.error = "vector animation phase local time is invalid";
    return output;
  }
  const VectorTimeMapping mapping{
      segment->sourceInUs,
      segment->sourceOutUs,
      segment->rateNumerator,
      segment->rateDenominator,
      segment->loopMode,
      segment->direction,
      true,
      segment->holdAfter,
  };
  const auto sampled = MapVectorTime(mapping, phaseLocalUs);
  if (!sampled.valid) {
    output.error = sampled.error;
    return output;
  }
  output.phaseLocalUs = phaseLocalUs;
  output.sourceUs = sampled.sourceUs;
  output.valid = true;
  return output;
}

} // namespace videocut::vector
