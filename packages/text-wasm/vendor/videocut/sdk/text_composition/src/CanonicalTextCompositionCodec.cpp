#include "videocut/text/internal/CanonicalByteCodec.h"
#include "videocut/text/internal/CanonicalTextPropertyCodec.h"
#include "videocut/text_composition/TextCompositionDocument.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace videocut::text_composition {
namespace {

constexpr std::string_view kCanonicalDocumentDomain{
    "videocut.text-composition.current"};
constexpr std::size_t kMaximumCanonicalDocumentBytes = 256U * 1024U * 1024U;
constexpr std::size_t kMaximumCanonicalStringBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumCanonicalCollectionElements = 1'000'000U;
constexpr std::size_t kMaximumAnimationStages = 512U;
constexpr std::size_t kMaximumAnimationInstructions = 65'536U;
constexpr std::size_t kMaximumAnimationParameters = 512U;
constexpr std::size_t kMaximumAnimationParameterComponents = 64U;
constexpr std::size_t kMaximumRenderGroupRanges = 16'384U;
constexpr std::size_t kMaximumPropertyAssignments = 16'384U;
constexpr std::size_t kMaximumBackdropLayers = 64U;
constexpr std::size_t kMaximumExecutionGraphNodes = 4'096U;
constexpr std::size_t kMaximumExecutionGraphInputs = 256U;
constexpr std::size_t kMaximumExecutionGraphChildren = 512U;
constexpr std::size_t kMaximumExecutionGraphDepth = 32U;

struct CodecContext final {
  const TextCompositionLimits &limits;

  [[nodiscard]] std::size_t maximumStringBytes() const noexcept {
    return std::min(
        kMaximumCanonicalStringBytes,
        std::max(limits.maximumIdentityBytes,
                 limits.richText.maximumUtf8Bytes));
  }
};

using CanonicalWriter = videocut::text::internal::CanonicalByteWriter;
using CanonicalReader = videocut::text::internal::CanonicalByteReader;

using text::internal::TransferEnum;
using text::internal::TransferOptional;

// Effect Program's name tables are its closed enum authority. Using a second
// last-enumerator list here can reject valid programs at the persistence edge.
// Keep their numeric wire representation unchanged and reject unknown values.
template <typename Archive, typename Enum, typename NameOf>
bool TransferNamedEnum(Archive &archive, Enum &value, NameOf nameOf) {
  using Storage = std::underlying_type_t<Enum>;
  std::uint64_t encoded = static_cast<std::uint64_t>(value);
  if (!archive.Unsigned(encoded) ||
      encoded > static_cast<std::uint64_t>(std::numeric_limits<Storage>::max()))
    return false;
  const auto decoded = static_cast<Enum>(encoded);
  if (nameOf(decoded).empty())
    return false;
  if constexpr (Archive::kReading)
    value = decoded;
  return true;
}

template <typename Archive, typename Value, typename Transfer>
bool TransferVector(Archive &archive, std::vector<Value> &values,
                    const std::size_t maximum, Transfer &&transfer) {
  return text::internal::TransferBoundedVector(
      archive, values, std::min(maximum, kMaximumCanonicalCollectionElements),
      std::forward<Transfer>(transfer));
}

template <typename Archive>
bool TransferBackdropLayerCurrentFields(Archive &archive,
                                        text::TextBackdropLayer &value) {
  const auto transferDimension = [&](float &dimension) {
    return archive.Float(dimension) && dimension > 0.0F &&
           dimension <= 65'536.0F;
  };
  const auto transferInsets = [&](text::Insets &insets) {
    const auto transferInset = [&](float &inset) {
      return archive.Float(inset) && inset >= 0.0F && inset <= 65'536.0F;
    };
    return transferInset(insets.left) && transferInset(insets.top) &&
           transferInset(insets.right) && transferInset(insets.bottom);
  };
  return TransferOptional(archive, value.sourceIntrinsicWidth,
                          transferDimension) &&
         TransferOptional(archive, value.sourceIntrinsicHeight,
                          transferDimension) &&
         value.sourceIntrinsicWidth.has_value() ==
             value.sourceIntrinsicHeight.has_value() &&
         TransferEnum(archive, value.fitMode,
                      text::TextBackdropFitMode::Stretch) &&
         archive.Float(value.sourcePivotX) && value.sourcePivotX >= 0.0F &&
         value.sourcePivotX <= 1.0F && archive.Float(value.sourcePivotY) &&
         value.sourcePivotY >= 0.0F && value.sourcePivotY <= 1.0F &&
         transferInsets(value.expand) && transferInsets(value.sourceOutsets);
}

template <typename Archive>
bool TransferBackdropStackCurrentFields(Archive &archive,
                                        text::TextBackdropStack &value) {
  std::uint64_t count = value.layers.size();
  if (!archive.Count(count, kMaximumBackdropLayers) ||
      count != value.layers.size())
    return false;
  for (auto &layer : value.layers) {
    if (!TransferBackdropLayerCurrentFields(archive, layer))
      return false;
  }
  return true;
}

template <typename Archive>
bool TransferStringVector(Archive &archive, std::vector<std::string> &values,
                          const std::size_t maximum) {
  return TransferVector(archive, values, maximum,
                        [&](auto &value) { return archive.String(value); });
}

template <typename Archive, typename Value>
bool TransferPropertyValue(Archive &archive, Value &value,
                           const CodecContext &context) {
  if constexpr (!Archive::kReading) {
    // The writer traverses its own document copy; transfer that field's payload.
    text::TextPropertyValue property{std::move(value)};
    std::vector<std::uint8_t> bytes;
    if (!text::internal::EncodeCanonicalTextPropertyValueInPlace(
            property, bytes, context.limits.richText))
      return false;
    if (!archive.Bytes(bytes))
      return false;
    if constexpr (std::is_same_v<Value, text::TextBackdropStack>)
      return TransferBackdropStackCurrentFields(
          archive, std::get<text::TextBackdropStack>(property));
    return true;
  } else {
    std::vector<std::uint8_t> bytes;
    text::TextPropertyValue property;
    if (!archive.Bytes(bytes) ||
        !text::DecodeCanonicalTextPropertyValue(bytes, property,
                                                context.limits.richText))
      return false;
    auto *typed = std::get_if<Value>(&property);
    if (typed == nullptr)
      return false;
    value = std::move(*typed);
    if constexpr (std::is_same_v<Value, text::TextBackdropStack>)
      return TransferBackdropStackCurrentFields(archive, value);
    return true;
  }
}

template <typename Archive>
bool TransferReferenceCanvas(Archive &archive, text::ReferenceCanvas &value) {
  return archive.Float(value.width) && value.width > 0.0F &&
         archive.Float(value.height) && value.height > 0.0F &&
         TransferEnum(archive, value.scalePolicy, text::CanvasScalePolicy::None);
}

template <typename Archive>
bool TransferTextStyle(Archive &archive, text::TextStyle &value,
                       const CodecContext &context) {
  return TransferPropertyValue(archive, value.font, context) &&
         archive.Float(value.fontSize) && value.fontSize > 0.0F &&
         archive.Float(value.letterSpacing) &&
         archive.Float(value.wordSpacing) &&
         archive.Float(value.baselineShift) &&
         TransferPropertyValue(archive, value.materials, context) &&
         TransferPropertyValue(archive, value.background, context) &&
         TransferPropertyValue(archive, value.decoration, context);
}

template <typename Archive>
bool TransferRun(Archive &archive, text::RichTextRun &value,
                 const CodecContext &context) {
  return archive.String(value.runId) && archive.String(value.utf8Text) &&
         archive.String(value.locale) &&
         TransferTextStyle(archive, value.style, context);
}

template <typename Archive>
bool TransferParagraph(Archive &archive, text::RichTextParagraph &value,
                       const CodecContext &context) {
  return archive.String(value.paragraphId) &&
         TransferPropertyValue(archive, value.style, context) &&
         TransferVector(archive, value.runs, context.limits.richText.maximumRuns,
                        [&](auto &run) {
                          return TransferRun(archive, run, context);
                        });
}

template <typename Archive>
bool TransferContentSlot(Archive &archive, text::TextContentSlot &value,
                         const CodecContext &context) {
  return archive.String(value.slotId) && archive.String(value.semanticRole) &&
         TransferStringVector(archive, value.paragraphIds,
                              context.limits.richText.maximumParagraphs) &&
         TransferStringVector(archive, value.runIds,
                              context.limits.richText.maximumRuns);
}

template <typename Archive>
bool TransferTimedTextSpan(Archive &archive, TimedTextSpan &value) {
  return archive.String(value.spanId) && archive.String(value.paragraphId) &&
         archive.String(value.runId) && archive.Unsigned(value.range.begin) &&
         archive.Unsigned(value.range.end) && value.range.begin <= value.range.end &&
         archive.Signed(value.startOffsetUs) &&
         archive.Signed(value.endOffsetUs) &&
         value.startOffsetUs <= value.endOffsetUs &&
         archive.String(value.semantic) &&
         TransferEnum(archive, value.progressMode,
                      text::TimedTextProgressMode::GraphemeSweep) &&
         archive.Signed(value.transitionEndOffsetUs);
}

template <typename Archive>
bool TransferTimedTextTrack(Archive &archive, TimedTextTrack &value,
                            const CodecContext &context) {
  return TransferEnum(archive, value.clock, TimedTextClock::ClipSourceLocal) &&
         TransferVector(archive, value.spans,
                        context.limits.richText.maximumTimedSpans,
                        [&](auto &span) {
                          return TransferTimedTextSpan(archive, span);
                        });
}

template <typename Archive>
bool TransferPropertyTarget(Archive &archive, text::TextPropertyTarget &value,
                            const CodecContext &context) {
  return TransferEnum(archive, value.scope,
                      text::TextPropertyScope::BackdropLayer) &&
         archive.String(value.contentSlotId) &&
         TransferStringVector(archive, value.paragraphIds,
                              context.limits.richText.maximumParagraphs) &&
         TransferStringVector(archive, value.runIds,
                              context.limits.richText.maximumRuns) &&
         TransferOptional(archive, value.range,
                          [&](auto &range) {
                            return archive.Unsigned(range.begin) &&
                                   archive.Unsigned(range.end) &&
                                   range.begin <= range.end;
                          }) &&
         archive.String(value.layerId);
}

template <typename Archive>
bool TransferPropertyAssignment(Archive &archive,
                                text::TextPropertyAssignment &value,
                                const CodecContext &context) {
  if (!TransferEnum(archive, value.address.property,
                    text::TextPropertyId::BackdropEnabled) ||
      !TransferPropertyTarget(archive, value.address.target, context) ||
      !TransferEnum(archive, value.disposition,
                    text::TextPropertyDisposition::Inherit) ||
      !TransferEnum(archive, value.combineMode,
                    text::TextPropertyCombineMode::ColorMix))
    return false;
  if constexpr (!Archive::kReading) {
    std::vector<std::uint8_t> bytes;
    if (!text::internal::EncodeCanonicalTextPropertyValueInPlace(
            value.value, bytes, context.limits.richText))
      return false;
    return archive.Bytes(bytes);
  } else {
    std::vector<std::uint8_t> bytes;
    return archive.Bytes(bytes) &&
           text::DecodeCanonicalTextPropertyValue(
               bytes, value.value, context.limits.richText);
  }
}

template <typename Archive>
bool TransferPropertyPatch(Archive &archive, text::TextPropertyPatch &value,
                           const CodecContext &context) {
  return archive.String(value.patchId) &&
         archive.Unsigned(value.baseRevision) &&
         TransferVector(archive, value.assignments,
                        kMaximumPropertyAssignments,
                        [&](auto &assignment) {
                          return TransferPropertyAssignment(archive, assignment,
                                                            context);
                        });
}

template <typename Archive>
bool TransferKeyframe(Archive &archive, text::TextKeyframe &value) {
  return archive.Double(value.offset) && archive.Double(value.value) &&
         archive.Double(value.tangentIn) &&
         archive.Double(value.tangentOut) &&
         archive.Boolean(value.cubicBezier) &&
         archive.Double(value.bezierTimeIn) &&
         archive.Double(value.bezierTimeOut);
}

template <typename Archive>
bool TransferSelectorKeyframe(Archive &archive,
                              text::TextSelectorKeyframe &value) {
  return archive.Double(value.offset) && archive.Double(value.value) &&
         archive.Double(value.tangentIn) &&
         archive.Double(value.tangentOut) &&
         archive.Boolean(value.cubicBezier) &&
         archive.Double(value.bezierTimeIn) &&
         archive.Double(value.bezierTimeOut);
}

template <typename Archive>
bool TransferUnitSelector(Archive &archive, text::TextUnitSelector &value,
                          const CodecContext &context) {
  const auto transferKeyframes = [&](auto &keyframes) {
    return TransferVector(
        archive, keyframes,
        context.limits.richText.maximumAnimationKeyframesPerTrack,
        [&](auto &keyframe) {
          return TransferSelectorKeyframe(archive, keyframe);
        });
  };
  return TransferEnum(archive, value.kind, text::TextSelectorKind::Time) &&
         TransferEnum(archive, value.basedOn, text::TextUnitBasis::All) &&
         archive.Double(value.rangeStart) && archive.Double(value.rangeEnd) &&
         archive.Double(value.offset) && archive.Double(value.stagger) &&
         archive.Double(value.edgeSmooth) &&
         TransferEnum(archive, value.shape, text::TextSelectorShape::Square) &&
         archive.Boolean(value.constrained) &&
         TransferEnum(archive, value.order, text::TextUnitOrder::Random) &&
         archive.Double(value.randomSeed) &&
         archive.Double(value.intensity) &&
         archive.Double(value.intensityStart) &&
         archive.Double(value.intensityEnd) &&
         archive.Double(value.timeStart1) &&
         archive.Double(value.timeStart2) &&
         archive.Double(value.timeEnd1) && archive.Double(value.timeEnd2) &&
         archive.Boolean(value.timeCycle) &&
         transferKeyframes(value.rangeStartKeyframes) &&
         transferKeyframes(value.rangeEndKeyframes) &&
         transferKeyframes(value.offsetKeyframes) &&
         transferKeyframes(value.intensityKeyframes);
}

template <typename Archive>
bool TransferAnimatorTrack(Archive &archive, text::TextAnimatorTrack &value,
                           const CodecContext &context) {
  return TransferEnum(archive, value.property,
                      text::TextAnimatedProperty::DistanceFromCenter) &&
         TransferVector(
             archive, value.keyframes,
             context.limits.richText.maximumAnimationKeyframesPerTrack,
             [&](auto &keyframe) {
               return TransferKeyframe(archive, keyframe);
             });
}

template <typename Archive>
bool TransferColorComponents(Archive &archive, text::Color &value) {
  return archive.Float(value.red) && archive.Float(value.green) &&
         archive.Float(value.blue) && archive.Float(value.alpha);
}

template <typename Archive>
bool TransferColor(Archive &archive, text::Color &value) {
  if (!TransferColorComponents(archive, value))
    return false;
  const auto valid = [](const float component) {
    return component >= 0.0F && component <= 1.0F;
  };
  return valid(value.red) && valid(value.green) && valid(value.blue) &&
         valid(value.alpha);
}

template <typename Archive>
bool TransferColorKeyframe(Archive &archive,
                           text::TextColorKeyframe &value) {
  return archive.Float(value.offset) &&
         TransferColor(archive, value.value) &&
         TransferColorComponents(archive, value.tangentIn) &&
         TransferColorComponents(archive, value.tangentOut) &&
         archive.Boolean(value.cubicBezier) &&
         archive.Float(value.bezierTimeIn) &&
         archive.Float(value.bezierTimeOut);
}

template <typename Archive>
bool TransferRevealCursor(Archive &archive, text::RevealCursor &value) {
  return archive.Boolean(value.enabled) &&
         TransferColor(archive, value.color) && archive.Float(value.width) &&
         value.width >= 0.0F && archive.Signed(value.periodUs) &&
         value.periodUs > 0 && archive.Float(value.dutyCycle) &&
         value.dutyCycle >= 0.0F && value.dutyCycle <= 1.0F;
}

template <typename Archive>
bool TransferAnimator(Archive &archive, text::TextAnimatorSpec &value,
                      const CodecContext &context) {
  return archive.String(value.animatorId) &&
         TransferStringVector(archive, value.paragraphIds,
                              context.limits.richText.maximumParagraphs) &&
         TransferStringVector(archive, value.runIds,
                              context.limits.richText.maximumRuns) &&
         TransferVector(archive, value.selectors,
                        context.limits.richText.maximumAnimationTracks,
                        [&](auto &selector) {
                          return TransferUnitSelector(archive, selector,
                                                      context);
                        }) &&
         TransferVector(archive, value.tracks,
                        context.limits.richText.maximumAnimationTracks,
                        [&](auto &track) {
                          return TransferAnimatorTrack(archive, track, context);
                        }) &&
         TransferVector(
             archive, value.fillColorKeyframes,
             context.limits.richText.maximumAnimationKeyframesPerTrack,
             [&](auto &keyframe) {
               return TransferColorKeyframe(archive, keyframe);
             }) &&
         archive.String(value.effectProgramId) &&
         TransferEnum(archive, value.positionMode,
                      text::TextPositionMode::SpaceY) &&
         TransferEnum(archive, value.anchor, text::TextUnitAnchor::LineBox) &&
         TransferEnum(archive, value.anchorBasis,
                      text::TextAnimatorAnchorBasis::Page) &&
         TransferEnum(archive, value.anchorMode,
                      text::TextAnimatorAnchorMode::SelectorInfluenced) &&
         archive.Float(value.anchorOffsetX) &&
         archive.Float(value.anchorOffsetY) &&
         TransferEnum(archive, value.projection.kind,
                      text::TextProjectionKind::Perspective3D) &&
         archive.Float(value.projection.fieldOfViewDegrees) &&
         value.projection.fieldOfViewDegrees > 0.0F &&
         value.projection.fieldOfViewDegrees < 180.0F &&
         archive.Float(value.projection.vanishingPointX) &&
         archive.Float(value.projection.vanishingPointY) &&
         TransferEnum(archive, value.presentation,
                      text::TextUnitPresentation::ActiveFill) &&
         archive.Float(value.fadeFraction) && value.fadeFraction >= 0.0F &&
         value.fadeFraction <= 1.0F &&
         TransferColor(archive, value.activeColor) &&
         TransferRevealCursor(archive, value.cursor);
}

template <typename Archive>
bool TransferTimeDriver(Archive &archive,
                        text::TextAnimationTimeDriver &value,
                        const CodecContext &context) {
  if (!TransferEnum(archive, value.kind,
                    text::TextAnimationTimeDriverKind::TimedRanges) ||
      !archive.Signed(value.startOffsetUs) ||
      !archive.Signed(value.durationUs) || value.durationUs < 0 ||
      !TransferEnum(archive, value.playback,
                    text::TextAnimationPlaybackMode::Hold) ||
      !TransferOptional(
          archive, value.timedRanges,
          [&](auto &timed) {
            return TransferEnum(archive, timed.mode,
                                text::TextTimedDriverMode::ActiveHold) &&
                   TransferStringVector(
                       archive, timed.spanIds,
                       context.limits.richText.maximumTimedSpans) &&
                   TransferOptional(archive, timed.activePatch,
                                    [&](auto &patch) {
                                      return TransferPropertyPatch(
                                          archive, patch, context);
                                    });
          }))
    return false;
  return true;
}

template <typename Archive>
bool TransferLayerAnimationKeyframe(Archive &archive,
                                    text::TextLayerAnimationKeyframe &value) {
  return archive.Float(value.offset) && archive.Float(value.value) &&
         TransferEnum(archive, value.easing,
                      text::TextAnimationEasing::EaseInOut);
}

template <typename Archive>
bool TransferLayerAnimationTrack(Archive &archive,
                                 text::TextLayerAnimationTrack &value,
                                 const CodecContext &context) {
  return TransferEnum(archive, value.property,
                      text::TextLayerAnimationProperty::RotationDegrees) &&
         TransferVector(
             archive, value.keyframes,
             context.limits.richText.maximumAnimationKeyframesPerTrack,
             [&](auto &keyframe) {
               return TransferLayerAnimationKeyframe(archive, keyframe);
             });
}

template <typename Archive>
bool TransferLayerAnimationClip(Archive &archive,
                                text::TextLayerAnimationClip &value,
                                const CodecContext &context) {
  return archive.String(value.clipId) && archive.String(value.presetId) &&
         TransferEnum(archive, value.phase,
                      text::TextLayerAnimationPhase::Caption) &&
         archive.Signed(value.durationUs) && value.durationUs > 0 &&
         TransferVector(archive, value.tracks,
                        context.limits.richText.maximumAnimationTracks,
                        [&](auto &track) {
                          return TransferLayerAnimationTrack(archive, track,
                                                             context);
                        });
}

template <typename Archive>
bool TransferPostEffectParameter(Archive &archive,
                                 text::TextPostEffectParameter &value,
                                 const CodecContext &context) {
  return archive.String(value.name) &&
         TransferVector(archive, value.values,
                        kMaximumAnimationParameterComponents,
                        [&](auto &component) {
                          return archive.Float(component);
                        }) &&
         TransferVector(
             archive, value.keyframes,
             context.limits.richText.maximumAnimationKeyframesPerTrack,
             [&](auto &keyframe) {
               return TransferKeyframe(archive, keyframe);
             });
}

template <typename Archive>
bool TransferPostEffect(Archive &archive, text::TextPostEffectSpec &value,
                        const CodecContext &context) {
  return archive.String(value.effectId) &&
         TransferEnum(archive, value.kind,
                      text::TextPostEffectKind::PulseEnvelope) &&
         TransferVector(archive, value.inputIds, 64U,
                        [&](auto &inputId) {
                          return archive.String(inputId);
                        }) &&
         archive.Float(value.amount) && archive.Float(value.paddingPx) &&
         value.paddingPx >= 0.0F &&
         TransferVector(
             archive, value.amountKeyframes,
             context.limits.richText.maximumAnimationKeyframesPerTrack,
             [&](auto &keyframe) {
               return TransferKeyframe(archive, keyframe);
             }) &&
         TransferVector(archive, value.parameters,
                        kMaximumAnimationParameters,
                        [&](auto &parameter) {
                          return TransferPostEffectParameter(archive, parameter,
                                                             context);
                        });
}

template <typename Archive>
bool TransferDecorationAnchorKeyframe(
    Archive &archive, text::TextDecorationAnchorKeyframe &value) {
  return archive.Float(value.offset) &&
         TransferEnum(archive, value.value,
                      text::TextAnimatedDecorationAnchor::CanvasCenter);
}

template <typename Archive>
bool TransferDecorationFitKeyframe(
    Archive &archive, text::TextDecorationFitKeyframe &value) {
  return archive.Float(value.offset) &&
         TransferEnum(archive, value.value,
                      text::TextAnimatedDecorationFit::FitShortSide);
}

template <typename Archive>
bool TransferDecorationAnimation(
    Archive &archive, text::TextDecorationAnimationSpec &value,
    const CodecContext &context) {
  const auto transferKeyframes = [&](auto &keyframes) {
    return TransferVector(
        archive, keyframes,
        context.limits.richText.maximumAnimationKeyframesPerTrack,
        [&](auto &keyframe) { return TransferKeyframe(archive, keyframe); });
  };
  return archive.String(value.decorationId) && archive.String(value.assetId) &&
         TransferEnum(archive, value.anchor,
                      text::TextAnimatedDecorationAnchor::CanvasCenter) &&
         TransferVector(
             archive, value.anchorKeyframes,
             context.limits.richText.maximumAnimationKeyframesPerTrack,
             [&](auto &keyframe) {
               return TransferDecorationAnchorKeyframe(archive, keyframe);
             }) &&
         TransferEnum(archive, value.fit,
                      text::TextAnimatedDecorationFit::FitShortSide) &&
         TransferVector(
             archive, value.fitKeyframes,
             context.limits.richText.maximumAnimationKeyframesPerTrack,
             [&](auto &keyframe) {
               return TransferDecorationFitKeyframe(archive, keyframe);
             }) &&
         TransferEnum(archive, value.extentSpace,
                      text::TextDecorationExtentSpace::CanvasFull) &&
         TransferEnum(archive, value.inherit,
                      text::TextDecorationTransformInherit::TranslateOnly) &&
         TransferEnum(archive, value.playback,
                      text::TextAnimationPlaybackMode::Hold) &&
         archive.Float(value.expandRatioX) && value.expandRatioX > 0.0F &&
         archive.Float(value.expandRatioY) && value.expandRatioY > 0.0F &&
         archive.Float(value.sourceOutsets.left) &&
         value.sourceOutsets.left >= 0.0F &&
         value.sourceOutsets.left <= 65'536.0F &&
         archive.Float(value.sourceOutsets.top) &&
         value.sourceOutsets.top >= 0.0F &&
         value.sourceOutsets.top <= 65'536.0F &&
         archive.Float(value.sourceOutsets.right) &&
         value.sourceOutsets.right >= 0.0F &&
         value.sourceOutsets.right <= 65'536.0F &&
         archive.Float(value.sourceOutsets.bottom) &&
         value.sourceOutsets.bottom >= 0.0F &&
         value.sourceOutsets.bottom <= 65'536.0F &&
         archive.Float(value.pivotX) && archive.Float(value.pivotY) &&
         archive.Float(value.offsetX) && archive.Float(value.offsetY) &&
         archive.Float(value.relativeOffsetX) &&
         archive.Float(value.relativeOffsetY) && archive.Float(value.scaleX) &&
         value.scaleX > 0.0F && archive.Float(value.scaleY) &&
         value.scaleY > 0.0F && archive.Float(value.rotationXDegrees) &&
         archive.Float(value.rotationYDegrees) &&
         archive.Float(value.rotationDegrees) && archive.Float(value.opacity) &&
         value.opacity >= 0.0F && value.opacity <= 1.0F &&
         transferKeyframes(value.pivotXKeyframes) &&
         transferKeyframes(value.pivotYKeyframes) &&
         transferKeyframes(value.offsetXKeyframes) &&
         transferKeyframes(value.offsetYKeyframes) &&
         transferKeyframes(value.relativeOffsetXKeyframes) &&
         transferKeyframes(value.relativeOffsetYKeyframes) &&
         transferKeyframes(value.scaleXKeyframes) &&
         transferKeyframes(value.scaleYKeyframes) &&
         transferKeyframes(value.rotationXKeyframes) &&
         transferKeyframes(value.rotationYKeyframes) &&
         transferKeyframes(value.rotationKeyframes) &&
         transferKeyframes(value.opacityKeyframes) &&
         transferKeyframes(value.assetProgressKeyframes);
}

template <typename Archive>
bool TransferRenderGroupMode(Archive &archive,
                             text::TextRenderGroupMode &value) {
  std::uint64_t encoded = static_cast<std::uint64_t>(value);
  if (!archive.Unsigned(encoded))
    return false;
  if (encoded > static_cast<std::uint64_t>(text::TextRenderGroupMode::PerWord) &&
      encoded != static_cast<std::uint64_t>(text::TextRenderGroupMode::Custom))
    return false;
  if constexpr (Archive::kReading)
    value = static_cast<text::TextRenderGroupMode>(encoded);
  return true;
}

template <typename Archive>
bool TransferRenderGroupTimeRange(Archive &archive,
                                  text::TextRenderGroupTimeRange &value) {
  return archive.Signed(value.startTimeUs) &&
         archive.Signed(value.endTimeUs) &&
         value.startTimeUs < value.endTimeUs;
}

template <typename Archive>
bool TransferRenderGroupCustomRange(Archive &archive,
                                    text::TextRenderGroupCustomRange &value) {
  return archive.Signed(value.startIndex) &&
         archive.Signed(value.endIndex) && value.startIndex <= value.endIndex &&
         TransferOptional(archive, value.localTime,
                          [&](auto &range) {
                            return TransferRenderGroupTimeRange(archive, range);
                          }) &&
         archive.Float(value.intensity);
}

template <typename Archive>
bool TransferRenderGroup(Archive &archive, text::TextRenderGroupSpec &value) {
  std::int64_t priority = value.priority;
  std::uint64_t randomSeed = value.randomSeed;
  if (!archive.Float(value.expandRatioX) || value.expandRatioX <= 0.0F ||
      !archive.Float(value.expandRatioY) || value.expandRatioY <= 0.0F ||
      !TransferRenderGroupMode(archive, value.mode) ||
      !archive.Float(value.offset) ||
      !TransferOptional(archive, value.duration,
                        [&](auto &duration) {
                          return TransferRenderGroupTimeRange(archive, duration);
                        }) ||
      !archive.Signed(priority) ||
      priority < std::numeric_limits<std::int32_t>::min() ||
      priority > std::numeric_limits<std::int32_t>::max() ||
      !archive.Unsigned(randomSeed) ||
      randomSeed > std::numeric_limits<std::uint32_t>::max() ||
      !archive.Boolean(value.randomSort) ||
      !TransferEnum(archive, value.shape, text::TextSelectorShape::Square) ||
      !TransferVector(archive, value.customRanges, kMaximumRenderGroupRanges,
                      [&](auto &range) {
                        return TransferRenderGroupCustomRange(archive, range);
                      }))
    return false;
  if constexpr (Archive::kReading) {
    value.priority = static_cast<std::int32_t>(priority);
    value.randomSeed = static_cast<std::uint32_t>(randomSeed);
  }
  return true;
}

template <typename Archive>
bool TransferEffectInstruction(Archive &archive,
                               text::TextEffectInstruction &value) {
  std::uint64_t outputRegister = value.outputRegister;
  if (!TransferNamedEnum(archive, value.opcode, text::TextEffectOpcodeName) ||
      !archive.Unsigned(outputRegister) ||
      outputRegister > std::numeric_limits<std::uint32_t>::max() ||
      !TransferVector(
          archive, value.inputRegisters, kMaximumAnimationInstructions,
          [&](std::uint32_t &input) {
            std::uint64_t encoded = input;
            if (!archive.Unsigned(encoded) ||
                encoded > std::numeric_limits<std::uint32_t>::max())
              return false;
            if constexpr (Archive::kReading)
              input = static_cast<std::uint32_t>(encoded);
            return true;
          }) ||
      !TransferVector(archive, value.immediates,
                      kMaximumAnimationInstructions,
                      [&](double &immediate) {
                        return archive.Double(immediate);
                      }) ||
      !TransferOptional(archive, value.input,
                        [&](auto &input) {
                          return TransferNamedEnum(archive, input,
                                                   text::TextEffectInputName);
                        }))
    return false;
  if constexpr (Archive::kReading)
    value.outputRegister = static_cast<std::uint32_t>(outputRegister);
  return true;
}

template <typename Archive>
bool TransferEffectExecutionParameterBinding(
    Archive &archive, text::TextEffectExecutionParameterBinding &value) {
  std::uint64_t slot = value.slot;
  if (!archive.String(value.nodeId) ||
      !TransferNamedEnum(archive, value.parameter,
                         text::TextEffectExecutionParameterKindName) ||
      !TransferNamedEnum(archive, value.domain,
                         text::TextEffectExecutionParameterDomainName) ||
      !TransferNamedEnum(archive, value.valueSpace,
                         text::TextEffectExecutionParameterSpaceName) ||
      !archive.Unsigned(slot) ||
      slot > std::numeric_limits<std::uint32_t>::max() ||
      !TransferVector(
          archive, value.registerIndices,
          kMaximumAnimationParameterComponents,
          [&](std::uint32_t &registerIndex) {
            std::uint64_t encoded = registerIndex;
            if (!archive.Unsigned(encoded) ||
                encoded > std::numeric_limits<std::uint32_t>::max()) {
              return false;
            }
            if constexpr (Archive::kReading)
              registerIndex = static_cast<std::uint32_t>(encoded);
            return true;
          })) {
    return false;
  }
  if constexpr (Archive::kReading)
    value.slot = static_cast<std::uint32_t>(slot);
  return true;
}

template <typename Archive>
bool TransferEffectOutputBinding(Archive &archive,
                                 text::TextEffectOutputBinding &value) {
  std::uint64_t registerIndex = value.registerIndex;
  std::uint64_t transformIndex = value.transformIndex;
  if (!TransferNamedEnum(archive, value.output, text::TextEffectOutputName) ||
      !archive.Unsigned(registerIndex) ||
      registerIndex > std::numeric_limits<std::uint32_t>::max() ||
      !archive.Unsigned(transformIndex) ||
      transformIndex > std::numeric_limits<std::uint32_t>::max())
    return false;
  if constexpr (Archive::kReading) {
    value.registerIndex = static_cast<std::uint32_t>(registerIndex);
    value.transformIndex = static_cast<std::uint32_t>(transformIndex);
  }
  return true;
}

template <typename Archive>
bool TransferEffectStage(Archive &archive,
                         text::TextEffectProgramStageIR &value) {
  std::uint64_t registerCount = value.registerCount;
  if (!archive.String(value.stageId) ||
      !TransferNamedEnum(archive, value.kind, text::TextEffectStageKindName) ||
      !archive.Unsigned(registerCount) ||
      registerCount > std::numeric_limits<std::uint32_t>::max() ||
      !TransferVector(archive, value.instructions,
                      kMaximumAnimationInstructions,
                      [&](auto &instruction) {
                        return TransferEffectInstruction(archive, instruction);
                      }) ||
      !TransferVector(archive, value.outputs,
                      kMaximumAnimationInstructions,
                      [&](auto &output) {
                        return TransferEffectOutputBinding(archive, output);
                      }) ||
      !TransferVector(
          archive, value.executionParameterBindings,
          kMaximumAnimationParameters,
          [&](auto &binding) {
            return TransferEffectExecutionParameterBinding(archive, binding);
          }))
    return false;
  if constexpr (Archive::kReading)
    value.registerCount = static_cast<std::uint32_t>(registerCount);
  return true;
}

template <typename Archive>
bool TransferEffectProgram(Archive &archive, text::TextEffectProgramIR &value) {
  return archive.String(value.programId) &&
         archive.Unsigned(value.randomSeed) &&
         TransferVector(archive, value.stages, kMaximumAnimationStages,
                        [&](auto &stage) {
                          return TransferEffectStage(archive, stage);
                        });
}

template <typename Archive>
bool TransferAnimationLayer(Archive &archive,
                            text::TextAnimationLayerSpec &value,
                            const CodecContext &context) {
  return archive.String(value.layerId) && archive.Boolean(value.enabled) &&
         TransferPropertyTarget(archive, value.target, context) &&
         TransferEnum(archive, value.combineMode,
                      text::TextPropertyCombineMode::ColorMix) &&
         TransferTimeDriver(archive, value.timeDriver, context) &&
         TransferOptional(archive, value.layerTrack,
                          [&](auto &track) {
                            return TransferLayerAnimationClip(archive, track,
                                                              context);
                          }) &&
         TransferVector(archive, value.animators,
                        context.limits.richText.maximumAnimationTracks,
                        [&](auto &animator) {
                          return TransferAnimator(archive, animator, context);
                        }) &&
         TransferOptional(archive, value.renderGroup,
                          [&](auto &renderGroup) {
                            return TransferRenderGroup(archive, renderGroup);
                          }) &&
         TransferVector(archive, value.decorations,
                        context.limits.maximumDecorations,
                        [&](auto &decoration) {
                          return TransferDecorationAnimation(archive, decoration,
                                                             context);
                        }) &&
         TransferVector(archive, value.postEffects,
                        context.limits.richText.maximumAnimationTracks,
                        [&](auto &effect) {
                          return TransferPostEffect(archive, effect, context);
                        }) &&
         archive.Boolean(value.requiresTimedText);
}

template <typename Archive>
bool TransferPhysicsSpec(Archive &archive, text::TextEffectPhysicsSpec &value) {
  std::uint64_t fixedStepHz = value.fixedStepHz;
  std::uint64_t angleSeedStride = value.angleSeedStride;
  std::uint64_t speedSeedStride = value.speedSeedStride;
  std::uint64_t rotationSeedStride = value.rotationSeedStride;
  if (!archive.Unsigned(fixedStepHz) ||
      fixedStepHz > std::numeric_limits<std::uint32_t>::max() ||
      !archive.Float(value.gravityX) || !archive.Float(value.gravityY) ||
      !archive.Float(value.angularDamping) ||
      !archive.Signed(value.explosionDelayUs) ||
      !archive.Signed(value.explosionForceDurationUs) ||
      !archive.Signed(value.explosionRampUpUs) ||
      !archive.Float(value.explosionAccelerationMin) ||
      !archive.Float(value.explosionAccelerationMax) ||
      !archive.Float(value.explosionOriginYShiftFactor) ||
      !archive.Float(value.explosionAngleRangeDegrees) ||
      !archive.Float(value.initialAngularVelocityScale) ||
      !archive.Unsigned(value.randomSeed) || !archive.Float(value.randomPhase) ||
      !archive.Float(value.randomScale) ||
      !archive.Unsigned(angleSeedStride) ||
      angleSeedStride > std::numeric_limits<std::uint32_t>::max() ||
      !archive.Unsigned(speedSeedStride) ||
      speedSeedStride > std::numeric_limits<std::uint32_t>::max() ||
      !archive.Unsigned(rotationSeedStride) ||
      rotationSeedStride > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  if constexpr (Archive::kReading) {
    value.fixedStepHz = static_cast<std::uint32_t>(fixedStepHz);
    value.angleSeedStride = static_cast<std::uint32_t>(angleSeedStride);
    value.speedSeedStride = static_cast<std::uint32_t>(speedSeedStride);
    value.rotationSeedStride = static_cast<std::uint32_t>(rotationSeedStride);
  }
  return true;
}

template <typename Archive>
bool TransferCollisionSpec(Archive &archive,
                           text::TextEffectCollisionSpec &value) {
  return archive.Float(value.fixedBoxWidth) &&
         archive.Float(value.fixedBoxHeight) &&
         archive.Float(value.boxCenterXScale) &&
         archive.Float(value.wallDamping) &&
         archive.Float(value.sideWallVelocityScale) &&
         archive.Float(value.topWallVelocityScale) &&
         archive.Float(value.bottomWallDamping) &&
         archive.Float(value.wallTorqueScale) &&
         archive.Float(value.collisionBoundaryScale) &&
         archive.Float(value.collisionRestitution) &&
         archive.Float(value.collisionTorqueScale) &&
         archive.Float(value.minimumCellSize) &&
         archive.Float(value.cellSizeScale) &&
         archive.Boolean(value.expandHorizontalToContent) &&
         archive.Boolean(value.expandTopToContent) &&
         archive.Boolean(value.collideLeft) &&
         archive.Boolean(value.collideRight) &&
         archive.Boolean(value.collideTop) &&
         archive.Boolean(value.collideBottom);
}

template <typename Archive>
bool TransferExecutionStaticAffine(
    Archive &archive, text::TextEffectExecutionStaticAffine &value) {
  return archive.Float(value.translationX) &&
         archive.Float(value.translationY) && archive.Float(value.scaleX) &&
         archive.Float(value.scaleY) &&
         archive.Float(value.rotationDegrees) && archive.Float(value.pivotX) &&
         archive.Float(value.pivotY);
}

template <typename Archive>
bool TransferExecutionCamera(Archive &archive,
                             text::TextEffectExecutionCamera &value) {
  for (auto &component : value.worldToClip) {
    if (!archive.Float(component))
      return false;
  }
  for (auto &component : value.viewport) {
    if (!archive.Float(component))
      return false;
  }
  return true;
}

template <typename Archive>
bool TransferExecutionNode(Archive &archive,
                           text::TextEffectExecutionNode &value,
                           const CodecContext &context,
                           const std::size_t depth,
                           std::size_t &transferredNodeCount) {
  if (depth > kMaximumExecutionGraphDepth ||
      transferredNodeCount == kMaximumExecutionGraphNodes)
    return false;
  ++transferredNodeCount;
  return archive.String(value.nodeId) &&
         archive.String(value.ownerLayerId) &&
         TransferEnum(archive, value.kind,
                      text::TextEffectExecutionNodeKind::Operator) &&
         TransferEnum(
             archive, value.capability,
             text::TextEffectExecutionCapability::
                 MaterialNoiseThresholdDissolve) &&
         TransferStringVector(archive, value.inputIds,
                              kMaximumExecutionGraphInputs) &&
         TransferStringVector(archive, value.resourceIds,
                              kMaximumExecutionGraphInputs) &&
         TransferOptional(
             archive, value.postEffectKind,
             [&](auto &kind) {
               return TransferEnum(
                   archive, kind,
                   text::TextPostEffectKind::PulseEnvelope);
             }) &&
         archive.String(value.stateId) && archive.Unsigned(value.randomSeed) &&
         archive.String(value.historyId) &&
         TransferOptional(archive, value.timeDriver,
                          [&](auto &driver) {
                            return TransferTimeDriver(archive, driver, context);
                          }) &&
         TransferOptional(archive, value.physicsSpec, [&](auto &spec) {
           return TransferPhysicsSpec(archive, spec);
         }) &&
         TransferOptional(archive, value.collisionSpec, [&](auto &spec) {
           return TransferCollisionSpec(archive, spec);
         }) &&
         TransferOptional(archive, value.staticAffine, [&](auto &affine) {
           return TransferExecutionStaticAffine(archive, affine);
         }) &&
         TransferOptional(archive, value.camera, [&](auto &camera) {
           return TransferExecutionCamera(archive, camera);
         }) &&
         TransferVector(
             archive, value.children, kMaximumExecutionGraphChildren,
             [&](auto &child) {
               return TransferExecutionNode(archive, child, context,
                                            depth + 1U,
                                            transferredNodeCount);
             });
}

template <typename Archive>
bool TransferExecutionGraph(Archive &archive,
                            text::TextEffectExecutionGraph &value,
                            const CodecContext &context) {
  std::size_t transferredNodeCount = 0U;
  return TransferVector(
      archive, value.nodes, kMaximumExecutionGraphNodes, [&](auto &node) {
        return TransferExecutionNode(archive, node, context, 0U,
                                     transferredNodeCount);
      });
}

template <typename Archive>
bool TransferAnimationStack(Archive &archive, text::TextAnimationStack &value,
                            const CodecContext &context) {
  return TransferVector(archive, value.effectPrograms,
                        context.limits.richText.maximumAnimationTracks,
                        [&](auto &program) {
                          return TransferEffectProgram(archive, program);
                        }) &&
         TransferVector(archive, value.layers,
                        context.limits.richText.maximumAnimationTracks,
                        [&](auto &layer) {
                          return TransferAnimationLayer(archive, layer, context);
                        }) &&
         TransferExecutionGraph(archive, value.executionGraph, context);
}

template <typename Archive>
bool TransferVectorFieldValue(Archive &archive,
                              vector::VectorFieldValue &value) {
  if (!TransferEnum(archive, value.kind, vector::VectorFieldKind::Number))
    return false;
  switch (value.kind) {
  case vector::VectorFieldKind::Color:
    return archive.Float(value.color.red) && value.color.red >= 0.0F &&
           value.color.red <= 1.0F && archive.Float(value.color.green) &&
           value.color.green >= 0.0F && value.color.green <= 1.0F &&
           archive.Float(value.color.blue) && value.color.blue >= 0.0F &&
           value.color.blue <= 1.0F && archive.Float(value.color.alpha) &&
           value.color.alpha >= 0.0F && value.color.alpha <= 1.0F;
  case vector::VectorFieldKind::Text:
    return archive.String(value.text);
  case vector::VectorFieldKind::Number:
    return archive.Double(value.number);
  }
  return false;
}

template <typename Archive>
bool TransferVectorFieldOverride(Archive &archive,
                                 vector::VectorFieldOverride &value) {
  return archive.String(value.fieldId) &&
         TransferVectorFieldValue(archive, value.value);
}

template <typename Archive>
bool TransferDecorationAssetReference(Archive &archive,
                                      DecorationAssetReference &value,
                                      const CodecContext &context) {
  return archive.String(value.assetId) && archive.String(value.digest) &&
         archive.String(value.mediaType) &&
         archive.Float(value.intrinsicWidth) && value.intrinsicWidth > 0.0F &&
         archive.Float(value.intrinsicHeight) && value.intrinsicHeight > 0.0F &&
         archive.Unsigned(value.estimatedResourceBytes) &&
         value.estimatedResourceBytes <=
             context.limits.maximumSingleDecorationResourceBytes &&
         TransferVector(archive, value.staticFieldOverrides,
                        context.limits.maximumOverridesPerDecoration,
                        [&](auto &field) {
                          return TransferVectorFieldOverride(archive, field);
                        });
}

template <typename Archive>
bool TransferDecorationTarget(Archive &archive, DecorationTarget &value) {
  return TransferEnum(archive, value.scope,
                      DecorationTargetScope::Utf8Range) &&
         archive.String(value.paragraphId) && archive.String(value.runId) &&
         archive.Unsigned(value.range.begin) &&
         archive.Unsigned(value.range.end) &&
         value.range.begin <= value.range.end;
}

template <typename Archive>
bool TransferDecorationInstancePattern(Archive &archive,
                                       DecorationInstancePattern &value,
                                       const CodecContext &context) {
  return TransferVector(
             archive, value.assetVariants,
             context.limits.maximumVariantsPerDecoration,
             [&](auto &variant) {
               return archive.String(variant.variantId) &&
                      TransferDecorationAssetReference(archive, variant.asset,
                                                       context);
             }) &&
         TransferEnum(archive, value.selectionPolicy,
                      DecorationVariantSelectionPolicy::ExplicitPattern) &&
         TransferVector(
             archive, value.explicitPattern,
             context.limits.maximumExplicitPatternLength,
             [&](std::uint32_t &index) {
               std::uint64_t encoded = index;
               if (!archive.Unsigned(encoded) ||
                   encoded > std::numeric_limits<std::uint32_t>::max())
                 return false;
               if constexpr (Archive::kReading)
                 index = static_cast<std::uint32_t>(encoded);
               return true;
             }) &&
         [&]() {
           std::uint64_t seed = value.randomSeed;
           if (!archive.Unsigned(seed) ||
               seed > std::numeric_limits<std::uint32_t>::max())
             return false;
           if constexpr (Archive::kReading)
             value.randomSeed = static_cast<std::uint32_t>(seed);
           return true;
         }() &&
         archive.Float(value.offsetXStep) &&
         archive.Float(value.offsetYStep) && archive.Float(value.scaleStep) &&
         archive.Float(value.rotationStepDegrees) &&
         TransferEnum(archive, value.phasePolicy,
                      DecorationPerIndexPhasePolicy::DeterministicSeed) &&
         archive.Signed(value.phaseStepUs);
}

template <typename Archive>
bool TransferDecorationLocalTransform(Archive &archive,
                                      DecorationLocalTransform &value) {
  return archive.Float(value.offsetX) && archive.Float(value.offsetY) &&
         archive.Float(value.scaleX) && value.scaleX > 0.0F &&
         archive.Float(value.scaleY) && value.scaleY > 0.0F &&
         archive.Float(value.rotationDegrees) && archive.Float(value.opacity) &&
         value.opacity >= 0.0F && value.opacity <= 1.0F;
}

template <typename Archive>
bool TransferDecorationAssetPlayback(Archive &archive,
                                     DecorationAssetPlayback &value,
                                     const CodecContext &context) {
  return TransferEnum(archive, value.clock, DecorationAssetClock::Target) &&
         TransferEnum(archive, value.mode, DecorationPlaybackMode::Hold) &&
         archive.Signed(value.sourceInUs) && value.sourceInUs >= 0 &&
         archive.Signed(value.sourceOutUs) &&
         value.sourceOutUs > value.sourceInUs &&
         value.sourceOutUs <= context.limits.maximumAssetDurationUs &&
         archive.Double(value.speed) && value.speed > 0.0 &&
         archive.Signed(value.phaseUs);
}

template <typename Archive>
bool TransferDecorationBinding(Archive &archive,
                               VectorDecorationBinding &value,
                               const CodecContext &context) {
  std::int64_t zOrder = value.zOrder;
  if (!archive.String(value.decorationId) ||
      !archive.Boolean(value.enabled) ||
      !TransferDecorationAssetReference(archive, value.asset, context) ||
      !TransferOptional(archive, value.instancePattern,
                        [&](auto &pattern) {
                          return TransferDecorationInstancePattern(
                              archive, pattern, context);
                        }) ||
      !TransferEnum(archive, value.mode,
                    VectorDecorationMode::CompositionOverlay) ||
      !TransferDecorationTarget(archive, value.target) ||
      !TransferEnum(archive, value.placementDriver,
                    DecorationPlacementDriver::GeometryOnly) ||
      !TransferEnum(archive, value.anchor, DecorationAnchor::CaretPath) ||
      !TransferEnum(archive, value.fit, DecorationFit::Native) ||
      !TransferPropertyValue(archive, value.padding, context) ||
      !TransferDecorationLocalTransform(archive, value.localTransform) ||
      !TransferDecorationAssetPlayback(archive, value.assetPlayback, context) ||
      !archive.Signed(zOrder) ||
      zOrder < std::numeric_limits<std::int32_t>::min() ||
      zOrder > std::numeric_limits<std::int32_t>::max() ||
      !TransferEnum(archive, value.transformInherit,
                    DecorationTransformInherit::TranslateOnly) ||
      !TransferEnum(archive, value.sampling,
                    DecorationSamplingPolicy::LinearClamp) ||
      !TransferEnum(archive, value.effectScope,
                    DecorationEffectScope::AfterGroupEffect) ||
      !TransferEnum(archive, value.fallback,
                    DecorationFallbackPolicy::ClipVisibleInstances) ||
      !archive.String(value.fallbackAssetId))
    return false;
  if constexpr (Archive::kReading)
    value.zOrder = static_cast<std::int32_t>(zOrder);
  return true;
}

template <typename Archive>
bool TransferResourceReference(Archive &archive, TextResourceReference &value) {
  return archive.String(value.resourceId) &&
         TransferEnum(archive, value.kind, TextResourceKind::FloatTexture) &&
         TransferEnum(archive, value.ownership,
                      TextResourceOwnership::System) &&
         archive.String(value.assetId) && archive.String(value.digest) &&
         archive.String(value.mediaType);
}

template <typename Archive>
bool TransferPresentation(Archive &archive,
                          TextCompositionPresentation &value,
                          const CodecContext &context) {
  return TransferReferenceCanvas(archive, value.referenceCanvas) &&
         TransferPropertyValue(archive, value.authoredLayoutFrame, context) &&
         TransferPropertyValue(archive, value.writingMode, context) &&
         TransferPropertyValue(archive, value.appearance.backdrops, context) &&
         TransferPropertyValue(archive, value.appearance.sdfMaterial, context) &&
         TransferPropertyValue(archive, value.appearance.bend, context) &&
         TransferPropertyValue(archive, value.appearance.path, context) &&
         archive.Boolean(value.appearance.visualExtent.allowControlOverflow) &&
         TransferOptional(
             archive, value.appearance.visualExtent.maximumExtentWidth,
             [&](float &maximum) {
               return archive.Float(maximum) && maximum > 0.0F;
             }) &&
         TransferOptional(
             archive, value.appearance.visualExtent.maximumExtentHeight,
             [&](float &maximum) {
               return archive.Float(maximum) && maximum > 0.0F;
             }) &&
         archive.Float(value.appearance.globalAlpha) &&
         value.appearance.globalAlpha >= 0.0F &&
         value.appearance.globalAlpha <= 1.0F;
}

template <typename Archive>
bool TransferDecorationBudget(Archive &archive,
                              TextCompositionDecorationBudget &value) {
  std::uint64_t maximumDecorations = value.maximumDecorations;
  std::uint64_t maximumInstancesPerBinding = value.maximumInstancesPerBinding;
  std::uint64_t maximumInstances = value.maximumInstances;
  std::uint64_t maximumInstanceDimension = value.maximumInstanceDimension;
  if (!archive.Unsigned(maximumDecorations) ||
      maximumDecorations > std::numeric_limits<std::uint32_t>::max() ||
      !archive.Unsigned(maximumInstancesPerBinding) ||
      maximumInstancesPerBinding > std::numeric_limits<std::uint32_t>::max() ||
      !archive.Unsigned(maximumInstances) ||
      maximumInstances > std::numeric_limits<std::uint32_t>::max() ||
      !archive.Unsigned(value.maximumPixelsPerBinding) ||
      !archive.Unsigned(value.maximumPixels) ||
      !archive.Unsigned(value.maximumWorkingSetBytes) ||
      !archive.Unsigned(maximumInstanceDimension) ||
      maximumInstanceDimension > std::numeric_limits<std::uint32_t>::max() ||
      !archive.Unsigned(value.maximumResourceBytesPerDecoration) ||
      !archive.Unsigned(value.maximumResourceBytes))
    return false;
  if constexpr (Archive::kReading) {
    value.maximumDecorations =
        static_cast<std::uint32_t>(maximumDecorations);
    value.maximumInstancesPerBinding =
        static_cast<std::uint32_t>(maximumInstancesPerBinding);
    value.maximumInstances = static_cast<std::uint32_t>(maximumInstances);
    value.maximumInstanceDimension =
        static_cast<std::uint32_t>(maximumInstanceDimension);
  }
  return true;
}

template <typename Archive>
bool TransferDocument(Archive &archive, TextCompositionDocument &document,
                      const CodecContext &context) {
  std::string domain{kCanonicalDocumentDomain};
  std::uint64_t schemaRevision = document.schemaRevision;
  if (!archive.String(domain) || domain != kCanonicalDocumentDomain ||
      !archive.Unsigned(schemaRevision) ||
      schemaRevision > std::numeric_limits<std::uint32_t>::max() ||
      !archive.Signed(document.durationTicks) || document.durationTicks < 0 ||
      !TransferEnum(archive, document.role, TextRole::Title) ||
      !TransferVector(archive, document.contentSlots,
                      context.limits.richText.maximumSlots,
                      [&](auto &slot) {
                        return TransferContentSlot(archive, slot, context);
                      }) ||
      !TransferVector(archive, document.content,
                      context.limits.richText.maximumParagraphs,
                      [&](auto &paragraph) {
                        return TransferParagraph(archive, paragraph, context);
                      }) ||
      !TransferOptional(archive, document.timedText,
                        [&](auto &timedText) {
                          return TransferTimedTextTrack(archive, timedText,
                                                        context);
                        }) ||
      !TransferPresentation(archive, document.presentation, context) ||
      !TransferAnimationStack(archive, document.animations, context) ||
      !TransferVector(archive, document.decorations,
                      context.limits.maximumDecorations,
                      [&](auto &decoration) {
                        return TransferDecorationBinding(archive, decoration,
                                                         context);
                      }) ||
      !TransferVector(archive, document.resources,
                      context.limits.maximumResources,
                      [&](auto &resource) {
                        return TransferResourceReference(archive, resource);
                      }) ||
      !TransferEnum(archive, document.effectPolicy,
                    CompositionEffectPolicy::TextAndInsideGroupDecorations) ||
      !TransferDecorationBudget(archive, document.decorationBudget) ||
      !TransferOptional(archive, document.templateOrigin,
                        [&](auto &origin) {
                          return archive.String(origin.templateId) &&
                                 archive.String(origin.packageDigest);
                        }) ||
      !TransferOptional(archive, document.bubbleTemplateOrigin,
                        [&](auto &origin) {
                          return archive.String(origin.templateId) &&
                                 archive.String(origin.packageDigest);
                        }) ||
      !TransferOptional(archive, document.flowerTemplateOrigin,
                        [&](auto &origin) {
                          return archive.String(origin.templateId) &&
                                 archive.String(origin.packageDigest);
                        }) ||
      !TransferOptional(archive, document.animationTemplateOrigin,
                        [&](auto &origin) {
                          return archive.String(origin.templateId) &&
                                 archive.String(origin.packageDigest);
                        }))
    return false;
  if constexpr (Archive::kReading)
    document.schemaRevision = static_cast<std::uint32_t>(schemaRevision);
  return true;
}

} // namespace

bool EncodeCanonicalTextCompositionDocument(
    const TextCompositionDocument &document, std::vector<std::uint8_t> &output,
    const TextCompositionLimits &limits) {
  if (!ValidateTextCompositionDocument(document, limits).valid)
    return false;
  TextCompositionDocument canonical = document;
  const CodecContext context{limits};
  CanonicalWriter writer(kMaximumCanonicalDocumentBytes,
                         context.maximumStringBytes());
  if (!TransferDocument(writer, canonical, context) || !writer.ok())
    return false;
  auto encoded = writer.Take();
  output.swap(encoded);
  return true;
}

bool DecodeCanonicalTextCompositionDocument(
    const std::vector<std::uint8_t> &bytes, TextCompositionDocument &output,
    const TextCompositionLimits &limits) {
  const CodecContext context{limits};
  CanonicalReader reader(bytes, kMaximumCanonicalDocumentBytes,
                         context.maximumStringBytes());
  TextCompositionDocument decoded;
  if (!TransferDocument(reader, decoded, context) || !reader.Complete() ||
      !ValidateTextCompositionDocument(decoded, limits).valid)
    return false;
  std::vector<std::uint8_t> canonical;
  if (!EncodeCanonicalTextCompositionDocument(decoded, canonical, limits) ||
      canonical != bytes)
    return false;
  output = std::move(decoded);
  return true;
}

} // namespace videocut::text_composition
