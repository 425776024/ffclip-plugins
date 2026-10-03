#include "videocut/text/TextAnimation.h"
#include "TextAnimatorSampler.h"

#include "QtTextSelectorRandom.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace videocut::text {

bool IsValidUtf8(const std::string &value) noexcept {
  const auto *bytes = reinterpret_cast<const unsigned char *>(value.data());
  std::size_t index = 0;
  while (index < value.size()) {
    const unsigned char first = bytes[index];
    std::size_t count = 0;
    std::uint32_t scalar = 0;
    if (first <= 0x7FU) {
      ++index;
      continue;
    }
    if ((first & 0xE0U) == 0xC0U) {
      count = 2;
      scalar = first & 0x1FU;
    } else if ((first & 0xF0U) == 0xE0U) {
      count = 3;
      scalar = first & 0x0FU;
    } else if ((first & 0xF8U) == 0xF0U) {
      count = 4;
      scalar = first & 0x07U;
    } else
      return false;
    if (count > value.size() - index)
      return false;
    for (std::size_t offset = 1; offset < count; ++offset) {
      const unsigned char continuation = bytes[index + offset];
      if ((continuation & 0xC0U) != 0x80U)
        return false;
      scalar = (scalar << 6U) | (continuation & 0x3FU);
    }
    if ((count == 2 && scalar < 0x80U) || (count == 3 && scalar < 0x800U) ||
        (count == 4 && scalar < 0x10000U) || scalar > 0x10FFFFU ||
        (scalar >= 0xD800U && scalar <= 0xDFFFU))
      return false;
    index += count;
  }
  return true;
}

RevealSample SampleReveal(const RevealAnimation &animation,
                          std::int64_t clipLocalTimeUs,
                          std::size_t unitCount) noexcept {
  RevealSample result;
  if (unitCount == 0 || clipLocalTimeUs < animation.startOffsetUs)
    return result;
  const std::int64_t relative = clipLocalTimeUs - animation.startOffsetUs;
  double progress = 0.0;
  if (animation.durationUs <= 0)
    progress = 1.0;
  else
    progress = std::clamp(static_cast<double>(relative) /
                              static_cast<double>(animation.durationUs),
                          0.0, 1.0);
  const double exactUnits = progress * static_cast<double>(unitCount);
  result.visibleUnits =
      std::min(unitCount, static_cast<std::size_t>(std::floor(exactUnits)));
  if (result.visibleUnits < unitCount && animation.fadeFraction > 0.0F) {
    const double fraction =
        exactUnits - static_cast<double>(result.visibleUnits);
    result.nextUnitOpacity = static_cast<float>(std::clamp(
        fraction / static_cast<double>(animation.fadeFraction), 0.0, 1.0));
    if (result.nextUnitOpacity >= 1.0F) {
      ++result.visibleUnits;
      result.nextUnitOpacity = 0.0F;
    }
  }
  if (animation.cursor.enabled) {
    const std::int64_t period =
        std::max<std::int64_t>(1, animation.cursor.periodUs);
    const std::int64_t phase = relative % period;
    result.cursorVisible =
        static_cast<double>(phase) / static_cast<double>(period) <
        animation.cursor.dutyCycle;
  }
  return result;
}

namespace {

float ApplyEasing(const TextAnimationEasing easing,
                  const float progress) noexcept {
  const float value = std::clamp(progress, 0.0F, 1.0F);
  switch (easing) {
  case TextAnimationEasing::Linear:
    return value;
  case TextAnimationEasing::EaseIn:
    return value * value;
  case TextAnimationEasing::EaseOut: {
    const float inverse = 1.0F - value;
    return 1.0F - inverse * inverse;
  }
  case TextAnimationEasing::EaseInOut:
    return value < 0.5F ? 2.0F * value * value
                        : 1.0F - std::pow(-2.0F * value + 2.0F, 2.0F) * 0.5F;
  }
  return value;
}

float SampleTrack(const TextLayerAnimationTrack &track,
                  const float progress) noexcept {
  if (track.keyframes.empty())
    return 0.0F;
  if (progress < track.keyframes.front().offset)
    return track.keyframes.front().value;
  if (progress >= track.keyframes.back().offset)
    return track.keyframes.back().value;
  const auto right = std::upper_bound(
      track.keyframes.begin(), track.keyframes.end(), progress,
      [](const float value, const TextLayerAnimationKeyframe &keyframe) {
        return value < keyframe.offset;
      });
  if (right == track.keyframes.begin() || right == track.keyframes.end()) {
    return right == track.keyframes.end() ? track.keyframes.back().value
                                          : right->value;
  }
  const auto &left = *std::prev(right);
  const float span = right->offset - left.offset;
  if (span <= 0.0F)
    return right->value;
  const float eased = ApplyEasing(left.easing, (progress - left.offset) / span);
  return left.value + (right->value - left.value) * eased;
}

TextLayerAnimationSample SampleClip(const TextLayerAnimationClip &clip,
                                    const float progress) noexcept {
  TextLayerAnimationSample result;
  result.phase = clip.phase;
  result.active = true;
  for (const auto &track : clip.tracks) {
    const float value = SampleTrack(track, progress);
    switch (track.property) {
    case TextLayerAnimationProperty::Opacity:
      result.opacity = value;
      break;
    case TextLayerAnimationProperty::PositionX:
      result.positionX = value;
      break;
    case TextLayerAnimationProperty::PositionY:
      result.positionY = value;
      break;
    case TextLayerAnimationProperty::ScaleX:
      result.scaleX = value;
      break;
    case TextLayerAnimationProperty::ScaleY:
      result.scaleY = value;
      break;
    case TextLayerAnimationProperty::RotationDegrees:
      result.rotationDegrees = value;
      break;
    }
  }
  result.opacity = std::clamp(result.opacity, 0.0F, 1.0F);
  return result;
}

TextLayerAnimationTrack
Track(const TextLayerAnimationProperty property,
      std::initializer_list<TextLayerAnimationKeyframe> keyframes) {
  TextLayerAnimationTrack result;
  result.property = property;
  result.keyframes.assign(keyframes.begin(), keyframes.end());
  return result;
}

TextLayerAnimationKeyframe
Key(const float offset, const float value,
    const TextAnimationEasing easing = TextAnimationEasing::Linear) {
  return {offset, value, easing};
}

} // namespace

namespace {

template <typename Real>
Real PhaseProgress(const std::int64_t elapsedUs,
                   const std::int64_t durationUs) noexcept {
  if constexpr (std::is_same_v<Real, double>) {
    // SegmentJS does not divide the integer timeline directly. The captured
    // native call chain first exposes seconds, maps them through the 3-second
    // Studio timeline, and the custom executor divides by that same timeline:
    //   relativeTime = elapsedSeconds / durationSeconds * 3
    //   progress     = relativeTime / 3
    // Keeping those binary64 operations observable is required for exact JSC
    // inputs; direct elapsedUs/durationUs differs by one ULP at 8 of the 20
    // canonical 3-second preview samples.
    constexpr double kMicrosecondsPerSecond = 1'000'000.0;
    constexpr double kStudioTotalTimeSeconds = 3.0;
    const double elapsedSeconds =
        static_cast<double>(elapsedUs) / kMicrosecondsPerSecond;
    const double durationSeconds = std::max(
        static_cast<double>(durationUs) / kMicrosecondsPerSecond, 1.0e-6);
    const double relativeTimeSeconds =
        elapsedSeconds / durationSeconds * kStudioTotalTimeSeconds;
    return relativeTimeSeconds / kStudioTotalTimeSeconds;
  }
  return static_cast<Real>(elapsedUs) / static_cast<Real>(durationUs);
}

TextAnimationTimeDriverKind
DriverForPhase(const TextLayerAnimationPhase phase) noexcept {
  switch (phase) {
  case TextLayerAnimationPhase::Enter:
    return TextAnimationTimeDriverKind::EnterPhase;
  case TextLayerAnimationPhase::Loop:
    return TextAnimationTimeDriverKind::LoopPhase;
  case TextLayerAnimationPhase::Exit:
    return TextAnimationTimeDriverKind::ExitPhase;
  case TextLayerAnimationPhase::Caption:
    return TextAnimationTimeDriverKind::CaptionPhase;
  }
  return TextAnimationTimeDriverKind::ClipLocal;
}

template <typename Real>
Real PlaybackProgress(const TextAnimationTimeDriver &driver,
                      const std::int64_t timeUs, bool &active) noexcept {
  // Avoid signed overflow when callers probe the closed evaluator with
  // extreme timeline values.  A time before a non-negative start offset is
  // inactive regardless of the exact (unrepresentable) difference.
  const auto relative =
      timeUs < driver.startOffsetUs
          ? std::numeric_limits<std::int64_t>::min()
          : timeUs - driver.startOffsetUs;
  if (relative < 0) {
    active = false;
    return Real{0};
  }
  const auto duration = std::max<std::int64_t>(1, driver.durationUs);
  switch (driver.playback) {
  case TextAnimationPlaybackMode::Once:
    active = relative <= duration;
    return std::clamp(PhaseProgress<Real>(relative, duration), Real{0},
                      Real{1});
  case TextAnimationPlaybackMode::Hold:
    active = true;
    return std::clamp(PhaseProgress<Real>(relative, duration), Real{0},
                      Real{1});
  case TextAnimationPlaybackMode::Loop: {
    active = true;
    const auto wrapped = relative % duration;
    return PhaseProgress<Real>(wrapped, duration);
  }
  case TextAnimationPlaybackMode::PingPong: {
    active = true;
    // Keep the doubled period in-range.  For durations above INT64_MAX/2,
    // every representable relative time lies in the first mathematical
    // cycle, so the branch below computes the reflected half without forming
    // `2 * duration`.
    std::int64_t directed = 0;
    if (duration > std::numeric_limits<std::int64_t>::max() / 2) {
      directed = relative <= duration ? relative
                                      : duration - (relative - duration);
    } else {
      const auto cycle = duration * 2;
      const auto wrapped = relative % cycle;
      directed = wrapped <= duration ? wrapped : cycle - wrapped;
    }
    return PhaseProgress<Real>(directed, duration);
  }
  }
  active = false;
  return Real{0};
}

template <typename Real>
Real TimedProgress(const TextAnimationTimeDriver &driver,
                   const std::int64_t timeUs,
                   const std::vector<TimedTextSpan> &spans,
                   bool &active) noexcept {
  Real result = Real{0};
  active = false;
  const auto mode = driver.timedRanges ? driver.timedRanges->mode
                                       : TextTimedDriverMode::SpanProgress;
  for (const auto &span : spans) {
    if (driver.timedRanges && !driver.timedRanges->spanIds.empty() &&
        std::find(driver.timedRanges->spanIds.begin(),
                  driver.timedRanges->spanIds.end(),
                  span.spanId) == driver.timedRanges->spanIds.end()) {
      continue;
    }
    if (timeUs < span.startOffsetUs || timeUs >= span.endOffsetUs)
      continue;
    active = true;
    if (mode == TextTimedDriverMode::SpanStep ||
        mode == TextTimedDriverMode::ActiveHold) {
      result = Real{1};
      continue;
    }
    const auto transitionEnd =
        span.transitionEndOffsetUs > span.startOffsetUs
            ? std::min(span.transitionEndOffsetUs, span.endOffsetUs)
            : span.endOffsetUs;
    const auto duration =
        std::max<std::int64_t>(1, transitionEnd - span.startOffsetUs);
    result = std::max(
        result, std::clamp(static_cast<Real>(timeUs - span.startOffsetUs) /
                               static_cast<Real>(duration),
                           Real{0}, Real{1}));
  }
  return result;
}

template <typename Real> struct EvaluatedAnimationLayerSample final {
  std::string layerId;
  Real progress{Real{0}};
  bool active{false};
  std::int64_t durationUs{0};
};

template <typename Real>
EvaluatedAnimationLayerSample<Real>
SampleLayer(const TextAnimationLayerSpec &layer, const std::int64_t timeUs,
            const std::int64_t durationUs,
            const ResolvedTextAnimationDurations &resolved,
            const std::vector<TimedTextSpan> &timedSpans) noexcept {
  EvaluatedAnimationLayerSample<Real> result;
  result.layerId = layer.layerId;
  if (!layer.enabled || durationUs <= 0)
    return result;
  const auto time = std::clamp(timeUs, std::int64_t{0}, durationUs);
  switch (layer.timeDriver.kind) {
  case TextAnimationTimeDriverKind::EnterPhase:
    result.durationUs = resolved.enterUs;
    if (resolved.enterUs > 0 && time < resolved.enterUs) {
      result.active = true;
      result.progress = std::clamp(PhaseProgress<Real>(time, resolved.enterUs),
                                   Real{0}, Real{1});
    }
    break;
  case TextAnimationTimeDriverKind::ExitPhase: {
    result.durationUs = resolved.exitUs;
    const auto begin = durationUs - resolved.exitUs;
    if (resolved.exitUs > 0 && time >= begin) {
      result.active = true;
      result.progress = std::clamp(
          PhaseProgress<Real>(time - begin, resolved.exitUs), Real{0}, Real{1});
    }
    break;
  }
  case TextAnimationTimeDriverKind::LoopPhase:
    result.durationUs = std::max<std::int64_t>(1, layer.timeDriver.durationUs);
    if (resolved.loopRegionUs > 0 && time >= resolved.enterUs &&
        time < durationUs - resolved.exitUs) {
      result.progress = PlaybackProgress<Real>(
          layer.timeDriver, time - resolved.enterUs, result.active);
    }
    break;
  case TextAnimationTimeDriverKind::CaptionPhase:
    result.durationUs = std::max<std::int64_t>(1, layer.timeDriver.durationUs);
    result.progress =
        PlaybackProgress<Real>(layer.timeDriver, time, result.active);
    break;
  case TextAnimationTimeDriverKind::ClipLocal:
    result.durationUs = std::max<std::int64_t>(1, layer.timeDriver.durationUs);
    result.progress =
        PlaybackProgress<Real>(layer.timeDriver, time, result.active);
    break;
  case TextAnimationTimeDriverKind::TimedRanges:
    result.progress =
        TimedProgress<Real>(layer.timeDriver, time, timedSpans, result.active);
    break;
  }
  return result;
}

template <typename Real>
Real SelectorShape(const TextSelectorShape shape, const Real value) noexcept {
  const auto p = std::clamp(value, Real{0}, Real{1});
  switch (shape) {
  case TextSelectorShape::Linear:
  case TextSelectorShape::RampUp:
    return p;
  case TextSelectorShape::RampDown:
    return Real{1} - p;
  case TextSelectorShape::Triangle:
    return Real{1} - std::abs(Real{2} * p - Real{1});
  case TextSelectorShape::Round:
    return std::sqrt(
        std::max(Real{0}, Real{1} - (p - Real{1}) * (p - Real{1})));
  case TextSelectorShape::Smooth:
  case TextSelectorShape::CustomCubic:
    return p * p * (Real{3} - Real{2} * p);
  case TextSelectorShape::Square:
    return Real{1};
  }
  return p;
}

std::size_t OrderedUnit(const TextUnitSelector &selector,
                        const std::size_t index,
                        const std::size_t count,
                        QtTextSelectorRandomMap *randomOrder = nullptr) noexcept {
  if (count == 0)
    return 0;
  switch (selector.order) {
  case TextUnitOrder::Forward:
    return index;
  case TextUnitOrder::Backward:
    return count - 1U - index;
  case TextUnitOrder::CenterOut: {
    const auto left = (count - 1U) / 2U;
    if (index <= left)
      return (left - index) * 2U;
    return (index - left) * 2U - 1U;
  }
  case TextUnitOrder::Random: {
    QtTextSelectorRandomBasis basis = QtTextSelectorRandomBasis::Letter;
    if (selector.basedOn == TextUnitBasis::Word)
      basis = QtTextSelectorRandomBasis::Word;
    else if (selector.basedOn == TextUnitBasis::Line ||
             selector.basedOn == TextUnitBasis::All)
      basis = QtTextSelectorRandomBasis::Line;
    try {
      // The renderer presents one already-segmented selector domain here, so
      // Qt's BaseSelector range is [0,count). Newline placeholders are not
      // members of this compact domain; the topology builder excludes them
      // before sampling. Preserve the shipped seedrandom + Fisher-Yates order
      // instead of approximating it with an unrelated hash permutation.
      QtTextSelectorRandomMap localOrder;
      auto &map = randomOrder ? *randomOrder : localOrder;
      if (map.empty()) {
        map = BuildQtTextSelectorRandomMap(
            selector.randomSeed, {{0U, count}}, basis);
      }
      if (index < map.size() && map[index])
        return *map[index];
    } catch (...) {
      // Sampling is noexcept. Allocation/conversion failure leaves the stable
      // source order intact rather than terminating a render worker.
    }
    return index;
  }
  }
  return index;
}

template <typename Real>
Real CubicBezierCoordinate(const Real t, const Real p0, const Real p1,
                           const Real p2, const Real p3) noexcept {
  const Real inverse = Real{1} - t;
  return p0 * inverse * inverse * inverse +
         Real{3} * p1 * t * inverse * inverse + Real{3} * p2 * t * t * inverse +
         p3 * t * t * t;
}

float SolveQtCubicBezierTime(const float x, const float p1,
                             const float p2) noexcept {
  // VideoFusion 11.3.0's native getInterpolationCubicBezier implementation
  // evaluates this path in binary32.  Its endpoint/derivative tolerance is
  // 1e-6 and its sampled-X convergence tolerance is 1e-3.  Keep the authored
  // values in float even when the surrounding evaluator exposes a double
  // receipt: carrying extra precision changes the selected animation sample.
  if (x < 0.0F || std::fabs(x) < 1.0e-6F)
    return 0.0F;
  if (x > 1.0F || std::fabs(x - 1.0F) < 1.0e-6F)
    return 1.0F;
  const float a = 1.0F - 3.0F * p2 + 3.0F * p1;
  const float b = 3.0F * p2 - 6.0F * p1;
  const float c = 3.0F * p1;
  float parameter = x;
  for (int iteration = 0; iteration < 8; ++iteration) {
    const float error = ((a * parameter + b) * parameter + c) * parameter - x;
    if (std::fabs(error) < 1.0e-3F)
      return parameter;
    const float derivative =
        (3.0F * a * parameter + 2.0F * b) * parameter + c;
    if (std::fabs(derivative) < 1.0e-6F)
      break;
    parameter = std::clamp(parameter - error / derivative, 0.0F, 1.0F);
  }
  float lower = 0.0F;
  float upper = 1.0F;
  while (lower < upper && std::fabs(upper - lower) > 1.0e-6F) {
    const float candidate = (upper - lower) * 0.5F + lower;
    const float sampled = ((a * candidate + b) * candidate + c) * candidate;
    if (std::fabs(sampled - x) < 1.0e-3F)
      return candidate;
    if (x > sampled)
      lower = candidate;
    else
      upper = candidate;
  }
  return std::clamp(parameter, 0.0F, 1.0F);
}

template <typename Real, typename Keyframe>
Real SampleScalarKeyframes(const std::vector<Keyframe> &keyframes,
                           const Real progress, const Real fallback) noexcept {
  if (keyframes.empty())
    return fallback;
  // Qt Studio curves preserve source order for coincident keyframes.  The
  // first key at an offset closes the segment on its left and the last key at
  // that same offset opens the segment on its right, which represents an
  // authored instantaneous discontinuity without inventing an epsilon.
  if (progress < static_cast<Real>(keyframes.front().offset))
    return static_cast<Real>(keyframes.front().value);
  if (progress >= static_cast<Real>(keyframes.back().offset))
    return static_cast<Real>(keyframes.back().value);
  const auto right =
      std::upper_bound(keyframes.begin(), keyframes.end(), progress,
                       [](const Real value, const Keyframe &keyframe) {
                         return value < static_cast<Real>(keyframe.offset);
                       });
  if (right == keyframes.begin() || right == keyframes.end())
    return right == keyframes.end() ? static_cast<Real>(keyframes.back().value)
                                    : static_cast<Real>(right->value);
  const auto &left = *std::prev(right);
  const Real leftOffset = static_cast<Real>(left.offset);
  const Real rightOffset = static_cast<Real>(right->offset);
  const Real leftValue = static_cast<Real>(left.value);
  const Real rightValue = static_cast<Real>(right->value);
  const Real span = rightOffset - leftOffset;
  if (span <= Real{0})
    return rightValue;
  const Real linearProgress =
      std::clamp((progress - leftOffset) / span, Real{0}, Real{1});
  const bool qtCurve = left.cubicBezier || right->cubicBezier;
  Real t = linearProgress;
  const Real leftTimeOut = static_cast<Real>(left.bezierTimeOut);
  const Real rightTimeIn = static_cast<Real>(right->bezierTimeIn);
  if (qtCurve &&
      (std::fabs(leftTimeOut) > Real{0} || std::fabs(rightTimeIn) > Real{0})) {
    t = SolveQtCubicBezierTime(linearProgress, leftTimeOut / span,
                               Real{1} + rightTimeIn / span);
  }
  if (qtCurve) {
    const Real controlOut =
        leftValue + static_cast<Real>(left.tangentOut) * leftTimeOut;
    const Real controlIn =
        rightValue + static_cast<Real>(right->tangentIn) * rightTimeIn;
    const bool cubicValue = std::fabs(controlOut - leftValue) >= Real{1.0e-5} ||
                            std::fabs(controlIn - rightValue) >= Real{1.0e-5};
    return cubicValue ? CubicBezierCoordinate(t, leftValue, controlOut,
                                              controlIn, rightValue)
                      : leftValue + (rightValue - leftValue) * t;
  }
  const Real t2 = t * t;
  const Real t3 = t2 * t;
  return (Real{2} * t3 - Real{3} * t2 + Real{1}) * leftValue +
         (t3 - Real{2} * t2 + t) * static_cast<Real>(left.tangentOut) * span +
         (-Real{2} * t3 + Real{3} * t2) * rightValue +
         (t3 - t2) * static_cast<Real>(right->tangentIn) * span;
}

template <typename Real>
Real SampleSelectorKeyframes(const std::vector<TextSelectorKeyframe> &keyframes,
                             const Real progress,
                             const Real fallback) noexcept {
  return SampleScalarKeyframes<Real>(keyframes, progress, fallback);
}

TextSelectorAttributeSample SampleSelectorAttributes(
    const TextUnitSelector &selector, const double layerProgress) noexcept {
  TextSelectorAttributeSample result;
  if (selector.kind != TextSelectorKind::Time) {
    result.rangeStart = SampleSelectorKeyframes(
        selector.rangeStartKeyframes, layerProgress, selector.rangeStart);
    result.rangeEnd = SampleSelectorKeyframes(
        selector.rangeEndKeyframes, layerProgress, selector.rangeEnd);
    result.offset = SampleSelectorKeyframes(
        selector.offsetKeyframes, layerProgress, selector.offset);
  }
  result.intensity = SampleSelectorKeyframes(
      selector.intensityKeyframes, layerProgress, selector.intensity);
  return result;
}

template <typename Real>
Real SelectorWeight(const TextUnitSelector &selector, const std::size_t index,
                    const std::size_t count,
                    const Real layerProgress,
                    QtTextSelectorRandomMap *randomOrder,
                    const TextSelectorAttributeSample *attributes) noexcept {
  if (count == 0)
    return Real{0};
  const auto ordered = OrderedUnit(selector, index, count, randomOrder);
  const Real countValue = static_cast<Real>(count);
  const auto intensityBegin = static_cast<std::size_t>(std::clamp(
      std::floor(static_cast<Real>(selector.intensityStart) * countValue +
                 Real{0.5}),
      Real{0}, countValue));
  const auto intensityEnd = static_cast<std::size_t>(std::clamp(
      std::floor(static_cast<Real>(selector.intensityEnd) * countValue +
                 Real{0.5}),
      Real{0}, countValue));
  if (index < intensityBegin || index >= intensityEnd)
    return Real{0};
  const auto sampled =
      attributes ? *attributes : SampleSelectorAttributes(selector, layerProgress);
  const Real sampledStart = static_cast<Real>(sampled.rangeStart);
  const Real sampledEnd = static_cast<Real>(sampled.rangeEnd);
  const Real sampledOffset = static_cast<Real>(sampled.offset);
  const Real sampledIntensity = static_cast<Real>(sampled.intensity);
  // Qt's TextSelector evaluates the mapped unit index against a range in unit
  // coordinates. Selector-attribute curves are sampled on the layer clock;
  // they are not an extra phase term in the selector weight.
  const Real start = (sampledStart + sampledOffset) * countValue - Real{0.5};
  const Real end = (sampledEnd + sampledOffset) * countValue - Real{0.5};
  const Real unit = static_cast<Real>(ordered);
  Real weight = Real{0};
  switch (selector.shape) {
  case TextSelectorShape::Square: {
    if (unit < start || unit > end)
      break;
    const Real edge =
        std::clamp(static_cast<Real>(selector.edgeSmooth), Real{0}, Real{1}) *
        Real{0.5};
    if (edge > Real{0.000001} && unit <= start + edge)
      weight = (unit - start) / edge;
    else if (edge > Real{0.000001} && unit >= end - edge)
      weight = (end - unit) / edge;
    else
      weight = Real{1};
    break;
  }
  case TextSelectorShape::RampUp:
    weight = unit < start ? Real{0}
             : unit > end ? Real{1}
                          : (unit - start) / (end - start);
    break;
  case TextSelectorShape::RampDown:
    weight = unit < start ? Real{1}
             : unit > end ? Real{0}
                          : (end - unit) / (end - start);
    break;
  case TextSelectorShape::Triangle: {
    if (unit < start || unit > end)
      break;
    const Real middle = (start + end) * Real{0.5};
    weight = unit < middle ? (unit - start) / (middle - start)
                           : (end - unit) / (end - middle);
    break;
  }
  case TextSelectorShape::Linear:
  case TextSelectorShape::Round:
  case TextSelectorShape::Smooth:
  case TextSelectorShape::CustomCubic: {
    if (unit < start || unit > end)
      break;
    const Real range = std::max(Real{0.0001}, end - start);
    weight = SelectorShape(selector.shape, (unit - start) / range);
    break;
  }
  }
  if (!std::isfinite(weight))
    weight = Real{0};
  return std::clamp(weight * sampledIntensity, Real{0}, Real{1});
}

template <typename Real>
Real TimeSelectorProgress(
    const TextUnitSelector &selector, const std::size_t index,
    const std::size_t count, const Real layerProgress,
    QtTextSelectorRandomMap *randomOrder) noexcept {
  if (count == 0)
    return layerProgress;
  const auto ordered = OrderedUnit(selector, index, count, randomOrder);
  const Real unit =
      (static_cast<Real>(ordered) + Real{0.5}) / static_cast<Real>(count);
  const Real start = static_cast<Real>(selector.timeStart1) +
                     unit * (static_cast<Real>(selector.timeStart2) -
                             static_cast<Real>(selector.timeStart1));
  const Real end = static_cast<Real>(selector.timeEnd1) +
                   unit * (static_cast<Real>(selector.timeEnd2) -
                           static_cast<Real>(selector.timeEnd1));
  if (end <= start)
    return Real{1};
  const Real duration = end - start;
  if (!selector.timeCycle) {
    if (layerProgress < start)
      return Real{0};
    if (layerProgress > end)
      return Real{1};
    return std::clamp((layerProgress - start) / duration, Real{0}, Real{1});
  }
  // Qt only enters its modulo branch strictly outside the authored interval.
  // The inclusive end point is therefore the held last frame (1), not the
  // first frame of the next cycle (0).
  if (layerProgress >= start && layerProgress <= end)
    return std::clamp((layerProgress - start) / duration, Real{0}, Real{1});
  Real offset = std::fmod(layerProgress - start, duration);
  if (offset < Real{0})
    offset += duration;
  return std::clamp(offset / duration, Real{0}, Real{1});
}

float SampleTextKeyframes(const std::vector<TextKeyframe> &keyframes,
                          const float progress, const float fallback) noexcept {
  return SampleScalarKeyframes<float>(keyframes, progress, fallback);
}

template <typename Real>
Real SampleAnimatorTrack(const TextAnimatorTrack &track,
                         const Real progress) noexcept {
  return SampleScalarKeyframes<Real>(track.keyframes, progress, Real{0});
}

Color SampleColorKeyframes(const std::vector<TextColorKeyframe> &keyframes,
                           const float progress) noexcept {
  if (keyframes.empty())
    return {1.0F, 1.0F, 1.0F, 1.0F};
  if (progress < keyframes.front().offset)
    return keyframes.front().value;
  if (progress >= keyframes.back().offset)
    return keyframes.back().value;
  const auto right = std::upper_bound(
      keyframes.begin(), keyframes.end(), progress,
      [](const float value, const TextColorKeyframe &keyframe) {
        return value < keyframe.offset;
      });
  const auto &left = *std::prev(right);
  const float span = right->offset - left.offset;
  if (span <= 0.0F)
    return right->value;
  const float linearProgress =
      std::clamp((progress - left.offset) / span, 0.0F, 1.0F);
  const bool qtCurve = left.cubicBezier || right->cubicBezier;
  float t = linearProgress;
  if (qtCurve && (std::fabs(left.bezierTimeOut) > 0.0F ||
                  std::fabs(right->bezierTimeIn) > 0.0F)) {
    t = SolveQtCubicBezierTime(linearProgress, left.bezierTimeOut / span,
                               1.0F + right->bezierTimeIn / span);
  }
  const auto addScaled = [](const Color &value, const Color &tangent,
                            const float scale) {
    return Color{
        value.red + tangent.red * scale, value.green + tangent.green * scale,
        value.blue + tangent.blue * scale, value.alpha + tangent.alpha * scale};
  };
  const Color controlOut =
      addScaled(left.value, left.tangentOut, left.bezierTimeOut);
  const Color controlIn =
      addScaled(right->value, right->tangentIn, right->bezierTimeIn);
  if (qtCurve) {
    // The Qt TextAnimator stores a color as four independent numeric
    // channels. Its cubic interpolation runs the same value-coordinate
    // Bezier on R, G, B and A; QColor/HSV conversion is not part of this
    // path. The captured Letter packet for pop.09 at local progress 0.4 is
    // exactly [1, .8, .88, 1] for white -> [1, .5, .7, 1].
    const auto sample = [&](const float start, const float out,
                            const float in, const float end) {
      return std::clamp(CubicBezierCoordinate(t, start, out, in, end),
                        0.0F, 1.0F);
    };
    return {sample(left.value.red, controlOut.red, controlIn.red,
                   right->value.red),
            sample(left.value.green, controlOut.green, controlIn.green,
                   right->value.green),
            sample(left.value.blue, controlOut.blue, controlIn.blue,
                   right->value.blue),
            sample(left.value.alpha, controlOut.alpha, controlIn.alpha,
                   right->value.alpha)};
  }
  const float t2 = t * t;
  const float t3 = t2 * t;
  const auto sample = [&](const float l, const float r, const float out,
                          const float in) {
    return (2.0F * t3 - 3.0F * t2 + 1.0F) * l +
           (t3 - 2.0F * t2 + t) * out * span + (-2.0F * t3 + 3.0F * t2) * r +
           (t3 - t2) * in * span;
  };
  return {
      std::clamp(sample(left.value.red, right->value.red, left.tangentOut.red,
                        right->tangentIn.red),
                 0.0F, 1.0F),
      std::clamp(sample(left.value.green, right->value.green,
                        left.tangentOut.green, right->tangentIn.green),
                 0.0F, 1.0F),
      std::clamp(sample(left.value.blue, right->value.blue,
                        left.tangentOut.blue, right->tangentIn.blue),
                 0.0F, 1.0F),
      std::clamp(sample(left.value.alpha, right->value.alpha,
                        left.tangentOut.alpha, right->tangentIn.alpha),
                 0.0F, 1.0F),
  };
}


bool NameIn(const std::string_view name,
            const std::initializer_list<std::string_view> allowed) noexcept {
  return std::find(allowed.begin(), allowed.end(), name) != allowed.end();
}

bool IndexedNameIn(
    const std::string_view name,
    const std::initializer_list<std::string_view> prefixes) noexcept {
  return std::any_of(prefixes.begin(), prefixes.end(), [&](const auto prefix) {
    return name.size() == prefix.size() + 1U &&
           name.compare(0U, prefix.size(), prefix) == 0 &&
           name.back() >= '1' && name.back() <= '8';
  });
}

bool IsTypedPostEffectParameter(const TextPostEffectKind kind,
                                const TextPostEffectParameter &parameter)
    noexcept {
  const std::string_view name{parameter.name};
  const auto hasArity = [&](const std::size_t expected) {
    return parameter.values.size() == expected &&
           (expected == 1U || parameter.keyframes.empty());
  };
  const auto scalar =
      [&](const std::initializer_list<std::string_view> allowed) {
    return NameIn(name, allowed) && hasArity(1U);
  };
  const auto vector2 =
      [&](const std::initializer_list<std::string_view> allowed) {
    return NameIn(name, allowed) && hasArity(2U);
  };
  const auto color4 =
      [&](const std::initializer_list<std::string_view> allowed) {
    return NameIn(name, allowed) && hasArity(4U);
  };
  switch (kind) {
  case TextPostEffectKind::TurbulenceDisplacement:
    return scalar({"complexity", "cycle", "evolution", "fix_type",
                   "motion_tile_type", "offset_x", "offset_y",
                   "picture_scale", "qualityType", "quantity", "size",
                   "type"});
  case TextPostEffectKind::DirectionalBlur:
    return scalar({"angle", "radius", "blurIntensity", "directionNum",
                   "exposure", "quality", "spaceDither", "borderType",
                   "blendMode"});
  case TextPostEffectKind::GodRay:
    return scalar({"angle", "angleRange", "angleType", "blendMode",
                   "borderType", "brightness", "center_x", "center_y",
                   "colorDecay", "colorType", "displayRayOnly", "dither",
                   "grayscaleCorrection", "intensity",
                   "inverseGammaCorrection", "lightSource",
                   "noiseIntensity", "quality", "range", "scaleType",
                   "smoothness", "useAlphaThreshold", "useAngle",
                   "weightDecay"}) ||
           vector2({"center", "directionPoint"}) || color4({"lightColor"});
  case TextPostEffectKind::LinearWipe:
    return scalar({"progress", "rotation", "feather"});
  case TextPostEffectKind::Dust:
    return scalar({"brightness", "contrast", "quantity", "complexity",
                   "evolution", "cycle", "offset_x", "offset_y",
                   "mask_noise_brightness", "mask_noise_contrast",
                   "mask_noise_quantity", "mask_noise_complexity",
                   "mask_noise_evolution", "mask_noise_cycle",
                   "mask_noise_offset_x", "mask_noise_offset_y",
                   "distorIns", "gravity", "gravityRot", "maskType",
                   "maskFeather", "mask_line_rot", "progress"});
  case TextPostEffectKind::DeepGlow:
    return scalar({"downSample", "exposure", "gammaCorrect", "gammaValue",
                   "glowIter", "quality", "radius", "stepsMult",
                   "kernelStrideBase", "kernelStrideAspectPower",
                   "intensity", "threshold", "blendMode", "blueOffset",
                   "ca", "glowFromAlpha", "greenOffset", "ratio",
                   "redOffset", "rotate", "sourceOpacity", "tint",
                   "tintMix", "tintMode", "unmult"}) ||
           color4({"color", "tintColor"});
  case TextPostEffectKind::SGlow:
    return scalar({"radius", "intensity", "threshold", "quality",
                   "brightness", "combine", "dither", "edgeMode",
                   "glowFromAlpha", "glowUnderSource", "glowWidth", "show",
                   "sourceOpacity", "widthBlue", "widthGreen", "widthRed",
                   "widthX", "widthY"}) ||
           color4({"color", "glowColor", "thresholdAddColor"});
  case TextPostEffectKind::SoftGlow:
    return scalar({"displayGlow", "exposure",
                   "foldSubunitExposureIntoIntensity", "glowIntensity",
                   "grayScale", "lowIntensityScheduleScale", "quality",
                   "thresholdHigh", "thresholdLow", "thresholdSmooth",
                   "thresholdType"}) ||
           color4({"glowColor"});
  case TextPostEffectKind::RadianceGlow:
    return scalar({"angle", "directionNum", "displayGlow",
                   "erodeIterations", "erodeStepMulplier", "exposure",
                   "glowIntensity", "grayScale", "quality", "spaceDither",
                   "borderType", "thresholdHigh", "thresholdLow",
                   "thresholdSmooth", "thresholdType", "useMask"}) ||
           color4({"glowColor"});
  case TextPostEffectKind::RadialBlur:
    return scalar({"amount", "blurAlpha", "blurType", "borderType",
                   "center_x", "center_y", "dither",
                   "inverseGammaCorrection", "lightIntensity",
                   "lightTransferMode", "quality", "weightDecay"});
  case TextPostEffectKind::Shake:
    return scalar({"ampRatio", "bAmpRatio", "bPhase", "blurEnabled",
                   "blurIntensity", "blurQuality", "frqRatio", "gAmpRatio",
                   "gPhase", "phase0", "rAmpRatio", "rPhase", "rgbFrq",
                   "rgbRnd", "scale0", "seed", "wAmp", "wAmpR", "wFrq",
                   "wFrqR", "wPhase", "xAmp", "xAmpR", "xFill", "xFrq",
                   "xFrqR", "xPhase", "yAmp", "yAmpR", "yFill", "yFrq",
                   "yFrqR", "yPhase", "zAmp", "zAmpR", "zFrq", "zFrqR",
                   "zPhase"});
  case TextPostEffectKind::Trail:
    return scalar({"baseEnabled", "blur", "hintHue", "hintMax", "hintMin",
                   "hintOffset", "hintProfile", "weaken"}) ||
           color4({"baseHint"});
  case TextPostEffectKind::WaveWarp:
    if (name == "amplitude")
      return hasArity(1U) || hasArity(2U);
    return scalar({"wavelength", "phase", "angle", "antiAliasing",
                   "direction", "fixedType", "speed", "type"});
  case TextPostEffectKind::DistortChroma:
    return scalar({"amount", "amountRelX", "amountRelY", "angle",
                   "blurLens", "blurMap", "mix", "quality", "rotateWarpDir",
                   "steps", "stride", "warpBlue", "warpRed", "wrapModeX",
                   "wrapModeY"}) ||
           color4({"color1", "color2", "color3"});
  case TextPostEffectKind::ChromaticAberration:
    return scalar({"offsetX", "offsetY", "amount", "angle"});
  case TextPostEffectKind::GaussianBlur:
    return scalar({"blurAlpha", "blurDirection", "blurIntensity",
                   "borderType", "gamma", "horizontalStrength",
                   "inverseGammaCorrection", "quality", "spaceDither",
                   "verticalStrength"});
  case TextPostEffectKind::AlphaOutline:
    return scalar({"intensity", "offsetX", "offsetY", "scaleX", "scaleY",
                   "size"}) ||
           color4({"outlineColor"});
  case TextPostEffectKind::MultiShadow:
    if (scalar({"count", "distance", "angle", "blur", "spread",
                "layerNum", "oriAlpha", "isAssociation", "oriBgAlpha",
                "oriBgFeather", "oriBgPad", "oriPivotX", "oriPivotY",
                "oriPositionX", "oriPositionY", "oriRotation", "oriScaleX",
                "oriScaleY", "unifiedScale"}) ||
        (IndexedNameIn(name,
                       {"alpha_", "positionX_", "positionY_", "scale_",
                        "rotation_", "bgAlpha_", "bgFeather_", "bgPad_"}) &&
         hasArity(1U))) {
      return true;
    }
    return vector2({"textExpandRatio"}) ||
           color4({"color", "oriBgColor", "oriColor"}) ||
           (IndexedNameIn(name, {"color_", "bgColor_"}) && hasArity(4U));
  case TextPostEffectKind::SimpleChoker:
    return scalar({"amount", "radius", "softness", "threshold",
                   "chokeMatte", "colorTolerance", "enableColorKey",
                   "quality", "view"}) ||
           color4({"simpleColorKey"});
  case TextPostEffectKind::CCLens:
    return scalar({"size", "radius", "convergence", "distortion", "fieldOfView",
                   "invert", "orientation"}) ||
           vector2({"center"});
  case TextPostEffectKind::OpticsCompensation:
    return scalar({"fieldOfView", "reverseLensDistortion", "viewMode",
                   "optimalPixels", "resize", "antiAliasing", "fillBorders",
                   "fov", "fovOrientation", "inverseLensDistortion"}) ||
           vector2({"center"});
  case TextPostEffectKind::Projection:
    return scalar({"rotationX", "rotationY", "rotationZ", "perspective",
                   "fieldOfView", "focalLength", "scale", "backAlpha",
                   "mixWithBlack", "offsetY_0", "shakeScale", "shakeTime"}) ||
           vector2({"center", "vanishingPoint"});
  case TextPostEffectKind::DynamicSignalGlitch:
    return scalar({"amount", "blockSize", "chromaShift", "scanline",
                   "seed", "frequency", "phase", "speed", "intensity",
                   "initialGlitchStrength", "initialFlashFrequency",
                   "initialColorOffset", "initialDistortionAmount"});
  case TextPostEffectKind::ElectricPulseMotionBlur:
    return scalar({"amount", "angle", "radius", "samples", "pulse",
                   "phase", "exposure", "speed", "intensity",
                   "maxBlurStrength", "minBlurStrength", "syncCycle"});
  case TextPostEffectKind::PulseEnvelope:
    return scalar({"intensity", "maxBlurStrength", "minBlurStrength",
                   "syncCycle"});
  case TextPostEffectKind::MorphologicalOutline:
    return scalar({"intensity", "ratio", "size", "scaleX", "scaleY",
                   "offsetX", "offsetY"}) ||
           color4({"outlineColor"});
  case TextPostEffectKind::AlternatingSegmentMask:
    return scalar({"intensity", "segmentWidth", "rotationAngle"}) ||
           (name == "displacement" &&
            (hasArity(2U) || hasArity(3U))) ||
           color4({"segmentColor"});
  case TextPostEffectKind::CenterSplitDisplacement:
    return scalar({"intensity", "ratio", "pointA", "pointB", "split"});
  case TextPostEffectKind::CutAndDrop:
    return scalar({"intensity", "ratio", "knifeProgress", "dropStrand",
                   "slant"});
  case TextPostEffectKind::BlockGlitchDotMatrix:
    return scalar({"intensity", "ratio", "glitchAmount", "blockScale",
                   "jitterSpeed"});
  case TextPostEffectKind::ProceduralFlameOutline:
    return scalar({"intensity", "ratio", "loopTime", "flameSpeed",
                   "noiseScale"}) ||
           color4({"flameColor"});
  case TextPostEffectKind::CylindricalScroll:
    return scalar({"intensity", "ratio", "scrollProgress", "itemCount",
                   "tunnelRadius", "fov", "pauseStrength"});
  case TextPostEffectKind::SplitSqueezeWave:
    return scalar({"intensity", "ratio", "waveHeight", "waveFreq"});
  case TextPostEffectKind::PerspectiveEchoTrail:
    return scalar({"intensity", "ratio", "echoCount", "echoSpacing",
                   "perspective", "curveTension"}) ||
           color4({"outlineColor"});
  }
  return false;
}

template <typename Keyframe, typename Value>
Value SampleStepKeyframes(const std::vector<Keyframe> &keyframes,
                          const float progress, Value fallback) noexcept {
  if (keyframes.empty())
    return fallback;
  const auto right =
      std::upper_bound(keyframes.begin(), keyframes.end(), progress,
                       [](const float value, const Keyframe &keyframe) {
                         return value < keyframe.offset;
                       });
  return right == keyframes.begin() ? keyframes.front().value
                                    : std::prev(right)->value;
}

} // namespace

double SampleTextKeyframeCurve(const std::vector<TextKeyframe> &keyframes,
                               const double progress,
                               const double fallback) noexcept {
  return SampleScalarKeyframes<double>(keyframes, progress, fallback);
}

ResolvedTextAnimationDurations
ResolveTextAnimationDurations(const TextAnimationStack &animations,
                              const std::int64_t clipDurationUs) noexcept {
  std::int64_t enter = 0;
  std::int64_t exit = 0;
  for (const auto &layer : animations.layers) {
    if (!layer.enabled)
      continue;
    if (layer.timeDriver.kind == TextAnimationTimeDriverKind::EnterPhase)
      enter = std::max(enter, layer.timeDriver.durationUs);
    else if (layer.timeDriver.kind == TextAnimationTimeDriverKind::ExitPhase)
      exit = std::max(exit, layer.timeDriver.durationUs);
  }
  ResolvedTextAnimationDurations result;
  const auto duration = std::max<std::int64_t>(0, clipDurationUs);
  if (duration == 0)
    return result;
  const auto authoredEnter = std::max<std::int64_t>(0, enter);
  const auto authoredExit = std::max<std::int64_t>(0, exit);
  const long double authoredTotal = static_cast<long double>(authoredEnter) +
                                    static_cast<long double>(authoredExit);
  if (authoredTotal <= static_cast<long double>(duration)) {
    result.enterUs = authoredEnter;
    result.exitUs = authoredExit;
  } else {
    result.enterUs =
        authoredTotal <= 0.0L
            ? 0
            : static_cast<std::int64_t>(std::floor(
                  static_cast<long double>(duration) *
                  static_cast<long double>(authoredEnter) / authoredTotal));
    result.exitUs = duration - result.enterUs;
  }
  result.loopRegionUs = duration - result.enterUs - result.exitUs;
  return result;
}

TextAnimationLayerSample SampleTextAnimationLayer(
    const TextAnimationLayerSpec &layer, const std::int64_t clipLocalTimeUs,
    const std::int64_t clipDurationUs,
    const std::vector<TimedTextSpan> &timedSpans) noexcept {
  TextAnimationStack stack;
  stack.layers.push_back(layer);
  return SampleTextAnimationLayer(stack, layer, clipLocalTimeUs, clipDurationUs,
                                  timedSpans);
}

TextAnimationLayerSample SampleTextAnimationLayer(
    const TextAnimationStack &stack, const TextAnimationLayerSpec &layer,
    const std::int64_t clipLocalTimeUs, const std::int64_t clipDurationUs,
    const std::vector<TimedTextSpan> &timedSpans) noexcept {
  const auto resolved = ResolveTextAnimationDurations(stack, clipDurationUs);
  const auto duration = std::max<std::int64_t>(0, clipDurationUs);
  const auto precise = SampleLayer<double>(layer, clipLocalTimeUs, duration,
                                           resolved, timedSpans);
  return {precise.layerId, static_cast<float>(precise.progress),
          precise.active};
}

TextAnimationLayerEvaluationSample SampleTextAnimationLayerEvaluation(
    const TextAnimationStack &stack, const TextAnimationLayerSpec &layer,
    const std::int64_t clipLocalTimeUs, const std::int64_t clipDurationUs,
    const std::vector<TimedTextSpan> &timedSpans) noexcept {
  const auto resolved = ResolveTextAnimationDurations(stack, clipDurationUs);
  const auto duration = std::max<std::int64_t>(0, clipDurationUs);
  const auto precise = SampleLayer<double>(layer, clipLocalTimeUs, duration,
                                           resolved, timedSpans);
  return {precise.layerId, precise.progress, precise.active, precise.durationUs};
}

namespace {

TextUnitAnimationSample
SampleTextAnimatorImpl(const TextAnimatorSpec &animator,
                       const std::size_t unitIndex, const std::size_t unitCount,
                       const double layerProgress,
                       QtTextSelectorRandomMap *randomOrders = nullptr,
                       const TextAnimatorFrameSample *frameSample =
                           nullptr) noexcept {
  using Real = double;
  Real positionX = Real{0};
  Real positionY = Real{0};
  Real positionZ = Real{0};
  Real scaleX = Real{1};
  Real scaleY = Real{1};
  Real rotationX = Real{0};
  Real rotationY = Real{0};
  Real rotationZ = Real{0};
  Real shearX = Real{0};
  Real shearY = Real{0};
  Real opacity = Real{1};
  Real tracking = Real{0};
  Real blurRadius = Real{0};
  Real distanceFromCenter = Real{0};
  Real anchorOffsetX = Real{0};
  Real anchorOffsetY = Real{0};
  Real influence = animator.selectors.empty() ? Real{1} : Real{0};
  Real trackProgress = layerProgress;
  for (std::size_t selectorIndex = 0; selectorIndex < animator.selectors.size();
       ++selectorIndex) {
    const auto &selector = animator.selectors[selectorIndex];
    auto *randomOrder = randomOrders ? randomOrders + selectorIndex : nullptr;
    const auto *attributes = frameSample && !frameSample->selectors.empty()
                                 ? &frameSample->selectors[selectorIndex]
                                 : nullptr;
    if (selector.kind == TextSelectorKind::Time) {
      const Real sampledIntensity = attributes
          ? static_cast<Real>(attributes->intensity)
          : SampleSelectorKeyframes(selector.intensityKeyframes, layerProgress,
                                    static_cast<Real>(selector.intensity));
      const Real countValue = static_cast<Real>(unitCount);
      const auto intensityBegin = static_cast<std::size_t>(std::clamp(
          std::floor(static_cast<Real>(selector.intensityStart) * countValue +
                     Real{0.5}),
          Real{0}, countValue));
      const auto intensityEnd = static_cast<std::size_t>(std::clamp(
          std::floor(static_cast<Real>(selector.intensityEnd) * countValue +
                     Real{0.5}),
          Real{0}, countValue));
      if (unitIndex >= intensityBegin && unitIndex < intensityEnd) {
        influence += sampledIntensity;
        trackProgress = TimeSelectorProgress(selector, unitIndex, unitCount,
                                             layerProgress, randomOrder);
      }
    } else {
      influence +=
          SelectorWeight(selector, unitIndex, unitCount, layerProgress,
                         randomOrder, attributes);
    }
  }
  influence = std::clamp(influence, Real{0}, Real{1});

  // Presentation is part of the animator's typed runtime contract, not UI
  // metadata. Reveal advances over the same ordered unit topology used by
  // selectors, while ActiveFill highlights the current ordered unit without
  // changing layout. Keeping this in the pure animator sampler makes preview
  // and export consume identical state and avoids a second reveal path in the
  // renderer.
  const auto orderedUnit = [&]() noexcept {
    return animator.selectors.empty()
               ? unitIndex
               : OrderedUnit(animator.selectors.front(), unitIndex,
                             unitCount, randomOrders);
  }();
  const Real presentationProgress =
      std::clamp(static_cast<Real>(layerProgress), Real{0}, Real{1});
  const Real exactPresentedUnits =
      presentationProgress * static_cast<Real>(unitCount);
  if (animator.presentation == TextUnitPresentation::Reveal) {
    const auto completeUnits = static_cast<std::size_t>(
        std::floor(exactPresentedUnits));
    Real revealOpacity = orderedUnit < completeUnits ? Real{1} : Real{0};
    if (orderedUnit == completeUnits && completeUnits < unitCount &&
        animator.fadeFraction > 0.0F) {
      const Real partial =
          exactPresentedUnits - static_cast<Real>(completeUnits);
      revealOpacity = std::clamp(
          partial / static_cast<Real>(animator.fadeFraction), Real{0},
          Real{1});
    }
    opacity *= revealOpacity;
  }
  if (influence > Real{0}) {
    const Real anchorInfluence =
        animator.anchorMode == TextAnimatorAnchorMode::Fixed ? Real{1}
                                                             : influence;
    anchorOffsetX = static_cast<Real>(animator.anchorOffsetX) * anchorInfluence;
    anchorOffsetY = static_cast<Real>(animator.anchorOffsetY) * anchorInfluence;
  }
  for (std::size_t trackIndex = 0; trackIndex < animator.tracks.size();
       ++trackIndex) {
    const auto &track = animator.tracks[trackIndex];
    const auto value = frameSample && !frameSample->tracks.empty()
                           ? frameSample->tracks[trackIndex]
                           : SampleAnimatorTrack(track, trackProgress);
    switch (track.property) {
    case TextAnimatedProperty::Opacity:
      opacity *= Real{1} + (value - Real{1}) * influence;
      break;
    case TextAnimatedProperty::PositionX:
      positionX += value * influence;
      break;
    case TextAnimatedProperty::PositionY:
      positionY += value * influence;
      break;
    case TextAnimatedProperty::ScaleX:
      scaleX *= Real{1} + (value - Real{1}) * influence;
      break;
    case TextAnimatedProperty::ScaleY:
      scaleY *= Real{1} + (value - Real{1}) * influence;
      break;
    case TextAnimatedProperty::PositionZ:
      positionZ += value * influence;
      break;
    case TextAnimatedProperty::RotationX:
      rotationX += value * influence;
      break;
    case TextAnimatedProperty::RotationY:
      rotationY += value * influence;
      break;
    case TextAnimatedProperty::RotationZ:
      rotationZ += value * influence;
      break;
    case TextAnimatedProperty::ShearX:
      shearX += value * influence;
      break;
    case TextAnimatedProperty::ShearY:
      shearY += value * influence;
      break;
    case TextAnimatedProperty::Tracking:
      tracking += value * influence;
      break;
    case TextAnimatedProperty::BlurRadius:
      blurRadius += value * influence;
      break;
    case TextAnimatedProperty::DistanceFromCenter:
      distanceFromCenter += value * influence;
      break;
    }
  }

  TextUnitAnimationSample result;
  result.positionX = static_cast<float>(positionX);
  result.positionY = static_cast<float>(positionY);
  result.positionZ = static_cast<float>(positionZ);
  result.scaleX = static_cast<float>(scaleX);
  result.scaleY = static_cast<float>(scaleY);
  result.rotationX = static_cast<float>(rotationX);
  result.rotationY = static_cast<float>(rotationY);
  result.rotationZ = static_cast<float>(rotationZ);
  result.shearX = static_cast<float>(shearX);
  result.shearY = static_cast<float>(shearY);
  result.opacity = static_cast<float>(std::clamp(opacity, Real{0}, Real{1}));
  result.tracking = static_cast<float>(tracking);
  result.blurRadius = static_cast<float>(blurRadius);
  result.distanceFromCenter = static_cast<float>(distanceFromCenter);
  result.anchorOffsetX = static_cast<float>(anchorOffsetX);
  result.anchorOffsetY = static_cast<float>(anchorOffsetY);
  if (!animator.fillColorKeyframes.empty()) {
    result.fillColor = frameSample && frameSample->fillColor
        ? *frameSample->fillColor
        : SampleColorKeyframes(animator.fillColorKeyframes,
                               static_cast<float>(trackProgress));
    result.fillColorInfluence = static_cast<float>(influence);
  } else if (animator.presentation == TextUnitPresentation::ActiveFill &&
             unitCount > 0U) {
    const auto activeUnit = std::min(
        unitCount - 1U,
        static_cast<std::size_t>(std::floor(exactPresentedUnits)));
    if (orderedUnit == activeUnit) {
      result.fillColor = animator.activeColor;
      result.fillColorInfluence = 1.0F;
    }
  }
  return result;
}

} // namespace

TextAnimatorSampler::TextAnimatorSampler(const TextAnimatorSpec &animator,
                                         const std::size_t unitCount,
                                         const double layerProgress) noexcept
    : animator_(animator), unitCount_(unitCount), layerProgress_(layerProgress) {
  if (unitCount > 0U &&
      std::any_of(animator.selectors.begin(), animator.selectors.end(),
                  [](const auto &selector) {
                    return selector.order == TextUnitOrder::Random;
                  })) {
    try {
      randomOrders_.resize(animator.selectors.size());
    } catch (...) {
      // If the cache cannot be allocated, keep the uncached sampling path.
    }
  }
  if (unitCount > 1U) {
    try {
      if (std::any_of(animator.selectors.begin(), animator.selectors.end(),
                      [](const auto &selector) {
                        return !selector.intensityKeyframes.empty() ||
                               (selector.kind != TextSelectorKind::Time &&
                                (!selector.rangeStartKeyframes.empty() ||
                                 !selector.rangeEndKeyframes.empty() ||
                                 !selector.offsetKeyframes.empty()));
                      })) {
        frameSample_.selectors.resize(animator.selectors.size());
        for (std::size_t i = 0; i < animator.selectors.size(); ++i)
          frameSample_.selectors[i] =
              SampleSelectorAttributes(animator.selectors[i], layerProgress);
      }
      // Time selectors can overwrite trackProgress independently for each
      // group. Only an animator without them has frame-constant track/color
      // samples; selector attributes always use the unchanged layer clock.
      if (std::none_of(animator.selectors.begin(), animator.selectors.end(),
                       [](const auto &selector) {
                         return selector.kind == TextSelectorKind::Time;
                       })) {
        frameSample_.tracks.resize(animator.tracks.size());
        for (std::size_t i = 0; i < animator.tracks.size(); ++i)
          frameSample_.tracks[i] =
              SampleAnimatorTrack(animator.tracks[i], layerProgress);
        if (!animator.fillColorKeyframes.empty())
          frameSample_.fillColor = SampleColorKeyframes(
              animator.fillColorKeyframes, static_cast<float>(layerProgress));
      }
    } catch (...) {
      // A failed resize leaves that cache empty; sampling computes its values
      // directly. Previously completed caches remain valid.
    }
  }
}

TextUnitAnimationSample TextAnimatorSampler::Sample(
    const std::size_t unitIndex) noexcept {
  return SampleTextAnimatorImpl(animator_, unitIndex, unitCount_, layerProgress_,
                                randomOrders_.empty() ? nullptr
                                                      : randomOrders_.data(),
                                &frameSample_);
}

TextUnitAnimationSample SampleTextAnimator(const TextAnimatorSpec &animator,
                                           const std::size_t unitIndex,
                                           const std::size_t unitCount,
                                           const double layerProgress) noexcept {
  return SampleTextAnimatorImpl(animator, unitIndex, unitCount, layerProgress);
}

std::vector<ResolvedTextRenderGroupRange>
ResolveTextRenderGroupTopology(const TextRenderGroupSpec &spec,
                               const TextRenderGroupUnitTopology &topology) {
  std::vector<ResolvedTextRenderGroupRange> result;
  const auto clipEndpoint = [&](const std::int64_t value) noexcept {
    if (value <= 0)
      return std::size_t{0};
    const auto unsignedValue = static_cast<std::uint64_t>(value);
    if (unsignedValue >= static_cast<std::uint64_t>(topology.letterCount))
      return topology.letterCount;
    return static_cast<std::size_t>(unsignedValue);
  };
  const auto appendResolvedRange =
      [&](const std::size_t start, const std::size_t end,
          const std::optional<TextRenderGroupTimeRange> localTime,
          const float intensity) {
        if (start >= end)
          return;
        // A square selector is a hard membership test.  Zero/negative-weight
        // slices are outside the selected set and therefore must not allocate
        // or execute a RenderGroup; a positive authored intensity remains the
        // independent post-effect mix strength.
        if (spec.shape == TextSelectorShape::Square &&
            (!std::isfinite(intensity) || intensity <= 0.0F)) {
          return;
        }
        result.push_back({start, end, localTime, intensity});
      };
  const auto appendTopologyRange = [&](const TextRenderGroupIndexRange &range) {
    const auto start = clipEndpoint(range.startIndex);
    const auto end = clipEndpoint(range.endIndex);
    appendResolvedRange(start, end, std::nullopt, 1.0F);
  };

  switch (spec.mode) {
  case TextRenderGroupMode::Page:
    // The reference topology constructs one Page group even for empty text.
    if (topology.letterCount == 0U)
      result.push_back({0U, 0U, std::nullopt, 1.0F});
    else
      appendResolvedRange(0U, topology.letterCount, std::nullopt, 1.0F);
    break;
  case TextRenderGroupMode::PerLetter:
    result.reserve(topology.letterCount);
    for (std::size_t index = 0; index < topology.letterCount; ++index)
      appendResolvedRange(index, index + 1U, std::nullopt, 1.0F);
    break;
  case TextRenderGroupMode::PerLine:
    result.reserve(topology.lineRanges.size());
    for (const auto &range : topology.lineRanges)
      appendTopologyRange(range);
    break;
  case TextRenderGroupMode::PerWord:
    result.reserve(topology.wordRanges.size());
    for (const auto &range : topology.wordRanges)
      appendTopologyRange(range);
    break;
  case TextRenderGroupMode::Custom:
    result.reserve(spec.customRanges.size());
    for (const auto &range : spec.customRanges) {
      const auto start = clipEndpoint(range.startIndex);
      const auto end = clipEndpoint(range.endIndex);
      appendResolvedRange(start, end, range.localTime, range.intensity);
    }
    break;
  }
  return result;
}

ResolvedTextRenderGroupFrame::ResolvedTextRenderGroupFrame(
    const ResolvedTextRenderGroupRange &resolvedRange,
    const TextRenderGroupContinuousRect &continuousSourceRect,
    const double resolvedExpandRatioX, const double resolvedExpandRatioY,
    const TextRenderGroupContinuousRect &continuousExpandedRect,
    const TextRenderGroupRasterSize resolvedNaturalRasterSize,
    const TextRenderGroupRasterSize resolvedRasterSize,
    const double resolvedQuantScaleX, const double resolvedQuantScaleY,
    const double resolvedRasterScaleX,
    const double resolvedRasterScaleY) noexcept
    : range(resolvedRange), sourceRect(continuousSourceRect),
      expandRatioX(resolvedExpandRatioX), expandRatioY(resolvedExpandRatioY),
      expandedRect(continuousExpandedRect),
      naturalRasterSize(resolvedNaturalRasterSize),
      rasterSize(resolvedRasterSize), quantScaleX(resolvedQuantScaleX),
      quantScaleY(resolvedQuantScaleY), rasterScaleX(resolvedRasterScaleX),
      rasterScaleY(resolvedRasterScaleY) {}

std::optional<ResolvedTextRenderGroupFrame>
ResolveTextRenderGroupFrame(const ResolvedTextRenderGroupRange &range,
                            const TextRenderGroupContinuousRect &sourceRect,
                            const double expandRatioX,
                            const double expandRatioY) noexcept {
  constexpr double kMaximumExpandRatio = 64.0;
  const bool validRange =
      range.startIndex < range.endIndex && std::isfinite(range.intensity) &&
      std::fabs(range.intensity) <= 65'536.0F &&
      (!range.localTime ||
       range.localTime->endTimeUs > range.localTime->startTimeUs);
  const bool validSource =
      std::isfinite(sourceRect.originX) && std::isfinite(sourceRect.originY) &&
      std::isfinite(sourceRect.extentWidth) &&
      std::isfinite(sourceRect.extentHeight) && sourceRect.extentWidth > 0.0 &&
      sourceRect.extentHeight > 0.0;
  const bool validRatios =
      std::isfinite(expandRatioX) && std::isfinite(expandRatioY) &&
      expandRatioX > 0.0 && expandRatioX <= kMaximumExpandRatio &&
      expandRatioY > 0.0 && expandRatioY <= kMaximumExpandRatio;
  if (!validRange || !validSource || !validRatios)
    return std::nullopt;

  const double expandedWidth = sourceRect.extentWidth * expandRatioX;
  const double expandedHeight = sourceRect.extentHeight * expandRatioY;
  const TextRenderGroupContinuousRect expandedRect{
      sourceRect.originX + sourceRect.extentWidth * 0.5 - expandedWidth * 0.5,
      sourceRect.originY + sourceRect.extentHeight * 0.5 - expandedHeight * 0.5,
      expandedWidth,
      expandedHeight,
  };
  if (!std::isfinite(expandedRect.originX) ||
      !std::isfinite(expandedRect.originY) ||
      !std::isfinite(expandedRect.extentWidth) ||
      !std::isfinite(expandedRect.extentHeight) ||
      expandedRect.extentWidth <= 0.0 || expandedRect.extentHeight <= 0.0) {
    return std::nullopt;
  }

  const auto naturalDimension =
      [](const double extent) -> std::optional<std::uint32_t> {
    const double rounded = std::ceil(extent);
    if (!std::isfinite(rounded) || rounded < 1.0 ||
        rounded >
            static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
      return std::nullopt;
    }
    return static_cast<std::uint32_t>(rounded);
  };
  const auto naturalWidth = naturalDimension(expandedRect.extentWidth);
  const auto naturalHeight = naturalDimension(expandedRect.extentHeight);
  if (!naturalWidth || !naturalHeight)
    return std::nullopt;

  const TextRenderGroupRasterSize naturalRasterSize{*naturalWidth,
                                                    *naturalHeight};
  TextRenderGroupRasterSize rasterSize = naturalRasterSize;
  const auto maximumNaturalDimension =
      std::max(naturalRasterSize.width, naturalRasterSize.height);
  if (maximumNaturalDimension > kTextRenderGroupMaximumRasterDimension) {
    const auto scaledShortDimension = [](const std::uint32_t shorter,
                                         const std::uint32_t longer) {
      const float scaled =
          static_cast<float>(shorter) / static_cast<float>(longer) *
          static_cast<float>(kTextRenderGroupMaximumRasterDimension);
      return std::max(1U, static_cast<std::uint32_t>(scaled));
    };
    if (naturalRasterSize.width >= naturalRasterSize.height) {
      rasterSize.width = kTextRenderGroupMaximumRasterDimension;
      rasterSize.height = scaledShortDimension(naturalRasterSize.height,
                                               naturalRasterSize.width);
    } else {
      rasterSize.width = scaledShortDimension(naturalRasterSize.width,
                                              naturalRasterSize.height);
      rasterSize.height = kTextRenderGroupMaximumRasterDimension;
    }
  }

  const double quantScaleX =
      static_cast<double>(rasterSize.width) / expandedRect.extentWidth;
  const double quantScaleY =
      static_cast<double>(rasterSize.height) / expandedRect.extentHeight;
  const double rasterScaleX = static_cast<double>(rasterSize.width) /
                              static_cast<double>(naturalRasterSize.width);
  const double rasterScaleY = static_cast<double>(rasterSize.height) /
                              static_cast<double>(naturalRasterSize.height);
  if (!std::isfinite(quantScaleX) || !std::isfinite(quantScaleY) ||
      !std::isfinite(rasterScaleX) || !std::isfinite(rasterScaleY) ||
      quantScaleX <= 0.0 || quantScaleY <= 0.0 || rasterScaleX <= 0.0 ||
      rasterScaleY <= 0.0) {
    return std::nullopt;
  }

  return ResolvedTextRenderGroupFrame(
      range, sourceRect, expandRatioX, expandRatioY, expandedRect,
      naturalRasterSize, rasterSize, quantScaleX, quantScaleY, rasterScaleX,
      rasterScaleY);
}

TextRenderGroupClockSample
SampleTextRenderGroupClock(const ResolvedTextRenderGroupRange &range,
                           const std::int64_t timeUs,
                           const std::int64_t wholeTimeUs) noexcept {
  TextRenderGroupClockSample result;
  result.hasLocalTime = range.localTime.has_value();
  if (wholeTimeUs < 0)
    return result;

  if (!range.localTime) {
    result.valid = true;
    result.phase = timeUs < 0             ? TextRenderGroupClockPhase::Before
                   : timeUs > wholeTimeUs ? TextRenderGroupClockPhase::After
                                          : TextRenderGroupClockPhase::Active;
    result.effectTimeUs = static_cast<double>(timeUs);
    if (wholeTimeUs > 0) {
      result.progress =
          static_cast<double>(timeUs) / static_cast<double>(wholeTimeUs);
      result.heldProgress = std::clamp(result.progress, 0.0, 1.0);
      result.heldEffectTimeUs =
          result.heldProgress * static_cast<double>(wholeTimeUs);
    }
    return result;
  }

  const auto &localTime = *range.localTime;
  if (localTime.endTimeUs <= localTime.startTimeUs)
    return result;
  result.phase =
      timeUs < localTime.startTimeUs ? TextRenderGroupClockPhase::Before
      : timeUs > localTime.endTimeUs ? TextRenderGroupClockPhase::After
                                     : TextRenderGroupClockPhase::Active;
  const long double elapsed = static_cast<long double>(timeUs) -
                              static_cast<long double>(localTime.startTimeUs);
  const long double duration = static_cast<long double>(localTime.endTimeUs) -
                               static_cast<long double>(localTime.startTimeUs);
  result.progress = static_cast<double>(elapsed / duration);
  result.heldProgress = std::clamp(result.progress, 0.0, 1.0);
  result.effectTimeUs = result.progress * static_cast<double>(wholeTimeUs);
  result.heldEffectTimeUs =
      result.heldProgress * static_cast<double>(wholeTimeUs);
  result.valid = std::isfinite(result.progress) &&
                 std::isfinite(result.effectTimeUs) &&
                 std::isfinite(result.heldEffectTimeUs);
  if (!result.valid)
    result.phase = TextRenderGroupClockPhase::Invalid;
  return result;
}

std::vector<TextTimedPropertySample>
SampleTextTimedPropertyPatches(const TextAnimationStack &stack,
                               const std::int64_t clipLocalTimeUs,
                               const std::int64_t clipDurationUs,
                               const std::vector<TimedTextSpan> &timedSpans) {
  std::vector<TextTimedPropertySample> result;
  if (clipDurationUs <= 0 || timedSpans.empty())
    return result;
  const auto time =
      std::clamp(clipLocalTimeUs, std::int64_t{0}, clipDurationUs);
  for (const auto &layer : stack.layers) {
    if (!layer.enabled ||
        layer.timeDriver.kind != TextAnimationTimeDriverKind::TimedRanges ||
        !layer.timeDriver.timedRanges ||
        !layer.timeDriver.timedRanges->activePatch ||
        layer.timeDriver.timedRanges->activePatch->assignments.empty()) {
      continue;
    }
    const auto &driver = *layer.timeDriver.timedRanges;
    for (const auto &span : timedSpans) {
      if (!driver.spanIds.empty() &&
          std::find(driver.spanIds.begin(), driver.spanIds.end(),
                    span.spanId) == driver.spanIds.end()) {
        continue;
      }
      if (time < span.startOffsetUs || time >= span.endOffsetUs)
        continue;
      TextTimedDriverMode mode = driver.mode;
      if (mode == TextTimedDriverMode::SpanProgress) {
        mode = span.progressMode == TimedTextProgressMode::GraphemeSweep
                   ? TextTimedDriverMode::GraphemeSweep
                   : TextTimedDriverMode::SpanStep;
      }
      float progress = 1.0F;
      if (mode == TextTimedDriverMode::GraphemeSweep) {
        const auto transitionEnd =
            span.transitionEndOffsetUs > span.startOffsetUs
                ? std::min(span.transitionEndOffsetUs, span.endOffsetUs)
                : span.endOffsetUs;
        const auto transitionDuration =
            std::max<std::int64_t>(1, transitionEnd - span.startOffsetUs);
        progress = std::clamp(static_cast<float>(time - span.startOffsetUs) /
                                  static_cast<float>(transitionDuration),
                              0.0F, 1.0F);
      }
      result.push_back({
          layer.layerId,
          span.spanId,
          span.paragraphId,
          span.runId,
          span.utf8Begin,
          span.utf8End,
          mode,
          progress,
          span.opacity,
          *driver.activePatch,
      });
    }
  }
  return result;
}

TextDecorationAnimationSample SampleTextDecorationAnimation(
    const TextDecorationAnimationSpec &decoration, const float layerProgress) noexcept {
  const float progress = std::clamp(layerProgress, 0.0F, 1.0F);
  TextDecorationAnimationSample result;
  result.anchor = SampleStepKeyframes(decoration.anchorKeyframes, progress,
                                      decoration.anchor);
  result.fit =
      SampleStepKeyframes(decoration.fitKeyframes, progress, decoration.fit);
  result.pivotX = std::clamp(SampleTextKeyframes(decoration.pivotXKeyframes,
                                                 progress, decoration.pivotX),
                             -64.0F, 64.0F);
  result.pivotY = std::clamp(SampleTextKeyframes(decoration.pivotYKeyframes,
                                                 progress, decoration.pivotY),
                             -64.0F, 64.0F);
  result.offsetX = std::clamp(SampleTextKeyframes(decoration.offsetXKeyframes,
                                                  progress, decoration.offsetX),
                              -65'536.0F, 65'536.0F);
  result.offsetY = std::clamp(SampleTextKeyframes(decoration.offsetYKeyframes,
                                                  progress, decoration.offsetY),
                              -65'536.0F, 65'536.0F);
  result.relativeOffsetX =
      std::clamp(SampleTextKeyframes(decoration.relativeOffsetXKeyframes,
                                     progress, decoration.relativeOffsetX),
                 -64.0F, 64.0F);
  result.relativeOffsetY =
      std::clamp(SampleTextKeyframes(decoration.relativeOffsetYKeyframes,
                                     progress, decoration.relativeOffsetY),
                 -64.0F, 64.0F);
  result.scaleX = std::clamp(SampleTextKeyframes(decoration.scaleXKeyframes,
                                                 progress, decoration.scaleX),
                             0.01F, 64.0F);
  result.scaleY = std::clamp(SampleTextKeyframes(decoration.scaleYKeyframes,
                                                 progress, decoration.scaleY),
                             0.01F, 64.0F);
  result.rotationXDegrees =
      std::clamp(SampleTextKeyframes(decoration.rotationXKeyframes, progress,
                                     decoration.rotationXDegrees),
                 -3'600.0F, 3'600.0F);
  result.rotationYDegrees =
      std::clamp(SampleTextKeyframes(decoration.rotationYKeyframes, progress,
                                     decoration.rotationYDegrees),
                 -3'600.0F, 3'600.0F);
  result.rotationDegrees =
      std::clamp(SampleTextKeyframes(decoration.rotationKeyframes, progress,
                                     decoration.rotationDegrees),
                 -3'600.0F, 3'600.0F);
  result.opacity = std::clamp(SampleTextKeyframes(decoration.opacityKeyframes,
                                                  progress, decoration.opacity),
                              0.0F, 1.0F);
  result.assetProgress = std::clamp(
      SampleTextKeyframes(decoration.assetProgressKeyframes, progress, progress),
      0.0F, 1.0F);
  return result;
}

std::optional<TextPostEffectKind>
ParseTextPostEffectKind(const std::string_view nodeKind) noexcept {
  if (nodeKind == "turbulence_displacement" ||
      nodeKind == "turbulencedisplacement")
    return TextPostEffectKind::TurbulenceDisplacement;
  if (nodeKind == "directional_blur" || nodeKind == "directionalblurs")
    return TextPostEffectKind::DirectionalBlur;
  if (nodeKind == "god_ray" || nodeKind == "godray")
    return TextPostEffectKind::GodRay;
  if (nodeKind == "linear_wipe" || nodeKind == "wipe_mask" ||
      nodeKind == "linearwipe")
    return TextPostEffectKind::LinearWipe;
  if (nodeKind == "dust")
    return TextPostEffectKind::Dust;
  if (nodeKind == "deep_glow" || nodeKind == "deepglowsimple")
    return TextPostEffectKind::DeepGlow;
  if (nodeKind == "s_glow" || nodeKind == "sglow")
    return TextPostEffectKind::SGlow;
  if (nodeKind == "soft_glow" || nodeKind == "softglow" ||
      nodeKind == "bloom" || nodeKind == "glow")
    return TextPostEffectKind::SoftGlow;
  if (nodeKind == "radial_blur" || nodeKind == "radialblur")
    return TextPostEffectKind::RadialBlur;
  if (nodeKind == "shake" || nodeKind == "transform_jitter")
    return TextPostEffectKind::Shake;
  if (nodeKind == "trail")
    return TextPostEffectKind::Trail;
  if (nodeKind == "wave_warp" || nodeKind == "wavewarp")
    return TextPostEffectKind::WaveWarp;
  if (nodeKind == "distort_chroma" || nodeKind == "distortchroma")
    return TextPostEffectKind::DistortChroma;
  if (nodeKind == "gaussian_blur" || nodeKind == "gaussianblur" ||
      nodeKind == "blur")
    return TextPostEffectKind::GaussianBlur;
  if (nodeKind == "chromatic_aberration" ||
      nodeKind == "chromaticaberration")
    return TextPostEffectKind::ChromaticAberration;
  if (nodeKind == "alpha_outline" || nodeKind == "alphaoutline")
    return TextPostEffectKind::AlphaOutline;
  if (nodeKind == "radiance_glow" || nodeKind == "radianceglow")
    return TextPostEffectKind::RadianceGlow;
  if (nodeKind == "multi_shadow_echo" || nodeKind == "multi_shadow" ||
      nodeKind == "mutishadow")
    return TextPostEffectKind::MultiShadow;
  if (nodeKind == "simple_choker" || nodeKind == "simplechoker")
    return TextPostEffectKind::SimpleChoker;
  if (nodeKind == "cc_lens" || nodeKind == "cclens")
    return TextPostEffectKind::CCLens;
  if (nodeKind == "optics_compensation" ||
      nodeKind == "opticscompensation")
    return TextPostEffectKind::OpticsCompensation;
  if (nodeKind == "projection")
    return TextPostEffectKind::Projection;
  if (nodeKind == "dynamic_signal_glitch" ||
      nodeKind == "plugin-dynamicsignalglitch")
    return TextPostEffectKind::DynamicSignalGlitch;
  if (nodeKind == "electric_pulse_motion_blur" ||
      nodeKind == "plugin-electricpulsemotionblur")
    return TextPostEffectKind::ElectricPulseMotionBlur;
  if (nodeKind == "morphological_outline" ||
      nodeKind == "plugin-template-value")
    return TextPostEffectKind::MorphologicalOutline;
  if (nodeKind == "alternating_segment_mask" ||
      nodeKind == "plugin-template-4631")
    return TextPostEffectKind::AlternatingSegmentMask;
  if (nodeKind == "center_split_displacement" ||
      nodeKind == "plugin-template-ea51")
    return TextPostEffectKind::CenterSplitDisplacement;
  if (nodeKind == "strand_knife_wipe" || nodeKind == "cut_and_drop" ||
      nodeKind == "plugin-template-d3e1")
    return TextPostEffectKind::CutAndDrop;
  if (nodeKind == "block_glitch_dot_matrix" ||
      nodeKind == "plugin-template-8d72")
    return TextPostEffectKind::BlockGlitchDotMatrix;
  if (nodeKind == "procedural_flame_outline" ||
      nodeKind == "plugin-template-e4ef")
    return TextPostEffectKind::ProceduralFlameOutline;
  if (nodeKind == "cylindrical_scroll" ||
      nodeKind == "plugin-template-f5c3")
    return TextPostEffectKind::CylindricalScroll;
  if (nodeKind == "split_squeeze_wave" ||
      nodeKind == "plugin-template-4d45")
    return TextPostEffectKind::SplitSqueezeWave;
  if (nodeKind == "perspective_echo_trail" ||
      nodeKind == "plugin-template-096f")
    return TextPostEffectKind::PerspectiveEchoTrail;
  if (nodeKind == "pulse_envelope" || nodeKind == "pulseenvelope")
    return TextPostEffectKind::PulseEnvelope;
  return std::nullopt;
}

TextAnimationEnvelope
ResolveTextAnimationEnvelope(const TextAnimationStack &animations,
                             const float baseWidth,
                             const float baseHeight) noexcept {
  TextAnimationEnvelope result;
  if (!std::isfinite(baseWidth) || !std::isfinite(baseHeight) ||
      baseWidth <= 0.0F || baseHeight <= 0.0F) {
    return result;
  }
  float maximumPositionX = 0.0F;
  float maximumPositionY = 0.0F;
  float maximumTracking = 0.0F;
  float maximumBlur = 0.0F;
  float maximumScaleX = 1.0F;
  float maximumScaleY = 1.0F;
  float maximumPositionZ = 0.0F;
  bool rotated = false;
  bool perspective = false;
  for (const auto &layer : animations.layers) {
    if (!layer.enabled)
      continue;
    for (const auto &animator : layer.animators) {
      float animatorPositionX = 0.0F;
      float animatorPositionY = 0.0F;
      float animatorDistanceFromCenter = 0.0F;
      perspective = perspective || animator.projection.kind ==
                                       TextProjectionKind::Perspective3D;
      for (const auto &track : animator.tracks) {
        for (const auto &keyframe : track.keyframes) {
          const float magnitude = std::fabs(keyframe.value);
          switch (track.property) {
          case TextAnimatedProperty::PositionX:
            animatorPositionX = std::max(animatorPositionX, magnitude);
            break;
          case TextAnimatedProperty::PositionY:
            animatorPositionY = std::max(animatorPositionY, magnitude);
            break;
          case TextAnimatedProperty::PositionZ:
            maximumPositionZ = std::max(maximumPositionZ, magnitude);
            break;
          case TextAnimatedProperty::ScaleX:
            maximumScaleX = std::max(maximumScaleX, magnitude);
            break;
          case TextAnimatedProperty::ScaleY:
            maximumScaleY = std::max(maximumScaleY, magnitude);
            break;
          case TextAnimatedProperty::RotationX:
          case TextAnimatedProperty::RotationY:
          case TextAnimatedProperty::RotationZ:
          case TextAnimatedProperty::ShearX:
          case TextAnimatedProperty::ShearY:
            rotated = rotated || magnitude > 0.0001F;
            break;
          case TextAnimatedProperty::Tracking:
            maximumTracking = std::max(maximumTracking, magnitude);
            break;
          case TextAnimatedProperty::BlurRadius:
            maximumBlur = std::max(maximumBlur, magnitude);
            break;
          case TextAnimatedProperty::DistanceFromCenter:
            animatorDistanceFromCenter =
                std::max(animatorDistanceFromCenter, magnitude);
            break;
          case TextAnimatedProperty::Opacity:
            break;
          }
        }
      }
      switch (animator.positionMode) {
      case TextPositionMode::AbsolutePixels:
        maximumPositionX = std::max(maximumPositionX, animatorPositionX);
        maximumPositionY = std::max(maximumPositionY, animatorPositionY);
        break;
      case TextPositionMode::TextBoundsOffset:
        // TextBoundsOffset stores normalized half-extents. Recording it as
        // authored pixels underestimates even small values such as 0.2 by two
        // orders of magnitude on a studio canvas and clips transformed unit
        // slices at the recording edge.
        maximumPositionX = std::max(
            maximumPositionX, animatorPositionX * baseWidth * 0.5F);
        maximumPositionY = std::max(
            maximumPositionY, animatorPositionY * baseHeight * 0.5F);
        break;
      case TextPositionMode::DistanceFromCenter:
      case TextPositionMode::SpaceX:
      case TextPositionMode::SpaceY: {
        // The distance driver moves from a unit's immutable position toward a
        // normalized scope center. One full scope plus the authored center
        // offset is a conservative closed bound for every unit in the scope.
        const float distanceX =
            animatorDistanceFromCenter *
            (baseWidth + animatorPositionX * baseWidth * 0.5F);
        const float distanceY =
            animatorDistanceFromCenter *
            (baseHeight + animatorPositionY * baseHeight * 0.5F);
        if (animator.positionMode != TextPositionMode::SpaceY)
          maximumPositionX = std::max(maximumPositionX, distanceX);
        if (animator.positionMode != TextPositionMode::SpaceX)
          maximumPositionY = std::max(maximumPositionY, distanceY);
        break;
      }
      }
    }
    for (const auto &effect : layer.postEffects) {
      result.effectPaddingPx += std::max(0.0F, effect.paddingPx);
    }
  }
  float perspectiveScale = 1.0F;
  if (perspective && maximumPositionZ > 0.0F) {
    // Admission bounds Z and FOV. Four is the maximum perspective magnifier
    // used by the renderer before the near plane rejects the sample.
    perspectiveScale = 4.0F;
  }
  maximumScaleX *= perspectiveScale;
  maximumScaleY *= perspectiveScale;
  float horizontal = maximumPositionX + maximumTracking + maximumBlur +
                     result.effectPaddingPx +
                     std::max(0.0F, maximumScaleX - 1.0F) * baseWidth * 0.5F;
  float vertical = maximumPositionY + maximumBlur + result.effectPaddingPx +
                   std::max(0.0F, maximumScaleY - 1.0F) * baseHeight * 0.5F;
  if (rotated) {
    const float radius = std::hypot(baseWidth, baseHeight) * 0.5F;
    horizontal += std::max(0.0F, radius - baseWidth * 0.5F);
    vertical += std::max(0.0F, radius - baseHeight * 0.5F);
  }
  result.left = result.right = horizontal;
  result.top = result.bottom = vertical;
  return result;
}

TextLayerAnimationSample SampleTextLayerAnimation(
    const TextAnimationStack &animations, const std::int64_t clipLocalTimeUs,
    const std::int64_t clipDurationUs,
    const std::vector<TimedTextSpan> &timedSpans) noexcept {
  TextLayerAnimationSample result;
  const auto duration = std::max<std::int64_t>(0, clipDurationUs);
  if (duration == 0)
    return result;
  const auto resolved = ResolveTextAnimationDurations(animations, duration);
  for (const auto &layer : animations.layers) {
    const auto sampled = SampleLayer<float>(layer, clipLocalTimeUs, duration,
                                            resolved, timedSpans);
    if (!sampled.active)
      continue;
    result.active = true;
    if (layer.layerTrack) {
      const auto layerSample = SampleClip(*layer.layerTrack, sampled.progress);
      result.positionX += layerSample.positionX;
      result.positionY += layerSample.positionY;
      result.scaleX *= layerSample.scaleX;
      result.scaleY *= layerSample.scaleY;
      result.rotationDegrees += layerSample.rotationDegrees;
      result.opacity *= layerSample.opacity;
      result.phase = layerSample.phase;
    }
  }
  result.opacity = std::clamp(result.opacity, 0.0F, 1.0F);
  return result;
}

TextLayerAnimationSample SampleTextLayerAnimationClips(
    const std::vector<TextLayerAnimationClip> &clips,
    const std::int64_t clipLocalTimeUs,
    const std::int64_t clipDurationUs) noexcept {
  TextLayerAnimationSample result;
  const auto duration = std::max<std::int64_t>(0, clipDurationUs);
  if (duration == 0 || clips.empty())
    return result;

  std::int64_t enterDurationUs = 0;
  std::int64_t exitDurationUs = 0;
  for (const auto &clip : clips) {
    if (clip.phase == TextLayerAnimationPhase::Enter)
      enterDurationUs = std::max(enterDurationUs, clip.durationUs);
    else if (clip.phase == TextLayerAnimationPhase::Exit)
      exitDurationUs = std::max(exitDurationUs, clip.durationUs);
  }
  const auto time = std::clamp(clipLocalTimeUs, std::int64_t{0}, duration);
  const auto loopStart = std::min(duration, std::max<std::int64_t>(0,
                                                                  enterDurationUs));
  const auto loopEnd = std::max(
      loopStart, duration - std::min(duration, std::max<std::int64_t>(
                                                0, exitDurationUs)));

  const auto combine = [&](const TextLayerAnimationSample &sample) {
    result.active = true;
    result.positionX += sample.positionX;
    result.positionY += sample.positionY;
    result.scaleX *= sample.scaleX;
    result.scaleY *= sample.scaleY;
    result.rotationDegrees += sample.rotationDegrees;
    result.opacity *= sample.opacity;
    result.phase = sample.phase;
  };
  for (const auto &clip : clips) {
    if (clip.durationUs <= 0)
      continue;
    std::optional<float> progress;
    switch (clip.phase) {
    case TextLayerAnimationPhase::Enter:
      if (time <= clip.durationUs) {
        progress = static_cast<float>(std::clamp(
            static_cast<double>(time) / static_cast<double>(clip.durationUs),
            0.0, 1.0));
      }
      break;
    case TextLayerAnimationPhase::Loop:
      if (time >= loopStart && time < loopEnd) {
        const auto elapsed = time - loopStart;
        progress = static_cast<float>(
            static_cast<double>(elapsed % clip.durationUs) /
            static_cast<double>(clip.durationUs));
      }
      break;
    case TextLayerAnimationPhase::Exit: {
      const auto start = std::max<std::int64_t>(0, duration - clip.durationUs);
      if (time >= start) {
        progress = static_cast<float>(std::clamp(
            static_cast<double>(time - start) /
                static_cast<double>(clip.durationUs),
            0.0, 1.0));
      }
      break;
    }
    case TextLayerAnimationPhase::Caption:
      progress = static_cast<float>(std::clamp(
          static_cast<double>(time) / static_cast<double>(clip.durationUs),
          0.0, 1.0));
      break;
    }
    if (progress)
      combine(SampleClip(clip, *progress));
  }
  result.opacity = std::clamp(result.opacity, 0.0F, 1.0F);
  return result;
}

void UpsertTextPhaseAnimation(TextAnimationStack &stack,
                              const TextLayerAnimationPhase phase,
                              std::optional<TextLayerAnimationClip> clip) {
  const auto driver = DriverForPhase(phase);
  stack.layers.erase(
      std::remove_if(stack.layers.begin(), stack.layers.end(),
                     [&](const TextAnimationLayerSpec &candidate) {
                       return candidate.timeDriver.kind == driver &&
                              candidate.layerTrack.has_value();
                     }),
      stack.layers.end());
  if (!clip) {
    return;
  }
  TextAnimationLayerSpec layer;
  layer.layerId = clip->clipId;
  layer.timeDriver.kind = driver;
  layer.timeDriver.durationUs = clip->durationUs;
  layer.timeDriver.playback =
      phase == TextLayerAnimationPhase::Loop
          ? TextAnimationPlaybackMode::Loop
      : phase == TextLayerAnimationPhase::Caption
          ? TextAnimationPlaybackMode::Hold
          : TextAnimationPlaybackMode::Once;
  layer.layerTrack = std::move(clip);
  stack.layers.push_back(std::move(layer));
}

void ReplaceTextRevealAnimations(TextAnimationStack &stack,
                                 std::vector<RevealAnimation> reveals) {
  stack.layers.erase(
      std::remove_if(stack.layers.begin(), stack.layers.end(),
                     [](const TextAnimationLayerSpec &layer) {
                       return std::any_of(
                           layer.animators.begin(), layer.animators.end(),
                           [](const TextAnimatorSpec &animator) {
                             return animator.presentation !=
                                    TextUnitPresentation::Transform;
                           });
                     }),
      stack.layers.end());
  for (const auto &reveal : reveals) {
    TextAnimationLayerSpec layer;
    layer.layerId = reveal.trackId;
    layer.timeDriver.kind = TextAnimationTimeDriverKind::ClipLocal;
    layer.timeDriver.startOffsetUs = reveal.startOffsetUs;
    layer.timeDriver.durationUs = reveal.durationUs;
    layer.timeDriver.playback = TextAnimationPlaybackMode::Hold;
    TextAnimatorSpec animator;
    animator.animatorId = reveal.trackId + "-units";
    animator.paragraphIds = reveal.paragraphIds;
    animator.runIds = reveal.runIds;
    animator.presentation =
        reveal.presentation == RevealPresentation::ActiveUnitHighlight
            ? TextUnitPresentation::ActiveFill
            : TextUnitPresentation::Reveal;
    animator.fadeFraction = reveal.fadeFraction;
    animator.activeColor = reveal.activeColor;
    animator.cursor = reveal.cursor;
    TextUnitSelector selector;
    selector.basedOn = reveal.unit == RevealUnit::Word ? TextUnitBasis::Word
                       : reveal.unit == RevealUnit::Line
                           ? TextUnitBasis::Line
                           : TextUnitBasis::Grapheme;
    selector.order = reveal.direction == RevealDirection::Reverse
                         ? TextUnitOrder::Backward
                         : TextUnitOrder::Forward;
    animator.selectors.push_back(std::move(selector));
    layer.animators.push_back(std::move(animator));
    stack.layers.push_back(std::move(layer));
  }
}

bool ValidateTextLayerAnimationClip(const TextLayerAnimationClip &clip,
                                    std::string *error) noexcept {
  const auto fail = [&](const std::string_view message) {
    if (error != nullptr)
      *error = std::string(message);
    return false;
  };
  if (clip.clipId.empty() || clip.clipId.size() > 512U ||
      !IsValidUtf8(clip.clipId) || clip.presetId.size() > 512U ||
      !IsValidUtf8(clip.presetId) ||
      clip.phase < TextLayerAnimationPhase::Enter ||
      clip.phase > TextLayerAnimationPhase::Caption || clip.durationUs <= 0 ||
      clip.durationUs > 3'600'000'000LL || clip.tracks.empty() ||
      clip.tracks.size() > 128U) {
    return fail("text layer animation clip identity, phase or budget is invalid");
  }
  bool properties[6] = {false, false, false, false, false, false};
  for (const auto &track : clip.tracks) {
    if (track.property < TextLayerAnimationProperty::Opacity ||
        track.property > TextLayerAnimationProperty::RotationDegrees) {
      return fail("text layer animation property is invalid");
    }
    const auto property = static_cast<std::size_t>(track.property);
    if (properties[property])
      return fail("text layer animation properties must be unique");
    properties[property] = true;
    if (track.keyframes.empty() || track.keyframes.size() > 64U)
      return fail("text layer animation keyframe budget is invalid");
    float previous = -1.0F;
    for (const auto &keyframe : track.keyframes) {
      if (!std::isfinite(keyframe.offset) || keyframe.offset < 0.0F ||
          keyframe.offset > 1.0F || keyframe.offset < previous ||
          !std::isfinite(keyframe.value) ||
          std::fabs(keyframe.value) > 65'536.0F ||
          keyframe.easing < TextAnimationEasing::Linear ||
          keyframe.easing > TextAnimationEasing::EaseInOut) {
        return fail("text layer animation keyframe is invalid");
      }
      previous = keyframe.offset;
    }
  }
  return true;
}

bool ValidateTextAnimationStack(const TextAnimationStack &stack,
                                const bool hasTimedText,
                                std::string *error) noexcept {
  const auto fail = [&](const std::string_view message) {
    if (error != nullptr)
      *error = std::string(message);
    return false;
  };
  const auto validColor = [](const Color &color) noexcept {
    return std::isfinite(color.red) && color.red >= 0.0F && color.red <= 1.0F &&
           std::isfinite(color.green) && color.green >= 0.0F &&
           color.green <= 1.0F && std::isfinite(color.blue) &&
           color.blue >= 0.0F && color.blue <= 1.0F &&
           std::isfinite(color.alpha) && color.alpha >= 0.0F &&
           color.alpha <= 1.0F;
  };
  const auto validBezierTiming = [](const auto &keyframe) noexcept {
    return std::isfinite(keyframe.bezierTimeIn) &&
           std::isfinite(keyframe.bezierTimeOut) &&
           std::fabs(keyframe.bezierTimeIn) <= 64.0F &&
           std::fabs(keyframe.bezierTimeOut) <= 64.0F;
  };
  const auto validTarget = [](const TextPropertyTarget &target) {
    const auto validIds = [](const std::vector<std::string> &ids) {
      return std::all_of(ids.begin(), ids.end(), [](const std::string &id) {
        return !id.empty() && id.size() <= 512U && IsValidUtf8(id);
      });
    };
    if (!validIds(target.paragraphIds) || !validIds(target.runIds) ||
        (!target.contentSlotId.empty() &&
         (target.contentSlotId.size() > 512U ||
          !IsValidUtf8(target.contentSlotId))) ||
        (!target.layerId.empty() &&
         (target.layerId.size() > 512U || !IsValidUtf8(target.layerId)))) {
      return false;
    }
    switch (target.scope) {
    case TextPropertyScope::Composition:
      return target.contentSlotId.empty() && target.paragraphIds.empty() &&
             target.runIds.empty() && !target.range && target.layerId.empty();
    case TextPropertyScope::ContentSlot:
      return !target.contentSlotId.empty() && target.paragraphIds.empty() &&
             target.runIds.empty() && !target.range && target.layerId.empty();
    case TextPropertyScope::Paragraph:
      return target.contentSlotId.empty() && !target.paragraphIds.empty() &&
             target.runIds.empty() && !target.range && target.layerId.empty();
    case TextPropertyScope::Run:
      return target.contentSlotId.empty() && target.paragraphIds.empty() &&
             !target.runIds.empty() && !target.range && target.layerId.empty();
    case TextPropertyScope::Utf8Range:
      return target.contentSlotId.empty() && target.paragraphIds.empty() &&
             target.runIds.size() == 1U && target.range &&
             target.range->begin < target.range->end && target.layerId.empty();
    case TextPropertyScope::GlyphMaterialLayer:
      return !target.layerId.empty() && !target.range;
    case TextPropertyScope::BackdropLayer:
      return !target.layerId.empty() && target.contentSlotId.empty() &&
             target.paragraphIds.empty() && target.runIds.empty() &&
             !target.range;
    }
    return false;
  };
  const auto validIdentityList = [](const std::vector<std::string> &ids) {
    return std::all_of(ids.begin(), ids.end(), [](const std::string &id) {
      return !id.empty() && id.size() <= 512U && IsValidUtf8(id);
    });
  };
  const auto validExecutionCapability =
      [](const TextEffectExecutionNodeKind kind,
         const TextEffectExecutionCapability capability) {
    switch (kind) {
    case TextEffectExecutionNodeKind::Scene:
      return capability == TextEffectExecutionCapability::ScenePrefab ||
             capability == TextEffectExecutionCapability::SceneEntity ||
             capability == TextEffectExecutionCapability::SceneMesh ||
             capability == TextEffectExecutionCapability::SceneCamera ||
             capability == TextEffectExecutionCapability::SceneProjection ||
             capability == TextEffectExecutionCapability::SceneClone;
    case TextEffectExecutionNodeKind::MaterialPass:
      return capability == TextEffectExecutionCapability::MaterialColorPass ||
             capability == TextEffectExecutionCapability::MaterialDepthPass ||
             capability == TextEffectExecutionCapability::MaterialProgramPass ||
             (capability >=
                  TextEffectExecutionCapability::MaterialAlphaModulate &&
              capability <= TextEffectExecutionCapability::
                                MaterialNoiseThresholdDissolve);
    case TextEffectExecutionNodeKind::PostEffectPass:
      return capability == TextEffectExecutionCapability::None;
    case TextEffectExecutionNodeKind::RenderTarget:
      return capability ==
                 TextEffectExecutionCapability::RenderTargetOffscreen ||
             capability ==
                 TextEffectExecutionCapability::RenderTargetExpanded;
    case TextEffectExecutionNodeKind::History:
      return capability == TextEffectExecutionCapability::HistoryFeedback;
    case TextEffectExecutionNodeKind::MediaInput:
      return capability == TextEffectExecutionCapability::MediaFont ||
             capability == TextEffectExecutionCapability::MediaImage ||
             capability == TextEffectExecutionCapability::MediaImageSequence ||
             capability == TextEffectExecutionCapability::MediaVideo ||
             capability == TextEffectExecutionCapability::MediaMesh ||
             capability == TextEffectExecutionCapability::MediaParticle;
    case TextEffectExecutionNodeKind::State:
      return capability ==
                 TextEffectExecutionCapability::StateDeterministicRandom ||
             capability == TextEffectExecutionCapability::StatePhysics ||
             capability == TextEffectExecutionCapability::StateCollision ||
             capability == TextEffectExecutionCapability::StateParticle;
    case TextEffectExecutionNodeKind::Composite:
      return capability == TextEffectExecutionCapability::CompositeSourceOver;
    case TextEffectExecutionNodeKind::Layout:
      return capability == TextEffectExecutionCapability::LayoutGlyphRun ||
             capability ==
                 TextEffectExecutionCapability::LayoutCaptionModule ||
             capability == TextEffectExecutionCapability::LayoutPagedText ||
             capability == TextEffectExecutionCapability::LayoutTimedLyric;
    case TextEffectExecutionNodeKind::Selector:
      return capability == TextEffectExecutionCapability::SelectorBase ||
             capability == TextEffectExecutionCapability::SelectorTime;
    case TextEffectExecutionNodeKind::Operator:
      return capability >= TextEffectExecutionCapability::OperatorProgram &&
             capability <= TextEffectExecutionCapability::OperatorLifecycle;
    }
    return false;
  };
  std::unordered_set<std::string> executionNodeIds;
  std::unordered_map<std::string, std::string> executionNodeOwners;
  std::size_t executionNodeCount = 0U;
  const auto validPhysicsSpec = [](const TextEffectPhysicsSpec &spec) {
    const std::array<float, 9U> scalars{
        spec.gravityX,
        spec.gravityY,
        spec.angularDamping,
        spec.explosionAccelerationMin,
        spec.explosionAccelerationMax,
        spec.explosionOriginYShiftFactor,
        spec.explosionAngleRangeDegrees,
        spec.initialAngularVelocityScale,
        spec.randomPhase};
    return spec.fixedStepHz > 0U && spec.fixedStepHz <= 240U &&
           std::all_of(scalars.begin(), scalars.end(), [](const float value) {
             return std::isfinite(value);
           }) &&
           std::isfinite(spec.randomScale) && spec.randomScale != 0.0F &&
           std::fabs(spec.gravityX) <= 1.0e6F &&
           std::fabs(spec.gravityY) <= 1.0e6F &&
           spec.angularDamping >= 0.0F && spec.angularDamping <= 1.0F &&
           spec.explosionDelayUs >= 0 &&
           spec.explosionDelayUs <= 60'000'000 &&
           spec.explosionForceDurationUs > 0 &&
           spec.explosionForceDurationUs <= 60'000'000 &&
           spec.explosionRampUpUs > 0 &&
           spec.explosionRampUpUs <= spec.explosionForceDurationUs &&
           spec.explosionAccelerationMin >= 0.0F &&
           spec.explosionAccelerationMax >=
               spec.explosionAccelerationMin &&
           spec.explosionAccelerationMax <= 1.0e6F &&
           std::fabs(spec.explosionOriginYShiftFactor) <= 64.0F &&
           spec.explosionAngleRangeDegrees >= 0.0F &&
           spec.explosionAngleRangeDegrees <= 360.0F &&
           std::fabs(spec.initialAngularVelocityScale) <= 100.0F &&
           std::fabs(spec.randomScale) <= 1.0e9F &&
           spec.angleSeedStride <= 1'000'000U &&
           spec.speedSeedStride <= 1'000'000U &&
           spec.rotationSeedStride <= 1'000'000U;
  };
  const auto validCollisionSpec = [](const TextEffectCollisionSpec &spec) {
    const std::array<float, 13U> scalars{
        spec.fixedBoxWidth,
        spec.fixedBoxHeight,
        spec.boxCenterXScale,
        spec.wallDamping,
        spec.sideWallVelocityScale,
        spec.topWallVelocityScale,
        spec.bottomWallDamping,
        spec.wallTorqueScale,
        spec.collisionBoundaryScale,
        spec.collisionRestitution,
        spec.collisionTorqueScale,
        spec.minimumCellSize,
        spec.cellSizeScale};
    return std::all_of(scalars.begin(), scalars.end(), [](const float value) {
             return std::isfinite(value);
           }) &&
           spec.fixedBoxWidth > 0.0F && spec.fixedBoxWidth <= 65'536.0F &&
           spec.fixedBoxHeight > 0.0F && spec.fixedBoxHeight <= 65'536.0F &&
           std::fabs(spec.boxCenterXScale) <= 64.0F &&
           spec.wallDamping >= 0.0F && spec.wallDamping <= 1.0F &&
           spec.sideWallVelocityScale >= 0.0F &&
           spec.sideWallVelocityScale <= 1.0F &&
           spec.topWallVelocityScale >= 0.0F &&
           spec.topWallVelocityScale <= 1.0F &&
           spec.bottomWallDamping >= 0.0F &&
           spec.bottomWallDamping <= 1.0F &&
           std::fabs(spec.wallTorqueScale) <= 100.0F &&
           spec.collisionBoundaryScale > 0.0F &&
           spec.collisionBoundaryScale <= 8.0F &&
           spec.collisionRestitution >= 0.0F &&
           spec.collisionRestitution <= 1.0F &&
           std::fabs(spec.collisionTorqueScale) <= 100.0F &&
           spec.minimumCellSize > 0.0F &&
           spec.minimumCellSize <= 65'536.0F &&
           spec.cellSizeScale > 0.0F && spec.cellSizeScale <= 64.0F;
  };
  const auto validStaticAffine =
      [](const TextEffectExecutionStaticAffine &affine) {
        const std::array<float, 7U> values{
            affine.translationX, affine.translationY, affine.scaleX,
            affine.scaleY,       affine.rotationDegrees,
            affine.pivotX,       affine.pivotY};
        return std::all_of(values.begin(), values.end(), [](const float value) {
                 return std::isfinite(value);
               }) &&
               std::fabs(affine.translationX) <= 1.0e6F &&
               std::fabs(affine.translationY) <= 1.0e6F &&
               std::fabs(affine.scaleX) <= 1.0e3F &&
               std::fabs(affine.scaleY) <= 1.0e3F &&
               std::fabs(affine.rotationDegrees) <= 1.0e6F &&
               std::fabs(affine.pivotX) <= 1.0e6F &&
               std::fabs(affine.pivotY) <= 1.0e6F;
      };
  const auto validExecutionCamera = [](const TextEffectExecutionCamera &camera) {
    const bool finiteMatrix =
        std::all_of(camera.worldToClip.begin(), camera.worldToClip.end(),
                    [](const float value) { return std::isfinite(value); });
    const bool finiteViewport =
        std::all_of(camera.viewport.begin(), camera.viewport.end(),
                    [](const float value) { return std::isfinite(value); });
    return finiteMatrix && finiteViewport && camera.viewport[0U] >= 0.0F &&
           camera.viewport[1U] >= 0.0F && camera.viewport[2U] > 0.0F &&
           camera.viewport[3U] > 0.0F &&
           camera.viewport[0U] + camera.viewport[2U] <= 1.0F &&
           camera.viewport[1U] + camera.viewport[3U] <= 1.0F;
  };
  std::function<bool(const std::vector<TextEffectExecutionNode> &,
                     std::size_t, std::string_view)>
      validExecutionNodes;
  validExecutionNodes = [&](const std::vector<TextEffectExecutionNode> &nodes,
                            const std::size_t depth,
                            const std::string_view parentOwner) {
    if (depth > 32U)
      return false;
    for (const auto &node : nodes) {
      if (++executionNodeCount > 4'096U || node.nodeId.empty() ||
          node.nodeId.size() > 512U || !IsValidUtf8(node.nodeId) ||
          (node.ownerLayerId.size() > 512U ||
           !IsValidUtf8(node.ownerLayerId)) ||
          (!parentOwner.empty() && node.ownerLayerId != parentOwner) ||
          (!node.ownerLayerId.empty() &&
           std::none_of(stack.layers.begin(), stack.layers.end(),
                        [&](const auto &layer) {
                          return layer.layerId == node.ownerLayerId;
                        })) ||
          !executionNodeIds.emplace(node.nodeId).second ||
          node.kind < TextEffectExecutionNodeKind::Scene ||
          node.kind > TextEffectExecutionNodeKind::Operator ||
          !validExecutionCapability(node.kind, node.capability) ||
          node.inputIds.size() > 256U || node.resourceIds.size() > 256U ||
          node.children.size() > 512U ||
          !validIdentityList(node.inputIds) ||
          !validIdentityList(node.resourceIds)) {
        return false;
      }
      const bool physicsNode =
          node.kind == TextEffectExecutionNodeKind::State &&
          node.capability == TextEffectExecutionCapability::StatePhysics;
      const bool collisionNode =
          node.kind == TextEffectExecutionNodeKind::State &&
          node.capability == TextEffectExecutionCapability::StateCollision;
      const bool deterministicRandomNode =
          node.kind == TextEffectExecutionNodeKind::State &&
          node.capability ==
              TextEffectExecutionCapability::StateDeterministicRandom;
      const bool cameraNode =
          (node.kind == TextEffectExecutionNodeKind::Scene &&
           (node.capability == TextEffectExecutionCapability::SceneCamera ||
            node.capability ==
                TextEffectExecutionCapability::SceneProjection)) ||
          (node.kind == TextEffectExecutionNodeKind::MediaInput &&
           node.capability == TextEffectExecutionCapability::MediaParticle);
      if ((node.physicsSpec &&
           (!physicsNode || !validPhysicsSpec(*node.physicsSpec))) ||
          (node.collisionSpec &&
           (!collisionNode || !validCollisionSpec(*node.collisionSpec))) ||
          (node.randomSeed != 0U && !deterministicRandomNode) ||
          (node.staticAffine &&
           ((node.kind != TextEffectExecutionNodeKind::Scene &&
             node.kind != TextEffectExecutionNodeKind::MaterialPass) ||
            !validStaticAffine(*node.staticAffine))) ||
          (node.camera &&
           (!cameraNode || !validExecutionCamera(*node.camera)))) {
        return false;
      }
      // Requiring an already-authored input makes the stored order itself a
      // deterministic topological order and rejects cycles without rewriting.
      if (std::any_of(node.inputIds.begin(), node.inputIds.end(),
                      [&](const std::string &inputId) {
                        return executionNodeIds.find(inputId) ==
                                   executionNodeIds.end() ||
                               inputId == node.nodeId ||
                               (!node.ownerLayerId.empty() &&
                                executionNodeOwners.count(inputId) != 0U &&
                                !executionNodeOwners.at(inputId).empty() &&
                                executionNodeOwners.at(inputId) !=
                                    node.ownerLayerId);
                      })) {
        return false;
      }
      executionNodeOwners.emplace(node.nodeId, node.ownerLayerId);
      const bool postNode =
          node.kind == TextEffectExecutionNodeKind::PostEffectPass;
      if (postNode != node.postEffectKind.has_value() ||
          (node.postEffectKind &&
           (*node.postEffectKind < TextPostEffectKind::TurbulenceDisplacement ||
            *node.postEffectKind > TextPostEffectKind::PulseEnvelope))) {
        return false;
      }
      if (((node.kind == TextEffectExecutionNodeKind::PostEffectPass ||
            node.kind == TextEffectExecutionNodeKind::Composite) &&
           node.inputIds.empty()) ||
          (node.capability == TextEffectExecutionCapability::
                                  MaterialHsvOriginalOverBlur &&
           node.inputIds.size() != 2U) ||
          (node.capability == TextEffectExecutionCapability::
                                  MaterialNoiseThresholdDissolve &&
           node.inputIds.size() != 2U) ||
          (node.kind == TextEffectExecutionNodeKind::MediaInput &&
           node.resourceIds.empty())) {
        return false;
      }
      if ((node.kind == TextEffectExecutionNodeKind::State &&
           node.stateId.empty()) ||
          (node.kind == TextEffectExecutionNodeKind::History &&
           node.historyId.empty()) ||
          (!node.stateId.empty() &&
           (node.stateId.size() > 512U || !IsValidUtf8(node.stateId))) ||
          (!node.historyId.empty() &&
           (node.historyId.size() > 512U || !IsValidUtf8(node.historyId)))) {
        return false;
      }
      if (node.timeDriver) {
        const auto &driver = *node.timeDriver;
        if (driver.startOffsetUs < 0 || driver.durationUs < 0 ||
            driver.kind < TextAnimationTimeDriverKind::EnterPhase ||
            driver.kind > TextAnimationTimeDriverKind::TimedRanges ||
            driver.playback < TextAnimationPlaybackMode::Once ||
            driver.playback > TextAnimationPlaybackMode::Hold ||
            (driver.kind == TextAnimationTimeDriverKind::TimedRanges &&
             (!driver.timedRanges || !hasTimedText))) {
          return false;
        }
      }
      if (!validExecutionNodes(node.children, depth + 1U,
                               node.ownerLayerId))
        return false;
    }
    return true;
  };
  if (!validExecutionNodes(stack.executionGraph.nodes, 0U, {}))
    return fail("text effect execution graph is invalid");
  if (stack.effectPrograms.size() > 2'048U || stack.layers.size() > 128)
    return fail("text animation stack layer or program count is invalid");
  std::vector<std::string_view> effectProgramIds;
  effectProgramIds.reserve(stack.effectPrograms.size());
  for (const auto &program : stack.effectPrograms) {
    std::string programError;
    if (!ValidateTextEffectProgramIR(program, &programError))
      return fail(programError);
    if (std::find(effectProgramIds.begin(), effectProgramIds.end(),
                  program.programId) != effectProgramIds.end()) {
      return fail("text effect program identities must be unique");
    }
    effectProgramIds.push_back(program.programId);
  }
  std::vector<std::string_view> boundEffectProgramIds;
  boundEffectProgramIds.reserve(stack.effectPrograms.size());
  std::vector<std::string_view> postEffectIds;
  std::vector<std::string_view> identities;
  identities.reserve(stack.layers.size());
  for (const auto &layer : stack.layers) {
    if (layer.layerId.empty() || layer.layerId.size() > 512 ||
        !IsValidUtf8(layer.layerId) ||
        layer.timeDriver.startOffsetUs < 0 || layer.timeDriver.durationUs < 0 ||
        layer.animators.size() > 16 || layer.decorations.size() > 128U ||
        layer.postEffects.size() > 128U ||
        layer.combineMode < TextPropertyCombineMode::Replace ||
        layer.combineMode > TextPropertyCombineMode::ColorMix ||
        layer.target.scope < TextPropertyScope::Composition ||
        layer.target.scope > TextPropertyScope::BackdropLayer ||
        !validTarget(layer.target) ||
        layer.timeDriver.kind < TextAnimationTimeDriverKind::EnterPhase ||
        layer.timeDriver.kind > TextAnimationTimeDriverKind::TimedRanges ||
        layer.timeDriver.playback < TextAnimationPlaybackMode::Once ||
        layer.timeDriver.playback > TextAnimationPlaybackMode::Hold) {
      return fail("text animation layer identity, clock or budget is invalid");
    }
    if (std::find(identities.begin(), identities.end(), layer.layerId) !=
        identities.end())
      return fail("text animation layer identities must be unique");
    identities.push_back(layer.layerId);
    if (layer.layerTrack) {
      std::string clipError;
      if (!ValidateTextLayerAnimationClip(*layer.layerTrack, &clipError))
        return fail(clipError);
      if (layer.layerTrack->durationUs != layer.timeDriver.durationUs ||
          DriverForPhase(layer.layerTrack->phase) != layer.timeDriver.kind) {
        return fail("text layer animation clip clock is inconsistent");
      }
    }
    if (layer.renderGroup) {
      const auto &renderGroup = *layer.renderGroup;
      const bool validMode =
          renderGroup.mode == TextRenderGroupMode::Page ||
          renderGroup.mode == TextRenderGroupMode::PerLetter ||
          renderGroup.mode == TextRenderGroupMode::PerLine ||
          renderGroup.mode == TextRenderGroupMode::PerWord ||
          renderGroup.mode == TextRenderGroupMode::Custom;
      if (!validMode || !std::isfinite(renderGroup.expandRatioX) ||
          !std::isfinite(renderGroup.expandRatioY) ||
          renderGroup.expandRatioX <= 0.0F ||
          renderGroup.expandRatioX > 64.0F ||
          renderGroup.expandRatioY <= 0.0F ||
          renderGroup.expandRatioY > 64.0F ||
          !std::isfinite(renderGroup.offset) ||
          std::fabs(renderGroup.offset) > 64.0F ||
          renderGroup.shape < TextSelectorShape::Linear ||
          renderGroup.shape > TextSelectorShape::Square ||
          renderGroup.customRanges.size() > 16'384U ||
          (renderGroup.duration && renderGroup.duration->endTimeUs <=
                                       renderGroup.duration->startTimeUs)) {
        return fail("text animation render group is invalid");
      }
      for (const auto &range : renderGroup.customRanges) {
        if (!std::isfinite(range.intensity) ||
            std::fabs(range.intensity) > 65'536.0F ||
            (range.localTime &&
             range.localTime->endTimeUs <= range.localTime->startTimeUs)) {
          return fail("text animation render group range is invalid");
        }
      }
    }
    if ((layer.requiresTimedText ||
         layer.timeDriver.kind == TextAnimationTimeDriverKind::TimedRanges) &&
        !hasTimedText) {
      return fail("timed text animation requires canonical timed ranges");
    }
    if (layer.timeDriver.kind == TextAnimationTimeDriverKind::TimedRanges &&
        !layer.timeDriver.timedRanges) {
      return fail("timed text animation driver is incomplete");
    }
    if (layer.timeDriver.timedRanges) {
      const auto &timed = *layer.timeDriver.timedRanges;
      if (timed.mode < TextTimedDriverMode::SpanStep ||
          timed.mode > TextTimedDriverMode::ActiveHold ||
          timed.spanIds.size() > 16'384U) {
        return fail("timed text animation property driver is invalid");
      }
      std::vector<std::string_view> spanIds;
      spanIds.reserve(timed.spanIds.size());
      for (const auto &spanId : timed.spanIds) {
        if (spanId.empty() || spanId.size() > 512U || !IsValidUtf8(spanId) ||
            std::find(spanIds.begin(), spanIds.end(), spanId) !=
                spanIds.end()) {
          return fail("timed text animation span targets are invalid");
        }
        spanIds.push_back(spanId);
      }
      if (timed.activePatch) {
        const auto validation = ValidateTextPropertyPatch(*timed.activePatch);
        if (!validation.valid) {
          return fail("timed text active property patch is invalid");
        }
        for (const auto &assignment : timed.activePatch->assignments) {
          const auto *descriptor =
              DescribeTextProperty(assignment.address.property);
          const auto timedScopeMask =
              TextPropertyScopeBit(TextPropertyScope::Utf8Range) |
              TextPropertyScopeBit(TextPropertyScope::GlyphMaterialLayer);
          if (!descriptor || !descriptor->animatable ||
              (descriptor->legalScopeMask & timedScopeMask) == 0U) {
            return fail(
                "timed text property has no range-addressable scope");
          }
        }
      }
    }
    for (const auto &animator : layer.animators) {
      if (animator.animatorId.empty() || animator.animatorId.size() > 512 ||
          !IsValidUtf8(animator.animatorId) || animator.selectors.size() > 8 ||
          animator.tracks.size() > 16 ||
          !validIdentityList(animator.paragraphIds) ||
          !validIdentityList(animator.runIds) ||
          animator.effectProgramId.size() > 512U ||
          !IsValidUtf8(animator.effectProgramId) ||
          !std::isfinite(animator.fadeFraction) ||
          animator.fadeFraction < 0.0F || animator.fadeFraction > 1.0F ||
          animator.anchorBasis < TextAnimatorAnchorBasis::Letter ||
          animator.anchorBasis > TextAnimatorAnchorBasis::Page ||
          animator.anchorMode < TextAnimatorAnchorMode::Fixed ||
          animator.anchorMode > TextAnimatorAnchorMode::SelectorInfluenced ||
          !std::isfinite(animator.anchorOffsetX) ||
          !std::isfinite(animator.anchorOffsetY) ||
          std::fabs(animator.anchorOffsetX) > 64.0F ||
          std::fabs(animator.anchorOffsetY) > 64.0F ||
          animator.positionMode < TextPositionMode::AbsolutePixels ||
          animator.positionMode > TextPositionMode::SpaceY ||
          animator.anchor < TextUnitAnchor::GlyphCenter ||
          animator.anchor > TextUnitAnchor::LineBox ||
          animator.projection.kind < TextProjectionKind::Planar2D ||
          animator.projection.kind > TextProjectionKind::Perspective3D ||
          animator.presentation < TextUnitPresentation::Transform ||
          animator.presentation > TextUnitPresentation::ActiveFill ||
          !std::isfinite(animator.projection.fieldOfViewDegrees) ||
          animator.projection.fieldOfViewDegrees <= 0.0F ||
          animator.projection.fieldOfViewDegrees >= 180.0F ||
          !std::isfinite(animator.projection.vanishingPointX) ||
          !std::isfinite(animator.projection.vanishingPointY) ||
          !validColor(animator.activeColor) ||
          !validColor(animator.cursor.color) ||
          !std::isfinite(animator.cursor.width) ||
          animator.cursor.width < 0.0F ||
          animator.cursor.periodUs <= 0 ||
          !std::isfinite(animator.cursor.dutyCycle) ||
          animator.cursor.dutyCycle < 0.0F ||
          animator.cursor.dutyCycle > 1.0F) {
        return fail("text animator identity, projection or budget is invalid");
      }
      if (!animator.effectProgramId.empty()) {
        if (FindTextEffectProgram(stack.effectPrograms,
                                  animator.effectProgramId) == nullptr) {
          return fail("text animator references a missing effect program");
        }
        if (std::find(boundEffectProgramIds.begin(),
                      boundEffectProgramIds.end(),
                      animator.effectProgramId) ==
            boundEffectProgramIds.end()) {
          boundEffectProgramIds.push_back(animator.effectProgramId);
        }
      }
      for (const auto &selector : animator.selectors) {
        if (!std::isfinite(selector.rangeStart) ||
            !std::isfinite(selector.rangeEnd) ||
            !std::isfinite(selector.offset) ||
            !std::isfinite(selector.stagger) ||
            !std::isfinite(selector.edgeSmooth) ||
            !std::isfinite(selector.randomSeed) ||
            !std::isfinite(selector.intensity) ||
            !std::isfinite(selector.intensityStart) ||
            !std::isfinite(selector.intensityEnd) ||
            !std::isfinite(selector.timeStart1) ||
            !std::isfinite(selector.timeStart2) ||
            !std::isfinite(selector.timeEnd1) ||
            !std::isfinite(selector.timeEnd2) || selector.rangeStart < 0.0F ||
            selector.rangeStart > 1.0F || selector.rangeEnd < 0.0F ||
            selector.rangeEnd > 1.0F || selector.stagger < 0.0F ||
            selector.edgeSmooth < 0.0F || selector.randomSeed < 0.0 ||
            selector.randomSeed >
                static_cast<double>(
                    std::numeric_limits<std::uint32_t>::max()) ||
            selector.edgeSmooth > 1.0F || selector.intensity < 0.0F ||
            selector.intensity > 1.0F || selector.intensityStart < 0.0F ||
            selector.intensityStart > 1.0F || selector.intensityEnd < 0.0F ||
            selector.intensityEnd > 1.0F ||
            selector.kind < TextSelectorKind::Range ||
            selector.kind > TextSelectorKind::Time ||
            selector.basedOn < TextUnitBasis::Grapheme ||
            selector.basedOn > TextUnitBasis::All ||
            selector.shape < TextSelectorShape::Linear ||
            selector.shape > TextSelectorShape::Square ||
            selector.order < TextUnitOrder::Forward ||
            selector.order > TextUnitOrder::Random ||
            std::fabs(selector.timeStart1) > 64.0F ||
            std::fabs(selector.timeStart2) > 64.0F ||
            std::fabs(selector.timeEnd1) > 64.0F ||
            std::fabs(selector.timeEnd2) > 64.0F) {
          return fail("text animation selector is invalid");
        }
        const auto validSelectorTrack = [&](const auto &keyframes,
                                            const double minimum,
                                            const double maximum) {
          if (keyframes.empty())
            return true;
          if (keyframes.size() > 64U)
            return false;
          double previous = -1.0;
          for (const auto &keyframe : keyframes) {
            if (!std::isfinite(keyframe.offset) ||
                !std::isfinite(keyframe.value) ||
                !std::isfinite(keyframe.tangentIn) ||
                !std::isfinite(keyframe.tangentOut) ||
                !validBezierTiming(keyframe) || keyframe.offset < 0.0F ||
                keyframe.offset > 1.0F || keyframe.offset < previous ||
                keyframe.value < minimum || keyframe.value > maximum) {
              return false;
            }
            previous = keyframe.offset;
          }
          return true;
        };
        const double unbounded = std::numeric_limits<double>::max();
        if (!validSelectorTrack(selector.rangeStartKeyframes, 0.0, 1.0) ||
            !validSelectorTrack(selector.rangeEndKeyframes, 0.0, 1.0) ||
            !validSelectorTrack(selector.offsetKeyframes, -unbounded,
                                unbounded) ||
            !validSelectorTrack(selector.intensityKeyframes, 0.0, 1.0)) {
          return fail("text animation selector keyframes are invalid");
        }
      }
      if (animator.fillColorKeyframes.size() > 64U)
        return fail("text animator fill-color keyframe count is invalid");
      float previousColorOffset = -1.0F;
      for (const auto &keyframe : animator.fillColorKeyframes) {
        const auto finiteColor = [](const Color &value) {
          return std::isfinite(value.red) && std::isfinite(value.green) &&
                 std::isfinite(value.blue) && std::isfinite(value.alpha);
        };
        if (!std::isfinite(keyframe.offset) || keyframe.offset < 0.0F ||
            keyframe.offset > 1.0F || keyframe.offset < previousColorOffset ||
            !validColor(keyframe.value) || !finiteColor(keyframe.tangentIn) ||
            !finiteColor(keyframe.tangentOut) || !validBezierTiming(keyframe)) {
          return fail("text animator fill-color keyframes are invalid");
        }
        previousColorOffset = keyframe.offset;
      }
      if (!animator.selectors.empty() &&
          std::any_of(animator.selectors.begin(), animator.selectors.end(),
                      [&](const TextUnitSelector &selector) {
                        return selector.basedOn !=
                               animator.selectors.front().basedOn;
                      })) {
        return fail("selectors in one text animator must share a unit basis");
      }
      for (const auto &track : animator.tracks) {
        if (track.keyframes.empty() || track.keyframes.size() > 64)
          return fail("text animator track keyframe count is invalid");
        double previous = -1.0;
        for (const auto &keyframe : track.keyframes) {
          if (!std::isfinite(keyframe.offset) ||
              !std::isfinite(keyframe.value) ||
              !std::isfinite(keyframe.tangentIn) ||
              !std::isfinite(keyframe.tangentOut) ||
              !validBezierTiming(keyframe) || keyframe.offset < 0.0F ||
              keyframe.offset > 1.0F || keyframe.offset < previous) {
            return fail("text animator keyframes are invalid");
          }
          previous = keyframe.offset;
        }
        const auto validValue = [&](const double value) {
          switch (track.property) {
          case TextAnimatedProperty::Opacity:
            return value >= 0.0F && value <= 1.0F;
          case TextAnimatedProperty::ScaleX:
          case TextAnimatedProperty::ScaleY:
            return value >= 0.0 && value <= 64.0;
          case TextAnimatedProperty::BlurRadius:
            return value >= 0.0F && value <= 512.0F;
          case TextAnimatedProperty::PositionX:
          case TextAnimatedProperty::PositionY:
          case TextAnimatedProperty::PositionZ:
          case TextAnimatedProperty::Tracking:
          case TextAnimatedProperty::DistanceFromCenter:
            return std::fabs(value) <= 65'536.0F;
          case TextAnimatedProperty::RotationX:
          case TextAnimatedProperty::RotationY:
          case TextAnimatedProperty::RotationZ:
          case TextAnimatedProperty::ShearX:
          case TextAnimatedProperty::ShearY:
            return std::fabs(value) <= 3'600.0F;
          }
          return false;
        };
        if (std::any_of(track.keyframes.begin(), track.keyframes.end(),
                        [&](const TextKeyframe &keyframe) {
                          return !validValue(keyframe.value);
                        })) {
          return fail("text animator track value exceeds the native bound");
        }
      }
    }
    std::vector<std::string_view> decorationIds;
    decorationIds.reserve(layer.decorations.size());
    const auto validDecorationCurve = [&](const auto &keyframes,
                                          const double minimum,
                                          const double maximum) {
      if (keyframes.size() > 64U)
        return false;
      double previous = -1.0;
      for (const auto &keyframe : keyframes) {
        if (!std::isfinite(keyframe.offset) ||
            !std::isfinite(keyframe.value) ||
            !std::isfinite(keyframe.tangentIn) ||
            !std::isfinite(keyframe.tangentOut) ||
            !validBezierTiming(keyframe) || keyframe.offset < 0.0 ||
            keyframe.offset > 1.0 || keyframe.offset < previous ||
            keyframe.value < minimum || keyframe.value > maximum) {
          return false;
        }
        previous = keyframe.offset;
      }
      return true;
    };
    for (const auto &decoration : layer.decorations) {
      if (decoration.decorationId.empty() ||
          decoration.decorationId.size() > 512U ||
          !IsValidUtf8(decoration.decorationId) ||
          decoration.assetId.size() > 512U ||
          !IsValidUtf8(decoration.assetId) ||
          std::find(decorationIds.begin(), decorationIds.end(),
                    decoration.decorationId) != decorationIds.end() ||
          decoration.anchor <
              TextAnimatedDecorationAnchor::TextBoundsCenter ||
          decoration.anchor > TextAnimatedDecorationAnchor::CanvasCenter ||
          decoration.fit < TextAnimatedDecorationFit::Inherit ||
          decoration.fit > TextAnimatedDecorationFit::FitShortSide ||
          decoration.extentSpace < TextDecorationExtentSpace::TextLocal ||
          decoration.extentSpace > TextDecorationExtentSpace::CanvasFull ||
          decoration.inherit < TextDecorationTransformInherit::Full ||
          decoration.inherit > TextDecorationTransformInherit::TranslateOnly ||
          decoration.playback < TextAnimationPlaybackMode::Once ||
          decoration.playback > TextAnimationPlaybackMode::Hold ||
          !std::isfinite(decoration.expandRatioX) ||
          !std::isfinite(decoration.expandRatioY) ||
          decoration.expandRatioX <= 0.0F ||
          decoration.expandRatioY <= 0.0F ||
          decoration.expandRatioX > 64.0F ||
          decoration.expandRatioY > 64.0F ||
          !std::isfinite(decoration.sourceOutsets.left) ||
          !std::isfinite(decoration.sourceOutsets.top) ||
          !std::isfinite(decoration.sourceOutsets.right) ||
          !std::isfinite(decoration.sourceOutsets.bottom) ||
          decoration.sourceOutsets.left < 0.0F ||
          decoration.sourceOutsets.top < 0.0F ||
          decoration.sourceOutsets.right < 0.0F ||
          decoration.sourceOutsets.bottom < 0.0F ||
          decoration.sourceOutsets.left > 65'536.0F ||
          decoration.sourceOutsets.top > 65'536.0F ||
          decoration.sourceOutsets.right > 65'536.0F ||
          decoration.sourceOutsets.bottom > 65'536.0F ||
          !std::isfinite(decoration.pivotX) ||
          !std::isfinite(decoration.pivotY) ||
          !std::isfinite(decoration.offsetX) ||
          !std::isfinite(decoration.offsetY) ||
          !std::isfinite(decoration.relativeOffsetX) ||
          !std::isfinite(decoration.relativeOffsetY) ||
          !std::isfinite(decoration.scaleX) ||
          !std::isfinite(decoration.scaleY) || decoration.scaleX < 0.0F ||
          decoration.scaleY < 0.0F || decoration.scaleX > 64.0F ||
          decoration.scaleY > 64.0F ||
          !std::isfinite(decoration.rotationXDegrees) ||
          !std::isfinite(decoration.rotationYDegrees) ||
          !std::isfinite(decoration.rotationDegrees) ||
          !std::isfinite(decoration.opacity) || decoration.opacity < 0.0F ||
          decoration.opacity > 1.0F ||
          std::fabs(decoration.offsetX) > 65'536.0F ||
          std::fabs(decoration.offsetY) > 65'536.0F ||
          std::fabs(decoration.rotationXDegrees) > 3'600.0F ||
          std::fabs(decoration.rotationYDegrees) > 3'600.0F ||
          std::fabs(decoration.rotationDegrees) > 3'600.0F) {
        return fail("text decoration animation is invalid");
      }
      decorationIds.push_back(decoration.decorationId);
      const auto validStepTrack = [](const auto &keyframes, const auto minimum,
                                     const auto maximum) {
        float previous = -1.0F;
        for (const auto &keyframe : keyframes) {
          if (!std::isfinite(keyframe.offset) || keyframe.offset < 0.0F ||
              keyframe.offset > 1.0F || keyframe.offset < previous ||
              keyframe.value < minimum || keyframe.value > maximum) {
            return false;
          }
          previous = keyframe.offset;
        }
        return keyframes.size() <= 64U;
      };
      if (!validStepTrack(
              decoration.anchorKeyframes,
              TextAnimatedDecorationAnchor::TextBoundsCenter,
              TextAnimatedDecorationAnchor::CanvasCenter) ||
          !validStepTrack(decoration.fitKeyframes,
                          TextAnimatedDecorationFit::Inherit,
                          TextAnimatedDecorationFit::FitShortSide) ||
          !validDecorationCurve(decoration.pivotXKeyframes, -64.0, 64.0) ||
          !validDecorationCurve(decoration.pivotYKeyframes, -64.0, 64.0) ||
          !validDecorationCurve(decoration.offsetXKeyframes, -65'536.0,
                                65'536.0) ||
          !validDecorationCurve(decoration.offsetYKeyframes, -65'536.0,
                                65'536.0) ||
          !validDecorationCurve(decoration.relativeOffsetXKeyframes, -64.0,
                                64.0) ||
          !validDecorationCurve(decoration.relativeOffsetYKeyframes, -64.0,
                                64.0) ||
          !validDecorationCurve(decoration.scaleXKeyframes, 0.0, 64.0) ||
          !validDecorationCurve(decoration.scaleYKeyframes, 0.0, 64.0) ||
          !validDecorationCurve(decoration.rotationXKeyframes, -3'600.0,
                                3'600.0) ||
          !validDecorationCurve(decoration.rotationYKeyframes, -3'600.0,
                                3'600.0) ||
          !validDecorationCurve(decoration.rotationKeyframes, -3'600.0,
                                3'600.0) ||
          !validDecorationCurve(decoration.opacityKeyframes, 0.0, 1.0) ||
          !validDecorationCurve(decoration.assetProgressKeyframes, -64.0,
                                64.0)) {
        return fail("text decoration animation keyframes are invalid");
      }
    }
    float effectPadding = 0.0F;
    for (const auto &effect : layer.postEffects) {
      if (effect.effectId.empty() || !IsValidUtf8(effect.effectId) ||
          effect.effectId.size() > 512U || effect.inputIds.size() > 64U ||
          std::find(postEffectIds.begin(), postEffectIds.end(),
                    effect.effectId) != postEffectIds.end() ||
          effect.kind < TextPostEffectKind::TurbulenceDisplacement ||
          effect.kind > TextPostEffectKind::PulseEnvelope ||
          !std::isfinite(effect.amount) || !std::isfinite(effect.paddingPx) ||
          effect.amount < 0.0F || effect.amount > 16.0F ||
          effect.paddingPx < 0.0F || effect.paddingPx > 512.0F) {
        return fail("text animation post effect is invalid");
      }
      postEffectIds.push_back(effect.effectId);
      std::vector<std::string_view> inputIds;
      inputIds.reserve(effect.inputIds.size());
      for (const auto &inputId : effect.inputIds) {
        if (inputId.empty() || inputId.size() > 512U ||
            !IsValidUtf8(inputId) || inputId == effect.effectId ||
            std::find(inputIds.begin(), inputIds.end(), inputId) !=
                inputIds.end()) {
          return fail("text animation post effect DAG input is invalid");
        }
        inputIds.push_back(inputId);
      }
      if (effect.amountKeyframes.size() > 64U)
        return fail("text animation post effect keyframe budget is invalid");
      double previousEffectOffset = -1.0;
      for (const auto &keyframe : effect.amountKeyframes) {
        if (!std::isfinite(keyframe.offset) || !std::isfinite(keyframe.value) ||
            !std::isfinite(keyframe.tangentIn) ||
            !std::isfinite(keyframe.tangentOut) ||
            !validBezierTiming(keyframe) || keyframe.offset < 0.0F ||
            keyframe.offset > 1.0F || keyframe.offset < previousEffectOffset ||
            keyframe.value < 0.0F || keyframe.value > 16.0F) {
          return fail("text animation post effect keyframes are invalid");
        }
        previousEffectOffset = keyframe.offset;
      }
      if (effect.parameters.size() > 128U)
        return fail("text animation post effect parameter budget is invalid");
      std::vector<std::string_view> parameterNames;
      parameterNames.reserve(effect.parameters.size());
      for (const auto &parameter : effect.parameters) {
        if (parameter.name.empty() || parameter.name.size() > 256U ||
            !IsValidUtf8(parameter.name) || parameter.values.empty() ||
            parameter.values.size() > 4U || parameter.keyframes.size() > 64U ||
            !IsTypedPostEffectParameter(effect.kind, parameter) ||
            std::find(parameterNames.begin(), parameterNames.end(),
                      parameter.name) != parameterNames.end() ||
            std::any_of(parameter.values.begin(), parameter.values.end(),
                        [](const float value) {
                          return !std::isfinite(value) ||
                                 std::fabs(value) > 1'000'000.0F;
                        })) {
          return fail("text animation post effect parameter is invalid");
        }
        parameterNames.push_back(parameter.name);
        double previousParameterOffset = -1.0;
        for (const auto &keyframe : parameter.keyframes) {
          if (!std::isfinite(keyframe.offset) ||
              !std::isfinite(keyframe.value) ||
              !std::isfinite(keyframe.tangentIn) ||
              !std::isfinite(keyframe.tangentOut) ||
              !validBezierTiming(keyframe) || keyframe.offset < 0.0F ||
              keyframe.offset > 1.0F ||
              keyframe.offset < previousParameterOffset ||
              std::fabs(keyframe.value) > 1'000'000.0F ||
              std::fabs(keyframe.tangentIn) > 1'000'000.0F ||
              std::fabs(keyframe.tangentOut) > 1'000'000.0F) {
            return fail(
                "text animation post effect parameter keyframes are invalid");
          }
          previousParameterOffset = keyframe.offset;
        }
      }
      effectPadding += effect.paddingPx;
    }
    if (effectPadding > 1024.0F)
      return fail("text animation post effect envelope exceeds the budget");
  }
  if (boundEffectProgramIds.size() != stack.effectPrograms.size())
    return fail("text effect program library contains an unbound program");
  return true;
}

bool IsTextLayerAnimationPresetSupported(
    const TextLayerAnimationPhase phase,
    const std::string_view presetId) noexcept {
  if (presetId == "none")
    return true;
  switch (phase) {
  case TextLayerAnimationPhase::Enter:
    return presetId == "fade" || presetId == "slideUp" || presetId == "pop";
  case TextLayerAnimationPhase::Loop:
    return presetId == "pulse" || presetId == "float" || presetId == "breathe";
  case TextLayerAnimationPhase::Exit:
    return presetId == "fade" || presetId == "slideDown" ||
           presetId == "shrink";
  case TextLayerAnimationPhase::Caption:
    return presetId == "fade" || presetId == "pulse" ||
           presetId == "breathe";
  }
  return false;
}

std::optional<TextLayerAnimationClip>
MakeTextLayerAnimationPreset(const TextLayerAnimationPhase phase,
                             const std::string_view presetId,
                             const std::int64_t durationUs) {
  if (!IsTextLayerAnimationPresetSupported(phase, presetId) ||
      presetId == "none" || durationUs <= 0) {
    return std::nullopt;
  }
  TextLayerAnimationClip result;
  result.clipId = phase == TextLayerAnimationPhase::Enter  ? "phase-enter"
                  : phase == TextLayerAnimationPhase::Loop ? "phase-loop"
                  : phase == TextLayerAnimationPhase::Exit ? "phase-exit"
                                                           : "phase-caption";
  result.presetId = std::string(presetId);
  result.phase = phase;
  result.durationUs = durationUs;
  if (phase == TextLayerAnimationPhase::Enter) {
    if (presetId == "fade") {
      result.tracks.push_back(Track(
          TextLayerAnimationProperty::Opacity,
          {Key(0.0F, 0.0F, TextAnimationEasing::EaseOut), Key(1.0F, 1.0F)}));
    } else if (presetId == "slideUp") {
      result.tracks = {
          Track(
              TextLayerAnimationProperty::Opacity,
              {Key(0.0F, 0.0F, TextAnimationEasing::EaseOut), Key(1.0F, 1.0F)}),
          Track(TextLayerAnimationProperty::PositionY,
                {Key(0.0F, 0.12F, TextAnimationEasing::EaseOut),
                 Key(1.0F, 0.0F)}),
      };
    } else {
      result.tracks = {
          Track(TextLayerAnimationProperty::Opacity,
                {Key(0.0F, 0.0F, TextAnimationEasing::EaseOut),
                 Key(0.45F, 1.0F), Key(1.0F, 1.0F)}),
          Track(TextLayerAnimationProperty::ScaleX,
                {Key(0.0F, 0.72F, TextAnimationEasing::EaseOut),
                 Key(0.7F, 1.06F, TextAnimationEasing::EaseInOut),
                 Key(1.0F, 1.0F)}),
          Track(TextLayerAnimationProperty::ScaleY,
                {Key(0.0F, 0.72F, TextAnimationEasing::EaseOut),
                 Key(0.7F, 1.06F, TextAnimationEasing::EaseInOut),
                 Key(1.0F, 1.0F)}),
      };
    }
  } else if (phase == TextLayerAnimationPhase::Loop ||
             phase == TextLayerAnimationPhase::Caption) {
    if (presetId == "fade") {
      result.tracks.push_back(Track(
          TextLayerAnimationProperty::Opacity,
          {Key(0.0F, 0.0F, TextAnimationEasing::EaseOut), Key(1.0F, 1.0F)}));
    } else if (presetId == "pulse") {
      const auto pulse = {
          Key(0.0F, 1.0F, TextAnimationEasing::EaseInOut),
          Key(0.5F, 1.06F, TextAnimationEasing::EaseInOut),
          Key(1.0F, 1.0F),
      };
      result.tracks = {
          Track(TextLayerAnimationProperty::ScaleX, pulse),
          Track(TextLayerAnimationProperty::ScaleY, pulse),
      };
    } else if (presetId == "float") {
      result.tracks.push_back(
          Track(TextLayerAnimationProperty::PositionY,
                {Key(0.0F, 0.0F, TextAnimationEasing::EaseInOut),
                 Key(0.25F, -0.025F, TextAnimationEasing::EaseInOut),
                 Key(0.5F, 0.0F, TextAnimationEasing::EaseInOut),
                 Key(0.75F, 0.025F, TextAnimationEasing::EaseInOut),
                 Key(1.0F, 0.0F)}));
    } else {
      result.tracks.push_back(Track(
          TextLayerAnimationProperty::Opacity,
          {Key(0.0F, 0.82F, TextAnimationEasing::EaseInOut),
           Key(0.5F, 1.0F, TextAnimationEasing::EaseInOut), Key(1.0F, 0.82F)}));
    }
  } else if (presetId == "fade") {
    result.tracks.push_back(
        Track(TextLayerAnimationProperty::Opacity,
              {Key(0.0F, 1.0F, TextAnimationEasing::EaseIn), Key(1.0F, 0.0F)}));
  } else if (presetId == "slideDown") {
    result.tracks = {
        Track(TextLayerAnimationProperty::Opacity,
              {Key(0.0F, 1.0F, TextAnimationEasing::EaseIn), Key(1.0F, 0.0F)}),
        Track(TextLayerAnimationProperty::PositionY,
              {Key(0.0F, 0.0F, TextAnimationEasing::EaseIn), Key(1.0F, 0.12F)}),
    };
  } else {
    result.tracks = {
        Track(TextLayerAnimationProperty::Opacity,
              {Key(0.0F, 1.0F, TextAnimationEasing::EaseIn), Key(1.0F, 0.0F)}),
        Track(TextLayerAnimationProperty::ScaleX,
              {Key(0.0F, 1.0F, TextAnimationEasing::EaseIn), Key(1.0F, 0.72F)}),
        Track(TextLayerAnimationProperty::ScaleY,
              {Key(0.0F, 1.0F, TextAnimationEasing::EaseIn), Key(1.0F, 0.72F)}),
    };
  }
  return result;
}

} // namespace videocut::text
