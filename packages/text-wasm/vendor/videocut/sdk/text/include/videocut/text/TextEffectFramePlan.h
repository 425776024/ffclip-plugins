#pragma once

#include "videocut/text/RichTextDocument.h"
#include "videocut/text/TextAnimation.h"
#include "videocut/text/TextLayerAppearance.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace videocut::text {

struct TextEffectRect final {
  float x{0.0F};
  float y{0.0F};
  float width{0.0F};
  float height{0.0F};
};

struct TextEffectUnitInput final {
  /// Unique non-positional identity used by random evaluation and post-reflow
  /// rebinding. Duplicate identities make a frame invalid.
  std::uint64_t stableUnitId{0U};
  /// Zero-based position in TextEffectFrameInput::units. Neighbor lookup and
  /// the default transform anchor range use this authored order.
  std::size_t index{0U};
  std::size_t renderIndex{0U};
  std::int32_t type{0};
  std::size_t row{0U};
  std::size_t indexInRow{0U};
  float fontSize{0.0F};
  /// Tight visible glyph bounds in the frame's coordinate domain. Product
  /// evaluation frames use top-left/Y-down reference coordinates; the private
  /// Qt Effect Program projection frame uses centered/Y-up source coordinates.
  TextEffectRect tightRect{};
  /// Non-tight layout bounds in the same domain. Selector position uses this
  /// rect while transform anchors use tightRect unless explicitly authored.
  TextEffectRect rect{};
  /// Qt Effect Program-facing unit rect. Product frames populate the legacy
  /// script/letter bounds here while keeping selector layout bounds in rect.
  TextEffectRect programRect{};
  float initialPositionX{0.0F};
  float initialPositionY{0.0F};
  Color instanceColor{1.0F, 1.0F, 1.0F, 1.0F};
  /// Canonical shaped-owner identity used by animation targeting. UTF-8
  /// offsets are document-relative; word offsets share that same domain.
  std::string contentSlotId;
  std::string paragraphId;
  std::string runId;
  std::uint64_t documentUtf8Begin{0U};
  std::uint64_t documentUtf8End{0U};
  std::uint64_t documentWordUtf8Begin{0U};
  std::uint64_t documentWordUtf8End{0U};
  std::size_t visualLineIndex{0U};
  /// Baseline for this unit's visual line in the same domain as rect/tightRect.
  float baselineY{0.0F};
  /// Dense authored-order index among normal drawable units. Absence marks an
  /// auxiliary unit such as whitespace or a line-break placeholder. When a
  /// program consumes normal-unit inputs, all present indices must form the
  /// exact sequence [0, normalUnitCount).
  std::optional<std::size_t> normalUnitIndex;
  /// Unicode scalar represented by this unit, or zero for a grapheme cluster
  /// that cannot be represented by one scalar.
  std::uint32_t unicodeCodepoint{0U};
};

struct TextEffectFrameInput final {
  double progress{0.0};
  std::int64_t timeUs{0};
  std::vector<TextEffectUnitInput> units;
  std::vector<TextEffectRect> tightRowRects;
  std::vector<TextEffectRect> rowRects;
  TextEffectRect tightTextRect{};
  TextEffectRect textRect{};
  /// Required exactly when a program consumes is_normal_unit,
  /// normal_unit_index, or normal_unit_count. Keeping it optional makes an
  /// unpopulated caller fail closed instead of silently treating all units as
  /// normal.
  std::optional<std::size_t> normalUnitCount;
  /// Source canvas before the SDF guard, in the frame's coordinate domain.
  /// Required when a program reads canvas_rect_width/height; absence fails
  /// closed instead of substituting the logical wrapping box.
  std::optional<TextEffectRect> canvasRect{std::nullopt};
  /// Physical render destination pixels; never rescaled with text geometry.
  /// Required when a program reads output_width/height.
  std::optional<std::array<float, 2>> outputSize{std::nullopt};
  /// Signed source-design -> output axis scales, supplied by the renderer's
  /// presentation transform. This metadata is not projected a second time.
  std::optional<std::array<float, 2>> sourceToOutputScale{std::nullopt};
  /// Physical output position of the centered source-design origin. This
  /// metadata remains in output pixels when glyph geometry is projected.
  std::optional<std::array<float, 2>> sourceOriginOutput{std::nullopt};
  /// Resolved animation clock duration, independent of geometry. Required
  /// when a program reads animation_duration_us; must be positive.
  std::optional<std::int64_t> animationDurationUs{std::nullopt};
  /// Layout authority's writing mode; required by is_vertical_writing.
  std::optional<TextWritingMode> writingMode{std::nullopt};
};

struct TextEffectMatrix4x4 final {
  /// Metal-compatible column-major storage. This is also the sole matrix byte
  /// order consumed by non-Apple renderers.
  std::array<float, 16> columnMajor{
      1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
      0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
};

/// Renderer-independent authored transform input. It never reaches a drawing
/// backend; ResolveTextEffectTransformPlan resolves it exactly once.
struct TextEffectTransformComponents final {
  float offsetX{0.0F};
  float offsetY{0.0F};
  float offsetZ{0.0F};
  float rotationX{0.0F};
  float rotationY{0.0F};
  float rotationZ{0.0F};
  /// Anchor-relative skew angles in degrees.
  float shearX{0.0F};
  float shearY{0.0F};
  float scaleX{1.0F};
  float scaleY{1.0F};
  float scaleZ{1.0F};
  float anchorX{0.0F};
  float anchorY{0.0F};
  float anchorZ{0.0F};
  TextProjectionProfile projection{};
  /// Non-empty half-open range into TextEffectFrameInput::units.
  std::size_t anchorRangeBegin{0U};
  std::size_t anchorRangeEnd{0U};
};

/// Runtime-only deferred anchor contract for one decomposed Qt Effect Program
/// transform. Program sampling stays in the evaluator; after an absolute font
/// size rebuild the same sampled components are resolved once more against the
/// final shaped units named here.
struct TextEffectProgramTransformRetargetPlan final {
  TextEffectTransformComponents sourceComponents{};
  std::vector<std::uint64_t> anchorStableUnitIds;
  /// Scene-entity transforms are anchored to the reference canvas;
  /// letter-domain transforms remain anchored to the named shaped units.
  bool anchorToReferenceCanvas{false};
};

struct TextEffectTransformPlan final {
  TextEffectMatrix4x4 localToText{};
  TextEffectRect tightAnchorBounds{};
  TextEffectRect layoutAnchorBounds{};
  std::optional<TextEffectProgramTransformRetargetPlan> programRetarget;
};

struct TextEffectUnitFramePlan final {
  std::uint64_t stableUnitId{0U};
  std::optional<float> opacity;
  std::optional<Color> instanceColor;
  /// Absolute shaping size. This is consumed before final layout/SDF and must
  /// never be represented as a picture or compositor scale.
  std::optional<float> absoluteFontSize;
  /// Per-unit signed-distance blur radius. The renderer samples this before
  /// material coverage; it is never a final-RGBA image filter.
  std::optional<float> sdfBlurRadius;
  std::vector<TextEffectTransformPlan> transforms;
  /// Optional BMP non-surrogate glyph substitution applied before final
  /// shaping. The unit identity and animation topology remain unchanged.
  std::optional<std::uint32_t> replacementCodepoint;
};

enum class TextEffectLayoutMutationKind : std::uint8_t {
  AbsoluteFontSize = 0,
  LetterSpacing,
  WordSpacing,
  BaselineShift,
  ParagraphLineHeight,
};

struct TextEffectLayoutMutation final {
  TextEffectLayoutMutationKind kind{
      TextEffectLayoutMutationKind::AbsoluteFontSize};
  std::uint64_t stableUnitId{0U};
  TextPropertyCombineMode combineMode{TextPropertyCombineMode::Replace};
  float value{0.0F};
};

enum class TextEffectResourceKind : std::uint8_t {
  Unspecified = 0,
  Font,
  Texture,
  Vector,
  Animated,
  Mesh,
  FloatTexture,
};

struct TextEffectResourceSample final {
  std::string resourceId;
  std::string assetId;
  std::string digest;
  std::int64_t localTimeUs{0};
  TextEffectResourceKind kind{TextEffectResourceKind::Unspecified};
  std::string mediaType;
};

struct TextEffectSize final {
  float width{0.0F};
  float height{0.0F};
};

struct TextEffectOutsets final {
  float left{0.0F};
  float top{0.0F};
  float right{0.0F};
  float bottom{0.0F};
};

struct TextEffectUnitRange final {
  std::size_t begin{0U};
  std::size_t end{0U};
};

struct TextEffectGlyphMaterialPass final {
  std::string passId;
  std::string layerId;
  std::int32_t zOrder{0};
  TextBlendMode blend{TextBlendMode::SourceOver};
  TextPropertyCombineMode combineMode{TextPropertyCombineMode::Replace};
  TextGlyphMaterialLayer material{TextFillLayer{}};
  std::vector<std::uint64_t> stableUnitIds;
};

struct TextEffectBackdropPass final {
  std::string passId;
  std::string layerId;
  std::int32_t zOrder{-100};
  TextBackdropSource source{RoundedRectBackdrop{}};
  TextBackdropTransform transform{};
  TextEffectUnitRange basedRange{};
  TextEffectRect basedRect{};
  TextEffectSize sourceIntrinsicSize{};
  TextBackdropFitMode fitMode{TextBackdropFitMode::Stretch};
  float sourcePivotX{0.5F};
  float sourcePivotY{0.5F};
  TextEffectOutsets expand{};
  TextEffectOutsets sourceOutsets{};
  TextEffectRect bounds{};
  std::int64_t localTimeUs{0};
};

enum class TextEffectDecorationEffectScope : std::uint8_t {
  InsideRenderGroupBehindText = 0,
  InsideRenderGroupInFrontOfText,
  AfterRenderGroupEffect,
};

struct TextEffectDecorationPass final {
  std::string passId;
  std::string layerId;
  std::string decorationId;
  std::string assetId;
  std::int32_t zOrder{0};
  TextEffectDecorationEffectScope effectScope{
      TextEffectDecorationEffectScope::InsideRenderGroupInFrontOfText};
  TextEffectUnitRange basedRange{};
  TextEffectRect basedRect{};
  TextEffectSize sourceIntrinsicSize{};
  TextBackdropFitMode fitMode{TextBackdropFitMode::Stretch};
  float sourcePivotX{0.5F};
  float sourcePivotY{0.5F};
  TextEffectOutsets expand{};
  TextEffectOutsets sourceOutsets{};
  TextEffectRect bounds{};
  TextEffectTransformPlan transform{};
  float opacity{1.0F};
  std::int64_t assetTimeUs{0};
};

struct TextEffectRenderGroupExecutionPlan final {
  std::string layerId;
  std::uint64_t renderGroupInstanceId{0U};
  TextRenderGroupSpec spec{};
  TextEffectUnitRange fixedUnitRange{};
  TextEffectRect fixedGeometryBounds{};
  TextEffectRect visualSourceBounds{};
  /// Empty retains the renderer's canonical glyph/backdrop source. Explicit
  /// IDs select named FramePlan components in authored order.
  std::vector<std::string> sourceInputIds;
  std::vector<std::string> behindDecorationInputIds;
  std::vector<std::string> frontDecorationInputIds;
};

struct TextEffectPostEffectNode final {
  std::string nodeId;
  std::string layerId;
  std::uint64_t effectNodeInstanceId{0U};
  TextPostEffectKind kind{TextPostEffectKind::GaussianBlur};
  std::vector<std::string> inputIds;
  std::vector<TextPostEffectParameter> parameters;
  float amount{0.0F};
  float paddingPx{0.0F};
  std::int64_t effectTimeUs{0};
  std::optional<TextEffectRenderGroupExecutionPlan> renderGroup;
};

enum class TextEffectCompositeItemKind : std::uint8_t {
  Backdrop = 0,
  GlyphMaterial,
  Decoration,
  PostEffect,
};

struct TextEffectCompositeItem final {
  std::string itemId;
  TextEffectCompositeItemKind kind{TextEffectCompositeItemKind::GlyphMaterial};
  std::int32_t zOrder{0};
  TextBlendMode blend{TextBlendMode::SourceOver};
};

struct TextEffectStateTransition final {
  std::string stateId;
  bool active{false};
  double progress{0.0};
};

/// Evaluated, source-free parameter packet addressed to one closed execution
/// graph node. nodeId is identity only; parameter and slot select executable
/// semantics. Page samples omit stableUnitId.
struct TextEffectExecutionParameterSample final {
  std::string nodeId;
  TextEffectExecutionParameterKind parameter{
      TextEffectExecutionParameterKind::PostEffectProgress};
  TextEffectExecutionParameterDomain domain{
      TextEffectExecutionParameterDomain::Page};
  TextEffectExecutionParameterSpace valueSpace{
      TextEffectExecutionParameterSpace::Unitless};
  std::uint32_t slot{0U};
  std::optional<std::uint64_t> stableUnitId;
  std::vector<float> values;
};

struct TextEffectExecutionNodeFramePlan final {
  std::string nodeId;
  TextEffectExecutionNodeKind kind{TextEffectExecutionNodeKind::Scene};
  TextEffectExecutionCapability capability{
      TextEffectExecutionCapability::None};
  std::vector<std::string> inputIds;
  std::vector<std::string> resourceIds;
  std::optional<TextPostEffectKind> postEffectKind;
  std::string stateId;
  std::string historyId;
  std::int64_t effectTimeUs{0};
  double progress{0.0};
  bool active{true};
  /// A node clock is independent of the parameter-owning animation layer.
  /// Executors must not replace it with the sampled post-effect owner clock.
  bool hasAuthoredTimeDriver{false};
  std::uint64_t stateRevision{0U};
  std::int64_t stateElapsedUs{0};
  std::uint64_t randomSeed{0U};
  std::uint64_t historyRevision{0U};
  std::optional<TextEffectPhysicsSpec> physicsSpec;
  std::optional<TextEffectCollisionSpec> collisionSpec;
  std::optional<TextEffectExecutionStaticAffine> staticAffine;
  std::optional<TextEffectExecutionCamera> camera;
  std::vector<TextEffectExecutionParameterSample> parameters;
  std::vector<TextEffectExecutionNodeFramePlan> children;
};

struct TextEffectExecutionGraphFramePlan final {
  std::vector<TextEffectExecutionNodeFramePlan> nodes;
};

struct TextEffectFrameCacheIdentities final {
  std::string resources;
  std::string layout;
  std::string glyphs;
  std::string backdrops;
  std::string postEffects;
  std::string composite;
  std::string executionGraph;
};

/// One bounds decision shared by preview, playback and export. Visual and
/// expandedRenderTargetBounds may exceed stable editor control geometry.
struct TextEffectFrameBounds final {
  TextEffectRect layoutBounds{};
  TextEffectRect controlBounds{};
  TextEffectRect inkBounds{};
  TextEffectRect visualBounds{};
  TextEffectRect expandedRenderTargetBounds{};
};

/// Pure per-sample output. It owns no authored state and is never serialized.
struct TextEffectFramePlan final {
  std::string programId;
  /// Native Letter writer coordinate convention selected from the persisted
  /// source creation component after sampled properties have been applied.
  TextSourceCreationComponent sourceCreationComponent{
      TextSourceCreationComponent::LegacyText};
  std::vector<TextPropertyAssignment> sampledProperties;
  std::vector<TextEffectUnitFramePlan> units;
  std::vector<TextEffectLayoutMutation> layoutMutations;
  std::vector<TextEffectGlyphMaterialPass> glyphMaterialPasses;
  std::vector<TextEffectBackdropPass> backdropPasses;
  std::vector<TextEffectDecorationPass> decorationPasses;
  std::vector<TextEffectPostEffectNode> postEffectNodes;
  std::vector<TextEffectCompositeItem> compositeOrder;
  std::vector<TextEffectResourceSample> resources;
  std::vector<TextEffectStateTransition> stateTransitions;
  /// Program samples are retained in authored evaluation order and also
  /// attached to the matching executionGraph node by EvaluateTextEffects.
  std::vector<TextEffectExecutionParameterSample> executionParameters;
  TextEffectFrameBounds bounds{};
  TextEffectFrameCacheIdentities cacheIdentities{};
  TextEffectExecutionGraphFramePlan executionGraph;
};

} // namespace videocut::text
