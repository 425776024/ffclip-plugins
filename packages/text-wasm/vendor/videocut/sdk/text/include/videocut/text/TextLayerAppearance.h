#pragma once

#include "videocut/text/RichTextDocument.h"
#include "videocut/text/TextTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace videocut::text {

enum class BubbleFamily : std::uint8_t {
  RoundRect = 0,
  Pill,
  Speech,
  Cloud,
  Spike,
  CaptionBar,
};

enum class BubbleTailEdge : std::uint8_t {
  None = 0,
  Top,
  Bottom,
  Left,
  Right,
};

struct BubbleTail final {
  BubbleTailEdge edge{BubbleTailEdge::None};
  float position{0.5F};
  float width{32.0F};
  float length{24.0F};
};

enum class TextBackdropChannel : std::uint8_t {
  Frame = 0,
  Bubble,
  Custom,
};

enum class TextBackdropFitPolicy : std::uint8_t {
  InkBounds = 0,
  LineUnion,
  LayoutBounds,
};

enum class TextBackdropFitMode : std::uint8_t {
  Width = 0,
  Height,
  LongSide,
  ShortSide,
  Stretch,
};

enum class TextBackdropTimeSource : std::uint8_t {
  Composition = 0,
  AnimationLayer,
  Source,
};

enum class TextBackdropPlaybackMode : std::uint8_t {
  Once = 0,
  Loop,
  PingPong,
  Hold,
};

struct TextBackdropTransform final {
  float offsetX{0.0F};
  float offsetY{0.0F};
  float scaleX{1.0F};
  float scaleY{1.0F};
  float rotationDegrees{0.0F};
  float opacity{1.0F};
};

struct RoundedRectBackdrop final {
  BubbleFamily family{BubbleFamily::RoundRect};
  TextMaterialBinding fill{};
  std::vector<TextStrokeLayer> strokes;
  float cornerRadius{12.0F};
  BubbleTail tail{};
  std::optional<float> authoredWidth;
  std::optional<float> authoredHeight;
};

enum class TextBackdropStretchMode : std::uint8_t {
  NineSlice = 0,
  Stretch,
  TileCenter,
};

struct NineSliceBackdrop final {
  TextureReference asset{};
  Insets capInsets{};
  Insets contentInsets{};
  float minimumContentWidth{0.0F};
  float minimumContentHeight{0.0F};
  TextBackdropStretchMode stretchMode{TextBackdropStretchMode::NineSlice};
  Color fallbackColor{0.0F, 0.0F, 0.0F, 0.65F};
};

struct VectorBackdrop final {
  TextureReference asset{};
  TextBackdropFitPolicy fit{TextBackdropFitPolicy::InkBounds};
  Color fallbackColor{0.0F, 0.0F, 0.0F, 0.65F};
};

struct AnimatedBackdrop final {
  TextureReference asset{};
  Insets contentInsets{};
  TextBackdropTimeSource timeSource{TextBackdropTimeSource::Composition};
  TextBackdropPlaybackMode playback{TextBackdropPlaybackMode::Loop};
  std::int64_t sourceInUs{0};
  std::int64_t sourceOutUs{1'000'000};
  std::int64_t phaseUs{0};
  Color fallbackColor{0.0F, 0.0F, 0.0F, 0.65F};
};

using TextBackdropSource =
    std::variant<RoundedRectBackdrop, NineSliceBackdrop, VectorBackdrop,
                 AnimatedBackdrop>;

struct TextBackdropLayer final {
  std::string layerId;
  TextBackdropChannel channel{TextBackdropChannel::Frame};
  bool enabled{false};
  TextBackdropSource source{RoundedRectBackdrop{}};
  /// Optional alpha-masked material applied to image/vector/animated source
  /// pixels. Templates use an EditableTextStyleSlot here when the user may
  /// replace only the backdrop color/gradient/texture while preserving the
  /// source geometry, border and animation.
  std::optional<TextMaterialBinding> materialOverride;
  Insets padding{24.0F, 12.0F, 24.0F, 12.0F};
  TextBackdropFitPolicy fit{TextBackdropFitPolicy::InkBounds};
  std::optional<float> sourceIntrinsicWidth;
  std::optional<float> sourceIntrinsicHeight;
  TextBackdropFitMode fitMode{TextBackdropFitMode::Stretch};
  float sourcePivotX{0.5F};
  float sourcePivotY{0.5F};
  Insets expand{};
  Insets sourceOutsets{};
  TextBackdropTransform transform{};
  std::int32_t zOrder{-100};
  std::vector<TextLayerAnimationClip> animationClips;
};

struct TextBackdropStack final {
  std::vector<TextBackdropLayer> layers;
};

/// Source component that authored the native Letter geometry contract. This
/// is persisted independently from template identity so renderers can honor
/// component-specific writer coordinates after the source project is gone.
enum class TextSourceCreationComponent : std::uint8_t {
  LegacyText = 0,
  SdfText,
};

/// Renderer-neutral signed-distance-field material contract. The text shaper
/// still owns glyph selection and placement; the raster lane materializes a
/// distance atlas and evaluates fill/outline/shadow thresholds in a shader.
/// This keeps SDF semantics available to preview and export without depending
/// on a third-party text renderer or any reference-rendered pixels.
struct TextSdfMaterial final {
  bool enabled{false};
  TextSourceCreationComponent sourceCreationComponent{
      TextSourceCreationComponent::LegacyText};
  /// Authored pixels represented between the 0.5 contour and either end of
  /// the normalized distance field.
  float distanceRange{172.0F};
  /// Canonical raster pixels reserved on either side of the glyph outline.
  /// This is intentionally independent from distanceRange: authoring units
  /// control material thresholds, while the atlas domain controls how far a
  /// wide outline or blurred material can be sampled before it is clipped.
  float rasterDistanceRange{30.0F};
  /// Multiplier applied to fwidth(distance) at the contour boundary.
  float smoothingScale{0.6F};
  /// Source DESIGN canvas carried by a native TextPro package. Enabled SDF
  /// material derives one uniform Studio-animation projection from
  /// referenceCanvas.width/sourceDesignWidth; sourceDesignHeight records the
  /// complete source coordinate contract and is not an independent Y scale or
  /// a request to re-run base text layout.
  float sourceDesignWidth{0.0F};
  float sourceDesignHeight{0.0F};
};

struct TextVisualExtentPolicy final {
  bool allowControlOverflow{true};
  std::optional<float> maximumExtentWidth;
  std::optional<float> maximumExtentHeight;
};

struct TextBend final {
  bool enabled{false};
  /// Signed bend in [-1, 1]. Positive values arch upward.
  float amount{0.0F};
};

struct TextPathPoint final {
  /// Coordinates are normalized to the authored layout box. Values outside
  /// [0, 1] are intentionally supported for paths that extend past the box.
  float x{0.0F};
  float y{0.0F};
};

enum class TextPathCommandKind : std::uint8_t {
  MoveTo = 0,
  LineTo,
  QuadraticTo,
  CubicTo,
  Close,
};

/// Renderer-neutral path verb. LineTo consumes [end], QuadraticTo consumes
/// [control1, end], and CubicTo consumes [control1, control2, end].
struct TextPathCommand final {
  TextPathCommandKind kind{TextPathCommandKind::MoveTo};
  TextPathPoint control1{};
  TextPathPoint control2{};
  TextPathPoint end{};
};

enum class TextPathOverflow : std::uint8_t {
  Clip = 0,
  Visible,
  ScaleToFit,
};

/// Layer-wide text-on-path authoring. Geometry is inline and normalized so a
/// saved project is deterministic and does not depend on a UI preset catalog.
struct TextPath final {
  bool enabled{false};
  std::string geometryId;
  std::string presetId;
  std::vector<TextPathCommand> commands;
  /// Fraction of total path length. Values outside [0, 1] intentionally
  /// support scrolling and multiple revolutions when [loop] is enabled.
  float startOffset{0.0F};
  /// Signed reference-canvas pixels along the path normal.
  float baselineOffset{0.0F};
  TextPathOverflow overflow{TextPathOverflow::Clip};
  bool loop{false};
  bool rotateToTangent{true};
  bool keepUpright{true};
};

/// Layer-wide effects that are independent from per-run typography.
struct TextLayerAppearance final {
  TextBackdropStack backdrops{};
  TextSdfMaterial sdfMaterial{};
  TextBend bend{};
  TextPath path{};
  TextVisualExtentPolicy visualExtent{};
  /// Static whole-layer alpha. GlobalAlpha automation multiplies this value
  /// and never rewrites authored RGBA.
  float globalAlpha{1.0F};
};

/// Installs one canonical built-in path. "none" clears and disables the path.
/// The generated geometry is persisted by callers, so renderer behavior never
/// depends on this preset helper after a project has been saved.
bool ConfigureTextPathPreset(TextPath &path, std::string_view presetId,
                             std::string *error = nullptr);

bool ValidateTextLayerAppearance(const TextLayerAppearance &appearance,
                                 std::string *error = nullptr);

} // namespace videocut::text
