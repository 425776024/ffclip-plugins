#pragma once

#include "videocut/text/RichTextDocument.h"
#include "videocut/text/TextAnimation.h"
#include "videocut/text/TextLayerAppearance.h"
#include "videocut/text/TextProperty.h"
#include "videocut/text/TextSchema.h"
#include "videocut/vector/VectorDocument.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace videocut::text_composition {

enum class DiagnosticSeverity : std::uint8_t {
  Information = 0,
  Warning,
  Error,
};

/// Stable, low-volume diagnostic emitted at the authored or sampling boundary.
/// A decoration diagnostic names that decoration as [subjectId], allowing the
/// caller to disable only the bad binding while the native text pass survives.
struct Diagnostic final {
  std::string code;
  DiagnosticSeverity severity{DiagnosticSeverity::Error};
  std::string stage;
  std::string subjectId;
  std::string message;
};

enum class TextRole : std::uint8_t {
  Normal = 0,
  Caption,
  Lyrics,
  Title,
};

enum class TimedTextClock : std::uint8_t {
  ClipSourceLocal = 0,
};

/// Optional timing metadata over the canonical rich-text content. It never
/// owns text and never creates a second clip/model identity.
struct TimedTextSpan final {
  std::string spanId;
  std::string paragraphId;
  std::string runId;
  text::TextUtf8Range range{};
  std::int64_t startOffsetUs{0};
  std::int64_t endOffsetUs{0};
  std::string semantic{"word"};
  text::TimedTextProgressMode progressMode{text::TimedTextProgressMode::Step};
  std::int64_t transitionEndOffsetUs{0};
};

struct TimedTextTrack final {
  TimedTextClock clock{TimedTextClock::ClipSourceLocal};
  std::vector<TimedTextSpan> spans;
};

struct TextCompositionPresentation final {
  text::ReferenceCanvas referenceCanvas{};
  /// Width/maximum-height authoring constraints. Renderer snapshots publish
  /// the intrinsic line-box height for the canonical rich text.
  text::LayoutBox authoredLayoutFrame{};
  text::TextWritingMode writingMode{text::TextWritingMode::Horizontal};
  text::TextLayerAppearance appearance{};
};

enum class TextResourceKind : std::uint8_t {
  Font = 0,
  Texture,
  Vector,
  Animated,
  Mesh,
  FloatTexture,
};

enum class TextResourceOwnership : std::uint8_t {
  Builtin = 0,
  ProjectManaged,
  System,
};

struct TextResourceReference final {
  std::string resourceId;
  TextResourceKind kind{TextResourceKind::Texture};
  TextResourceOwnership ownership{TextResourceOwnership::Builtin};
  std::string assetId;
  std::string digest;
  std::string mediaType;
};

enum class VectorDecorationMode : std::uint8_t {
  TrackedGrapheme = 0,
  SentenceEnvelope,
  PerGraphemeBackground,
  /// One vector/Lottie instance bound to the complete TextComposition local
  /// control geometry. Independent Vector clips continue to use their own
  /// timeline clip; this mode is the mixed-in Text clip form.
  CompositionOverlay,
};

enum class DecorationTargetScope : std::uint8_t {
  AllText = 0,
  Utf8Range,
};

struct DecorationTarget final {
  DecorationTargetScope scope{DecorationTargetScope::AllText};
  /// Stable rich-text owners for an authored UTF-8 range.
  std::string paragraphId;
  std::string runId;
  text::TextUtf8Range range{};
};

enum class DecorationPlacementDriver : std::uint8_t {
  SentenceUniformProgress = 0,
  GeometryOnly,
};

enum class DecorationAnchor : std::uint8_t {
  Above = 0,
  Below,
  Center,
  CaretPath,
};

enum class DecorationFit : std::uint8_t {
  Contain = 0,
  Cover,
  Stretch,
  Native,
};

enum class DecorationAssetClock : std::uint8_t {
  Composition = 0,
  Source,
  Target,
};

enum class DecorationPlaybackMode : std::uint8_t {
  Loop = 0,
  PingPong,
  Once,
  Hold,
};

struct DecorationAssetPlayback final {
  DecorationAssetClock clock{DecorationAssetClock::Composition};
  DecorationPlaybackMode mode{DecorationPlaybackMode::Loop};
  std::int64_t sourceInUs{0};
  std::int64_t sourceOutUs{1'000'000};
  double speed{1.0};
  std::int64_t phaseUs{0};
};

struct DecorationAssetReference final {
  std::string assetId;
  std::string digest;
  std::string mediaType;
  float intrinsicWidth{0.0F};
  float intrinsicHeight{0.0F};
  std::uint64_t estimatedResourceBytes{0};
  std::vector<vector::VectorFieldOverride> staticFieldOverrides;
};

enum class DecorationVariantSelectionPolicy : std::uint8_t {
  RepeatOne = 0,
  Cycle,
  DeterministicHash,
  ExplicitPattern,
};

enum class DecorationPerIndexPhasePolicy : std::uint8_t {
  Shared = 0,
  Stagger,
  DeterministicSeed,
};

struct DecorationAssetVariant final {
  std::string variantId;
  DecorationAssetReference asset{};
};

/// Optional per-grapheme instance program. An absent program preserves the
/// single authored [VectorDecorationBinding::asset] fast path.
struct DecorationInstancePattern final {
  std::vector<DecorationAssetVariant> assetVariants;
  DecorationVariantSelectionPolicy selectionPolicy{
      DecorationVariantSelectionPolicy::RepeatOne};
  std::vector<std::uint32_t> explicitPattern;
  std::uint32_t randomSeed{0};
  float offsetXStep{0.0F};
  float offsetYStep{0.0F};
  float scaleStep{0.0F};
  float rotationStepDegrees{0.0F};
  DecorationPerIndexPhasePolicy phasePolicy{
      DecorationPerIndexPhasePolicy::Shared};
  std::int64_t phaseStepUs{0};
};

struct DecorationLocalTransform final {
  float offsetX{0.0F};
  float offsetY{0.0F};
  float scaleX{1.0F};
  float scaleY{1.0F};
  float rotationDegrees{0.0F};
  float opacity{1.0F};
};

enum class DecorationTransformInherit : std::uint8_t {
  Full = 0,
  TranslateOnly,
};

enum class DecorationSamplingPolicy : std::uint8_t {
  Automatic = 0,
  LinearClamp,
};

enum class DecorationEffectScope : std::uint8_t {
  InsideGroupBehindText = 0,
  InsideGroupInFrontOfText,
  AfterGroupEffect,
};

enum class DecorationFallbackPolicy : std::uint8_t {
  DisableDecoration = 0,
  StaticFirstFrame,
  NativeBackground,
  ClipVisibleInstances,
};

struct VectorDecorationBinding final {
  std::string decorationId;
  bool enabled{true};
  DecorationAssetReference asset{};
  std::optional<DecorationInstancePattern> instancePattern;
  VectorDecorationMode mode{VectorDecorationMode::TrackedGrapheme};
  DecorationTarget target{};
  DecorationPlacementDriver placementDriver{
      DecorationPlacementDriver::GeometryOnly};
  DecorationAnchor anchor{DecorationAnchor::Center};
  DecorationFit fit{DecorationFit::Contain};
  text::Insets padding{};
  DecorationLocalTransform localTransform{};
  DecorationAssetPlayback assetPlayback{};
  std::int32_t zOrder{0};
  DecorationTransformInherit transformInherit{
      DecorationTransformInherit::Full};
  DecorationSamplingPolicy sampling{DecorationSamplingPolicy::Automatic};
  DecorationEffectScope effectScope{
      DecorationEffectScope::InsideGroupBehindText};
  DecorationFallbackPolicy fallback{
      DecorationFallbackPolicy::DisableDecoration};
  std::string fallbackAssetId;
};

enum class CompositionEffectPolicy : std::uint8_t {
  ApplyToWholeComposition = 0,
  TextOnly,
  TextAndInsideGroupDecorations,
};

struct TextCompositionDecorationBudget final {
  std::uint32_t maximumDecorations{128};
  std::uint32_t maximumInstancesPerBinding{4096};
  std::uint32_t maximumInstances{16'384};
  std::uint64_t maximumPixelsPerBinding{67'108'864};
  std::uint64_t maximumPixels{134'217'728};
  std::uint64_t maximumWorkingSetBytes{512U * 1024U * 1024U};
  std::uint32_t maximumInstanceDimension{16'384};
  std::uint64_t maximumResourceBytesPerDecoration{64U * 1024U * 1024U};
  std::uint64_t maximumResourceBytes{256U * 1024U * 1024U};
};

struct TextTemplateOrigin final {
  std::string templateId;
  std::string packageDigest;
};

/// Canonical renderer-neutral authored object. It is neither a temporary
/// render snapshot nor a UI DTO: source facts, local text presentation and
/// text-owned vector decorations have one durable owner. Output/group
/// transform and effect routing remain clip-specific; optional timed ranges
/// and every animation driver live in their canonical sibling fields.
struct TextCompositionDocument final {
  std::uint32_t schemaRevision{text::kTextSchemaRevision};
  std::int64_t durationTicks{0};
  TextRole role{TextRole::Normal};
  std::vector<text::TextContentSlot> contentSlots;
  std::vector<text::RichTextParagraph> content;
  std::optional<TimedTextTrack> timedText;
  TextCompositionPresentation presentation{};
  text::TextAnimationStack animations{};
  std::vector<VectorDecorationBinding> decorations;
  std::vector<TextResourceReference> resources;
  CompositionEffectPolicy effectPolicy{
      CompositionEffectPolicy::ApplyToWholeComposition};
  TextCompositionDecorationBudget decorationBudget{};
  std::optional<TextTemplateOrigin> templateOrigin;
  std::optional<TextTemplateOrigin> bubbleTemplateOrigin;
  std::optional<TextTemplateOrigin> flowerTemplateOrigin;
  std::optional<TextTemplateOrigin> animationTemplateOrigin;
};

struct TextCompositionLimits final {
  text::RichTextLimits richText{};
  std::size_t maximumIdentityBytes{512};
  std::size_t maximumResources{4096};
  std::size_t maximumDecorations{128};
  std::size_t maximumOverridesPerDecoration{128};
  std::size_t maximumVariantsPerDecoration{16};
  std::size_t maximumExplicitPatternLength{256};
  std::uint64_t maximumDecorationResourceBytes{256U * 1024U * 1024U};
  std::uint64_t maximumSingleDecorationResourceBytes{64U * 1024U * 1024U};
  std::int64_t maximumAssetDurationUs{86'400'000'000};
  float maximumLocalMagnitude{65'536.0F};
};

struct DecorationBindingValidationResult final {
  bool usable{false};
  std::vector<Diagnostic> diagnostics;
};

/// [valid] describes the text composition itself. Invalid decoration bindings
/// are listed in [disabledDecorationIds] and do not make the native text pass
/// invalid. [textOnlyFastPath] is true only when the authored decoration list
/// is empty, allowing callers to bypass decoration planning entirely.
struct TextCompositionValidationResult final {
  bool valid{false};
  bool textOnlyFastPath{false};
  std::vector<std::string> disabledDecorationIds;
  std::vector<std::size_t> disabledDecorationIndexes;
  std::vector<Diagnostic> diagnostics;
};

DecorationBindingValidationResult
ValidateVectorDecorationBinding(const TextCompositionDocument &document,
                                const VectorDecorationBinding &binding,
                                const TextCompositionLimits &limits = {});

TextCompositionValidationResult
ValidateTextCompositionDocument(const TextCompositionDocument &document,
                                const TextCompositionLimits &limits = {});

/// Encodes the sole current authored Text document as a deterministic bounded
/// binary closure. Invalid documents and non-finite authored values fail
/// closed; [output] is unchanged on failure.
bool EncodeCanonicalTextCompositionDocument(
    const TextCompositionDocument &document, std::vector<std::uint8_t> &output,
    const TextCompositionLimits &limits = {});

/// Decodes one complete current canonical Text document. Invalid enum,
/// length, range, non-finite, non-canonical, and trailing-byte inputs fail
/// closed; [output] is unchanged on failure.
bool DecodeCanonicalTextCompositionDocument(
    const std::vector<std::uint8_t> &bytes, TextCompositionDocument &output,
    const TextCompositionLimits &limits = {});

/// Canonical visual-and-authoring identity used by persistence, revision
/// projectors and runtime cache fences. It excludes project owner identity and
/// all external locators by construction.
std::string ComputeTextCompositionDocumentIdentity(
    const TextCompositionDocument &document);

std::size_t EstimateTextCompositionDocumentBytes(
    const TextCompositionDocument &document) noexcept;

/// Applies one run-local UTF-8 replacement to optional timing metadata.
/// Intersecting spans are removed; later spans shift by the byte delta.
void RebaseTimedTextForUtf8Edit(TimedTextTrack &track,
                                const std::string &paragraphId,
                                const std::string &runId,
                                text::TextUtf8Range replacedRange,
                                std::uint64_t replacementByteCount);

/// Returns spans intersecting one source-local window, clipped and rebased so
/// zero is the new clip source origin. Used by ordinary split/trim/duplicate.
TimedTextTrack SliceTimedTextTrack(const TimedTextTrack &track,
                                   std::int64_t beginUs, std::int64_t endUs);

} // namespace videocut::text_composition
