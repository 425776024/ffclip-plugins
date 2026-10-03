#include "videocut/text/TextEffectProgramIR.h"

#include "videocut/text/TextAnimation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace videocut::text {
namespace {

constexpr std::size_t kMaximumStages = 16U;
constexpr std::uint32_t kMaximumRegistersPerStage = 512U;
constexpr std::size_t kMaximumInstructionsPerStage = 2'048U;
constexpr std::size_t kMaximumOutputsPerStage = 256U;
constexpr std::size_t kMaximumExecutionParameterBindingsPerStage = 256U;
constexpr std::uint32_t kMaximumTransformsPerUnit = 32U;
constexpr std::uint32_t kMaximumMaterialParameterSlots = 32U;
constexpr std::size_t kMaximumDurationLanes = 65'536U;
constexpr std::size_t kMaximumImmediateLookupEntries = 65'536U;
constexpr double kMaximumSequentialRandomCallIndex = 262'143.0;

void SetValidationError(std::string *error,
                        const std::string_view message) noexcept {
  if (!error)
    return;
  try {
    error->assign(message.data(), message.size());
  } catch (...) {
    error->clear();
  }
}

template <typename Enum, std::size_t Size>
std::optional<Enum>
ParseEnumName(const std::array<std::pair<Enum, std::string_view>, Size> &names,
              const std::string_view name) noexcept {
  const auto found =
      std::find_if(names.begin(), names.end(),
                   [&](const auto &entry) { return entry.second == name; });
  return found == names.end() ? std::nullopt
                              : std::optional<Enum>{found->first};
}

template <typename Enum, std::size_t Size>
std::string_view
EnumName(const std::array<std::pair<Enum, std::string_view>, Size> &names,
         const Enum value) noexcept {
  const auto found =
      std::find_if(names.begin(), names.end(),
                   [&](const auto &entry) { return entry.first == value; });
  return found == names.end() ? std::string_view{} : found->second;
}

constexpr std::array<std::pair<TextEffectInput, std::string_view>, 84U>
    kInputNames{{
        {TextEffectInput::Progress, "progress"},
        {TextEffectInput::TimeUs, "time_us"},
        {TextEffectInput::UnitIndex, "unit_index"},
        {TextEffectInput::RenderIndex, "render_index"},
        {TextEffectInput::UnitType, "unit_type"},
        {TextEffectInput::RowIndex, "row_index"},
        {TextEffectInput::IndexInRow, "index_in_row"},
        {TextEffectInput::UnitCount, "unit_count"},
        {TextEffectInput::RowCount, "row_count"},
        {TextEffectInput::FontSize, "font_size"},
        {TextEffectInput::RectX, "rect_x"},
        {TextEffectInput::RectY, "rect_y"},
        {TextEffectInput::RectWidth, "rect_width"},
        {TextEffectInput::RectHeight, "rect_height"},
        {TextEffectInput::InitialPositionX, "initial_position_x"},
        {TextEffectInput::InitialPositionY, "initial_position_y"},
        {TextEffectInput::InstanceColorRed, "instance_color_red"},
        {TextEffectInput::InstanceColorGreen, "instance_color_green"},
        {TextEffectInput::InstanceColorBlue, "instance_color_blue"},
        {TextEffectInput::InstanceColorAlpha, "instance_color_alpha"},
        {TextEffectInput::RowRectX, "row_rect_x"},
        {TextEffectInput::RowRectY, "row_rect_y"},
        {TextEffectInput::RowRectWidth, "row_rect_width"},
        {TextEffectInput::RowRectHeight, "row_rect_height"},
        {TextEffectInput::RowInitialPositionSpanX,
         "row_initial_position_span_x"},
        {TextEffectInput::TextRectX, "text_rect_x"},
        {TextEffectInput::TextRectY, "text_rect_y"},
        {TextEffectInput::TextRectWidth, "text_rect_width"},
        {TextEffectInput::TextRectHeight, "text_rect_height"},
        {TextEffectInput::HasPreviousUnit, "has_previous_unit"},
        {TextEffectInput::PreviousInitialPositionX,
         "previous_initial_position_x"},
        {TextEffectInput::PreviousInitialPositionY,
         "previous_initial_position_y"},
        {TextEffectInput::PreviousRectX, "previous_rect_x"},
        {TextEffectInput::PreviousRectY, "previous_rect_y"},
        {TextEffectInput::PreviousRectWidth, "previous_rect_width"},
        {TextEffectInput::PreviousRectHeight, "previous_rect_height"},
        {TextEffectInput::HasNextUnit, "has_next_unit"},
        {TextEffectInput::NextInitialPositionX, "next_initial_position_x"},
        {TextEffectInput::NextInitialPositionY, "next_initial_position_y"},
        {TextEffectInput::NextRectX, "next_rect_x"},
        {TextEffectInput::NextRectY, "next_rect_y"},
        {TextEffectInput::NextRectWidth, "next_rect_width"},
        {TextEffectInput::NextRectHeight, "next_rect_height"},
        {TextEffectInput::RowUnitCount, "row_unit_count"},
        {TextEffectInput::ColumnUnitCount, "column_unit_count"},
        {TextEffectInput::CurrentPositionX, "current_position_x"},
        {TextEffectInput::CurrentPositionY, "current_position_y"},
        {TextEffectInput::PreviousCurrentPositionX,
         "previous_current_position_x"},
        {TextEffectInput::PreviousCurrentPositionY,
         "previous_current_position_y"},
        {TextEffectInput::NextCurrentPositionX, "next_current_position_x"},
        {TextEffectInput::NextCurrentPositionY, "next_current_position_y"},
        {TextEffectInput::HasCurrentPosition, "has_current_position"},
        {TextEffectInput::HasPreviousCurrentPosition,
         "has_previous_current_position"},
        {TextEffectInput::HasNextCurrentPosition, "has_next_current_position"},
        {TextEffectInput::IsNormalUnit, "is_normal_unit"},
        {TextEffectInput::NormalUnitIndex, "normal_unit_index"},
        {TextEffectInput::NormalUnitCount, "normal_unit_count"},
        {TextEffectInput::UnicodeCodepoint, "unicode_codepoint"},
        {TextEffectInput::CanvasRectWidth, "canvas_rect_width"},
        {TextEffectInput::CanvasRectHeight, "canvas_rect_height"},
        {TextEffectInput::OutputWidth, "output_width"},
        {TextEffectInput::OutputHeight, "output_height"},
        {TextEffectInput::AnimationDurationUs, "animation_duration_us"},
        {TextEffectInput::FirstUnitFontSize, "first_unit_font_size"},
        {TextEffectInput::FirstUnitColorRed, "first_unit_color_red"},
        {TextEffectInput::FirstUnitColorGreen, "first_unit_color_green"},
        {TextEffectInput::FirstUnitColorBlue, "first_unit_color_blue"},
        {TextEffectInput::LastUnitColorRed, "last_unit_color_red"},
        {TextEffectInput::LastUnitColorGreen, "last_unit_color_green"},
        {TextEffectInput::LastUnitColorBlue, "last_unit_color_blue"},
        {TextEffectInput::FirstUnitColorAlpha, "first_unit_color_alpha"},
        {TextEffectInput::FirstUnitRectWidth, "first_unit_rect_width"},
        {TextEffectInput::FirstUnitRectHeight, "first_unit_rect_height"},
        {TextEffectInput::RowInitialBoundsX, "row_initial_bounds_x"},
        {TextEffectInput::RowInitialBoundsY, "row_initial_bounds_y"},
        {TextEffectInput::RowInitialBoundsWidth, "row_initial_bounds_width"},
        {TextEffectInput::RowInitialBoundsHeight, "row_initial_bounds_height"},
        {TextEffectInput::IsVerticalWriting, "is_vertical_writing"},
        {TextEffectInput::SourceToOutputScaleX, "source_to_output_scale_x"},
        {TextEffectInput::SourceToOutputScaleY, "source_to_output_scale_y"},
        {TextEffectInput::SourceOriginOutputX, "source_origin_output_x"},
        {TextEffectInput::SourceOriginOutputY, "source_origin_output_y"},
        {TextEffectInput::MaxNormalUnitRectWidth, "max_normal_unit_rect_width"},
        {TextEffectInput::MaxNormalUnitRectHeight, "max_normal_unit_rect_height"},
    }};

constexpr std::array<std::pair<TextEffectOpcode, std::string_view>, 43U>
    kOpcodeNames{{
        {TextEffectOpcode::Constant, "constant"},
        {TextEffectOpcode::Input, "input"},
        {TextEffectOpcode::Add, "add"},
        {TextEffectOpcode::Subtract, "subtract"},
        {TextEffectOpcode::Multiply, "multiply"},
        {TextEffectOpcode::Divide, "divide"},
        {TextEffectOpcode::Minimum, "minimum"},
        {TextEffectOpcode::Maximum, "maximum"},
        {TextEffectOpcode::Clamp, "clamp"},
        {TextEffectOpcode::Mix, "mix"},
        {TextEffectOpcode::Power, "power"},
        {TextEffectOpcode::Sine, "sine"},
        {TextEffectOpcode::Cosine, "cosine"},
        {TextEffectOpcode::Absolute, "absolute"},
        {TextEffectOpcode::Floor, "floor"},
        {TextEffectOpcode::Ceil, "ceil"},
        {TextEffectOpcode::Modulo, "modulo"},
        {TextEffectOpcode::Negate, "negate"},
        {TextEffectOpcode::Float32, "float32"},
        {TextEffectOpcode::Step, "step"},
        {TextEffectOpcode::SquareRoot, "square_root"},
        {TextEffectOpcode::ArcSine, "arc_sine"},
        {TextEffectOpcode::Exponential, "exponential"},
        {TextEffectOpcode::LogOnePlus, "log_one_plus"},
        {TextEffectOpcode::LessThan, "less_than"},
        {TextEffectOpcode::LessThanOrEqual, "less_than_or_equal"},
        {TextEffectOpcode::Equal, "equal"},
        {TextEffectOpcode::Select, "select"},
        {TextEffectOpcode::SeededRandom, "seeded_random"},
        {TextEffectOpcode::SampleHoldNoise, "sample_hold_noise"},
        {TextEffectOpcode::ArcTangent, "arc_tangent"},
        {TextEffectOpcode::ArcTangent2, "arc_tangent2"},
        {TextEffectOpcode::PageSampleHoldNoise, "page_sample_hold_noise"},
        {TextEffectOpcode::DurationPrefix, "duration_prefix"},
        {TextEffectOpcode::DurationLaneProgress, "duration_lane_progress"},
        {TextEffectOpcode::CubicBezierTiming, "cubic_bezier_timing"},
        {TextEffectOpcode::SequentialRandom, "sequential_random"},
        {TextEffectOpcode::SeededShuffleRank, "seeded_shuffle_rank"},
        {TextEffectOpcode::SeededReverseShuffleRank,
         "seeded_reverse_shuffle_rank"},
        {TextEffectOpcode::ImmediateLookup, "immediate_lookup"},
        {TextEffectOpcode::CubicBezierCurve, "cubic_bezier_curve"},
        {TextEffectOpcode::EllipticRowLayout, "elliptic_row_layout"},
        {TextEffectOpcode::ValueNoise2D, "value_noise_2d"},
    }};

constexpr std::array<std::pair<TextEffectStageKind, std::string_view>, 4U>
    kStageKindNames{{
        {TextEffectStageKind::PerUnit, "per_unit"},
        {TextEffectStageKind::PreviousNeighbor, "previous_neighbor"},
        {TextEffectStageKind::BidirectionalNeighbor, "bidirectional_neighbor"},
        {TextEffectStageKind::ColumnGroup, "column_group"},
    }};

constexpr std::array<
    std::pair<TextEffectExecutionParameterKind, std::string_view>, 14U>
    kExecutionParameterNames{{
        {TextEffectExecutionParameterKind::PostEffectProgress,
         "post_effect_progress"},
        {TextEffectExecutionParameterKind::PostEffectAmplitude,
         "post_effect_amplitude"},
        {TextEffectExecutionParameterKind::PostEffectBlurRadius,
         "post_effect_blur_radius"},
        {TextEffectExecutionParameterKind::MaterialScalar,
         "material_scalar"},
        {TextEffectExecutionParameterKind::MaterialVector2,
         "material_vector2"},
        {TextEffectExecutionParameterKind::MaterialVector3,
         "material_vector3"},
        {TextEffectExecutionParameterKind::MaterialVector4,
         "material_vector4"},
        {TextEffectExecutionParameterKind::SceneTranslation,
         "scene_translation"},
        {TextEffectExecutionParameterKind::SceneScale, "scene_scale"},
        {TextEffectExecutionParameterKind::SceneOpacity, "scene_opacity"},
        {TextEffectExecutionParameterKind::NodeScalar, "node_scalar"},
        {TextEffectExecutionParameterKind::NodeVector2, "node_vector2"},
        {TextEffectExecutionParameterKind::NodeVector3, "node_vector3"},
        {TextEffectExecutionParameterKind::NodeVector4, "node_vector4"},
    }};

constexpr std::array<
    std::pair<TextEffectExecutionParameterDomain, std::string_view>, 2U>
    kExecutionParameterDomainNames{{
        {TextEffectExecutionParameterDomain::Page, "page"},
        {TextEffectExecutionParameterDomain::PerUnit, "per_unit"},
    }};

constexpr std::array<
    std::pair<TextEffectExecutionParameterSpace, std::string_view>, 4U>
    kExecutionParameterSpaceNames{{
        {TextEffectExecutionParameterSpace::Unitless, "unitless"},
        {TextEffectExecutionParameterSpace::SourcePixels, "source_pixels"},
        {TextEffectExecutionParameterSpace::ReferencePixels,
         "reference_pixels"},
        {TextEffectExecutionParameterSpace::WorldUnits, "world_units"},
    }};

constexpr std::array<std::pair<TextEffectOutput, std::string_view>, 45U>
    kOutputNames{{
        {TextEffectOutput::Opacity, "opacity"},
        {TextEffectOutput::InstanceColorRed, "instance_color_red"},
        {TextEffectOutput::InstanceColorGreen, "instance_color_green"},
        {TextEffectOutput::InstanceColorBlue, "instance_color_blue"},
        {TextEffectOutput::InstanceColorAlpha, "instance_color_alpha"},
        {TextEffectOutput::AbsoluteFontSize, "absolute_font_size"},
        {TextEffectOutput::BlurRadius, "blur_radius"},
        {TextEffectOutput::OffsetX, "offset_x"},
        {TextEffectOutput::OffsetY, "offset_y"},
        {TextEffectOutput::OffsetZ, "offset_z"},
        {TextEffectOutput::RotationX, "rotation_x"},
        {TextEffectOutput::RotationY, "rotation_y"},
        {TextEffectOutput::RotationZ, "rotation_z"},
        {TextEffectOutput::ShearX, "shear_x"},
        {TextEffectOutput::ShearY, "shear_y"},
        {TextEffectOutput::ScaleX, "scale_x"},
        {TextEffectOutput::ScaleY, "scale_y"},
        {TextEffectOutput::ScaleZ, "scale_z"},
        {TextEffectOutput::AnchorX, "anchor_x"},
        {TextEffectOutput::AnchorY, "anchor_y"},
        {TextEffectOutput::AnchorZ, "anchor_z"},
        {TextEffectOutput::AnchorRangeBegin, "anchor_range_begin"},
        {TextEffectOutput::AnchorRangeEnd, "anchor_range_end"},
        {TextEffectOutput::MatrixC0R0, "matrix_c0r0"},
        {TextEffectOutput::MatrixC0R1, "matrix_c0r1"},
        {TextEffectOutput::MatrixC0R2, "matrix_c0r2"},
        {TextEffectOutput::MatrixC0R3, "matrix_c0r3"},
        {TextEffectOutput::MatrixC1R0, "matrix_c1r0"},
        {TextEffectOutput::MatrixC1R1, "matrix_c1r1"},
        {TextEffectOutput::MatrixC1R2, "matrix_c1r2"},
        {TextEffectOutput::MatrixC1R3, "matrix_c1r3"},
        {TextEffectOutput::MatrixC2R0, "matrix_c2r0"},
        {TextEffectOutput::MatrixC2R1, "matrix_c2r1"},
        {TextEffectOutput::MatrixC2R2, "matrix_c2r2"},
        {TextEffectOutput::MatrixC2R3, "matrix_c2r3"},
        {TextEffectOutput::MatrixC3R0, "matrix_c3r0"},
        {TextEffectOutput::MatrixC3R1, "matrix_c3r1"},
        {TextEffectOutput::MatrixC3R2, "matrix_c3r2"},
        {TextEffectOutput::MatrixC3R3, "matrix_c3r3"},
        {TextEffectOutput::CurrentPositionX, "current_position_x"},
        {TextEffectOutput::CurrentPositionY, "current_position_y"},
        {TextEffectOutput::CurrentPositionValid, "current_position_valid"},
        {TextEffectOutput::ReplacementCodepoint, "replacement_codepoint"},
        {TextEffectOutput::LetterSpacingOffsetEm, "letter_spacing_offset_em"},
        {TextEffectOutput::LetterSpacingOffsetMaxEm, "letter_spacing_offset_max_em"},
    }};

bool ValidateArity(const TextEffectInstruction &instruction) noexcept {
  const auto inputs = instruction.inputRegisters.size();
  const auto immediates = instruction.immediates.size();
  switch (instruction.opcode) {
  case TextEffectOpcode::Constant:
    return inputs == 0U && immediates == 1U && !instruction.input;
  case TextEffectOpcode::Input:
    return inputs == 0U && immediates == 0U && instruction.input.has_value();
  case TextEffectOpcode::Add:
  case TextEffectOpcode::Subtract:
  case TextEffectOpcode::Multiply:
  case TextEffectOpcode::Divide:
  case TextEffectOpcode::Minimum:
  case TextEffectOpcode::Maximum:
  case TextEffectOpcode::Power:
  case TextEffectOpcode::Modulo:
  case TextEffectOpcode::Step:
  case TextEffectOpcode::ArcTangent2:
  case TextEffectOpcode::LessThan:
  case TextEffectOpcode::LessThanOrEqual:
  case TextEffectOpcode::Equal:
    return inputs == 2U && immediates == 0U && !instruction.input;
  case TextEffectOpcode::SeededShuffleRank:
    return inputs == 2U && immediates <= 1U && !instruction.input;
  case TextEffectOpcode::SeededReverseShuffleRank:
    return inputs == 3U && immediates == 0U && !instruction.input;
  case TextEffectOpcode::EllipticRowLayout:
    return inputs == 4U && immediates == 1U && !instruction.input;
  case TextEffectOpcode::Clamp:
  case TextEffectOpcode::Mix:
  case TextEffectOpcode::Select:
    return inputs == 3U && immediates == 0U && !instruction.input;
  case TextEffectOpcode::Sine:
  case TextEffectOpcode::Cosine:
  case TextEffectOpcode::Absolute:
  case TextEffectOpcode::Floor:
  case TextEffectOpcode::Ceil:
  case TextEffectOpcode::Negate:
  case TextEffectOpcode::Float32:
  case TextEffectOpcode::SquareRoot:
  case TextEffectOpcode::ArcSine:
  case TextEffectOpcode::ArcTangent:
  case TextEffectOpcode::Exponential:
  case TextEffectOpcode::LogOnePlus:
    return inputs == 1U && immediates == 0U && !instruction.input;
  case TextEffectOpcode::SeededRandom:
    return inputs <= 1U && immediates <= 1U && !instruction.input;
  case TextEffectOpcode::SampleHoldNoise:
  case TextEffectOpcode::PageSampleHoldNoise:
    return inputs == 1U && immediates >= 1U && immediates <= 2U &&
           !instruction.input;
  case TextEffectOpcode::ValueNoise2D:
    return inputs == 2U && immediates == 3U && !instruction.input;
  case TextEffectOpcode::DurationPrefix:
    return inputs == 1U && immediates >= 1U &&
           immediates <= kMaximumDurationLanes && !instruction.input;
  case TextEffectOpcode::DurationLaneProgress:
    return inputs == 2U && immediates >= 1U &&
           immediates <= kMaximumDurationLanes && !instruction.input;
  case TextEffectOpcode::CubicBezierTiming:
    return ((inputs == 1U && (immediates == 4U || immediates == 5U)) ||
            (inputs == 5U && immediates <= 1U)) &&
           !instruction.input;
  case TextEffectOpcode::CubicBezierCurve:
    return inputs == 1U && immediates >= 8U && immediates <= 386U &&
           (immediates - 2U) % 6U == 0U && !instruction.input;
  case TextEffectOpcode::SequentialRandom:
    return inputs == 1U && immediates == 0U && !instruction.input;
  case TextEffectOpcode::ImmediateLookup:
    return inputs == 1U && !instruction.immediates.empty() &&
           immediates <= kMaximumImmediateLookupEntries && !instruction.input;
  }
  return false;
}

bool IsTransformOutput(const TextEffectOutput output) noexcept {
  return output >= TextEffectOutput::OffsetX &&
         output <= TextEffectOutput::MatrixC3R3;
}

bool IsDirectMatrixOutput(const TextEffectOutput output) noexcept {
  return output >= TextEffectOutput::MatrixC0R0 &&
         output <= TextEffectOutput::MatrixC3R3;
}

bool IsPreviousNeighborInput(const TextEffectInput input) noexcept {
  return (input >= TextEffectInput::HasPreviousUnit &&
          input <= TextEffectInput::PreviousRectHeight) ||
         input == TextEffectInput::PreviousCurrentPositionX ||
         input == TextEffectInput::PreviousCurrentPositionY ||
         input == TextEffectInput::HasPreviousCurrentPosition;
}

bool IsNextNeighborInput(const TextEffectInput input) noexcept {
  return (input >= TextEffectInput::HasNextUnit &&
          input <= TextEffectInput::NextRectHeight) ||
         input == TextEffectInput::NextCurrentPositionX ||
         input == TextEffectInput::NextCurrentPositionY ||
         input == TextEffectInput::HasNextCurrentPosition;
}

bool ReadsCurrentPositionX(const TextEffectInput input) noexcept {
  return input == TextEffectInput::CurrentPositionX ||
         input == TextEffectInput::PreviousCurrentPositionX ||
         input == TextEffectInput::NextCurrentPositionX;
}

bool ReadsCurrentPositionY(const TextEffectInput input) noexcept {
  return input == TextEffectInput::CurrentPositionY ||
         input == TextEffectInput::PreviousCurrentPositionY ||
         input == TextEffectInput::NextCurrentPositionY;
}

bool ReadsCurrentPositionValidity(const TextEffectInput input) noexcept {
  return input == TextEffectInput::HasCurrentPosition ||
         input == TextEffectInput::HasPreviousCurrentPosition ||
         input == TextEffectInput::HasNextCurrentPosition ||
         ReadsCurrentPositionX(input) || ReadsCurrentPositionY(input);
}

bool StageAcceptsInput(const TextEffectStageKind kind,
                       const TextEffectInput input) noexcept {
  if (IsPreviousNeighborInput(input)) {
    return kind == TextEffectStageKind::PreviousNeighbor ||
           kind == TextEffectStageKind::BidirectionalNeighbor;
  }
  if (IsNextNeighborInput(input))
    return kind == TextEffectStageKind::BidirectionalNeighbor;
  if (input == TextEffectInput::ColumnUnitCount)
    return kind == TextEffectStageKind::ColumnGroup;
  return true;
}

} // namespace

std::optional<TextEffectInput>
ParseTextEffectInput(const std::string_view name) noexcept {
  return ParseEnumName(kInputNames, name);
}

std::optional<TextEffectOpcode>
ParseTextEffectOpcode(const std::string_view name) noexcept {
  return ParseEnumName(kOpcodeNames, name);
}

std::optional<TextEffectStageKind>
ParseTextEffectStageKind(const std::string_view name) noexcept {
  return ParseEnumName(kStageKindNames, name);
}

std::optional<TextEffectOutput>
ParseTextEffectOutput(const std::string_view name) noexcept {
  return ParseEnumName(kOutputNames, name);
}

std::optional<TextEffectExecutionParameterKind>
ParseTextEffectExecutionParameterKind(const std::string_view name) noexcept {
  return ParseEnumName(kExecutionParameterNames, name);
}

std::optional<TextEffectExecutionParameterDomain>
ParseTextEffectExecutionParameterDomain(const std::string_view name) noexcept {
  return ParseEnumName(kExecutionParameterDomainNames, name);
}

std::optional<TextEffectExecutionParameterSpace>
ParseTextEffectExecutionParameterSpace(const std::string_view name) noexcept {
  return ParseEnumName(kExecutionParameterSpaceNames, name);
}

std::string_view TextEffectInputName(const TextEffectInput input) noexcept {
  return EnumName(kInputNames, input);
}

std::string_view TextEffectOpcodeName(const TextEffectOpcode opcode) noexcept {
  return EnumName(kOpcodeNames, opcode);
}

std::string_view
TextEffectStageKindName(const TextEffectStageKind kind) noexcept {
  return EnumName(kStageKindNames, kind);
}

std::string_view TextEffectOutputName(const TextEffectOutput output) noexcept {
  return EnumName(kOutputNames, output);
}

std::string_view TextEffectExecutionParameterKindName(
    const TextEffectExecutionParameterKind parameter) noexcept {
  return EnumName(kExecutionParameterNames, parameter);
}

std::string_view TextEffectExecutionParameterDomainName(
    const TextEffectExecutionParameterDomain domain) noexcept {
  return EnumName(kExecutionParameterDomainNames, domain);
}

std::string_view TextEffectExecutionParameterSpaceName(
    const TextEffectExecutionParameterSpace space) noexcept {
  return EnumName(kExecutionParameterSpaceNames, space);
}

std::size_t TextEffectExecutionParameterComponentCount(
    const TextEffectExecutionParameterKind parameter) noexcept {
  switch (parameter) {
  case TextEffectExecutionParameterKind::PostEffectProgress:
  case TextEffectExecutionParameterKind::PostEffectAmplitude:
  case TextEffectExecutionParameterKind::PostEffectBlurRadius:
  case TextEffectExecutionParameterKind::MaterialScalar:
  case TextEffectExecutionParameterKind::NodeScalar:
    return 1U;
  case TextEffectExecutionParameterKind::MaterialVector2:
  case TextEffectExecutionParameterKind::SceneTranslation:
  case TextEffectExecutionParameterKind::SceneScale:
  case TextEffectExecutionParameterKind::NodeVector2:
    return 2U;
  case TextEffectExecutionParameterKind::MaterialVector3:
  case TextEffectExecutionParameterKind::NodeVector3:
    return 3U;
  case TextEffectExecutionParameterKind::MaterialVector4:
  case TextEffectExecutionParameterKind::NodeVector4:
    return 4U;
  case TextEffectExecutionParameterKind::SceneOpacity:
    return 1U;
  }
  return 0U;
}

bool IsPostEffectExecutionParameter(
    const TextEffectExecutionParameterKind parameter) noexcept {
  return parameter >= TextEffectExecutionParameterKind::PostEffectProgress &&
         parameter <=
             TextEffectExecutionParameterKind::PostEffectBlurRadius;
}

bool IsMaterialExecutionParameter(
    const TextEffectExecutionParameterKind parameter) noexcept {
  return parameter >= TextEffectExecutionParameterKind::MaterialScalar &&
         parameter <= TextEffectExecutionParameterKind::MaterialVector4;
}

bool IsSceneExecutionParameter(
    const TextEffectExecutionParameterKind parameter) noexcept {
  return parameter >= TextEffectExecutionParameterKind::SceneTranslation &&
         parameter <= TextEffectExecutionParameterKind::SceneOpacity;
}

bool IsNodeExecutionParameter(
    const TextEffectExecutionParameterKind parameter) noexcept {
  return parameter >= TextEffectExecutionParameterKind::NodeScalar &&
         parameter <= TextEffectExecutionParameterKind::NodeVector4;
}

bool ValidateTextEffectProgramIR(const TextEffectProgramIR &program,
                                 std::string *error) noexcept {
  const auto fail = [&](const std::string_view message) noexcept {
    SetValidationError(error, message);
    return false;
  };
  if (program.programId.empty() || program.programId.size() > 512U ||
      !IsValidUtf8(program.programId) || program.stages.empty() ||
      program.stages.size() > kMaximumStages) {
    return fail("text effect program identity or stage budget is invalid");
  }

  std::unordered_set<std::string> stageIds;
  try {
    stageIds.reserve(program.stages.size());
    bool hasCurrentPositionX = false;
    bool hasCurrentPositionY = false;
    bool hasCurrentPositionValidity = false;
    std::array<bool, kMaximumTransformsPerUnit> hasDecomposedTransform{};
    std::array<bool, kMaximumTransformsPerUnit> hasDirectMatrix{};
    std::unordered_set<std::string> executionParameterKeys;
    for (const auto &stage : program.stages) {
      if (stage.stageId.empty() || stage.stageId.size() > 512U ||
          !IsValidUtf8(stage.stageId) ||
          !stageIds.insert(stage.stageId).second ||
          TextEffectStageKindName(stage.kind).empty() ||
          stage.registerCount == 0U ||
          stage.registerCount > kMaximumRegistersPerStage ||
          stage.instructions.empty() ||
          stage.instructions.size() > kMaximumInstructionsPerStage ||
          stage.instructions.size() != stage.registerCount ||
          (stage.outputs.empty() &&
           stage.executionParameterBindings.empty()) ||
          stage.outputs.size() > kMaximumOutputsPerStage ||
          stage.executionParameterBindings.size() >
              kMaximumExecutionParameterBindingsPerStage) {
        return fail("text effect stage identity or budget is invalid");
      }
      std::vector<bool> written(stage.registerCount, false);
      for (const auto &instruction : stage.instructions) {
        if (instruction.outputRegister >= stage.registerCount ||
            written[instruction.outputRegister] ||
            TextEffectOpcodeName(instruction.opcode).empty() ||
            (instruction.input &&
             (TextEffectInputName(*instruction.input).empty() ||
              !StageAcceptsInput(stage.kind, *instruction.input))) ||
            !ValidateArity(instruction)) {
          return fail(
              "text effect instruction shape or destination is invalid");
        }
        if (instruction.input &&
            ((ReadsCurrentPositionX(*instruction.input) &&
              !hasCurrentPositionX) ||
             (ReadsCurrentPositionY(*instruction.input) &&
              !hasCurrentPositionY) ||
             (ReadsCurrentPositionValidity(*instruction.input) &&
              !hasCurrentPositionValidity))) {
          return fail(
              "text effect current-position input has no preceding producer");
        }
        for (const auto source : instruction.inputRegisters) {
          if (source >= stage.registerCount || !written[source])
            return fail("text effect instruction reads an unwritten register");
        }
        if (!std::all_of(
                instruction.immediates.begin(), instruction.immediates.end(),
                [](const double value) { return std::isfinite(value); })) {
          return fail("text effect instruction immediate is not finite");
        }
        if (instruction.opcode == TextEffectOpcode::ImmediateLookup &&
            !std::all_of(instruction.immediates.begin(),
                         instruction.immediates.end(), [](const double value) {
                           return std::floor(value) == value &&
                                  std::fabs(value) <= 9'007'199'254'740'991.0;
                         })) {
          return fail("text effect immediate-lookup entry is not an exact integer");
        }
        if (instruction.opcode == TextEffectOpcode::CubicBezierCurve) {
          const auto &knots = instruction.immediates;
          double previousTime = knots[0];
          for (std::size_t index = 2U; index < knots.size(); index += 6U) {
            const double duration = knots[index] - previousTime;
            if (!(duration > 0.0) || !std::isfinite(duration) ||
                knots[index + 2U] < 0.0 || knots[index + 2U] > 1.0 ||
                knots[index + 4U] < 0.0 || knots[index + 4U] > 1.0) {
              return fail("text effect cubic-bezier curve times or x controls are invalid");
            }
            previousTime = knots[index];
          }
        }
        if (instruction.opcode == TextEffectOpcode::EllipticRowLayout &&
            (instruction.immediates[0] < 0.0 ||
             instruction.immediates[0] > 3.0 ||
             std::floor(instruction.immediates[0]) != instruction.immediates[0])) {
          return fail("text elliptic row layout component is outside [0, 3]");
        }
        if ((instruction.opcode == TextEffectOpcode::SampleHoldNoise ||
             instruction.opcode == TextEffectOpcode::PageSampleHoldNoise) &&
            !(instruction.immediates[0] > 0.0)) {
          return fail("text effect sample-hold frequency is invalid");
        }
        if ((instruction.opcode == TextEffectOpcode::SeededShuffleRank ||
             instruction.opcode ==
                 TextEffectOpcode::SeededReverseShuffleRank) &&
            !instruction.immediates.empty() &&
            (instruction.immediates[0] < 0.0 ||
             instruction.immediates[0] >
                 kMaximumSequentialRandomCallIndex ||
             std::floor(instruction.immediates[0]) !=
                 instruction.immediates[0])) {
          return fail(
              "text effect seeded-shuffle start call index is invalid");
        }
        if (instruction.opcode == TextEffectOpcode::DurationPrefix ||
            instruction.opcode == TextEffectOpcode::DurationLaneProgress) {
          double cumulative = 0.0;
          for (const auto duration : instruction.immediates) {
            if (!(duration > 0.0) ||
                !std::isfinite(cumulative += duration)) {
              return fail("text effect duration lane is invalid");
            }
          }
        }
        if (instruction.opcode == TextEffectOpcode::CubicBezierTiming &&
            instruction.inputRegisters.size() == 1U &&
            (instruction.immediates[0] < 0.0 ||
             instruction.immediates[0] > 1.0 ||
             instruction.immediates[2] < 0.0 ||
             instruction.immediates[2] > 1.0)) {
          return fail("text effect cubic-bezier x controls are invalid");
        }
        if (instruction.opcode == TextEffectOpcode::CubicBezierTiming &&
            (instruction.immediates.size() == 5U ||
             instruction.immediates.size() == 1U) &&
            instruction.immediates.back() != 0.0 &&
            instruction.immediates.back() != 1.0) {
          return fail("text effect cubic-bezier source numeric mode is invalid");
        }
        written[instruction.outputRegister] = true;
      }
      std::unordered_set<std::uint64_t> stageOutputKeys;
      stageOutputKeys.reserve(stage.outputs.size());
      bool publishesCurrentPositionX = false;
      bool publishesCurrentPositionY = false;
      bool publishesCurrentPositionValidity = false;
      for (const auto &binding : stage.outputs) {
        const auto outputKey =
            (static_cast<std::uint64_t>(binding.transformIndex) << 32U) |
            static_cast<std::uint64_t>(binding.output);
        if (binding.registerIndex >= stage.registerCount ||
            !written[binding.registerIndex] ||
            TextEffectOutputName(binding.output).empty() ||
            !stageOutputKeys.insert(outputKey).second ||
            (IsTransformOutput(binding.output) &&
             binding.transformIndex >= kMaximumTransformsPerUnit) ||
            (!IsTransformOutput(binding.output) &&
             binding.transformIndex != 0U)) {
          return fail("text effect output binding is invalid");
        }
        if (IsTransformOutput(binding.output)) {
          const auto index = static_cast<std::size_t>(binding.transformIndex);
          if (IsDirectMatrixOutput(binding.output)) {
            if (hasDecomposedTransform[index]) {
              return fail("text effect transform mixes matrix and decomposed outputs");
            }
            hasDirectMatrix[index] = true;
          } else {
            if (hasDirectMatrix[index]) {
              return fail("text effect transform mixes matrix and decomposed outputs");
            }
            hasDecomposedTransform[index] = true;
          }
        }
        publishesCurrentPositionX |=
            binding.output == TextEffectOutput::CurrentPositionX;
        publishesCurrentPositionY |=
            binding.output == TextEffectOutput::CurrentPositionY;
        publishesCurrentPositionValidity |=
            binding.output == TextEffectOutput::CurrentPositionValid;
      }
      for (const auto &binding : stage.executionParameterBindings) {
        const auto componentCount =
            TextEffectExecutionParameterComponentCount(binding.parameter);
        const bool postEffect =
            IsPostEffectExecutionParameter(binding.parameter);
        const bool material = IsMaterialExecutionParameter(binding.parameter);
        const bool scene = IsSceneExecutionParameter(binding.parameter);
        const bool node = IsNodeExecutionParameter(binding.parameter);
        std::string key;
        try {
          key = std::to_string(binding.nodeId.size()) + ":" + binding.nodeId +
                ":" + std::to_string(static_cast<unsigned>(binding.parameter)) +
                ":" + std::to_string(binding.slot);
        } catch (...) {
          return fail("text effect execution parameter key allocation failed");
        }
        if (binding.nodeId.empty() || binding.nodeId.size() > 512U ||
            !IsValidUtf8(binding.nodeId) || componentCount == 0U ||
            TextEffectExecutionParameterKindName(binding.parameter).empty() ||
            TextEffectExecutionParameterDomainName(binding.domain).empty() ||
            TextEffectExecutionParameterSpaceName(binding.valueSpace).empty() ||
            binding.registerIndices.size() != componentCount ||
            (binding.valueSpace ==
                 TextEffectExecutionParameterSpace::WorldUnits &&
             !node) ||
            (postEffect &&
             (binding.domain != TextEffectExecutionParameterDomain::Page ||
              binding.slot != 0U)) ||
            (material && binding.slot >= kMaximumMaterialParameterSlots) ||
            (node && binding.slot >= kMaximumMaterialParameterSlots) ||
            (scene && binding.slot != 0U) ||
            (binding.parameter ==
                 TextEffectExecutionParameterKind::PostEffectProgress &&
             binding.valueSpace !=
                 TextEffectExecutionParameterSpace::Unitless) ||
            (binding.parameter ==
                 TextEffectExecutionParameterKind::PostEffectBlurRadius &&
             binding.valueSpace ==
                 TextEffectExecutionParameterSpace::Unitless) ||
            (binding.parameter ==
                 TextEffectExecutionParameterKind::SceneTranslation &&
             binding.valueSpace !=
                     TextEffectExecutionParameterSpace::SourcePixels &&
             binding.valueSpace !=
                     TextEffectExecutionParameterSpace::ReferencePixels) ||
            ((binding.parameter ==
                  TextEffectExecutionParameterKind::SceneScale ||
              binding.parameter ==
                  TextEffectExecutionParameterKind::SceneOpacity) &&
             binding.valueSpace !=
                 TextEffectExecutionParameterSpace::Unitless) ||
            !executionParameterKeys.insert(std::move(key)).second) {
          return fail("text effect execution parameter binding is invalid");
        }
        for (const auto source : binding.registerIndices) {
          if (source >= stage.registerCount || !written[source]) {
            return fail(
                "text effect execution parameter reads an unwritten register");
          }
        }
      }
      if ((publishesCurrentPositionX || publishesCurrentPositionY) !=
          publishesCurrentPositionValidity) {
        return fail("text effect current-position producer is incomplete");
      }
      hasCurrentPositionX |= publishesCurrentPositionX;
      hasCurrentPositionY |= publishesCurrentPositionY;
      hasCurrentPositionValidity |= publishesCurrentPositionValidity;
    }
  } catch (...) {
    return fail("text effect program validation allocation failed");
  }
  if (error)
    error->clear();
  return true;
}

const TextEffectProgramIR *
FindTextEffectProgram(const TextEffectProgramLibrary &library,
                      const std::string_view programId) noexcept {
  const auto found = std::find_if(library.begin(), library.end(),
                                  [&](const TextEffectProgramIR &candidate) {
                                    return candidate.programId == programId;
                                  });
  return found == library.end() ? nullptr : &*found;
}

} // namespace videocut::text
