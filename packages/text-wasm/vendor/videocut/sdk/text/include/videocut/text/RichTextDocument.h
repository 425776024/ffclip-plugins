#pragma once

#include "videocut/text/FontReference.h"
#include "videocut/text/TextTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace videocut::text {

enum class CanvasScalePolicy : std::uint8_t {
  Fit = 0,
  Fill,
  None,
};

enum class TextWritingMode : std::uint8_t {
  Horizontal = 0,
  VerticalRightToLeft,
  VerticalLeftToRight,
};

enum class TextLayoutSizingMode : std::uint8_t {
  AutoWidth = 0,
  AutoHeight,
  Fixed,
  FitText,
};

enum class VerticalAlignment : std::uint8_t {
  Top = 0,
  Center,
  Bottom,
};

enum class TextAlignment : std::uint8_t {
  Start = 0,
  Center,
  End,
  Justify,
};

enum class TextDirection : std::uint8_t {
  Auto = 0,
  LeftToRight,
  RightToLeft,
};

enum class TextOverflow : std::uint8_t {
  Clip = 0,
  Ellipsis,
  Visible,
};

enum class TextWrap : std::uint8_t {
  Word = 0,
  Character,
  None,
};

enum class TextLineBreakPolicy : std::uint8_t {
  Unicode = 0,
  CjkStrict,
  CjkLoose,
  Anywhere,
};

enum class TextHyphenation : std::uint8_t {
  None = 0,
  Automatic,
};

enum class PaintSpread : std::uint8_t {
  Clamp = 0,
  Repeat,
  Mirror,
};

enum class TextureSourceKind : std::uint8_t {
  Builtin = 0,
  ProjectManaged,
};

enum class TextureFit : std::uint8_t {
  Cover = 0,
  Contain,
  Stretch,
  Tile,
};

/// Selects the coordinate transform used to sample a texture paint.
/// ScopeBounds is the ordinary image-in-layout mapping. GlyphDistanceField
/// derives coordinates from each glyph's padded distance-field quad and can
/// select successive cells from an authored texture atlas. The latter keeps
/// decorative material geometry attached to the glyph through transforms and
/// animation without baking renderer-specific vertex data into the document.
enum class TextureMapping : std::uint8_t {
  ScopeBounds = 0,
  GlyphDistanceField,
};

enum class PaintCoordinateSpace : std::uint8_t {
  LayoutBox = 0,
  TextBounds,
  Grapheme,
};

/// Controls how authored gradient stops are materialized before sampling.
/// Continuous keeps the ordinary renderer-native gradient. Rgba8Lut256
/// reproduces engines that first bake a one-dimensional 8-bit lookup texture.
enum class GradientSampling : std::uint8_t {
  Continuous = 0,
  Rgba8Lut256,
};

enum class TextureOrientation : std::uint8_t {
  Up = 0,
  Right,
  Down,
  Left,
};

enum class TextBlendMode : std::uint8_t {
  SourceOver = 0,
  Multiply,
  Screen,
  Overlay,
  Add,
  Darken,
  Lighten,
};

struct ReferenceCanvas final {
  float width{1920.0F};
  float height{1080.0F};
  CanvasScalePolicy scalePolicy{CanvasScalePolicy::Fit};
};

struct LayoutBox final {
  float x{0.0F};
  float y{0.0F};
  /// Horizontal wrapping constraint. The published logical box keeps this
  /// width so width handles remain stable while line breaks change.
  float width{1920.0F};
  /// Vertical alignment/overflow constraint, not an authored visible height.
  /// Ordinary layout publishes its intrinsic line-box height, which may exceed
  /// this value, and never clips pixels here. Path/Bend text keeps the complete
  /// box because its normalized/deformation geometry depends on both
  /// dimensions.
  float height{1080.0F};
  TextLayoutSizingMode sizingMode{TextLayoutSizingMode::AutoHeight};
  float minimumWidth{0.0F};
  float minimumHeight{0.0F};
  std::optional<float> maximumWidth;
  std::optional<float> maximumHeight;
  float minimumFitFontSize{1.0F};
  float maximumFitFontSize{4096.0F};
  /// Extra source-authoring extent included by the finite FitText feedback
  /// measurement. It is scaled with the candidate font size and remains
  /// independent from the visible layout box and its padding.
  Insets fitMeasurementOutsets{};
  /// Optional source-space transform origin used when the FitText writer
  /// emits glyph geometry. Keeping this independent from the measured extent
  /// preserves an authored entity anchor while its text is resized.
  std::optional<float> fitOriginX;
  Insets padding{};
  VerticalAlignment verticalAlignment{VerticalAlignment::Center};
  /// Legacy layout-overflow preference. It never clips rendered pixels; the
  /// final canvas/output clip is the only visual scissor.
  bool clipOverflow{false};
  bool pixelSnap{false};
};

struct TextTabStop final {
  float position{0.0F};
  TextAlignment alignment{TextAlignment::Start};
  char32_t decimalCharacter{U'.'};
};

struct TextBoxBackground final {
  bool enabled{false};
  Color color{0.0F, 0.0F, 0.0F, 0.0F};
  Insets padding{};
  float cornerRadius{0.0F};
};

struct ParagraphStyle final {
  TextAlignment alignment{TextAlignment::Center};
  TextDirection direction{TextDirection::Auto};
  std::string locale{"und"};
  std::optional<std::uint32_t> maximumLines;
  TextOverflow overflow{TextOverflow::Clip};
  float lineHeight{1.0F};
  TextWrap wrap{TextWrap::Word};
  TextLineBreakPolicy lineBreakPolicy{TextLineBreakPolicy::Unicode};
  TextHyphenation hyphenation{TextHyphenation::None};
  float firstLineIndent{0.0F};
  float startIndent{0.0F};
  float endIndent{0.0F};
  float spacingBefore{0.0F};
  float spacingAfter{0.0F};
  bool hangingPunctuation{false};
  std::vector<TextTabStop> tabStops;
  /// Paragraph-scoped rounded background, painted behind all line boxes.
  TextBoxBackground background{};
};

struct GradientStop final {
  float offset{0.0F};
  Color color{1.0F, 1.0F, 1.0F, 1.0F};
};

struct TextureReference final {
  TextureSourceKind sourceKind{TextureSourceKind::Builtin};
  std::string assetId;
  std::string digest;
  std::string mediaType{"image/png"};
  std::string colorSpace{"srgb"};
  TextureOrientation orientation{TextureOrientation::Up};
};

struct TextMaterialCoordinates final {
  PaintCoordinateSpace coordinateSpace{PaintCoordinateSpace::LayoutBox};
  float coordinateOutset{0.0F};
  float coordinateScale{1.0F};
};

struct SolidTextMaterial final {
  Color color{1.0F, 1.0F, 1.0F, 1.0F};
};

struct LinearGradientTextMaterial final {
  std::vector<GradientStop> stops;
  float startX{0.0F};
  float startY{0.5F};
  float endX{1.0F};
  float endY{0.5F};
  PaintSpread spread{PaintSpread::Clamp};
  GradientSampling sampling{GradientSampling::Continuous};
  TextMaterialCoordinates coordinates{};
};

struct RadialGradientTextMaterial final {
  std::vector<GradientStop> stops;
  float centerX{0.5F};
  float centerY{0.5F};
  float radius{0.5F};
  PaintSpread spread{PaintSpread::Clamp};
  GradientSampling sampling{GradientSampling::Continuous};
  TextMaterialCoordinates coordinates{};
};

// Geometry and byte-sampling policy for a texture material's optional
// gradient underlay. Coordinates are normalized over the texture/SDF quad;
// the stops remain in TextureTextMaterial so legacy array-only documents can
// retain their established default projection.
struct LinearGradientProjection final {
  float startX{0.0F};
  float startY{0.5F};
  float endX{1.0F};
  float endY{0.5F};
  PaintSpread spread{PaintSpread::Clamp};
  GradientSampling sampling{GradientSampling::Continuous};
};

struct TextureTextMaterial final {
  TextureReference texture{};
  TextureFit fit{TextureFit::Cover};
  TextureMapping mapping{TextureMapping::ScopeBounds};
  TextMaterialCoordinates coordinates{};
  float scale{1.0F};
  float rotationDegrees{0.0F};
  // Fractions of the material coordinate bounds, not output pixels.
  float offsetX{0.0F};
  float offsetY{0.0F};
  bool flipX{false};
  bool flipY{false};
  std::uint16_t atlasColumns{1U};
  std::uint16_t atlasRows{1U};
  float textureOpacity{1.0F};
  float opacity{1.0F};
  bool sourceAlpha{false};
  std::optional<Color> underlayColor;
  std::vector<GradientStop> underlayGradient;
  LinearGradientProjection underlayGradientProjection{};
};

using TextMaterial =
    std::variant<SolidTextMaterial, LinearGradientTextMaterial,
                 RadialGradientTextMaterial, TextureTextMaterial>;

struct LiteralTextMaterial final {
  TextMaterial material{SolidTextMaterial{}};
};

struct EditableTextStyleSlot final {
  std::string semanticRole{"glyph.primaryFill"};
  TextMaterial fallback{SolidTextMaterial{}};
  std::optional<TextureReference> replacementMask;
};

using TextMaterialBinding =
    std::variant<LiteralTextMaterial, EditableTextStyleSlot>;

enum class TextShadowKind : std::uint8_t {
  Outer = 0,
  Inner,
};

enum class TextOuterShadowSmoothingMode : std::uint8_t {
  Auto = 0,
  Feather,
  DiffuseRoundMask,
};

/// A material-layer displacement authored in the normalized glyph coordinate
/// domain used by the source renderer.  Keeping radius and angle separate is
/// intentional: compatibility renderers evaluate cos/sin in the vertex stage,
/// and eagerly converting the pair to Cartesian pixels changes float32
/// rounding and animation sampling.  offsetX/offsetY remain the ordinary
/// editor-facing Cartesian fallback on each layer.
struct TextNormalizedPolarOffset final {
  float radius{0.0F};
  float angleRadians{0.0F};
};

struct TextFillLayer final {
  std::string layerId{"glyph.primaryFill"};
  std::int32_t zOrder{0};
  TextBlendMode blend{TextBlendMode::SourceOver};
  TextMaterialBinding material{};
  float offsetX{0.0F};
  float offsetY{0.0F};
  std::optional<TextNormalizedPolarOffset> normalizedPolarOffset;
};

struct TextNormalizedUvOffset final {
  float x{0.0F};
  float y{0.0F};
};

struct TextStrokeLayer final {
  std::string layerId;
  std::int32_t zOrder{-1};
  TextBlendMode blend{TextBlendMode::SourceOver};
  TextMaterialBinding material{};
  float width{0.0F};
  float innerRingWidth{0.0F};
  /// Optional signed start of the outline band in authored distance units.
  /// Negative values retain the inside-contour half of an asymmetric SDF
  /// outline. When absent, innerRingWidth keeps the ordinary editor contract.
  std::optional<float> signedStartWidth;
  float offsetX{0.0F};
  float offsetY{0.0F};
  float blurRadius{0.0F};
  float spread{0.0F};
  std::optional<TextNormalizedPolarOffset> normalizedPolarOffset;
};

struct TextShadowLayer final {
  std::string layerId;
  std::int32_t zOrder{-2};
  TextBlendMode blend{TextBlendMode::SourceOver};
  TextMaterialBinding material{};
  TextShadowKind kind{TextShadowKind::Outer};
  float offsetX{0.0F};
  float offsetY{0.0F};
  float blurRadius{0.0F};
  /// Outer LegacyFeather SDF masks accept negative spread to contract the
  /// contour before feathering. Other shadow modes require nonnegative spread.
  float spread{0.0F};
  float thicknessAngleDegrees{45.0F};
  float thicknessDistance{0.0F};
  TextOuterShadowSmoothingMode smoothing{
      TextOuterShadowSmoothingMode::Auto};
  float roundMaskIntensity{0.0F};
  /// Multiplier that maps the authored blur radius to the source TextPro
  /// fragment's u_extraSmooth domain.  TextPro presets carry both the direct
  /// (1.0) and legacy one-fifth (0.2) contracts, so this remains authored
  /// layer state rather than a renderer or template-id heuristic.
  float sdfBlurScale{0.2F};
  std::optional<TextNormalizedPolarOffset> normalizedPolarOffset;
  /// Optional source-renderer fragment-domain displacement. Inner shadows
  /// use this directly instead of reconstructing UV from authored pixels.
  std::optional<TextNormalizedUvOffset> normalizedUvOffset;
  /// Nested shadow-local stroke passes in authored order. Multiplicity is
  /// meaningful and these are not flattened into sibling material layers.
  std::vector<TextStrokeLayer> strokes;
};

struct TextGlowLayer final {
  std::string layerId;
  std::int32_t zOrder{-1};
  TextBlendMode blend{TextBlendMode::Add};
  TextMaterialBinding material{};
  float radius{18.0F};
  float spread{2.0F};
  float directionX{0.0F};
  float directionY{0.0F};
  std::optional<TextNormalizedPolarOffset> normalizedPolarOffset;
};

using TextGlyphMaterialLayer =
    std::variant<TextFillLayer, TextStrokeLayer, TextShadowLayer, TextGlowLayer>;

struct TextGlyphMaterialStack final {
  std::vector<TextGlyphMaterialLayer> layers{TextFillLayer{}};
};

enum class TextDecorationLineStyle : std::uint8_t {
  Solid = 0,
  Double,
  Dotted,
  Dashed,
  Wavy,
};

struct TextDecorationLine final {
  bool enabled{false};
  TextMaterialBinding material{};
  float thickness{1.0F};
  float offset{0.0F};
  TextDecorationLineStyle style{TextDecorationLineStyle::Solid};
  bool skipInk{true};
};

struct InlineTextDecoration final {
  TextDecorationLine underline{};
  TextDecorationLine strikeThrough{};
};

struct FontFeature final {
  std::string tag;
  std::uint32_t value{1U};
};

struct FontSpec final {
  FontReference primary;
  std::vector<FontReference> fallbacks;
  std::string family;
  std::string postscriptName;
  std::int32_t weight{400};
  std::int32_t width{5};
  FontSlant slant{FontSlant::Upright};
  std::uint32_t faceIndex{0};
  std::vector<FontAxis> variationAxes;
  std::vector<FontFeature> features;
  bool allowSystemGlyphFallback{false};
};

struct TextStyle final {
  FontSpec font;
  float fontSize{64.0F};
  float letterSpacing{0.0F};
  float wordSpacing{0.0F};
  float baselineShift{0.0F};
  TextGlyphMaterialStack materials{};
  /// Run-scoped rounded backgrounds follow shaped run boxes.
  TextBoxBackground background{};
  InlineTextDecoration decoration{};
};

struct RichTextRun final {
  std::string runId;
  std::string utf8Text;
  std::string locale{"und"};
  TextStyle style{};
};

struct RichTextParagraph final {
  std::string paragraphId;
  ParagraphStyle style{};
  std::vector<RichTextRun> runs;
};

struct TextContentSlot final {
  std::string slotId;
  std::string semanticRole;
  std::vector<std::string> paragraphIds;
  std::vector<std::string> runIds;
};

enum class RevealUnit : std::uint8_t {
  Grapheme = 0,
  Word,
  Line,
};

enum class RevealOrder : std::uint8_t {
  Logical = 0,
  Visual,
};

enum class RevealDirection : std::uint8_t {
  Forward = 0,
  Reverse,
};

enum class RevealPresentation : std::uint8_t {
  Reveal = 0,
  ActiveUnitHighlight,
};

struct RevealCursor final {
  bool enabled{false};
  Color color{1.0F, 1.0F, 1.0F, 1.0F};
  float width{2.0F};
  std::int64_t periodUs{700'000};
  float dutyCycle{0.55F};
};

struct RevealAnimation final {
  std::string trackId;
  std::vector<std::string> paragraphIds;
  std::vector<std::string> runIds;
  RevealUnit unit{RevealUnit::Grapheme};
  std::int64_t startOffsetUs{0};
  std::int64_t durationUs{1'000'000};
  RevealOrder order{RevealOrder::Logical};
  RevealDirection direction{RevealDirection::Forward};
  RevealPresentation presentation{RevealPresentation::Reveal};
  float fadeFraction{0.0F};
  Color activeColor{1.0F, 1.0F, 0.0F, 1.0F};
  RevealCursor cursor{};
};

enum class TimedTextSpanSemantic : std::uint8_t {
  Keyword = 0,
  KaraokeWord,
  Emphasis,
  Mention,
};

enum class TimedTextProgressMode : std::uint8_t {
  /// The complete range switches to the active paint at startOffsetUs.
  Step = 0,
  /// Progress advances through renderer-owned grapheme boundaries and never
  /// splits a combining sequence, emoji ZWJ sequence, or UTF-8 code point.
  GraphemeSweep,
};

/// A stable authored UTF-8 range whose paint changes over an absolute
/// clip-local half-open time interval.  The base run remains the inactive
/// style, so sampling never removes or reflows the surrounding sentence.
struct TimedTextSpan final {
  std::string spanId;
  TimedTextSpanSemantic semantic{TimedTextSpanSemantic::KaraokeWord};
  std::string paragraphId;
  std::string runId;
  std::size_t utf8Begin{0};
  std::size_t utf8End{0};
  std::int64_t startOffsetUs{0};
  std::int64_t endOffsetUs{0};
  TimedTextProgressMode progressMode{TimedTextProgressMode::Step};
  /// End of the progress transition. The active paint remains held until
  /// endOffsetUs, allowing ASS/KTV syllables to stay highlighted.
  std::int64_t transitionEndOffsetUs{0};
  /// Runtime-only opacity override for generated timed-span samples.
  std::optional<float> opacity;
};

enum class TextLayerAnimationPhase : std::uint8_t {
  Enter = 0,
  Loop,
  Exit,
  Caption,
};

enum class TextLayerAnimationProperty : std::uint8_t {
  Opacity = 0,
  PositionX,
  PositionY,
  ScaleX,
  ScaleY,
  RotationDegrees,
};

enum class TextAnimationEasing : std::uint8_t {
  Linear = 0,
  EaseIn,
  EaseOut,
  EaseInOut,
};

struct TextLayerAnimationKeyframe final {
  /// Normalized phase/cycle offset in the inclusive range [0, 1].
  float offset{0.0F};
  float value{0.0F};
  /// Interpolation from this keyframe to the following keyframe.
  TextAnimationEasing easing{TextAnimationEasing::Linear};
};

struct TextLayerAnimationTrack final {
  TextLayerAnimationProperty property{TextLayerAnimationProperty::Opacity};
  std::vector<TextLayerAnimationKeyframe> keyframes;
};

/// Renderer-neutral phase clip. Position values use the same signed normalized
/// canvas space as the layer presentation, scales are multiplicative, rotation
/// is expressed in degrees, and opacity is multiplicative.
struct TextLayerAnimationClip final {
  std::string clipId;
  std::string presetId;
  TextLayerAnimationPhase phase{TextLayerAnimationPhase::Enter};
  std::int64_t durationUs{500'000};
  std::vector<TextLayerAnimationTrack> tracks;
};

struct ResolvedRichTextView final {
  ReferenceCanvas referenceCanvas{};
  LayoutBox layoutBox{};
  TextWritingMode writingMode{TextWritingMode::Horizontal};
  std::vector<TextContentSlot> slots;
  std::vector<RichTextParagraph> paragraphs;
};

struct RichTextLimits final {
  std::size_t maximumParagraphs{128};
  std::size_t maximumRuns{4096};
  std::size_t maximumUtf8Bytes{4U * 1024U * 1024U};
  std::size_t maximumFallbackFontsPerRun{32};
  std::size_t maximumStrokesPerRun{8};
  // Layered flower materials use up to twelve independently authored shadows.
  std::size_t maximumShadowsPerRun{16};
  std::size_t maximumAnimationTracks{128};
  std::size_t maximumAnimationKeyframesPerTrack{64};
  std::size_t maximumTimedSpans{16'384};
  std::size_t maximumGradientStops{16};
  std::size_t maximumSlots{256};
  std::size_t maximumTabStopsPerParagraph{64};
  std::size_t maximumFontFeaturesPerRun{128};
  float maximumCanvasDimension{65'536.0F};
  float maximumFontSize{4096.0F};
};

struct RichTextValidationResult final {
  bool valid{false};
  std::vector<Diagnostic> diagnostics;
};

RichTextValidationResult
ValidateResolvedRichTextView(const ResolvedRichTextView &view,
                             const RichTextLimits &limits = {});

} // namespace videocut::text
