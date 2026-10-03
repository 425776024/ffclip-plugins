#pragma once

#include "videocut/text/RichTextDocument.h"
#include "videocut/text/TextEffectProgramIR.h"
#include "videocut/text/TextProperty.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace videocut::text {

enum class TextAnimationTimeDriverKind : std::uint8_t {
  EnterPhase = 0,
  ExitPhase,
  LoopPhase,
  CaptionPhase,
  ClipLocal,
  TimedRanges,
};

enum class TextAnimationPlaybackMode : std::uint8_t {
  Once = 0,
  Loop,
  PingPong,
  Hold,
};

enum class TextTimedDriverMode : std::uint8_t {
  SpanStep = 0,
  SpanProgress,
  GraphemeSweep,
  ActiveHold,
};

struct TextTimedRangeDriverSpec final {
  TextTimedDriverMode mode{TextTimedDriverMode::SpanProgress};
  /// Empty targets every canonical timed span. Non-empty vectors are stable
  /// span identities and preserve authored layer ordering for property
  /// composition.
  std::vector<std::string> spanIds;
  /// Generic animatable UTF-8-range properties. Authored targets are replaced
  /// with the active span range during sampling.
  std::optional<TextPropertyPatch> activePatch;
};

struct TextAnimationTimeDriver final {
  TextAnimationTimeDriverKind kind{TextAnimationTimeDriverKind::ClipLocal};
  std::int64_t startOffsetUs{0};
  std::int64_t durationUs{1'000'000};
  TextAnimationPlaybackMode playback{TextAnimationPlaybackMode::Once};
  std::optional<TextTimedRangeDriverSpec> timedRanges;
};

enum class TextUnitBasis : std::uint8_t {
  Grapheme = 0,
  Word,
  Line,
  All,
};

enum class TextSelectorShape : std::uint8_t {
  Linear = 0,
  RampUp,
  RampDown,
  Triangle,
  Round,
  Smooth,
  CustomCubic,
  Square,
};

enum class TextSelectorKind : std::uint8_t {
  Range = 0,
  Time,
};

enum class TextUnitOrder : std::uint8_t {
  Forward = 0,
  Backward,
  CenterOut,
  Random,
};

struct TextSelectorKeyframe final {
  double offset{0.0};
  double value{0.0};
  double tangentIn{0.0};
  double tangentOut{0.0};
  /// Qt/Amazing stores independent cubic control points for time and value.
  /// Tangents retain the value controls while these normalized time handles
  /// preserve the non-linear keyframe clock exactly.
  bool cubicBezier{false};
  double bezierTimeIn{0.0};
  double bezierTimeOut{0.0};
};

struct TextUnitSelector final {
  TextSelectorKind kind{TextSelectorKind::Range};
  TextUnitBasis basedOn{TextUnitBasis::Grapheme};
  double rangeStart{0.0};
  double rangeEnd{1.0};
  double offset{0.0};
  double stagger{0.0};
  double edgeSmooth{0.0};
  TextSelectorShape shape{TextSelectorShape::Linear};
  bool constrained{true};
  TextUnitOrder order{TextUnitOrder::Forward};
  /// Binary64 authored seed consumed directly by the deterministic random map.
  double randomSeed{0.0};
  double intensity{1.0};
  double intensityStart{0.0};
  double intensityEnd{1.0};
  /// Time selectors remap the owning animator's property clock independently
  /// for every selected unit. Values are normalized to the layer duration and
  /// may extend outside [0, 1] to hold the first/last property keyframe.
  double timeStart1{0.0};
  double timeStart2{0.0};
  double timeEnd1{1.0};
  double timeEnd2{1.0};
  bool timeCycle{false};
  std::vector<TextSelectorKeyframe> rangeStartKeyframes;
  std::vector<TextSelectorKeyframe> rangeEndKeyframes;
  /// Optional Qt/Lumi selector-attribute curves. A missing curve preserves the
  /// corresponding scalar field, which keeps older template payloads stable.
  std::vector<TextSelectorKeyframe> offsetKeyframes;
  std::vector<TextSelectorKeyframe> intensityKeyframes;
};

enum class TextAnimatedProperty : std::uint8_t {
  Opacity = 0,
  PositionX,
  PositionY,
  ScaleX,
  ScaleY,
  PositionZ,
  RotationX,
  RotationY,
  RotationZ,
  ShearX,
  ShearY,
  Tracking,
  BlurRadius,
  DistanceFromCenter,
};

struct TextKeyframe final {
  double offset{0.0};
  double value{0.0};
  double tangentIn{0.0};
  double tangentOut{0.0};
  bool cubicBezier{false};
  double bezierTimeIn{0.0};
  double bezierTimeOut{0.0};
};

struct TextAnimatorTrack final {
  TextAnimatedProperty property{TextAnimatedProperty::Opacity};
  std::vector<TextKeyframe> keyframes;
};

struct TextColorKeyframe final {
  float offset{0.0F};
  Color value{};
  Color tangentIn{0.0F, 0.0F, 0.0F, 0.0F};
  Color tangentOut{0.0F, 0.0F, 0.0F, 0.0F};
  bool cubicBezier{false};
  float bezierTimeIn{0.0F};
  float bezierTimeOut{0.0F};
};

enum class TextPositionMode : std::uint8_t {
  AbsolutePixels = 0,
  TextBoundsOffset,
  DistanceFromCenter,
  SpaceX,
  SpaceY,
};

enum class TextUnitAnchor : std::uint8_t {
  GlyphCenter = 0,
  Baseline,
  LineBox,
};

/// Geometry range used by Lumi's ExtraMatrix anchor calculation. This is
/// independent of TextUnitAnchor: the latter selects VideoCut's base transform
/// origin, while this range determines the rect to which normalized authored
/// anchor offsets are relative.
enum class TextAnimatorAnchorBasis : std::uint8_t {
  Letter = 0,
  Line,
  Word,
  Page,
};

/// Lumi TextAnchorClip mode. Fixed applies the full authored offset whenever
/// selector influence is non-zero; SelectorInfluenced scales it by that
/// influence.
enum class TextAnimatorAnchorMode : std::uint8_t {
  Fixed = 0,
  SelectorInfluenced,
};

enum class TextProjectionKind : std::uint8_t {
  Planar2D = 0,
  Perspective3D,
};

struct TextProjectionProfile final {
  TextProjectionKind kind{TextProjectionKind::Planar2D};
  float fieldOfViewDegrees{45.0F};
  float vanishingPointX{0.5F};
  float vanishingPointY{0.5F};
};

enum class TextUnitPresentation : std::uint8_t {
  Transform = 0,
  Reveal,
  ActiveFill,
};

/// One declarative per-unit animation. Target identity is optional; an empty
/// target means all canonical content. Reveal/cursor fields are part of this
/// animator instead of a parallel subtitle or reveal model.
struct TextAnimatorSpec final {
  std::string animatorId;
  std::vector<std::string> paragraphIds;
  std::vector<std::string> runIds;
  std::vector<TextUnitSelector> selectors;
  std::vector<TextAnimatorTrack> tracks;
  std::vector<TextColorKeyframe> fillColorKeyframes;
  /// Empty when selectors/tracks fully describe the animator. Otherwise this
  /// resolves into the owning stack's closed native effect-program library.
  std::string effectProgramId;
  TextPositionMode positionMode{TextPositionMode::AbsolutePixels};
  TextUnitAnchor anchor{TextUnitAnchor::GlyphCenter};
  TextAnimatorAnchorBasis anchorBasis{TextAnimatorAnchorBasis::Letter};
  TextAnimatorAnchorMode anchorMode{TextAnimatorAnchorMode::Fixed};
  float anchorOffsetX{0.0F};
  float anchorOffsetY{0.0F};
  TextProjectionProfile projection{};
  TextUnitPresentation presentation{TextUnitPresentation::Transform};
  float fadeFraction{0.0F};
  Color activeColor{1.0F, 1.0F, 0.0F, 1.0F};
  RevealCursor cursor{};
};

struct TextPostEffectParameter final {
  std::string name;
  /// Scalar, boolean (0/1), vector, or RGBA value. The closed renderer for the
  /// owning nodeKind interprets it; parameter names never become code.
  std::vector<float> values;
  /// Dynamic parameters are scalar in the reference format. Vector values
  /// stay in values until a typed vector-keyframe contract is introduced.
  std::vector<TextKeyframe> keyframes;
};

/// Closed native post-effect set accepted by the text renderer. Template
/// packages cannot introduce executable node kinds.
enum class TextPostEffectKind : std::uint8_t {
  TurbulenceDisplacement = 0,
  DirectionalBlur,
  GodRay,
  LinearWipe,
  Dust,
  DeepGlow,
  SGlow,
  SoftGlow,
  RadialBlur,
  Shake,
  Trail,
  WaveWarp,
  DistortChroma,
  GaussianBlur,
  ChromaticAberration,
  AlphaOutline,
  RadianceGlow,
  MultiShadow,
  SimpleChoker,
  CCLens,
  OpticsCompensation,
  Projection,
  DynamicSignalGlitch,
  ElectricPulseMotionBlur,
  MorphologicalOutline,
  AlternatingSegmentMask,
  CenterSplitDisplacement,
  CutAndDrop,
  BlockGlitchDotMatrix,
  ProceduralFlameOutline,
  CylindricalScroll,
  SplitSqueezeWave,
  PerspectiveEchoTrail,
  /// Standalone spatial pulse envelope recovered from the legacy
  /// ElectricPulseMotionBlur plug-in contract.  Directional blur remains a
  /// separate DAG node so Product templates can compose either capability.
  PulseEnvelope,
};

struct TextPostEffectSpec final {
  std::string effectId;
  TextPostEffectKind kind{TextPostEffectKind::GaussianBlur};
  /// Authored DAG inputs. Empty selects the compatibility rule in the pure
  /// evaluator: chain from the preceding enabled post node in declaration
  /// order. Non-empty inputs are preserved exactly and never rewritten by a
  /// renderer.
  std::vector<std::string> inputIds;
  /// Static strength retained for simple authoring controls. [kind] selects
  /// the one current native implementation.
  float amount{0.0F};
  float paddingPx{0.0F};
  std::vector<TextKeyframe> amountKeyframes;
  std::vector<TextPostEffectParameter> parameters;
};

enum class TextDecorationExtentSpace : std::uint8_t {
  TextLocal = 0,
  CanvasWidth,
  CanvasHeight,
  CanvasFull,
};

enum class TextDecorationTransformInherit : std::uint8_t {
  Full = 0,
  TranslateOnly,
};

enum class TextAnimatedDecorationAnchor : std::uint8_t {
  TextBoundsCenter = 0,
  CanvasCenter,
};

/// Inherit preserves the Decoration binding's fit. Other values are a typed
/// decoration-local override sampled with the owning animation layer.
enum class TextAnimatedDecorationFit : std::uint8_t {
  Inherit = 0,
  Contain,
  Cover,
  Stretch,
  Native,
  FitWidth,
  FitHeight,
  FitLongSide,
  FitShortSide,
};

struct TextDecorationAnchorKeyframe final {
  float offset{0.0F};
  TextAnimatedDecorationAnchor value{TextAnimatedDecorationAnchor::TextBoundsCenter};
};

struct TextDecorationFitKeyframe final {
  float offset{0.0F};
  TextAnimatedDecorationFit value{TextAnimatedDecorationFit::Inherit};
};

struct TextDecorationAnimationSpec final {
  std::string decorationId;
  std::string assetId;
  TextAnimatedDecorationAnchor anchor{TextAnimatedDecorationAnchor::TextBoundsCenter};
  std::vector<TextDecorationAnchorKeyframe> anchorKeyframes;
  TextAnimatedDecorationFit fit{TextAnimatedDecorationFit::Inherit};
  std::vector<TextDecorationFitKeyframe> fitKeyframes;
  TextDecorationExtentSpace extentSpace{TextDecorationExtentSpace::TextLocal};
  TextDecorationTransformInherit inherit{
      TextDecorationTransformInherit::Full};
  TextAnimationPlaybackMode playback{TextAnimationPlaybackMode::Once};
  float expandRatioX{1.0F};
  float expandRatioY{1.0F};
  /// Transparent asset pixels that must remain inside the allocation even
  /// when the visible alpha contour does not touch the decoded frame edge.
  Insets sourceOutsets{};
  /// Amazing Sprite2D ax/ay local offsets, resolved against the decoded logical
  /// texture and the fixed [-1,+1] mesh by the composition sampler.
  float pivotX{0.0F};
  float pivotY{0.0F};
  float offsetX{0.0F};
  float offsetY{0.0F};
  /// Offsets authored relative to the live Text bounds. Qt follower stickers
  /// use these values for px/py, so changing the bound text keeps the
  /// decoration attached to the same semantic location.
  float relativeOffsetX{0.0F};
  float relativeOffsetY{0.0F};
  float scaleX{1.0F};
  float scaleY{1.0F};
  /// Authored Text_BaseSticker Transform Euler angles. Amazing renders the flat
  /// Sprite2D through an orthographic camera, so X/Y rotations project to a
  /// deterministic affine basis instead of an asset-space image edit.
  float rotationXDegrees{0.0F};
  float rotationYDegrees{0.0F};
  float rotationDegrees{0.0F};
  float opacity{1.0F};
  std::vector<TextKeyframe> pivotXKeyframes;
  std::vector<TextKeyframe> pivotYKeyframes;
  std::vector<TextKeyframe> offsetXKeyframes;
  std::vector<TextKeyframe> offsetYKeyframes;
  std::vector<TextKeyframe> relativeOffsetXKeyframes;
  std::vector<TextKeyframe> relativeOffsetYKeyframes;
  std::vector<TextKeyframe> scaleXKeyframes;
  std::vector<TextKeyframe> scaleYKeyframes;
  std::vector<TextKeyframe> rotationXKeyframes;
  std::vector<TextKeyframe> rotationYKeyframes;
  std::vector<TextKeyframe> rotationKeyframes;
  std::vector<TextKeyframe> opacityKeyframes;
  /// Optional source-media progress. Empty preserves the owning layer's
  /// progress; authored keyframes support frame-accurate follower videos.
  std::vector<TextKeyframe> assetProgressKeyframes;
};

/// Renderer-neutral Text RenderGroup topology. Numeric values intentionally
/// match the authored runtime contract so imported profiles do not need a
/// renderer-specific compatibility enum.
enum class TextRenderGroupMode : std::uint8_t {
  Page = 0,
  PerLetter = 1,
  PerLine = 2,
  PerWord = 3,
  Custom = 99,
};

/// Signed authored range. Resolution clips both endpoints to the available
/// letter domain and emits only half-open [start, end) ranges.
struct TextRenderGroupIndexRange final {
  std::int64_t startIndex{0};
  std::int64_t endIndex{0};
};

/// Optional clock carried by a custom range slice. The affine relative clock
/// is `(time - start) / (end - start) * wholeTime`; endpoint hold is exposed
/// separately by TextRenderGroupClockSample instead of changing that formula.
struct TextRenderGroupTimeRange final {
  std::int64_t startTimeUs{0};
  std::int64_t endTimeUs{0};
};

struct TextRenderGroupCustomRange final {
  std::int64_t startIndex{0};
  std::int64_t endIndex{0};
  std::optional<TextRenderGroupTimeRange> localTime;
  float intensity{1.0F};
};

/// Native TextPro RenderGroup envelope and authored selector metadata. Mode,
/// expansion, explicit custom ranges and the closed selector shape participate
/// in topology resolution. Offset/priority/random/duration remain authored
/// metadata for their later effect-selector stages.
struct TextRenderGroupSpec final {
  float expandRatioX{1.0F};
  float expandRatioY{1.0F};
  TextRenderGroupMode mode{TextRenderGroupMode::Page};
  float offset{0.0F};
  std::optional<TextRenderGroupTimeRange> duration;
  std::int32_t priority{0};
  std::uint32_t randomSeed{0};
  bool randomSort{false};
  TextSelectorShape shape{TextSelectorShape::Square};
  std::vector<TextRenderGroupCustomRange> customRanges;
};

/// Direct renderer-neutral counterpart of the reference InitValues inputs:
/// letterCount, getLineList() and getWordList(). Layout/shaping remains the
/// authority for whitespace, hard-break and Unicode word boundaries.
struct TextRenderGroupUnitTopology final {
  std::size_t letterCount{0};
  std::vector<TextRenderGroupIndexRange> lineRanges;
  std::vector<TextRenderGroupIndexRange> wordRanges;
};

struct ResolvedTextRenderGroupRange final {
  std::size_t startIndex{0};
  std::size_t endIndex{0};
  std::optional<TextRenderGroupTimeRange> localTime;
  float intensity{1.0F};
};

/// Continuous range geometry in the caller's render space. Allocation
/// rounding is deliberately represented by TextRenderGroupRasterSize instead
/// of being written back into this rect.
struct TextRenderGroupContinuousRect final {
  double originX{0.0};
  double originY{0.0};
  double extentWidth{0.0};
  double extentHeight{0.0};
};

struct TextRenderGroupRasterSize final {
  std::uint32_t width{0U};
  std::uint32_t height{0U};
};

inline constexpr std::uint32_t kTextRenderGroupMaximumRasterDimension = 4096U;

/// Package-neutral, per-range frame snapshot. The resolver is the only public
/// construction path and every field is const so source, post-effect and
/// composite consumers share one range and geometry decision for the frame.
struct ResolvedTextRenderGroupFrame final {
  ResolvedTextRenderGroupFrame(const ResolvedTextRenderGroupFrame &) = default;
  ResolvedTextRenderGroupFrame(ResolvedTextRenderGroupFrame &&) = default;
  ResolvedTextRenderGroupFrame &
  operator=(const ResolvedTextRenderGroupFrame &) = delete;
  ResolvedTextRenderGroupFrame &
  operator=(ResolvedTextRenderGroupFrame &&) = delete;

  const ResolvedTextRenderGroupRange range;
  const TextRenderGroupContinuousRect sourceRect;
  const double expandRatioX;
  const double expandRatioY;
  const TextRenderGroupContinuousRect expandedRect;
  /// Positive ceil of the continuous expanded extent, before the engine's
  /// longest-edge safety reduction.
  const TextRenderGroupRasterSize naturalRasterSize;
  /// Physical allocation after the longest edge is limited to 4096 while
  /// preserving the natural integer aspect ratio.
  const TextRenderGroupRasterSize rasterSize;
  /// Physical pixels per continuous expanded unit. This includes both ceil
  /// quantization and any 4096 safety reduction.
  const double quantScaleX;
  const double quantScaleY;
  /// Physical allocation relative to the natural ceil allocation. It remains
  /// exactly one unless the 4096 safety reduction is active.
  const double rasterScaleX;
  const double rasterScaleY;

private:
  ResolvedTextRenderGroupFrame(
      const ResolvedTextRenderGroupRange &resolvedRange,
      const TextRenderGroupContinuousRect &continuousSourceRect,
      double resolvedExpandRatioX, double resolvedExpandRatioY,
      const TextRenderGroupContinuousRect &continuousExpandedRect,
      TextRenderGroupRasterSize resolvedNaturalRasterSize,
      TextRenderGroupRasterSize resolvedRasterSize, double resolvedQuantScaleX,
      double resolvedQuantScaleY, double resolvedRasterScaleX,
      double resolvedRasterScaleY) noexcept;

  friend std::optional<ResolvedTextRenderGroupFrame>
  ResolveTextRenderGroupFrame(
      const ResolvedTextRenderGroupRange &resolvedRange,
      const TextRenderGroupContinuousRect &continuousSourceRect,
      double resolvedExpandRatioX, double resolvedExpandRatioY) noexcept;
};

enum class TextRenderGroupClockPhase : std::uint8_t {
  Before = 0,
  Active,
  After,
  Invalid,
};

/// Both clocks are intentional: effectTimeUs preserves the exact unbounded
/// affine call-chain value, while heldEffectTimeUs/progress provide the
/// endpoint-hold projection used by closed keyframe samplers.
struct TextRenderGroupClockSample final {
  bool valid{false};
  bool hasLocalTime{false};
  TextRenderGroupClockPhase phase{TextRenderGroupClockPhase::Invalid};
  double progress{0.0};
  double heldProgress{0.0};
  double effectTimeUs{0.0};
  double heldEffectTimeUs{0.0};
};

struct TextAnimationLayerSpec final {
  std::string layerId;
  bool enabled{true};
  TextPropertyTarget target{};
  TextPropertyCombineMode combineMode{TextPropertyCombineMode::Replace};
  TextAnimationTimeDriver timeDriver{};
  std::optional<TextLayerAnimationClip> layerTrack;
  std::vector<TextAnimatorSpec> animators;
  std::optional<TextRenderGroupSpec> renderGroup;
  std::vector<TextDecorationAnimationSpec> decorations;
  std::vector<TextPostEffectSpec> postEffects;
  bool requiresTimedText{false};
};

/// Closed authored execution topology. Node and edge declaration order is
/// semantically significant and is preserved through frame planning.
enum class TextEffectExecutionNodeKind : std::uint8_t {
  Scene = 0,
  MaterialPass,
  PostEffectPass,
  RenderTarget,
  History,
  MediaInput,
  State,
  Composite,
  Layout,
  Selector,
  Operator,
};

/// Closed semantic role for a graph node. Stable identities and resource
/// references never double as executable type discriminators.
enum class TextEffectExecutionCapability : std::uint8_t {
  None = 0,
  LayoutGlyphRun,
  LayoutCaptionModule,
  LayoutPagedText,
  LayoutTimedLyric,
  SelectorBase,
  SelectorTime,
  OperatorProgram,
  OperatorKeyframe,
  OperatorStagger,
  OperatorWipe,
  OperatorTypewriter,
  OperatorReveal,
  OperatorClone,
  OperatorTransform,
  OperatorMaterial,
  OperatorSceneLookup,
  OperatorResource,
  OperatorCommand,
  OperatorEvent,
  OperatorGlyphSubstitution,
  OperatorLifecycle,
  ScenePrefab,
  SceneEntity,
  SceneMesh,
  SceneCamera,
  SceneProjection,
  SceneClone,
  MaterialColorPass,
  MaterialDepthPass,
  MaterialProgramPass,
  RenderTargetOffscreen,
  RenderTargetExpanded,
  HistoryFeedback,
  MediaFont,
  MediaImage,
  MediaImageSequence,
  MediaVideo,
  MediaMesh,
  MediaParticle,
  StateDeterministicRandom,
  StatePhysics,
  StateCollision,
  StateParticle,
  CompositeSourceOver,
  /// Closed material programs. These describe executable operators; authored
  /// node/resource identities never select renderer behavior.
  MaterialAlphaModulate,
  MaterialColorModulate,
  MaterialThresholdRevealBlur,
  MaterialThresholdTransitionBlur,
  MaterialGlyphUvBallisticEchoComposite,
  MaterialDirectionalBoxBlur,
  MaterialSeparableGaussianBlur,
  MaterialWeightedAxisBoxBlur,
  MaterialTurbulenceDisplacement,
  MaterialDeepGlowComposite,
  MaterialSdfAlphaOutline,
  MaterialSdfLightSweep,
  MaterialSdfCutGlow,
  MaterialShapedRampMask,
  MaterialMaskedDualOffsetCrossfade,
  MaterialLineColumnNoiseTrail,
  MaterialParticleScatterComposite,
  MaterialSdfWaveEnergy,
  MaterialMaskMotionNoiseGlow,
  MaterialFaceNoiseShadow,
  MaterialProjectionMeshComposite,
  MaterialVatRbdMesh,
  MaterialMultimeshTextComposite,
  MaterialRadialMediaComposite,
  MaterialSpiralSdfWarp,
  MaterialFluxTurbulentBlend,
  MaterialCubeProjectionComposite,
  MaterialCylinderProjectionComposite,
  MaterialMaskedCutLine,
  MaterialCutLineReveal,
  MaterialPackedMultiscaleGaussianBlur,
  MaterialDualPackedGlowComposite,
  MaterialEncodedAlphaDistanceBlur,
  MaterialSdfNormalLightSweep,
  MaterialLightSweepPackedGlowComposite,
  MaterialRadialDecayHsvGlow,
  MaterialFixedNineTapAxisBlur,
  MaterialHsvOriginalOverBlur,
  MaterialNoiseThresholdDissolve,
};

/// Closed fixed-step Verlet state. All distances and accelerations are in the
/// effect program's authored source-pixel space and are projected once by the
/// evaluator into the canonical reference canvas.
struct TextEffectPhysicsSpec final {
  std::uint32_t fixedStepHz{30U};
  float gravityX{0.0F};
  float gravityY{0.0F};
  float angularDamping{1.0F};
  std::int64_t explosionDelayUs{0};
  std::int64_t explosionForceDurationUs{0};
  std::int64_t explosionRampUpUs{0};
  float explosionAccelerationMin{0.0F};
  float explosionAccelerationMax{0.0F};
  float explosionOriginYShiftFactor{0.0F};
  float explosionAngleRangeDegrees{0.0F};
  float initialAngularVelocityScale{0.0F};
  std::uint64_t randomSeed{0U};
  float randomPhase{0.0F};
  float randomScale{1.0F};
  std::uint32_t angleSeedStride{1U};
  std::uint32_t speedSeedStride{1U};
  std::uint32_t rotationSeedStride{1U};
};

/// Closed collision and boundary solver paired with a preceding StatePhysics
/// node. Boundary enablement is explicit; no canvas or template heuristics are
/// consulted by the evaluator.
struct TextEffectCollisionSpec final {
  float fixedBoxWidth{0.0F};
  float fixedBoxHeight{0.0F};
  float boxCenterXScale{1.0F};
  float wallDamping{0.0F};
  float sideWallVelocityScale{0.0F};
  float topWallVelocityScale{0.0F};
  float bottomWallDamping{0.0F};
  float wallTorqueScale{0.0F};
  float collisionBoundaryScale{1.0F};
  float collisionRestitution{0.0F};
  float collisionTorqueScale{0.0F};
  float minimumCellSize{1.0F};
  float cellSizeScale{1.0F};
  bool expandHorizontalToContent{false};
  bool expandTopToContent{false};
  bool collideLeft{true};
  bool collideRight{true};
  bool collideTop{true};
  bool collideBottom{true};
};

/// Authored node-local 2D affine. Translation and pivot are canonical
/// reference pixels; scale is unitless and rotation is clockwise degrees in
/// the top-left Product coordinate system.
struct TextEffectExecutionStaticAffine final {
  float translationX{0.0F};
  float translationY{0.0F};
  float scaleX{1.0F};
  float scaleY{1.0F};
  float rotationDegrees{0.0F};
  float pivotX{0.0F};
  float pivotY{0.0F};
};

/// Closed world-to-presentation camera payload for scene-aware execution
/// nodes. The matrix is column-major and maps authored world coordinates to
/// clip coordinates. The viewport is a normalized, non-empty top-left rect.
struct TextEffectExecutionCamera final {
  std::array<float, 16U> worldToClip{
      1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
      0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
  std::array<float, 4U> viewport{0.0F, 0.0F, 1.0F, 1.0F};
};

struct TextEffectExecutionNode final {
  std::string nodeId;
  // An animation-template graph belongs to an authored layer independently
  // of its clock: ClipLocal and implicit clocks do not identify ownership.
  std::string ownerLayerId;
  TextEffectExecutionNodeKind kind{TextEffectExecutionNodeKind::Scene};
  TextEffectExecutionCapability capability{
      TextEffectExecutionCapability::None};
  std::vector<std::string> inputIds;
  std::vector<std::string> resourceIds;
  std::optional<TextPostEffectKind> postEffectKind;
  std::string stateId;
  // Authored seed for StateDeterministicRandom. Zero preserves compatibility
  // with older documents by selecting the host-provided deterministic seed.
  std::uint64_t randomSeed{0U};
  std::string historyId;
  std::optional<TextAnimationTimeDriver> timeDriver;
  std::optional<TextEffectPhysicsSpec> physicsSpec;
  std::optional<TextEffectCollisionSpec> collisionSpec;
  std::optional<TextEffectExecutionStaticAffine> staticAffine;
  std::optional<TextEffectExecutionCamera> camera;
  std::vector<TextEffectExecutionNode> children;
};

struct TextEffectExecutionGraph final {
  std::vector<TextEffectExecutionNode> nodes;
};

struct TextAnimationStack final {
  TextEffectProgramLibrary effectPrograms;
  std::vector<TextAnimationLayerSpec> layers;
  TextEffectExecutionGraph executionGraph;
};

/// Precise pure-evaluator clock sample. Rendering integrations keep this value
/// in binary64 until the final TextUnitAnimationSample boundary.
struct TextAnimationLayerEvaluationSample final {
  std::string layerId;
  double progress{0.0};
  bool active{false};
  /// Clock denominator after phase allocation; zero when no single clock
  /// duration exists (for example a collection of independent timed spans).
  std::int64_t durationUs{0};
};

struct TextUnitAnimationSample final {
  float positionX{0.0F};
  float positionY{0.0F};
  float positionZ{0.0F};
  float scaleX{1.0F};
  float scaleY{1.0F};
  float rotationX{0.0F};
  float rotationY{0.0F};
  float rotationZ{0.0F};
  float shearX{0.0F};
  float shearY{0.0F};
  float opacity{1.0F};
  float tracking{0.0F};
  float blurRadius{0.0F};
  float distanceFromCenter{0.0F};
  float anchorOffsetX{0.0F};
  float anchorOffsetY{0.0F};
  std::optional<Color> fillColor;
  float fillColorInfluence{0.0F};
};

struct TextAnimationLayerSample final {
  std::string layerId;
  float progress{0.0F};
  bool active{false};
};

struct TextTimedPropertySample final {
  std::string layerId;
  std::string spanId;
  std::string paragraphId;
  std::string runId;
  std::size_t utf8Begin{0};
  std::size_t utf8End{0};
  TextTimedDriverMode mode{TextTimedDriverMode::SpanStep};
  float progress{0.0F};
  std::optional<float> opacity;
  TextPropertyPatch patch{};
};

struct TextDecorationAnimationSample final {
  TextAnimatedDecorationAnchor anchor{TextAnimatedDecorationAnchor::TextBoundsCenter};
  TextAnimatedDecorationFit fit{TextAnimatedDecorationFit::Inherit};
  float pivotX{0.0F};
  float pivotY{0.0F};
  float offsetX{0.0F};
  float offsetY{0.0F};
  float relativeOffsetX{0.0F};
  float relativeOffsetY{0.0F};
  float scaleX{1.0F};
  float scaleY{1.0F};
  float rotationXDegrees{0.0F};
  float rotationYDegrees{0.0F};
  float rotationDegrees{0.0F};
  float opacity{1.0F};
  float assetProgress{0.0F};
};

/// Conservative neutral-layout expansion required by every authored
/// selector/track and post effect over the complete clip. Values are local
/// reference-canvas pixels on the corresponding side.
struct TextAnimationEnvelope final {
  float left{0.0F};
  float top{0.0F};
  float right{0.0F};
  float bottom{0.0F};
  float effectPaddingPx{0.0F};
};

struct RevealSample final {
  std::size_t visibleUnits{0};
  float nextUnitOpacity{0.0F};
  bool cursorVisible{false};
};

/// Pure absolute-time sampler. It never reads a wall clock and does not depend
/// on a previously sampled frame.
RevealSample SampleReveal(const RevealAnimation &animation,
                          std::int64_t clipLocalTimeUs,
                          std::size_t unitCount) noexcept;

struct TextLayerAnimationSample final {
  float positionX{0.0F};
  float positionY{0.0F};
  float scaleX{1.0F};
  float scaleY{1.0F};
  float rotationDegrees{0.0F};
  float opacity{1.0F};
  TextLayerAnimationPhase phase{TextLayerAnimationPhase::Loop};
  bool active{false};
};

struct ResolvedTextAnimationDurations final {
  std::int64_t enterUs{0};
  std::int64_t loopRegionUs{0};
  std::int64_t exitUs{0};
};

ResolvedTextAnimationDurations
ResolveTextAnimationDurations(const TextAnimationStack &animations,
                              std::int64_t clipDurationUs) noexcept;

/// Pure absolute-time progress for one authored layer. Timed range drivers
/// consume the canonical RichText timed-span projection and never own text.
TextAnimationLayerSample SampleTextAnimationLayer(
    const TextAnimationLayerSpec &layer, std::int64_t clipLocalTimeUs,
    std::int64_t clipDurationUs,
    const std::vector<TimedTextSpan> &timedSpans = {}) noexcept;

/// Stack-aware overload used by resolved child lanes. Enter/exit contraction
/// is computed from every enabled phase so a decoration and its host text share
/// exactly the same short-clip phase boundaries.
TextAnimationLayerSample SampleTextAnimationLayer(
    const TextAnimationStack &stack, const TextAnimationLayerSpec &layer,
    std::int64_t clipLocalTimeUs, std::int64_t clipDurationUs,
    const std::vector<TimedTextSpan> &timedSpans = {}) noexcept;

TextAnimationLayerEvaluationSample SampleTextAnimationLayerEvaluation(
    const TextAnimationStack &stack, const TextAnimationLayerSpec &layer,
    std::int64_t clipLocalTimeUs, std::int64_t clipDurationUs,
    const std::vector<TimedTextSpan> &timedSpans = {}) noexcept;

TextUnitAnimationSample SampleTextAnimator(const TextAnimatorSpec &animator,
                                           std::size_t unitIndex,
                                           std::size_t unitCount,
                                           double layerProgress) noexcept;

/// Resolves Page/Letter/Line/Word/Custom topology without depending on a
/// rendering backend. Line and word ranges come from the caller's canonical
/// layout/unit projection and are clipped, never re-segmented here.
std::vector<ResolvedTextRenderGroupRange>
ResolveTextRenderGroupTopology(const TextRenderGroupSpec &spec,
                               const TextRenderGroupUnitTopology &topology);

/// Resolves the one continuous geometry/allocation contract for a range.
/// Invalid ranges, non-finite or non-positive geometry/ratios, arithmetic
/// overflow and unrepresentable natural raster sizes fail closed.
std::optional<ResolvedTextRenderGroupFrame>
ResolveTextRenderGroupFrame(const ResolvedTextRenderGroupRange &range,
                            const TextRenderGroupContinuousRect &sourceRect,
                            double expandRatioX, double expandRatioY) noexcept;

/// Mirrors the range-slice relative clock. A range without localTime follows
/// the global effect clock (`effectTimeUs == timeUs`).
TextRenderGroupClockSample
SampleTextRenderGroupClock(const ResolvedTextRenderGroupRange &range,
                           std::int64_t timeUs,
                           std::int64_t wholeTimeUs) noexcept;

/// Samples a canonical scalar text curve in binary64, including Qt/Amazing's
/// independent cubic time handles. Narrow only at the renderer boundary.
double SampleTextKeyframeCurve(const std::vector<TextKeyframe> &keyframes,
                               double progress, double fallback) noexcept;

/// Resolves active timed property samples in authored layer order. Later
/// samples intentionally compose after earlier properties in the renderer.
std::vector<TextTimedPropertySample> SampleTextTimedPropertyPatches(
    const TextAnimationStack &stack, std::int64_t clipLocalTimeUs,
    std::int64_t clipDurationUs,
    const std::vector<TimedTextSpan> &timedSpans);

TextDecorationAnimationSample
SampleTextDecorationAnimation(const TextDecorationAnimationSpec &decoration,
                             float layerProgress) noexcept;

std::optional<TextPostEffectKind>
ParseTextPostEffectKind(std::string_view nodeKind) noexcept;

TextAnimationEnvelope
ResolveTextAnimationEnvelope(const TextAnimationStack &animations,
                             float baseWidth, float baseHeight) noexcept;

TextLayerAnimationSample
SampleTextLayerAnimation(const TextAnimationStack &animations,
                         std::int64_t clipLocalTimeUs,
                         std::int64_t clipDurationUs,
                         const std::vector<TimedTextSpan> &timedSpans = {})
    noexcept;

/// Samples phase clips owned by a material sub-layer (for example a bubble)
/// without applying them to the glyph/flower presentation.
TextLayerAnimationSample SampleTextLayerAnimationClips(
    const std::vector<TextLayerAnimationClip> &clips,
    std::int64_t clipLocalTimeUs, std::int64_t clipDurationUs) noexcept;

bool ValidateTextLayerAnimationClip(const TextLayerAnimationClip &clip,
                                    std::string *error = nullptr) noexcept;

void UpsertTextPhaseAnimation(TextAnimationStack &stack,
                              TextLayerAnimationPhase phase,
                              std::optional<TextLayerAnimationClip> clip);
void ReplaceTextRevealAnimations(TextAnimationStack &stack,
                                 std::vector<RevealAnimation> reveals);

bool ValidateTextAnimationStack(const TextAnimationStack &stack,
                                bool hasTimedText,
                                std::string *error = nullptr) noexcept;

/// Project-owned presets are materialized as ordinary typed tracks. Returning
/// nullopt for "none" lets callers clear a phase without magic render paths.
std::optional<TextLayerAnimationClip>
MakeTextLayerAnimationPreset(TextLayerAnimationPhase phase,
                             std::string_view presetId,
                             std::int64_t durationUs);

bool IsTextLayerAnimationPresetSupported(TextLayerAnimationPhase phase,
                                         std::string_view presetId) noexcept;

/// Strict UTF-8 validation used before text reaches ICU/SkParagraph.
bool IsValidUtf8(const std::string &value) noexcept;

} // namespace videocut::text
