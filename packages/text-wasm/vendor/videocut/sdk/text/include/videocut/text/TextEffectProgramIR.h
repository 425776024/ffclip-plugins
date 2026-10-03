#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace videocut::text {

enum class TextEffectValueType : std::uint8_t {
  Scalar = 0,
  Boolean,
};

/// Closed renderer-neutral inputs made available to a text effect program.
/// Geometry is supplied by the shaping/layout authority; programs cannot query
/// renderer objects, the filesystem, the network, or process-global state.
enum class TextEffectInput : std::uint8_t {
  Progress = 0,
  TimeUs,
  UnitIndex,
  RenderIndex,
  UnitType,
  RowIndex,
  IndexInRow,
  UnitCount,
  RowCount,
  FontSize,
  RectX,
  RectY,
  RectWidth,
  RectHeight,
  InitialPositionX,
  InitialPositionY,
  InstanceColorRed,
  InstanceColorGreen,
  InstanceColorBlue,
  InstanceColorAlpha,
  RowRectX,
  RowRectY,
  RowRectWidth,
  RowRectHeight,
  /// Distance between the leftmost and rightmost initial Letter positions on
  /// the current visual row. This is intentionally distinct from row bounds.
  RowInitialPositionSpanX,
  TextRectX,
  TextRectY,
  TextRectWidth,
  TextRectHeight,
  HasPreviousUnit,
  PreviousInitialPositionX,
  PreviousInitialPositionY,
  PreviousRectX,
  PreviousRectY,
  PreviousRectWidth,
  PreviousRectHeight,
  HasNextUnit,
  NextInitialPositionX,
  NextInitialPositionY,
  NextRectX,
  NextRectY,
  NextRectWidth,
  NextRectHeight,
  /// Number of ordinary drawable units on the current visual row.
  RowUnitCount,
  ColumnUnitCount,
  /// Stage-persistent positions published by an earlier stage. These are
  /// explicit scalar dataflow channels, not inferred renderer transforms.
  CurrentPositionX,
  CurrentPositionY,
  PreviousCurrentPositionX,
  PreviousCurrentPositionY,
  NextCurrentPositionX,
  NextCurrentPositionY,
  HasCurrentPosition,
  HasPreviousCurrentPosition,
  HasNextCurrentPosition,
  /// Dense caller-authored topology for ordinary drawable text units.
  /// normal_unit_index returns normal_unit_count (an out-of-range sentinel)
  /// for auxiliary units; is_normal_unit is the membership predicate.
  IsNormalUnit,
  NormalUnitIndex,
  NormalUnitCount,
  /// Unicode scalar represented by this shaped unit. Multi-scalar grapheme
  /// clusters expose zero and therefore cannot be substituted by a scalar
  /// lookup program.
  UnicodeCodepoint,
  /// Unpadded source canvas dimensions: shaped advances and font-em line
  /// cells. These do not include the wrapping constraint or SDF guard.
  CanvasRectWidth,
  CanvasRectHeight,
  /// Actual render destination dimensions in output pixels. These are
  /// independent of the text canvas and source/reference geometry bridge.
  OutputWidth,
  OutputHeight,
  /// Resolved duration of the animator's owning clock, in microseconds,
  /// including the enter/exit phase allocation within the clip.
  AnimationDurationUs,
  /// Explicit scope-wide style reads. Unlike per-unit inputs, these remain
  /// invariant when bound to Page parameters in heterogeneous styled text.
  FirstUnitFontSize,
  FirstUnitColorRed,
  FirstUnitColorGreen,
  FirstUnitColorBlue,
  LastUnitColorRed,
  LastUnitColorGreen,
  LastUnitColorBlue,
  FirstUnitColorAlpha,
  FirstUnitRectWidth,
  FirstUnitRectHeight,
  /// Union of unit-size rectangles centered on initial positions, per row.
  /// Distinct from the layout/ink bounds exposed by row_rect_*.
  RowInitialBoundsX,
  RowInitialBoundsY,
  RowInitialBoundsWidth,
  RowInitialBoundsHeight,
  IsVerticalWriting,
  /// Signed source-design unit to physical output pixel scale, before local
  /// animation. Rotation does not change these two axis magnitudes.
  SourceToOutputScaleX,
  SourceToOutputScaleY,
  /// Source-design origin after presentation, in physical output pixels.
  SourceOriginOutputX,
  SourceOriginOutputY,
  /// Maximum normal-letter extent in the current evaluation coordinate space.
  /// Computed once per frame, excluding non-letter topology units.
  MaxNormalUnitRectWidth,
  MaxNormalUnitRectHeight,
};

/// Auditable scalar register machine used by project-owned text templates.
/// The instruction set is intentionally closed and has no branch target,
/// loop, call, source-string, resource-load, or shader-source instruction.
enum class TextEffectOpcode : std::uint8_t {
  Constant = 0,
  Input,
  Add,
  Subtract,
  Multiply,
  Divide,
  Minimum,
  Maximum,
  Clamp,
  Mix,
  Power,
  Sine,
  Cosine,
  Absolute,
  Floor,
  Ceil,
  Modulo,
  Negate,
  Float32,
  Step,
  SquareRoot,
  ArcSine,
  Exponential,
  LogOnePlus,
  LessThan,
  LessThanOrEqual,
  Equal,
  Select,
  SeededRandom,
  /// Deterministic sample-and-hold noise scoped to the stable text unit.
  /// The sole input is sampled time; immediates are frequency and an optional
  /// authored channel.
  SampleHoldNoise,
  /// One-argument arctangent, in radians.
  ArcTangent,
  /// Two-argument arctangent, in radians. Inputs are y then x.
  ArcTangent2,
  /// Deterministic sample-and-hold noise shared by every unit on the page.
  /// The sole input is sampled time; immediates are frequency and an optional
  /// authored channel.
  PageSampleHoldNoise,
  /// Returns the sum of the positive authored durations preceding the lane
  /// selected by the sole input register. The lane may equal the duration
  /// count, in which case the total duration is returned.
  DurationPrefix,
  /// Maps a global authored time and lane index to that lane's clamped local
  /// progress. Immediates are the positive authored lane durations.
  DurationLaneProgress,
  /// Evaluates a monotonic-x cubic Bezier timing curve. One input supplies x
  /// with immediate x1, y1, x2, y2; five inputs supply x and those controls
  /// dynamically (for authored per-character curves). Either form may append
  /// a source numeric mode immediate:
  /// 0 (or omitted) uses binary32 and a 1e-5 parameter-interval threshold;
  /// 1 uses binary64 and a 1e-4 threshold. Both return the final interval's
  /// midpoint, including at x=0/1, as their source runtimes do.
  CubicBezierTiming,
  /// Returns the zero-based call_index value from the program-seeded
  /// Park-Miller sequence used by Lua 5.1's Darwin rand implementation. The
  /// result is normalized by RAND_MAX into [0, 1).
  SequentialRandom,
  /// Replays the program-seeded forward Fisher-Yates shuffle and returns the
  /// integer rank stored at zero-based unit_index. Inputs are unit_index then
  /// unit_count; an optional immediate is the zero-based sequence call at
  /// which shuffling starts (default zero). The shuffle consumes exactly
  /// unit_count - 1 sequential random values.
  SeededShuffleRank,
  /// Replays Lua's descending Fisher-Yates loop (`n = count .. 1`) and returns
  /// the rank at zero-based list_index. Inputs are list_index, unit_count, and
  /// the dynamic zero-based random-sequence start call. The n=1 draw is
  /// consumed so consecutive row-local shuffles preserve Lua's sequence.
  SeededReverseShuffleRank,
  /// Looks up the integer-valued entry selected by the sole input register.
  /// Immediates are the closed authored table and the index must be integral
  /// and in range.
  ImmediateLookup,
  /// A scalar piecewise cubic-Bezier curve in an explicitly authored time
  /// domain. Immediates: startTime, startValue, then up to 64 segments of
  /// endTime, endValue, x1, y1, x2, y2. Holds the endpoint values outside the
  /// table. Uses the same timing solver as CubicBezierTiming.
  CubicBezierCurve,
  /// Elliptic row arrangement with capacity/spacing recursion and bounded
  /// chord-angle solving. Inputs: source font size, source word gap, source
  /// line gap, vertical flag (0/1). Immediate selects x/y/rotation/scale (0–3).
  /// Inputs must be invariant across the stage; the layout is computed once.
  EllipticRowLayout,
  /// Bilinear value noise on a two-dimensional integer lattice. Inputs are
  /// x/y; immediates are the two sine-hash coefficients and amplitude.
  /// Independent of program seed, stable unit identity and register order.
  ValueNoise2D,
};

struct TextEffectInstruction final {
  TextEffectOpcode opcode{TextEffectOpcode::Constant};
  std::uint32_t outputRegister{0U};
  std::vector<std::uint32_t> inputRegisters;
  std::vector<double> immediates;
  std::optional<TextEffectInput> input;
};

/// Authored stage order is the evaluation and output-composition order; a later
/// stage may replace an earlier value for the same output. PerUnit is
/// embarrassingly parallel. Neighbor stages expose immutable neighboring
/// geometry plus the current-position snapshot committed by preceding stages;
/// they never observe writes from another unit in the stage being evaluated.
/// ColumnGroup additionally exposes the number of drawable units sharing
/// indexInRow.
enum class TextEffectStageKind : std::uint8_t {
  PerUnit = 0,
  PreviousNeighbor,
  BidirectionalNeighbor,
  ColumnGroup,
};

enum class TextEffectOutput : std::uint8_t {
  Opacity = 0,
  InstanceColorRed,
  InstanceColorGreen,
  InstanceColorBlue,
  InstanceColorAlpha,
  AbsoluteFontSize,
  BlurRadius,
  OffsetX,
  OffsetY,
  OffsetZ,
  RotationX,
  RotationY,
  RotationZ,
  ShearX,
  ShearY,
  ScaleX,
  ScaleY,
  ScaleZ,
  AnchorX,
  AnchorY,
  AnchorZ,
  AnchorRangeBegin,
  AnchorRangeEnd,
  /// Direct authored column-major transform. Binding any matrix component for
  /// a transform index selects direct-matrix mode for that transform; mixing
  /// it with decomposed transform outputs is invalid.
  MatrixC0R0,
  MatrixC0R1,
  MatrixC0R2,
  MatrixC0R3,
  MatrixC1R0,
  MatrixC1R1,
  MatrixC1R2,
  MatrixC1R3,
  MatrixC2R0,
  MatrixC2R1,
  MatrixC2R2,
  MatrixC2R3,
  MatrixC3R0,
  MatrixC3R1,
  MatrixC3R2,
  MatrixC3R3,
  /// Publishes one position component for later stages. A stage always reads
  /// the immutable position snapshot committed by preceding stages, so a
  /// producer stage and a neighbor consumer stage form a deterministic
  /// two-pass evaluation without unit-order feedback. A producer stage must
  /// bind current_position_valid together with at least one position axis;
  /// false excludes that unit from current-position neighbor lookup.
  CurrentPositionX,
  CurrentPositionY,
  CurrentPositionValid,
  /// Replaces this unit with one BMP non-surrogate scalar before final shaping.
  /// Zero preserves the source grapheme.
  ReplacementCodepoint,
  /// Adds tracking in font-em units to the authored letter spacing before
  /// shaping. Later stages replace this program's offset, not the base style.
  LetterSpacingOffsetEm,
  /// Adds the same tracking to every unit, in em units of the largest font
  /// in the program's input frame. The maximum includes spaces/newlines and
  /// is captured before this program changes font sizes or spacing.
  LetterSpacingOffsetMaxEm,
};

struct TextEffectOutputBinding final {
  TextEffectOutput output{TextEffectOutput::Opacity};
  std::uint32_t registerIndex{0U};
  /// Transform fields with the same index form one ordered transform. Ignored
  /// for opacity, color, absolute font-size, blur and current-position outputs.
  std::uint32_t transformIndex{0U};
};

/// Closed execution-graph parameter destinations. Material bindings address
/// a semantic renderer slot, never a source shader/uniform name.
enum class TextEffectExecutionParameterKind : std::uint8_t {
  PostEffectProgress = 0,
  PostEffectAmplitude,
  PostEffectBlurRadius,
  MaterialScalar,
  MaterialVector2,
  MaterialVector3,
  MaterialVector4,
  SceneTranslation,
  SceneScale,
  SceneOpacity,
  NodeScalar,
  NodeVector2,
  NodeVector3,
  NodeVector4,
};

enum class TextEffectExecutionParameterDomain : std::uint8_t {
  Page = 0,
  PerUnit,
};

/// SourcePixels are projected once into ReferencePixels before the sampled
/// program plan is merged into the canonical application FramePlan.
/// WorldUnits remain in the authored scene coordinate system until the
/// consuming execution node applies its camera exactly once.
enum class TextEffectExecutionParameterSpace : std::uint8_t {
  Unitless = 0,
  SourcePixels,
  ReferencePixels,
  WorldUnits,
};

struct TextEffectExecutionParameterBinding final {
  std::string nodeId;
  TextEffectExecutionParameterKind parameter{
      TextEffectExecutionParameterKind::PostEffectProgress};
  TextEffectExecutionParameterDomain domain{
      TextEffectExecutionParameterDomain::Page};
  TextEffectExecutionParameterSpace valueSpace{
      TextEffectExecutionParameterSpace::Unitless};
  /// Material and generic execution-node parameters use a slot. The closed
  /// native implementation for the target capability owns each slot's
  /// meaning.
  std::uint32_t slot{0U};
  /// Scalar/vector component order is canonical and must match parameter.
  std::vector<std::uint32_t> registerIndices;
};

struct TextEffectProgramStageIR final {
  std::string stageId;
  TextEffectStageKind kind{TextEffectStageKind::PerUnit};
  std::uint32_t registerCount{0U};
  std::vector<TextEffectInstruction> instructions;
  std::vector<TextEffectOutputBinding> outputs;
  std::vector<TextEffectExecutionParameterBinding> executionParameterBindings;
};

struct TextEffectProgramIR final {
  std::string programId;
  std::uint64_t randomSeed{0U};
  std::vector<TextEffectProgramStageIR> stages;
};

using TextEffectProgramLibrary = std::vector<TextEffectProgramIR>;

bool ValidateTextEffectProgramIR(const TextEffectProgramIR &program,
                                 std::string *error = nullptr) noexcept;

std::optional<TextEffectInput>
ParseTextEffectInput(std::string_view name) noexcept;
std::optional<TextEffectOpcode>
ParseTextEffectOpcode(std::string_view name) noexcept;
std::optional<TextEffectStageKind>
ParseTextEffectStageKind(std::string_view name) noexcept;
std::optional<TextEffectOutput>
ParseTextEffectOutput(std::string_view name) noexcept;
std::optional<TextEffectExecutionParameterKind>
ParseTextEffectExecutionParameterKind(std::string_view name) noexcept;
std::optional<TextEffectExecutionParameterDomain>
ParseTextEffectExecutionParameterDomain(std::string_view name) noexcept;
std::optional<TextEffectExecutionParameterSpace>
ParseTextEffectExecutionParameterSpace(std::string_view name) noexcept;

std::string_view TextEffectInputName(TextEffectInput input) noexcept;
std::string_view TextEffectOpcodeName(TextEffectOpcode opcode) noexcept;
std::string_view TextEffectStageKindName(TextEffectStageKind kind) noexcept;
std::string_view TextEffectOutputName(TextEffectOutput output) noexcept;
std::string_view TextEffectExecutionParameterKindName(
    TextEffectExecutionParameterKind parameter) noexcept;
std::string_view TextEffectExecutionParameterDomainName(
    TextEffectExecutionParameterDomain domain) noexcept;
std::string_view TextEffectExecutionParameterSpaceName(
    TextEffectExecutionParameterSpace space) noexcept;

std::size_t TextEffectExecutionParameterComponentCount(
    TextEffectExecutionParameterKind parameter) noexcept;
bool IsPostEffectExecutionParameter(
    TextEffectExecutionParameterKind parameter) noexcept;
bool IsMaterialExecutionParameter(
    TextEffectExecutionParameterKind parameter) noexcept;
bool IsSceneExecutionParameter(
    TextEffectExecutionParameterKind parameter) noexcept;
bool IsNodeExecutionParameter(
    TextEffectExecutionParameterKind parameter) noexcept;

const TextEffectProgramIR *
FindTextEffectProgram(const TextEffectProgramLibrary &library,
                      std::string_view programId) noexcept;

} // namespace videocut::text
