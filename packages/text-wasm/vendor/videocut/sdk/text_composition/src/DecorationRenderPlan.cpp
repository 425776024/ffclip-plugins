#include "videocut/text_composition/DecorationRenderPlan.h"

#include "TextSamplingSupport.h"
#include "videocut/frame/MediaTime.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace videocut::text_composition {
namespace {

void Add(std::vector<Diagnostic> &diagnostics, std::string code,
         std::string subject, std::string message,
         const DiagnosticSeverity severity = DiagnosticSeverity::Error) {
  diagnostics.push_back({std::move(code), severity, "decoration-plan",
                         std::move(subject), std::move(message)});
}

std::uint64_t SaturatingAdd(const std::uint64_t left,
                            const std::uint64_t right) noexcept {
  return right > std::numeric_limits<std::uint64_t>::max() - left
             ? std::numeric_limits<std::uint64_t>::max()
             : left + right;
}

std::uint64_t PixelBytes(const std::uint64_t pixels) noexcept {
  return pixels > std::numeric_limits<std::uint64_t>::max() / 4U
             ? std::numeric_limits<std::uint64_t>::max()
             : pixels * 4U;
}

bool FiniteRect(const text::Rect &rect) noexcept {
  return std::isfinite(rect.x) && std::isfinite(rect.y) &&
         std::isfinite(rect.width) && std::isfinite(rect.height) &&
         rect.width >= 0.0F && rect.height >= 0.0F &&
         std::isfinite(rect.x + rect.width) &&
         std::isfinite(rect.y + rect.height);
}

bool NonEmptyRect(const text::Rect &rect) noexcept {
  return FiniteRect(rect) && rect.width > 0.0F && rect.height > 0.0F;
}

text::Rect Union(const text::Rect &left, const text::Rect &right) noexcept {
  if (!NonEmptyRect(left))
    return right;
  if (!NonEmptyRect(right))
    return left;
  const auto minimumX = std::min(left.x, right.x);
  const auto minimumY = std::min(left.y, right.y);
  const auto maximumX =
      std::max(left.x + left.width, right.x + right.width);
  const auto maximumY =
      std::max(left.y + left.height, right.y + right.height);
  return {minimumX, minimumY, maximumX - minimumX, maximumY - minimumY};
}

DecorationOrthographicBasis Multiply(
    const DecorationOrthographicBasis &left,
    const DecorationOrthographicBasis &right) noexcept {
  return {
      left.xx * right.xx + left.xy * right.yx,
      left.xx * right.xy + left.xy * right.yy,
      left.yx * right.xx + left.yy * right.yx,
      left.yx * right.xy + left.yy * right.yy,
  };
}

std::pair<float, float>
TransformVector(const DecorationOrthographicBasis &basis, const float x,
                const float y) noexcept {
  return {basis.xx * x + basis.xy * y, basis.yx * x + basis.yy * y};
}

DecorationOrthographicBasis BasisFromScaleAndRotation(
    const float scaleX, const float scaleY,
    const float rotationDegrees) noexcept {
  const auto radians =
      rotationDegrees * 3.14159265358979323846F / 180.0F;
  const auto cosine = std::cos(radians);
  const auto sine = std::sin(radians);
  return {cosine * scaleX, -sine * scaleY, sine * scaleX,
          cosine * scaleY};
}

DecorationOrthographicBasis ProjectAnimationBasis(
    const text::TextDecorationAnimationSample &sample, const float scaleX,
    const float scaleY) noexcept {
  const auto radians = 3.14159265358979323846F / 180.0F;
  const auto rx = sample.rotationXDegrees * radians;
  const auto ry = sample.rotationYDegrees * radians;
  const auto rz = sample.rotationDegrees * radians;
  const auto cosineX = std::cos(rx);
  const auto sineX = std::sin(rx);
  const auto cosineY = std::cos(ry);
  const auto sineY = std::sin(ry);
  const auto cosineZ = std::cos(rz);
  const auto sineZ = std::sin(rz);
  return {
      cosineZ * cosineY * scaleX,
      (cosineZ * sineY * sineX - sineZ * cosineX) * scaleY,
      sineZ * cosineY * scaleX,
      (sineZ * sineY * sineX + cosineZ * cosineX) * scaleY,
  };
}

std::int64_t SourceTickDeltaToUs(const std::int64_t value,
                                 const std::int64_t origin) noexcept {
  const auto delta =
      (static_cast<long double>(value) - static_cast<long double>(origin)) *
      1'000'000.0L /
      static_cast<long double>(frame::kMediaTicksPerSecond);
  if (delta <= static_cast<long double>(
                   std::numeric_limits<std::int64_t>::min()))
    return std::numeric_limits<std::int64_t>::min();
  if (delta >= static_cast<long double>(
                   std::numeric_limits<std::int64_t>::max()))
    return std::numeric_limits<std::int64_t>::max();
  return static_cast<std::int64_t>(std::llround(delta));
}

struct RunGlobalRange final {
  std::string paragraphId;
  std::string runId;
  std::uint64_t begin{0U};
  std::uint64_t end{0U};
};

std::vector<RunGlobalRange>
BuildRunRanges(const TextCompositionDocument &document) {
  std::vector<RunGlobalRange> ranges;
  std::uint64_t offset = 0U;
  for (std::size_t paragraphIndex = 0U;
       paragraphIndex < document.content.size(); ++paragraphIndex) {
    const auto &paragraph = document.content[paragraphIndex];
    for (const auto &run : paragraph.runs) {
      ranges.push_back({paragraph.paragraphId, run.runId, offset,
                        offset + run.utf8Text.size()});
      offset += run.utf8Text.size();
    }
    if (paragraphIndex + 1U < document.content.size())
      ++offset;
  }
  return ranges;
}

struct ResolvedTarget final {
  bool valid{false};
  text::TextUtf8Range globalRange{};
};

ResolvedTarget ResolveTarget(const TextCompositionDocument &document,
                             const VectorDecorationBinding &binding) {
  const auto ranges = BuildRunRanges(document);
  if (binding.target.scope == DecorationTargetScope::AllText) {
    if (ranges.empty())
      return {};
    return {true, {ranges.front().begin, ranges.back().end}};
  }
  const auto found = std::find_if(
      ranges.begin(), ranges.end(), [&](const auto &range) {
        return range.paragraphId == binding.target.paragraphId &&
               range.runId == binding.target.runId;
      });
  if (found == ranges.end() ||
      binding.target.range.end > found->end - found->begin)
    return {};
  return {true,
          {found->begin + binding.target.range.begin,
           found->begin + binding.target.range.end}};
}

const std::vector<text::TextClusterMetrics> &
AuthoredClusters(const text::TextLayout &layout) noexcept {
  return layout.authoredClusters.empty() ? layout.clusters
                                         : layout.authoredClusters;
}

text::Rect ClusterBounds(const text::TextClusterMetrics &cluster) noexcept {
  text::Rect result;
  for (const auto &box : cluster.boxes)
    if (NonEmptyRect(box.bounds))
      result = Union(result, box.bounds);
  return result;
}

text::Rect ClusterCaret(const text::TextClusterMetrics &cluster,
                        const bool begin) noexcept {
  text::Rect result{};
  bool found = false;
  for (const auto &box : cluster.boxes) {
    const auto &candidate = begin ? box.utf8BeginCaret : box.utf8EndCaret;
    if (!FiniteRect(candidate))
      continue;
    if (!found || !begin)
      result = candidate;
    found = true;
    if (begin)
      break;
  }
  return found ? result : ClusterBounds(cluster);
}

std::size_t ClusterVisualLine(const text::TextLayout &layout,
                              const text::TextClusterMetrics &cluster) {
  const auto bounds = ClusterBounds(cluster);
  std::size_t selected = std::numeric_limits<std::size_t>::max();
  float bestOverlap = 0.0F;
  for (std::size_t index = 0U; index < layout.lines.size(); ++index) {
    const auto &line = layout.lines[index].bounds;
    if (!FiniteRect(line))
      continue;
    const float overlap =
        std::max(0.0F, std::min(bounds.y + bounds.height, line.y + line.height) -
                           std::max(bounds.y, line.y));
    if (overlap > bestOverlap) {
      bestOverlap = overlap;
      selected = index;
    }
  }
  return selected;
}

std::vector<const text::TextClusterMetrics *>
CollectClusters(const text::TextLayout &layout,
                const text::TextUtf8Range range) {
  std::vector<const text::TextClusterMetrics *> result;
  result.reserve(AuthoredClusters(layout).size());
  for (const auto &cluster : AuthoredClusters(layout)) {
    if (cluster.documentUtf8End > range.begin &&
        cluster.documentUtf8Begin < range.end &&
        NonEmptyRect(ClusterBounds(cluster)))
      result.push_back(&cluster);
  }
  std::stable_sort(
      result.begin(), result.end(),
      [&](const auto *left, const auto *right) {
        const auto leftLine = ClusterVisualLine(layout, *left);
        const auto rightLine = ClusterVisualLine(layout, *right);
        if (leftLine != rightLine)
          return leftLine < rightLine;
        const auto leftBounds = ClusterBounds(*left);
        const auto rightBounds = ClusterBounds(*right);
        if (leftBounds.y != rightBounds.y)
          return leftBounds.y < rightBounds.y;
        if (leftBounds.x != rightBounds.x)
          return leftBounds.x < rightBounds.x;
        return left->documentUtf8Begin < right->documentUtf8Begin;
      });
  return result;
}

double PositiveModulo(const double value, const double divisor) noexcept {
  if (!(divisor > 0.0))
    return 0.0;
  auto result = std::fmod(value, divisor);
  if (result < 0.0)
    result += divisor;
  return result;
}

struct AssetClockSample final {
  bool visible{false};
  std::int64_t timeUs{0};
  double progress{0.0};
};

AssetClockSample SampleAssetClock(const VectorDecorationBinding &binding,
                                  const DecorationSampleTime &time,
                                  const std::int64_t phaseOffsetUs = 0) noexcept {
  const auto &playback = binding.assetPlayback;
  const auto duration = playback.sourceOutUs - playback.sourceInUs;
  if (duration <= 0)
    return {};
  double ownerTime = 0.0;
  switch (playback.clock) {
  case DecorationAssetClock::Composition:
    ownerTime = static_cast<double>(detail::SaturatingDifference(
        time.absoluteCompositionUs, time.compositionBeginUs));
    break;
  case DecorationAssetClock::Source:
    ownerTime = static_cast<double>(SourceTickDeltaToUs(
        time.absoluteSourceTicks, time.sourceWindowBeginTicks));
    break;
  case DecorationAssetClock::Target:
    ownerTime = static_cast<double>(detail::SaturatingDifference(
        time.absoluteCompositionUs, time.compositionBeginUs));
    break;
  }
  ownerTime = ownerTime * playback.speed +
              static_cast<double>(playback.phaseUs) +
              static_cast<double>(phaseOffsetUs);
  const auto durationDouble = static_cast<double>(duration);
  AssetClockSample result;
  switch (playback.mode) {
  case DecorationPlaybackMode::Once:
    if (ownerTime < 0.0 || ownerTime >= durationDouble)
      return result;
    result.progress = ownerTime / durationDouble;
    break;
  case DecorationPlaybackMode::Hold:
    result.progress =
        std::clamp(ownerTime / durationDouble, 0.0,
                   std::nextafter(1.0, 0.0));
    break;
  case DecorationPlaybackMode::Loop:
    result.progress = PositiveModulo(ownerTime, durationDouble) / durationDouble;
    break;
  case DecorationPlaybackMode::PingPong: {
    const auto cycle = PositiveModulo(ownerTime, durationDouble * 2.0);
    result.progress = cycle <= durationDouble
                          ? cycle / durationDouble
                          : (durationDouble * 2.0 - cycle) / durationDouble;
    result.progress = std::min(result.progress, std::nextafter(1.0, 0.0));
    break;
  }
  }
  result.visible = true;
  result.timeUs = playback.sourceInUs + static_cast<std::int64_t>(
                                           std::floor(result.progress *
                                                      durationDouble));
  result.timeUs =
      std::clamp(result.timeUs, playback.sourceInUs,
                 playback.sourceOutUs - 1);
  return result;
}

std::uint64_t StableIndexHash(const std::string &identity,
                              const std::uint32_t seed,
                              const std::size_t index) noexcept {
  std::uint64_t value = 1469598103934665603ULL ^ seed;
  for (const unsigned char byte : identity) {
    value ^= byte;
    value *= 1099511628211ULL;
  }
  value ^= static_cast<std::uint64_t>(index);
  value *= 1099511628211ULL;
  value ^= value >> 32U;
  return value;
}

struct VariantSelection final {
  const DecorationAssetReference *asset{nullptr};
  std::string variantId;
  std::size_t patternIndex{0U};
};

VariantSelection SelectVariant(const VectorDecorationBinding &binding,
                               const std::size_t index) noexcept {
  if (!binding.instancePattern ||
      binding.instancePattern->assetVariants.empty())
    return {&binding.asset, {}, 0U};
  const auto &pattern = *binding.instancePattern;
  const auto count = pattern.assetVariants.size();
  std::size_t selected = 0U;
  switch (pattern.selectionPolicy) {
  case DecorationVariantSelectionPolicy::RepeatOne:
    selected = 0U;
    break;
  case DecorationVariantSelectionPolicy::Cycle:
    selected = index % count;
    break;
  case DecorationVariantSelectionPolicy::DeterministicHash:
    selected = static_cast<std::size_t>(
        StableIndexHash(binding.decorationId, pattern.randomSeed, index) % count);
    break;
  case DecorationVariantSelectionPolicy::ExplicitPattern:
    if (pattern.explicitPattern.empty())
      return {};
    selected = pattern.explicitPattern[index % pattern.explicitPattern.size()];
    if (selected >= count)
      return {};
    break;
  }
  return {&pattern.assetVariants[selected].asset,
          pattern.assetVariants[selected].variantId, selected};
}

std::int64_t PatternPhase(const VectorDecorationBinding &binding,
                          const VariantSelection &variant,
                          const std::size_t index) noexcept {
  if (!binding.instancePattern)
    return 0;
  switch (binding.instancePattern->phasePolicy) {
  case DecorationPerIndexPhasePolicy::Shared:
    return 0;
  case DecorationPerIndexPhasePolicy::Stagger:
  case DecorationPerIndexPhasePolicy::DeterministicSeed: {
    const auto multiplier =
        binding.instancePattern->phasePolicy ==
                DecorationPerIndexPhasePolicy::Stagger
            ? index
            : variant.patternIndex;
    const auto value = static_cast<long double>(multiplier) *
                       static_cast<long double>(
                           binding.instancePattern->phaseStepUs);
    if (value <= static_cast<long double>(
                     std::numeric_limits<std::int64_t>::min()))
      return std::numeric_limits<std::int64_t>::min();
    if (value >= static_cast<long double>(
                     std::numeric_limits<std::int64_t>::max()))
      return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(value);
  }
  }
  return 0;
}

struct Placement final {
  text::Rect bounds{};
  text::TextUtf8Range range{};
  std::size_t index{0U};
  text::Rect beginCaret{};
  text::Rect endCaret{};
  bool hasCaret{false};
};

text::Rect LerpRect(const text::Rect &from, const text::Rect &to,
                    const double progress) noexcept {
  const float p = static_cast<float>(std::clamp(progress, 0.0, 1.0));
  const auto lerp = [p](const float left, const float right) {
    return left + (right - left) * p;
  };
  return {lerp(from.x, to.x), lerp(from.y, to.y),
          lerp(from.width, to.width), lerp(from.height, to.height)};
}

text::Rect StablePresentationLetterBounds(
    const text::TextLayout &layout) noexcept {
  const auto project = [](const text::Rect &source,
                          const text::Rect &authoredFrame,
                          const text::Rect &presentationFrame) {
    if (!(authoredFrame.width > 0.0F && authoredFrame.height > 0.0F &&
          presentationFrame.width > 0.0F &&
          presentationFrame.height > 0.0F)) {
      return text::Rect{};
    }
    const auto scaleX = presentationFrame.width / authoredFrame.width;
    const auto scaleY = presentationFrame.height / authoredFrame.height;
    return text::Rect{
        presentationFrame.x + (source.x - authoredFrame.x) * scaleX,
        presentationFrame.y + (source.y - authoredFrame.y) * scaleY,
        source.width * scaleX, source.height * scaleY};
  };
  auto result = project(layout.authoredLetterBounds,
                        layout.authoredLogicalBounds, layout.logicalBounds);
  if (result.width <= 0.0F || result.height <= 0.0F) {
    result = project(layout.authoredLetterBounds,
                     layout.authoredControlBounds, layout.controlBounds);
  }
  return result;
}

std::vector<Placement>
BuildPlacements(const VectorDecorationBinding &binding,
                const ResolvedTarget &target,
                const std::vector<const text::TextClusterMetrics *> &clusters,
                const text::TextLayout &layout,
                const DecorationSampleTime &time,
                const bool useAuthoredLetterBounds) {
  std::vector<Placement> result;
  switch (binding.mode) {
  case VectorDecorationMode::TrackedGrapheme: {
    if (clusters.empty())
      break;
    std::size_t first = 0U;
    std::size_t second = 0U;
    double fraction = 0.0;
    if (binding.placementDriver ==
        DecorationPlacementDriver::SentenceUniformProgress) {
      const auto duration =
          static_cast<long double>(time.compositionEndUs) -
          static_cast<long double>(time.compositionBeginUs);
      const auto progress = duration <= 0
                                ? 0.0
                                : std::clamp(
                                      static_cast<double>(
                                          static_cast<long double>(
                                              time.absoluteCompositionUs) -
                                          static_cast<long double>(
                                              time.compositionBeginUs)) /
                                          static_cast<double>(duration),
                                      0.0, std::nextafter(1.0, 0.0));
      const double coordinate =
          progress * static_cast<double>(clusters.size() - 1U);
      first = std::min(
          clusters.size() - 1U,
          static_cast<std::size_t>(std::floor(coordinate)));
      second = std::min(first + 1U, clusters.size() - 1U);
      fraction = coordinate - static_cast<double>(first);
    }
    const auto *left = clusters[first];
    const auto *right = clusters[second];
    Placement placement;
    placement.bounds =
        LerpRect(ClusterBounds(*left), ClusterBounds(*right), fraction);
    placement.range = {
        std::min(left->documentUtf8Begin, right->documentUtf8Begin),
        std::max(left->documentUtf8End, right->documentUtf8End)};
    placement.index = first;
    placement.beginCaret =
        LerpRect(ClusterCaret(*left, true), ClusterCaret(*right, true),
                 fraction);
    placement.endCaret =
        LerpRect(ClusterCaret(*left, false), ClusterCaret(*right, false),
                 fraction);
    placement.hasCaret = true;
    result.push_back(std::move(placement));
    break;
  }
  case VectorDecorationMode::SentenceEnvelope: {
    text::Rect bounds;
    for (const auto *cluster : clusters)
      bounds = Union(bounds, ClusterBounds(*cluster));
    if (bounds.width > 0.0F && bounds.height > 0.0F)
      result.push_back({bounds, target.globalRange, 0U});
    break;
  }
  case VectorDecorationMode::PerGraphemeBackground:
    for (std::size_t index = 0U; index < clusters.size(); ++index) {
      const auto *cluster = clusters[index];
      Placement placement;
      placement.bounds = ClusterBounds(*cluster);
      placement.range = {cluster->documentUtf8Begin,
                         cluster->documentUtf8End};
      placement.index = index;
      placement.beginCaret = ClusterCaret(*cluster, true);
      placement.endCaret = ClusterCaret(*cluster, false);
      placement.hasCaret = true;
      result.push_back(std::move(placement));
    }
    break;
  case VectorDecorationMode::CompositionOverlay: {
    // This pass is composited in presentation pixels. The legacy Qt-aligned
    // showpiece bounds were mapped through the render-group transform before
    // decoration placement; authored reference-canvas bounds are not valid in
    // this domain.
    auto bounds = useAuthoredLetterBounds
                      ? StablePresentationLetterBounds(layout)
                      : layout.controlBounds;
    if (bounds.width <= 0.0F || bounds.height <= 0.0F)
      bounds = layout.controlBounds;
    if (bounds.width <= 0.0F || bounds.height <= 0.0F)
      bounds = layout.logicalBounds;
    if (bounds.width > 0.0F && bounds.height > 0.0F)
      result.push_back({bounds, target.globalRange, 0U});
    break;
  }
  }
  return result;
}

DecorationInstance MakeInstance(const VectorDecorationBinding &binding,
                                const Placement &placement,
                                const VariantSelection &variant,
                                const AssetClockSample &clock) {
  DecorationInstance instance;
  instance.instanceId = binding.decorationId + ":" +
                        std::to_string(placement.index);
  instance.variantId = variant.variantId;
  instance.asset = *variant.asset;
  instance.assetTimeUs = clock.timeUs;
  instance.targetRange = placement.range;
  instance.localTargetBounds = placement.bounds;
  const auto paddedWidth = placement.bounds.width + binding.padding.left +
                           binding.padding.right;
  const auto paddedHeight = placement.bounds.height + binding.padding.top +
                            binding.padding.bottom;
  const auto nativeWidth = std::max(variant.asset->intrinsicWidth, 0.0001F);
  const auto nativeHeight = std::max(variant.asset->intrinsicHeight, 0.0001F);
  const auto contain =
      std::min(paddedWidth / nativeWidth, paddedHeight / nativeHeight);
  const auto cover =
      std::max(paddedWidth / nativeWidth, paddedHeight / nativeHeight);
  switch (binding.fit) {
  case DecorationFit::Contain:
    instance.localScaleX = contain;
    instance.localScaleY = contain;
    break;
  case DecorationFit::Cover:
    instance.localScaleX = cover;
    instance.localScaleY = cover;
    break;
  case DecorationFit::Stretch:
    instance.localScaleX = paddedWidth / nativeWidth;
    instance.localScaleY = paddedHeight / nativeHeight;
    break;
  case DecorationFit::Native:
    break;
  }
  float patternOffsetX = 0.0F;
  float patternOffsetY = 0.0F;
  float patternScale = 0.0F;
  float patternRotation = 0.0F;
  if (binding.instancePattern) {
    const auto midpoint = static_cast<float>(
        binding.instancePattern->assetVariants.size() - 1U) *
                          0.5F;
    const auto centered = static_cast<float>(variant.patternIndex) - midpoint;
    patternOffsetX = centered * binding.instancePattern->offsetXStep;
    patternOffsetY = centered * binding.instancePattern->offsetYStep;
    patternScale = centered * binding.instancePattern->scaleStep;
    patternRotation =
        centered * binding.instancePattern->rotationStepDegrees;
  }
  instance.localScaleX *=
      binding.localTransform.scaleX * std::max(0.001F, 1.0F + patternScale);
  instance.localScaleY *=
      binding.localTransform.scaleY * std::max(0.001F, 1.0F + patternScale);
  instance.localTranslationX = placement.bounds.x +
                               placement.bounds.width * 0.5F +
                               binding.localTransform.offsetX + patternOffsetX;
  instance.localTranslationY = placement.bounds.y +
                               placement.bounds.height * 0.5F +
                               binding.localTransform.offsetY + patternOffsetY;
  if (binding.anchor == DecorationAnchor::CaretPath && placement.hasCaret &&
      FiniteRect(placement.beginCaret)) {
    instance.localTranslationX = placement.beginCaret.x +
                                 placement.beginCaret.width * 0.5F +
                                 binding.localTransform.offsetX + patternOffsetX;
    instance.localTranslationY = placement.beginCaret.y +
                                 placement.beginCaret.height * 0.5F +
                                 binding.localTransform.offsetY + patternOffsetY;
  }
  const auto displayHeight =
      nativeHeight * std::fabs(instance.localScaleY);
  switch (binding.anchor) {
  case DecorationAnchor::Above:
    instance.localTranslationY -=
        placement.bounds.height * 0.5F + displayHeight * 0.5F;
    break;
  case DecorationAnchor::Below:
    instance.localTranslationY +=
        placement.bounds.height * 0.5F + displayHeight * 0.5F;
    break;
  case DecorationAnchor::Center:
    break;
  case DecorationAnchor::CaretPath:
    break;
  }
  instance.localRotationDegrees =
      binding.localTransform.rotationDegrees + patternRotation;
  instance.opacity = binding.localTransform.opacity;
  instance.sampling = binding.sampling;
  instance.transformInherit = binding.transformInherit;
  return instance;
}

struct FitScale final {
  float x{1.0F};
  float y{1.0F};
};

FitScale ResolveBindingFit(const VectorDecorationBinding &binding,
                           const Placement &placement,
                           const DecorationAssetReference &asset) noexcept {
  const auto width = placement.bounds.width + binding.padding.left +
                     binding.padding.right;
  const auto height = placement.bounds.height + binding.padding.top +
                      binding.padding.bottom;
  const auto nativeWidth = std::max(asset.intrinsicWidth, 0.0001F);
  const auto nativeHeight = std::max(asset.intrinsicHeight, 0.0001F);
  const auto contain = std::min(width / nativeWidth, height / nativeHeight);
  const auto cover = std::max(width / nativeWidth, height / nativeHeight);
  switch (binding.fit) {
  case DecorationFit::Contain:
    return {contain, contain};
  case DecorationFit::Cover:
    return {cover, cover};
  case DecorationFit::Stretch:
    return {width / nativeWidth, height / nativeHeight};
  case DecorationFit::Native:
    return {};
  }
  return {};
}

std::pair<float, float>
ResolveAnimationExtent(const text::TextDecorationAnimationSpec &spec,
                       const Placement &placement,
                       const VectorDecorationBinding &binding,
                       const text::TextLayout &layout) noexcept {
  float width = placement.bounds.width + binding.padding.left +
                binding.padding.right;
  float height = placement.bounds.height + binding.padding.top +
                 binding.padding.bottom;
  const auto canvasWidth =
      layout.outputWidth == 0U
          ? std::max(layout.authoredControlBounds.width,
                     layout.controlBounds.width)
          : static_cast<float>(layout.outputWidth);
  const auto canvasHeight =
      layout.outputHeight == 0U
          ? std::max(layout.authoredControlBounds.height,
                     layout.controlBounds.height)
          : static_cast<float>(layout.outputHeight);
  switch (spec.extentSpace) {
  case text::TextDecorationExtentSpace::TextLocal:
    break;
  case text::TextDecorationExtentSpace::CanvasWidth:
    width = canvasWidth;
    break;
  case text::TextDecorationExtentSpace::CanvasHeight:
    height = canvasHeight;
    break;
  case text::TextDecorationExtentSpace::CanvasFull:
    width = canvasWidth;
    height = canvasHeight;
    break;
  }
  return {std::max(width * spec.expandRatioX, 0.0001F),
          std::max(height * spec.expandRatioY, 0.0001F)};
}

FitScale ResolveAnimatedFit(
    const text::TextDecorationAnimationSpec &spec,
    const text::TextDecorationAnimationSample &sample,
    const Placement &placement, const VectorDecorationBinding &binding,
    const DecorationAssetReference &asset,
    const text::TextLayout &layout) noexcept {
  if (sample.fit == text::TextAnimatedDecorationFit::Inherit)
    return {};
  const auto [width, height] =
      ResolveAnimationExtent(spec, placement, binding, layout);
  const auto nativeWidth = std::max(asset.intrinsicWidth, 0.0001F);
  const auto nativeHeight = std::max(asset.intrinsicHeight, 0.0001F);
  switch (sample.fit) {
  case text::TextAnimatedDecorationFit::Inherit:
    return {};
  case text::TextAnimatedDecorationFit::Contain: {
    const auto value = std::min(width / nativeWidth, height / nativeHeight);
    return {value, value};
  }
  case text::TextAnimatedDecorationFit::Cover: {
    const auto value = std::max(width / nativeWidth, height / nativeHeight);
    return {value, value};
  }
  case text::TextAnimatedDecorationFit::Stretch:
    return {width / nativeWidth, height / nativeHeight};
  case text::TextAnimatedDecorationFit::Native:
    return {};
  case text::TextAnimatedDecorationFit::FitWidth: {
    const auto value = width / nativeWidth;
    return {value, value};
  }
  case text::TextAnimatedDecorationFit::FitHeight: {
    const auto value = height / nativeHeight;
    return {value, value};
  }
  case text::TextAnimatedDecorationFit::FitLongSide: {
    const auto value = std::max(width, height) /
                       std::max(nativeWidth, nativeHeight);
    return {value, value};
  }
  case text::TextAnimatedDecorationFit::FitShortSide: {
    const auto value = std::min(width, height) /
                       std::min(nativeWidth, nativeHeight);
    return {value, value};
  }
  }
  return {};
}

struct SampledDecorationAnimation final {
  const text::TextDecorationAnimationSpec *spec{nullptr};
  text::TextDecorationAnimationSample sample{};
};

std::vector<SampledDecorationAnimation> SampleDecorationAnimations(
    const TextCompositionDocument &document, const std::string &decorationId,
    const DecorationSampleTime &time,
    const std::vector<text::TimedTextSpan> &timedSpans) {
  std::vector<SampledDecorationAnimation> result;
  const auto localTimeUs = detail::SaturatingDifference(
      time.absoluteCompositionUs, time.compositionBeginUs);
  const auto durationUs = detail::SaturatingDifference(
      time.compositionEndUs, time.compositionBeginUs);
  for (const auto &layer : document.animations.layers) {
    if (!layer.enabled)
      continue;
    const auto evaluation = text::SampleTextAnimationLayerEvaluation(
        document.animations, layer, localTimeUs, durationUs, timedSpans);
    if (!evaluation.active)
      continue;
    for (const auto &spec : layer.decorations) {
      if (spec.decorationId != decorationId)
        continue;
      result.push_back(
          {&spec, text::SampleTextDecorationAnimation(
                      spec, static_cast<float>(evaluation.progress))});
    }
  }
  return result;
}

float ResolveAssetProgress(const text::TextAnimationPlaybackMode playback,
                           const float progress) noexcept {
  const auto clamped = std::clamp(progress, 0.0F, 1.0F);
  switch (playback) {
  case text::TextAnimationPlaybackMode::Once:
  case text::TextAnimationPlaybackMode::Hold:
    return clamped;
  case text::TextAnimationPlaybackMode::Loop:
    return clamped >= 1.0F ? 0.0F : clamped;
  case text::TextAnimationPlaybackMode::PingPong:
    return clamped <= 0.5F ? clamped * 2.0F
                           : (1.0F - clamped) * 2.0F;
  }
  return clamped;
}

void ApplyDecorationAnimations(
    DecorationInstance &instance, const VectorDecorationBinding &binding,
    const Placement &placement, const text::TextLayout &layout,
    const std::vector<SampledDecorationAnimation> &animations,
    std::optional<vector::VectorAssetFrameSample> &assetFrameSample) noexcept {
  auto basis = BasisFromScaleAndRotation(
      instance.localScaleX, instance.localScaleY,
      instance.localRotationDegrees);
  for (const auto &animation : animations) {
    const auto &spec = *animation.spec;
    const auto &sample = animation.sample;
    if (!spec.assetId.empty() && spec.assetId != instance.asset.assetId)
      continue;

    const auto bindingFit = ResolveBindingFit(binding, placement, instance.asset);
    const auto animatedFit =
        ResolveAnimatedFit(spec, sample, placement, binding, instance.asset,
                           layout);
    const auto fitScaleX = sample.fit == text::TextAnimatedDecorationFit::Inherit
                               ? 1.0F
                               : animatedFit.x /
                                     std::max(std::fabs(bindingFit.x), 0.0001F);
    const auto fitScaleY = sample.fit == text::TextAnimatedDecorationFit::Inherit
                               ? 1.0F
                               : animatedFit.y /
                                     std::max(std::fabs(bindingFit.y), 0.0001F);
    const auto animatedBasis = ProjectAnimationBasis(
        sample, sample.scaleX * fitScaleX, sample.scaleY * fitScaleY);
    const auto composed = Multiply(basis, animatedBasis);

    // Amazing Sprite2D ax/ay are offsets on its fixed [-1,+1] logical mesh,
    // not a conventional scale/rotation pivot. Project that source-local
    // displacement through the completed authored+animation basis exactly
    // once. The logical (decoded) asset aspect participates in X; packed media
    // dimensions never do. Sprite local Y is upward.
    const auto intrinsicWidth =
        std::max(instance.asset.intrinsicWidth, 0.0001F);
    const auto intrinsicHeight =
        std::max(instance.asset.intrinsicHeight, 0.0001F);
    const auto logicalAspect = intrinsicWidth / intrinsicHeight;
    const auto pivotLocalX =
        sample.pivotX * logicalAspect * intrinsicWidth * 0.5F;
    const auto pivotLocalY = -sample.pivotY * intrinsicHeight * 0.5F;
    const auto [projectedPivotX, projectedPivotY] =
        TransformVector(composed, pivotLocalX, pivotLocalY);
    instance.localTranslationX += projectedPivotX;
    instance.localTranslationY += projectedPivotY;

    if (sample.anchor == text::TextAnimatedDecorationAnchor::CanvasCenter) {
      const auto canvasWidth =
          layout.outputWidth == 0U
              ? std::max(layout.authoredControlBounds.width,
                         layout.controlBounds.width)
              : static_cast<float>(layout.outputWidth);
      const auto canvasHeight =
          layout.outputHeight == 0U
              ? std::max(layout.authoredControlBounds.height,
                         layout.controlBounds.height)
              : static_cast<float>(layout.outputHeight);
      instance.localTranslationX += canvasWidth * 0.5F -
                                    (placement.bounds.x +
                                     placement.bounds.width * 0.5F);
      instance.localTranslationY += canvasHeight * 0.5F -
                                    (placement.bounds.y +
                                     placement.bounds.height * 0.5F);
    }

    const auto [extentWidth, extentHeight] =
        ResolveAnimationExtent(spec, placement, binding, layout);
    // Qt normalizes px/py against half the expanded text extent. Relative Y is
    // authored upward while composition coordinates are downward. These are
    // group-space translations and therefore do not inherit the sprite basis.
    instance.localTranslationX +=
        sample.offsetX + sample.relativeOffsetX * extentWidth * 0.5F;
    instance.localTranslationY +=
        sample.offsetY - sample.relativeOffsetY * extentHeight * 0.5F;
    instance.opacity =
        std::clamp(instance.opacity * sample.opacity, 0.0F, 1.0F);
    instance.transformInherit =
        spec.inherit == text::TextDecorationTransformInherit::TranslateOnly
            ? DecorationTransformInherit::TranslateOnly
            : DecorationTransformInherit::Full;

    const auto duration = binding.assetPlayback.sourceOutUs -
                          binding.assetPlayback.sourceInUs;
    if (duration > 0) {
      const auto progress = ResolveAssetProgress(spec.playback,
                                                 sample.assetProgress);
      assetFrameSample = vector::VectorAssetFrameSample{
          vector::kVectorAssetSamplingQtVideoAnimSeqByProgress,
          static_cast<double>(progress)};
      instance.assetTimeUs = binding.assetPlayback.sourceInUs +
                             static_cast<std::int64_t>(std::floor(
                                 static_cast<double>(duration) * progress));
      instance.assetTimeUs =
          std::clamp(instance.assetTimeUs, binding.assetPlayback.sourceInUs,
                     binding.assetPlayback.sourceOutUs - 1);
    }
    basis = composed;
  }
  instance.composedBasis = basis;
  instance.hasComposedBasis = !animations.empty();
}

struct InstanceCost final {
  std::uint64_t pixels{0U};
  bool dimensionExceeded{false};
};

InstanceCost Cost(const DecorationInstance &instance,
                  const DecorationRuntimeLimits &limits) noexcept {
  const auto basis = ResolveDecorationOrthographicBasis(instance);
  const long double width = std::ceil(
      std::fabs(static_cast<long double>(basis.xx) *
                instance.asset.intrinsicWidth) +
      std::fabs(static_cast<long double>(basis.xy) *
                instance.asset.intrinsicHeight));
  const long double height = std::ceil(
      std::fabs(static_cast<long double>(basis.yx) *
                instance.asset.intrinsicWidth) +
      std::fabs(static_cast<long double>(basis.yy) *
                instance.asset.intrinsicHeight));
  const bool dimensionExceeded =
      width > limits.maximumInstanceDimension ||
      height > limits.maximumInstanceDimension;
  const auto maximum =
      static_cast<long double>(std::numeric_limits<std::uint64_t>::max());
  const auto pixels = width * height;
  return {pixels >= maximum ? std::numeric_limits<std::uint64_t>::max()
                            : static_cast<std::uint64_t>(pixels),
          dimensionExceeded};
}

bool IntersectsViewport(const DecorationInstance &instance,
                        const text::TextLayout &layout) noexcept {
  if (layout.outputWidth == 0U || layout.outputHeight == 0U)
    return true;
  const auto basis = ResolveDecorationOrthographicBasis(instance);
  const auto halfWidth =
      (std::fabs(basis.xx) * instance.asset.intrinsicWidth +
       std::fabs(basis.xy) * instance.asset.intrinsicHeight) *
      0.5F;
  const auto halfHeight =
      (std::fabs(basis.yx) * instance.asset.intrinsicWidth +
       std::fabs(basis.yy) * instance.asset.intrinsicHeight) *
      0.5F;
  return instance.localTranslationX - halfWidth <
             static_cast<float>(layout.outputWidth) &&
         instance.localTranslationY - halfHeight <
             static_cast<float>(layout.outputHeight) &&
         instance.localTranslationX + halfWidth > 0.0F &&
         instance.localTranslationY + halfHeight > 0.0F;
}

void Disable(DecorationRenderItem &item, const std::string &code,
             const std::string &message) {
  item.enabled = false;
  item.visible = false;
  item.instances.clear();
  Add(item.diagnostics, code, item.decorationId, message);
}

} // namespace

DecorationOrthographicBasis
ResolveDecorationOrthographicBasis(const DecorationInstance &instance)
    noexcept {
  if (instance.hasComposedBasis)
    return instance.composedBasis;
  return BasisFromScaleAndRotation(instance.localScaleX, instance.localScaleY,
                                   instance.localRotationDegrees);
}

DecorationRasterAffine ResolveDecorationRasterAffine(
    const DecorationInstance &instance, const std::uint32_t sourceWidth,
    const std::uint32_t sourceHeight, const std::int32_t rasterOriginX,
    const std::int32_t rasterOriginY, const float rasterToOutputScaleX,
    const float rasterToOutputScaleY) noexcept {
  const auto basis = ResolveDecorationOrthographicBasis(instance);
  const float sourceCenterX = static_cast<float>(sourceWidth) * 0.5F;
  const float sourceCenterY = static_cast<float>(sourceHeight) * 0.5F;
  const float placeX = static_cast<float>(rasterOriginX) +
                       0.5F * rasterToOutputScaleX;
  const float placeY = static_cast<float>(rasterOriginY) +
                       0.5F * rasterToOutputScaleY;
  DecorationRasterAffine result;
  result.xx = basis.xx * rasterToOutputScaleX;
  result.xy = basis.xy * rasterToOutputScaleY;
  result.yx = basis.yx * rasterToOutputScaleX;
  result.yy = basis.yy * rasterToOutputScaleY;
  // A source texel centre is transformed through the authored basis. The
  // destination pixel centre belongs to output space and is subtracted only
  // after that transform. The half-pixel terms are raster-coordinate rules,
  // not authored effect parameters.
  result.xOffset = instance.localTranslationX - basis.xx * sourceCenterX -
                   basis.xy * sourceCenterY + basis.xx * placeX +
                   basis.xy * placeY - 0.5F;
  result.yOffset = instance.localTranslationY - basis.yx * sourceCenterX -
                   basis.yy * sourceCenterY + basis.yx * placeX +
                   basis.yy * placeY - 0.5F;
  return result;
}

DecorationRenderPlan
SampleDecorationRenderPlan(const TextCompositionDocument &document,
                           const text::TextLayout &localLayout,
                           const DecorationSampleTime &time,
                           const DecorationRuntimeLimits &runtimeLimits) {
  DecorationRenderPlan plan;
  plan.textOnlyFastPath = document.decorations.empty();
  if (plan.textOnlyFastPath)
    return plan;

  DecorationRuntimeLimits limits = runtimeLimits;
  limits.maximumBindings = std::min<std::size_t>(
      limits.maximumBindings, document.decorationBudget.maximumDecorations);
  limits.maximumInstancesPerBinding = std::min<std::size_t>(
      limits.maximumInstancesPerBinding,
      document.decorationBudget.maximumInstancesPerBinding);
  limits.maximumInstances = std::min<std::size_t>(
      limits.maximumInstances, document.decorationBudget.maximumInstances);
  limits.maximumPixelsPerBinding = std::min(
      limits.maximumPixelsPerBinding,
      document.decorationBudget.maximumPixelsPerBinding);
  limits.maximumPixels =
      std::min(limits.maximumPixels, document.decorationBudget.maximumPixels);
  limits.maximumWorkingSetBytes = std::min(
      limits.maximumWorkingSetBytes,
      document.decorationBudget.maximumWorkingSetBytes);
  limits.maximumInstanceDimension = std::min(
      limits.maximumInstanceDimension,
      document.decorationBudget.maximumInstanceDimension);

  const bool timeValid =
      time.compositionEndUs > time.compositionBeginUs &&
      time.absoluteCompositionUs >= time.compositionBeginUs &&
      time.absoluteCompositionUs < time.compositionEndUs &&
      time.sourceWindowEndTicks > time.sourceWindowBeginTicks &&
      time.absoluteSourceTicks >= time.sourceWindowBeginTicks &&
      time.absoluteSourceTicks < time.sourceWindowEndTicks;
  if (!timeValid)
    Add(plan.diagnostics, "text_composition.decoration.time_invalid", {},
        "sample clocks must lie inside non-empty half-open windows");

  if (AuthoredClusters(localLayout).size() >
      limits.maximumLayoutClustersScanned) {
    Add(plan.diagnostics, "text_composition.decoration.layout_budget", {},
        "canonical layout exceeds the bounded cluster scan budget");
    return plan;
  }

  std::unordered_set<std::string> identities;
  std::unordered_set<std::string> authoredResourceDigests;
  std::uint64_t authoredResourceBytes = 0U;
  std::unordered_set<std::string> residentDigests;
  std::uint64_t residentBytes = 0U;
  const auto timedSpans = detail::ResolveTimedSpans(
      document, time.sourceWindowBeginTicks, time.sourceWindowEndTicks);
  const auto count =
      std::min(document.decorations.size(), limits.maximumBindings);
  if (document.decorations.size() > count)
    Add(plan.diagnostics, "text_composition.decoration.binding_budget", {},
        "bindings beyond the runtime budget were omitted",
        DiagnosticSeverity::Warning);

  for (std::size_t bindingIndex = 0U; bindingIndex < count; ++bindingIndex) {
    const auto &binding = document.decorations[bindingIndex];
    DecorationRenderItem item;
    item.decorationId = binding.decorationId;
    item.primaryAsset = binding.asset;
    item.fallbackAssetId = binding.fallbackAssetId;
    item.mode = binding.mode;
    item.zOrder = binding.zOrder;
    item.effectScope = binding.effectScope;
    if (!binding.fallbackAssetId.empty()) {
      const auto fallback = std::find_if(
          document.resources.begin(), document.resources.end(),
          [&](const auto &resource) {
            return resource.resourceId == binding.fallbackAssetId ||
                   resource.assetId == binding.fallbackAssetId;
          });
      if (fallback != document.resources.end())
        item.fallbackAsset = *fallback;
    }
    const auto validation = ValidateVectorDecorationBinding(document, binding);
    item.diagnostics.insert(item.diagnostics.end(),
                            validation.diagnostics.begin(),
                            validation.diagnostics.end());
    if (!validation.usable || !timeValid ||
        !identities.insert(binding.decorationId).second) {
      if (validation.usable && !timeValid)
        Disable(item, "text_composition.decoration.time_unavailable",
                "invalid clocks disable this binding sample only");
      else if (validation.usable)
        Disable(item, "text_composition.decoration.duplicate_id",
                "duplicate decoration identity disables this binding");
      plan.diagnostics.insert(plan.diagnostics.end(), item.diagnostics.begin(),
                              item.diagnostics.end());
      plan.items.push_back(std::move(item));
      continue;
    }
    std::unordered_set<std::string> bindingResourceDigests;
    std::uint64_t bindingResourceBytes = 0U;
    const auto accountAuthoredResource = [&](const auto &asset) {
      if (authoredResourceDigests.count(asset.digest) != 0U ||
          !bindingResourceDigests.insert(asset.digest).second)
        return;
      bindingResourceBytes = SaturatingAdd(
          bindingResourceBytes, asset.estimatedResourceBytes);
    };
    accountAuthoredResource(binding.asset);
    if (binding.instancePattern)
      for (const auto &variant : binding.instancePattern->assetVariants)
        accountAuthoredResource(variant.asset);
    const auto authoredResourceMaximum = std::min(
        document.decorationBudget.maximumResourceBytes,
        TextCompositionLimits{}.maximumDecorationResourceBytes);
    if (bindingResourceBytes >
        authoredResourceMaximum -
            std::min(authoredResourceBytes, authoredResourceMaximum)) {
      Disable(item, "text_composition.decoration.resource_budget",
              "decoration exceeds the remaining authored resource budget");
      plan.diagnostics.insert(plan.diagnostics.end(), item.diagnostics.begin(),
                              item.diagnostics.end());
      plan.items.push_back(std::move(item));
      continue;
    }
    authoredResourceDigests.insert(bindingResourceDigests.begin(),
                                   bindingResourceDigests.end());
    authoredResourceBytes += bindingResourceBytes;
    const auto target = ResolveTarget(document, binding);
    if (!target.valid) {
      Disable(item, "text_composition.decoration.target_stale",
              "decoration target no longer resolves to canonical content");
      plan.diagnostics.insert(plan.diagnostics.end(), item.diagnostics.begin(),
                              item.diagnostics.end());
      plan.items.push_back(std::move(item));
      continue;
    }
    const auto sampledAnimations = time.suppressAnimation
        ? std::vector<SampledDecorationAnimation>{}
        : SampleDecorationAnimations(document, binding.decorationId, time,
                                     timedSpans);
    const bool useAuthoredLetterBounds =
        binding.mode == VectorDecorationMode::CompositionOverlay &&
        std::any_of(sampledAnimations.begin(), sampledAnimations.end(),
                    [](const auto &animation) {
                      return animation.spec &&
                             animation.spec->extentSpace ==
                                 text::TextDecorationExtentSpace::TextLocal;
                    });
    const auto clusters = CollectClusters(localLayout, target.globalRange);
    const auto placements = BuildPlacements(
        binding, target, clusters, localLayout, time,
        useAuthoredLetterBounds);
    item.enabled = true;
    for (const auto &placement : placements) {
      const auto variant = SelectVariant(binding, placement.index);
      if (!variant.asset)
        continue;
      const auto clock = SampleAssetClock(
          binding, time, PatternPhase(binding, variant, placement.index));
      if (!clock.visible)
        continue;
      auto instance = MakeInstance(binding, placement, variant, clock);
      ApplyDecorationAnimations(instance, binding, placement, localLayout,
                                sampledAnimations, item.assetFrameSample);
      item.assetTimeUs = instance.assetTimeUs;
      item.instances.push_back(std::move(instance));
    }
    item.visible = !item.instances.empty();
    if (!item.visible) {
      Add(item.diagnostics, "text_composition.decoration.inactive",
          item.decorationId,
          "target geometry or asset playback is inactive at this sample",
          DiagnosticSeverity::Information);
      plan.diagnostics.insert(plan.diagnostics.end(), item.diagnostics.begin(),
                              item.diagnostics.end());
      plan.items.push_back(std::move(item));
      continue;
    }

    auto measure = [&]() {
      std::uint64_t pixels = 0U;
      bool dimensionExceeded = false;
      for (const auto &instance : item.instances) {
        const auto cost = Cost(instance, limits);
        pixels = SaturatingAdd(pixels, cost.pixels);
        dimensionExceeded = dimensionExceeded || cost.dimensionExceeded;
      }
      return std::pair<std::uint64_t, bool>{pixels, dimensionExceeded};
    };
    const auto initialMeasurement = measure();
    std::uint64_t bindingPixels = initialMeasurement.first;
    bool dimensionExceeded = initialMeasurement.second;
    std::uint64_t newResourceBytes = 0U;
    std::unordered_set<std::string> itemDigests;
    const auto measureResources = [&]() {
      newResourceBytes = 0U;
      itemDigests.clear();
      for (const auto &instance : item.instances) {
        if (itemDigests.insert(instance.asset.digest).second &&
            residentDigests.count(instance.asset.digest) == 0U)
          newResourceBytes = SaturatingAdd(
              newResourceBytes, instance.asset.estimatedResourceBytes);
      }
    };
    measureResources();
    const auto exceeds = [&]() {
      return dimensionExceeded ||
             item.instances.size() > limits.maximumInstancesPerBinding ||
             item.instances.size() >
                 limits.maximumInstances -
                     std::min(plan.instanceCount, limits.maximumInstances) ||
             bindingPixels > limits.maximumPixelsPerBinding ||
             bindingPixels >
                 limits.maximumPixels -
                     std::min(plan.estimatedPixels, limits.maximumPixels) ||
             SaturatingAdd(
                 SaturatingAdd(residentBytes, newResourceBytes),
                 PixelBytes(SaturatingAdd(plan.estimatedPixels,
                                          bindingPixels))) >
                 limits.maximumWorkingSetBytes;
    };
    const auto clipVisible = [&]() {
      item.instances.erase(
          std::remove_if(item.instances.begin(), item.instances.end(),
                         [&](const auto &instance) {
                           return !IntersectsViewport(instance, localLayout);
                         }),
          item.instances.end());
      const auto available = std::min(
          limits.maximumInstancesPerBinding,
          limits.maximumInstances -
              std::min(plan.instanceCount, limits.maximumInstances));
      if (item.instances.size() > available)
        item.instances.resize(available);
      const auto measured = measure();
      bindingPixels = measured.first;
      dimensionExceeded = measured.second;
      measureResources();
    };

    if (exceeds()) {
      item.fallbackApplied = true;
      switch (binding.fallback) {
      case DecorationFallbackPolicy::DisableDecoration:
        Disable(item, "text_composition.decoration.budget_exceeded",
                "decoration exceeds runtime instance, pixel or memory budgets");
        break;
      case DecorationFallbackPolicy::StaticFirstFrame:
        if (item.instances.size() > 1U)
          item.instances.resize(1U);
        for (auto &instance : item.instances)
          instance.assetTimeUs = binding.assetPlayback.sourceInUs;
        item.assetTimeUs = binding.assetPlayback.sourceInUs;
        std::tie(bindingPixels, dimensionExceeded) = measure();
        measureResources();
        if (exceeds())
          Disable(item, "text_composition.decoration.static_budget_exceeded",
                  "first-frame fallback still exceeds runtime budgets");
        break;
      case DecorationFallbackPolicy::NativeBackground:
        item.nativeBackgroundFallback = true;
        clipVisible();
        newResourceBytes = 0U;
        if (item.instances.empty() || exceeds())
          Disable(item, "text_composition.decoration.native_budget_exceeded",
                  "native fallback cannot fit the runtime budgets");
        break;
      case DecorationFallbackPolicy::ClipVisibleInstances:
        clipVisible();
        if (item.instances.empty() || exceeds())
          Disable(item, "text_composition.decoration.clip_budget_exceeded",
                  "no visible instance fits the runtime budgets");
        break;
      }
    }

    if (item.enabled && item.visible && !item.instances.empty()) {
      if (!item.nativeBackgroundFallback) {
        residentDigests.insert(itemDigests.begin(), itemDigests.end());
        residentBytes = SaturatingAdd(residentBytes, newResourceBytes);
      }
      plan.instanceCount += item.instances.size();
      plan.estimatedPixels =
          SaturatingAdd(plan.estimatedPixels, bindingPixels);
      plan.estimatedWorkingSetBytes = SaturatingAdd(
          residentBytes, PixelBytes(plan.estimatedPixels));
    }
    plan.diagnostics.insert(plan.diagnostics.end(), item.diagnostics.begin(),
                            item.diagnostics.end());
    plan.items.push_back(std::move(item));
  }
  plan.textOnlyFastPath = std::none_of(
      plan.items.begin(), plan.items.end(),
      [](const auto &item) { return item.enabled && item.visible; });
  return plan;
}

} // namespace videocut::text_composition
