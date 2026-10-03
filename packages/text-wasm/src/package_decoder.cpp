// Extracted native package JSON decoders. See vendor/SOURCE.json for source ranges.
#include "package_decoder.h"
#include "videocut/text/TextEffectEvaluator.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
namespace videocut::text_wasm {
namespace {
[[nodiscard]] bool
HasExactJsonFields(const nlohmann::json &value,
                   const std::initializer_list<const char *> fields) {
  return value.is_object() && value.size() == fields.size() &&
         std::all_of(fields.begin(), fields.end(),
                     [&](const char *field) { return value.contains(field); });
}

[[nodiscard]] bool DecodeTextPackageUint32(const nlohmann::json &value,
                                           std::uint32_t &output) noexcept {
  if (!value.is_number_integer())
    return false;
  if (value.is_number_unsigned()) {
    const auto decoded = value.get<std::uint64_t>();
    if (decoded > std::numeric_limits<std::uint32_t>::max())
      return false;
    output = static_cast<std::uint32_t>(decoded);
    return true;
  }
  const auto decoded = value.get<std::int64_t>();
  if (decoded < 0 || decoded > static_cast<std::int64_t>(
                                   std::numeric_limits<std::uint32_t>::max()))
    return false;
  output = static_cast<std::uint32_t>(decoded);
  return true;
}

[[nodiscard]] bool DecodeTextPackageUint64(const nlohmann::json &value,
                                           std::uint64_t &output) noexcept {
  if (!value.is_number_integer())
    return false;
  if (value.is_number_unsigned()) {
    output = value.get<std::uint64_t>();
    return true;
  }
  const auto decoded = value.get<std::int64_t>();
  if (decoded < 0)
    return false;
  output = static_cast<std::uint64_t>(decoded);
  return true;
}

[[nodiscard]] bool
ValidateTextEffectProgramDocument(const nlohmann::json &document,
                                  std::string &error) {
  static constexpr std::size_t maximumProgramCount = 64U;
  static constexpr std::size_t maximumStageCount = 16U;
  static constexpr std::uint32_t maximumRegisterCount = 512U;
  static constexpr std::size_t maximumInstructionCount = 2'048U;
  static constexpr std::size_t maximumOutputCount = 256U;
  static constexpr std::size_t maximumExecutionParameterBindingCount = 256U;
  static constexpr std::size_t maximumInstructionImmediateCount = 65'536U;
  if (!HasExactJsonFields(document, {"format", "programs"}) ||
      !document["format"].is_string() ||
      document["format"].get<std::string>() != "videocut.text-effect-program" ||
      !document["programs"].is_array() ||
      document["programs"].size() > maximumProgramCount) {
    error = "builtin text effect-program entry is malformed";
    return false;
  }

  std::set<std::string> programIds;
  for (const auto &program : document["programs"]) {
    std::uint64_t randomSeed = 0U;
    if (!HasExactJsonFields(program, {"program_id", "random_seed", "stages"}) ||
        !program["program_id"].is_string() ||
        program["program_id"].get_ref<const std::string &>().empty() ||
        program["program_id"].get_ref<const std::string &>().size() > 512U ||
        !programIds.insert(program["program_id"].get<std::string>()).second ||
        !DecodeTextPackageUint64(program["random_seed"], randomSeed) ||
        !program["stages"].is_array() || program["stages"].empty() ||
        program["stages"].size() > maximumStageCount) {
      error = "builtin text effect program identity is invalid";
      return false;
    }
    (void)randomSeed;

    std::set<std::string> stageIds;
    for (const auto &stage : program["stages"]) {
      std::uint32_t registerCount = 0U;
      const bool hasExecutionParameterBindings =
          stage.contains("execution_parameter_bindings");
      if ((!hasExecutionParameterBindings &&
           !HasExactJsonFields(stage, {"stage_id", "kind", "register_count",
                                       "instructions", "outputs"})) ||
          (hasExecutionParameterBindings &&
           !HasExactJsonFields(stage,
                               {"stage_id", "kind", "register_count",
                                "instructions", "outputs",
                                "execution_parameter_bindings"})) ||
          !stage["stage_id"].is_string() ||
          stage["stage_id"].get_ref<const std::string &>().empty() ||
          stage["stage_id"].get_ref<const std::string &>().size() > 512U ||
          !stageIds.insert(stage["stage_id"].get<std::string>()).second ||
          !stage["kind"].is_string() ||
          !videocut::text::ParseTextEffectStageKind(
              stage["kind"].get_ref<const std::string &>()) ||
          !DecodeTextPackageUint32(stage["register_count"], registerCount) ||
          registerCount == 0U || registerCount > maximumRegisterCount ||
          !stage["instructions"].is_array() || stage["instructions"].empty() ||
          stage["instructions"].size() > maximumInstructionCount ||
          !stage["outputs"].is_array() ||
          stage["outputs"].size() > maximumOutputCount ||
          (hasExecutionParameterBindings &&
           (!stage["execution_parameter_bindings"].is_array() ||
            stage["execution_parameter_bindings"].size() >
                maximumExecutionParameterBindingCount)) ||
          (stage["outputs"].empty() &&
           (!hasExecutionParameterBindings ||
            stage["execution_parameter_bindings"].empty()))) {
        error = "builtin text effect program stage is invalid";
        return false;
      }

      for (const auto &instruction : stage["instructions"]) {
        std::uint32_t outputRegister = 0U;
        if (!HasExactJsonFields(instruction,
                                {"opcode", "output_register", "input_registers",
                                 "immediates", "input"}) ||
            !instruction["opcode"].is_string() ||
            !videocut::text::ParseTextEffectOpcode(
                instruction["opcode"].get_ref<const std::string &>()) ||
            !DecodeTextPackageUint32(instruction["output_register"],
                                     outputRegister) ||
            outputRegister >= registerCount ||
            !instruction["input_registers"].is_array() ||
            !instruction["immediates"].is_array() ||
            instruction["immediates"].size() >
                maximumInstructionImmediateCount) {
          error = "builtin text effect instruction is invalid";
          return false;
        }
        for (const auto &inputRegister : instruction["input_registers"]) {
          std::uint32_t decoded = 0U;
          if (!DecodeTextPackageUint32(inputRegister, decoded) ||
              decoded >= registerCount) {
            error = "builtin text effect instruction register is invalid";
            return false;
          }
        }
        for (const auto &immediate : instruction["immediates"]) {
          if (!immediate.is_number() ||
              !std::isfinite(immediate.get<double>())) {
            error = "builtin text effect instruction immediate is invalid";
            return false;
          }
        }
        const bool isInput =
            instruction["opcode"].get<std::string>() == "input";
        if ((isInput &&
             (!instruction["input"].is_string() ||
              !videocut::text::ParseTextEffectInput(
                  instruction["input"].get_ref<const std::string &>()))) ||
            (!isInput && !instruction["input"].is_null())) {
          error = "builtin text effect instruction input is invalid";
          return false;
        }
      }

      for (const auto &binding : stage["outputs"]) {
        std::uint32_t registerIndex = 0U;
        std::uint32_t transformIndex = 0U;
        if (!HasExactJsonFields(
                binding, {"output", "register_index", "transform_index"}) ||
            !binding["output"].is_string() ||
            !videocut::text::ParseTextEffectOutput(
                binding["output"].get_ref<const std::string &>()) ||
            !DecodeTextPackageUint32(binding["register_index"],
                                     registerIndex) ||
            registerIndex >= registerCount ||
            !DecodeTextPackageUint32(binding["transform_index"],
                                     transformIndex) ||
            transformIndex > 31U) {
          error = "builtin text effect output binding is invalid";
          return false;
        }
      }

      if (hasExecutionParameterBindings) {
        for (const auto &binding : stage["execution_parameter_bindings"]) {
          std::uint32_t slot = 0U;
          if (!HasExactJsonFields(
                  binding,
                  {"node_id", "parameter", "domain", "value_space", "slot",
                   "register_indices"}) ||
              !binding["node_id"].is_string() ||
              binding["node_id"].get_ref<const std::string &>().empty() ||
              binding["node_id"].get_ref<const std::string &>().size() >
                  512U ||
              !binding["parameter"].is_string() ||
              !videocut::text::ParseTextEffectExecutionParameterKind(
                  binding["parameter"].get_ref<const std::string &>()) ||
              !binding["domain"].is_string() ||
              !videocut::text::ParseTextEffectExecutionParameterDomain(
                  binding["domain"].get_ref<const std::string &>()) ||
              !binding["value_space"].is_string() ||
              !videocut::text::ParseTextEffectExecutionParameterSpace(
                  binding["value_space"].get_ref<const std::string &>()) ||
              !DecodeTextPackageUint32(binding["slot"], slot) ||
              !binding["register_indices"].is_array()) {
            error =
                "builtin text effect execution parameter binding is invalid";
            return false;
          }
          const auto parameter =
              *videocut::text::ParseTextEffectExecutionParameterKind(
                  binding["parameter"].get_ref<const std::string &>());
          if (binding["register_indices"].size() !=
              videocut::text::TextEffectExecutionParameterComponentCount(
                  parameter)) {
            error =
                "builtin text effect execution parameter arity is invalid";
            return false;
          }
          for (const auto &source : binding["register_indices"]) {
            std::uint32_t registerIndex = 0U;
            if (!DecodeTextPackageUint32(source, registerIndex) ||
                registerIndex >= registerCount) {
              error = "builtin text effect execution parameter register is "
                      "invalid";
              return false;
            }
          }
          (void)slot;
        }
      }
    }
  }
  error.clear();
  return true;
}

void RequireCurrentJsonObject(const nlohmann::json &value,
                              const std::string &context) {
  if (!value.is_object())
    throw std::invalid_argument(context + " must be an object");
}

void RequireOnlyCurrentJsonFields(
    const nlohmann::json &value,
    const std::initializer_list<const char *> fields,
    const std::string &context) {
  RequireCurrentJsonObject(value, context);
  for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
    if (std::none_of(fields.begin(), fields.end(), [&](const char *field) {
          return iterator.key() == field;
        })) {
      throw std::invalid_argument(context + " contains unsupported field " +
                                  iterator.key());
    }
  }
}

const nlohmann::json &RequireCurrentJsonMember(const nlohmann::json &value,
                                               const char *field,
                                               const std::string &context) {
  const auto found = value.find(field);
  if (found == value.end())
    throw std::invalid_argument(context + "." + field + " is required");
  return *found;
}

std::string RequireCurrentJsonString(const nlohmann::json &value,
                                     const char *field,
                                     const std::string &context) {
  const auto &member = RequireCurrentJsonMember(value, field, context);
  if (!member.is_string() || member.get_ref<const std::string &>().empty())
    throw std::invalid_argument(context + "." + field +
                                " must be a non-empty string");
  return member.get<std::string>();
}

std::string OptionalCurrentJsonString(const nlohmann::json &value,
                                      const char *field,
                                      std::string fallback = {}) {
  const auto found = value.find(field);
  if (found == value.end())
    return fallback;
  if (!found->is_string())
    throw std::invalid_argument(std::string(field) + " must be a string");
  return found->get<std::string>();
}

bool OptionalCurrentJsonBool(const nlohmann::json &value, const char *field,
                             const bool fallback) {
  const auto found = value.find(field);
  if (found == value.end())
    return fallback;
  if (!found->is_boolean())
    throw std::invalid_argument(std::string(field) + " must be boolean");
  return found->get<bool>();
}

double OptionalCurrentJsonNumber(const nlohmann::json &value, const char *field,
                                 const double fallback) {
  const auto found = value.find(field);
  if (found == value.end())
    return fallback;
  if (!found->is_number())
    throw std::invalid_argument(std::string(field) + " must be numeric");
  const auto decoded = found->get<double>();
  if (!std::isfinite(decoded))
    throw std::invalid_argument(std::string(field) + " must be finite");
  return decoded;
}

std::int64_t OptionalCurrentJsonInteger(const nlohmann::json &value,
                                        const char *field,
                                        const std::int64_t fallback) {
  const auto found = value.find(field);
  if (found == value.end())
    return fallback;
  if (found->is_number_unsigned()) {
    const auto decoded = found->get<std::uint64_t>();
    if (decoded >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      throw std::invalid_argument(std::string(field) + " exceeds int64");
    return static_cast<std::int64_t>(decoded);
  }
  if (!found->is_number_integer())
    throw std::invalid_argument(std::string(field) + " must be integer");
  return found->get<std::int64_t>();
}

std::vector<std::string>
OptionalCurrentJsonStringArray(const nlohmann::json &value, const char *field) {
  const auto found = value.find(field);
  if (found == value.end())
    return {};
  if (!found->is_array())
    throw std::invalid_argument(std::string(field) + " must be an array");
  std::vector<std::string> decoded;
  decoded.reserve(found->size());
  for (const auto &item : *found) {
    if (!item.is_string() || item.get_ref<const std::string &>().empty())
      throw std::invalid_argument(std::string(field) +
                                  " entries must be non-empty strings");
    decoded.push_back(item.get<std::string>());
  }
  return decoded;
}

videocut::text::Color DecodeCurrentTextColor(const nlohmann::json &value,
                                             const std::string &context) {
  if (!value.is_array() || value.size() != 4U)
    throw std::invalid_argument(context + " must be an RGBA array");
  std::array<float, 4U> components{};
  for (std::size_t index = 0U; index < components.size(); ++index) {
    if (!value[index].is_number())
      throw std::invalid_argument(context + " must contain numbers");
    const auto decoded = value[index].get<double>();
    if (!std::isfinite(decoded))
      throw std::invalid_argument(context + " must contain finite numbers");
    components[index] = static_cast<float>(decoded);
  }
  return {components[0], components[1], components[2], components[3]};
}

float DecodeCurrentTextFloat(const nlohmann::json &value,
                             const std::string &context) {
  if (!value.is_number())
    throw std::invalid_argument(context + " must be numeric");
  const auto decoded = value.get<double>();
  if (!std::isfinite(decoded) ||
      std::fabs(decoded) >
          static_cast<double>(std::numeric_limits<float>::max())) {
    throw std::invalid_argument(context + " must be a finite float");
  }
  return static_cast<float>(decoded);
}

std::pair<float, float> DecodeCurrentTextPoint(const nlohmann::json &value,
                                               const std::string &context) {
  if (!value.is_array() || value.size() != 2U)
    throw std::invalid_argument(context + " must be a two-component array");
  return {DecodeCurrentTextFloat(value[0], context + "[0]"),
          DecodeCurrentTextFloat(value[1], context + "[1]")};
}

videocut::text::TextMaterialCoordinates
DecodeCurrentTextMaterialCoordinates(const nlohmann::json &value,
                                     const std::string &context) {
  RequireOnlyCurrentJsonFields(value, {"space", "outset", "scale"}, context);
  videocut::text::TextMaterialCoordinates result;
  const auto space = OptionalCurrentJsonString(value, "space", "layout_box");
  using Space = videocut::text::PaintCoordinateSpace;
  if (space == "layout_box")
    result.coordinateSpace = Space::LayoutBox;
  else if (space == "text_bounds")
    result.coordinateSpace = Space::TextBounds;
  else if (space == "grapheme")
    result.coordinateSpace = Space::Grapheme;
  else
    throw std::invalid_argument(context + ".space is unsupported");
  result.coordinateOutset = static_cast<float>(
      OptionalCurrentJsonNumber(value, "outset", result.coordinateOutset));
  result.coordinateScale = static_cast<float>(
      OptionalCurrentJsonNumber(value, "scale", result.coordinateScale));
  if (!std::isfinite(result.coordinateOutset) ||
      !std::isfinite(result.coordinateScale)) {
    throw std::invalid_argument(context + " contains a non-finite float");
  }
  return result;
}

std::vector<videocut::text::GradientStop>
DecodeCurrentTextGradientStops(const nlohmann::json &value,
                               const std::string &context) {
  if (!value.is_array())
    throw std::invalid_argument(context + " must be an array");
  std::vector<videocut::text::GradientStop> result;
  result.reserve(value.size());
  for (std::size_t index = 0U; index < value.size(); ++index) {
    const auto itemContext = context + "[" + std::to_string(index) + "]";
    RequireOnlyCurrentJsonFields(value[index], {"offset", "color"},
                                 itemContext);
    const auto &offset =
        RequireCurrentJsonMember(value[index], "offset", itemContext);
    const auto &color =
        RequireCurrentJsonMember(value[index], "color", itemContext);
    result.push_back({DecodeCurrentTextFloat(offset, itemContext + ".offset"),
                      DecodeCurrentTextColor(color, itemContext + ".color")});
  }
  return result;
}

videocut::text::TextureReference
DecodeCurrentTextTextureReference(const nlohmann::json &value,
                                  const std::string &context) {
  RequireOnlyCurrentJsonFields(value,
                               {"source_kind", "asset_id", "digest",
                                "media_type", "color_space", "orientation"},
                               context);
  videocut::text::TextureReference result;
  const auto sourceKind =
      OptionalCurrentJsonString(value, "source_kind", "builtin");
  using SourceKind = videocut::text::TextureSourceKind;
  if (sourceKind == "builtin")
    result.sourceKind = SourceKind::Builtin;
  else if (sourceKind == "project_managed")
    result.sourceKind = SourceKind::ProjectManaged;
  else
    throw std::invalid_argument(context + ".source_kind is unsupported");
  result.assetId = RequireCurrentJsonString(value, "asset_id", context);
  result.digest = RequireCurrentJsonString(value, "digest", context);
  result.mediaType = RequireCurrentJsonString(value, "media_type", context);
  result.colorSpace = OptionalCurrentJsonString(value, "color_space", "srgb");
  const auto orientation =
      OptionalCurrentJsonString(value, "orientation", "up");
  using Orientation = videocut::text::TextureOrientation;
  if (orientation == "up")
    result.orientation = Orientation::Up;
  else if (orientation == "right")
    result.orientation = Orientation::Right;
  else if (orientation == "down")
    result.orientation = Orientation::Down;
  else if (orientation == "left")
    result.orientation = Orientation::Left;
  else
    throw std::invalid_argument(context + ".orientation is unsupported");
  return result;
}

videocut::text::TextMaterial
DecodeCurrentTextMaterial(const nlohmann::json &value,
                          const std::string &context) {
  const auto kind = RequireCurrentJsonString(value, "kind", context);
  if (kind == "solid") {
    RequireOnlyCurrentJsonFields(value, {"kind", "color"}, context);
    return videocut::text::SolidTextMaterial{DecodeCurrentTextColor(
        RequireCurrentJsonMember(value, "color", context), context + ".color")};
  }

  const auto decodeSpread = [&]() {
    const auto spread = OptionalCurrentJsonString(value, "spread", "clamp");
    using Spread = videocut::text::PaintSpread;
    if (spread == "clamp")
      return Spread::Clamp;
    if (spread == "repeat")
      return Spread::Repeat;
    if (spread == "mirror")
      return Spread::Mirror;
    throw std::invalid_argument(context + ".spread is unsupported");
  };
  const auto decodeSampling = [&]() {
    const auto sampling =
        OptionalCurrentJsonString(value, "sampling", "continuous");
    using Sampling = videocut::text::GradientSampling;
    if (sampling == "continuous")
      return Sampling::Continuous;
    if (sampling == "rgba8_lut_256")
      return Sampling::Rgba8Lut256;
    throw std::invalid_argument(context + ".sampling is unsupported");
  };
  const auto decodeCoordinates = [&]() {
    const auto found = value.find("coordinates");
    return found == value.end() ? videocut::text::TextMaterialCoordinates{}
                                : DecodeCurrentTextMaterialCoordinates(
                                      *found, context + ".coordinates");
  };

  if (kind == "linear_gradient") {
    RequireOnlyCurrentJsonFields(
        value,
        {"kind", "stops", "start", "end", "spread", "sampling", "coordinates"},
        context);
    videocut::text::LinearGradientTextMaterial result;
    result.stops = DecodeCurrentTextGradientStops(
        RequireCurrentJsonMember(value, "stops", context), context + ".stops");
    if (const auto start = value.find("start"); start != value.end()) {
      const auto point = DecodeCurrentTextPoint(*start, context + ".start");
      result.startX = point.first;
      result.startY = point.second;
    }
    if (const auto end = value.find("end"); end != value.end()) {
      const auto point = DecodeCurrentTextPoint(*end, context + ".end");
      result.endX = point.first;
      result.endY = point.second;
    }
    result.spread = decodeSpread();
    result.sampling = decodeSampling();
    result.coordinates = decodeCoordinates();
    return result;
  }
  if (kind == "radial_gradient") {
    RequireOnlyCurrentJsonFields(value,
                                 {"kind", "stops", "center", "radius", "spread",
                                  "sampling", "coordinates"},
                                 context);
    videocut::text::RadialGradientTextMaterial result;
    result.stops = DecodeCurrentTextGradientStops(
        RequireCurrentJsonMember(value, "stops", context), context + ".stops");
    if (const auto center = value.find("center"); center != value.end()) {
      const auto point = DecodeCurrentTextPoint(*center, context + ".center");
      result.centerX = point.first;
      result.centerY = point.second;
    }
    result.radius = static_cast<float>(
        OptionalCurrentJsonNumber(value, "radius", result.radius));
    result.spread = decodeSpread();
    result.sampling = decodeSampling();
    result.coordinates = decodeCoordinates();
    return result;
  }
  if (kind == "texture") {
    RequireOnlyCurrentJsonFields(
        value,
        {"kind", "texture", "fit", "mapping", "coordinates", "scale",
         "rotation_degrees", "offset_x", "offset_y", "flip_x", "flip_y",
         "atlas_columns", "atlas_rows", "texture_opacity", "opacity",
         "source_alpha", "underlay_color", "underlay_gradient",
         "underlay_gradient_projection"},
        context);
    videocut::text::TextureTextMaterial result;
    result.texture = DecodeCurrentTextTextureReference(
        RequireCurrentJsonMember(value, "texture", context),
        context + ".texture");
    const auto fit = OptionalCurrentJsonString(value, "fit", "cover");
    using Fit = videocut::text::TextureFit;
    if (fit == "cover")
      result.fit = Fit::Cover;
    else if (fit == "contain")
      result.fit = Fit::Contain;
    else if (fit == "stretch")
      result.fit = Fit::Stretch;
    else if (fit == "tile")
      result.fit = Fit::Tile;
    else
      throw std::invalid_argument(context + ".fit is unsupported");
    const auto mapping =
        OptionalCurrentJsonString(value, "mapping", "scope_bounds");
    using Mapping = videocut::text::TextureMapping;
    if (mapping == "scope_bounds")
      result.mapping = Mapping::ScopeBounds;
    else if (mapping == "glyph_distance_field")
      result.mapping = Mapping::GlyphDistanceField;
    else
      throw std::invalid_argument(context + ".mapping is unsupported");
    result.coordinates = decodeCoordinates();
    result.scale = static_cast<float>(
        OptionalCurrentJsonNumber(value, "scale", result.scale));
    result.rotationDegrees = static_cast<float>(OptionalCurrentJsonNumber(
        value, "rotation_degrees", result.rotationDegrees));
    result.offsetX = static_cast<float>(
        OptionalCurrentJsonNumber(value, "offset_x", result.offsetX));
    result.offsetY = static_cast<float>(
        OptionalCurrentJsonNumber(value, "offset_y", result.offsetY));
    result.flipX = OptionalCurrentJsonBool(value, "flip_x", result.flipX);
    result.flipY = OptionalCurrentJsonBool(value, "flip_y", result.flipY);
    const auto atlasColumns =
        OptionalCurrentJsonInteger(value, "atlas_columns", 1);
    const auto atlasRows = OptionalCurrentJsonInteger(value, "atlas_rows", 1);
    if (atlasColumns <= 0 || atlasColumns > 65'535 || atlasRows <= 0 ||
        atlasRows > 65'535) {
      throw std::invalid_argument(context + ".atlas geometry is invalid");
    }
    result.atlasColumns = static_cast<std::uint16_t>(atlasColumns);
    result.atlasRows = static_cast<std::uint16_t>(atlasRows);
    result.textureOpacity = static_cast<float>(OptionalCurrentJsonNumber(
        value, "texture_opacity", result.textureOpacity));
    result.opacity = static_cast<float>(
        OptionalCurrentJsonNumber(value, "opacity", result.opacity));
    result.sourceAlpha =
        OptionalCurrentJsonBool(value, "source_alpha", result.sourceAlpha);
    if (const auto color = value.find("underlay_color"); color != value.end()) {
      result.underlayColor =
          DecodeCurrentTextColor(*color, context + ".underlay_color");
    }
    if (const auto gradient = value.find("underlay_gradient");
        gradient != value.end()) {
      result.underlayGradient = DecodeCurrentTextGradientStops(
          *gradient, context + ".underlay_gradient");
    }
    if (const auto projection = value.find("underlay_gradient_projection");
        projection != value.end()) {
      const auto projectionContext = context + ".underlay_gradient_projection";
      RequireOnlyCurrentJsonFields(*projection,
                                   {"start", "end", "spread", "sampling"},
                                   projectionContext);
      if (const auto start = projection->find("start");
          start != projection->end()) {
        const auto point =
            DecodeCurrentTextPoint(*start, projectionContext + ".start");
        result.underlayGradientProjection.startX = point.first;
        result.underlayGradientProjection.startY = point.second;
      }
      if (const auto end = projection->find("end"); end != projection->end()) {
        const auto point =
            DecodeCurrentTextPoint(*end, projectionContext + ".end");
        result.underlayGradientProjection.endX = point.first;
        result.underlayGradientProjection.endY = point.second;
      }
      const auto spread =
          OptionalCurrentJsonString(*projection, "spread", "clamp");
      using Spread = videocut::text::PaintSpread;
      if (spread == "clamp")
        result.underlayGradientProjection.spread = Spread::Clamp;
      else if (spread == "repeat")
        result.underlayGradientProjection.spread = Spread::Repeat;
      else if (spread == "mirror")
        result.underlayGradientProjection.spread = Spread::Mirror;
      else
        throw std::invalid_argument(projectionContext +
                                    ".spread is unsupported");
      const auto sampling =
          OptionalCurrentJsonString(*projection, "sampling", "continuous");
      using Sampling = videocut::text::GradientSampling;
      if (sampling == "continuous")
        result.underlayGradientProjection.sampling = Sampling::Continuous;
      else if (sampling == "rgba8_lut_256")
        result.underlayGradientProjection.sampling = Sampling::Rgba8Lut256;
      else
        throw std::invalid_argument(projectionContext +
                                    ".sampling is unsupported");
    }
    return result;
  }
  throw std::invalid_argument(context + ".kind is unsupported");
}

template <typename Keyframe>
Keyframe DecodeCurrentTextScalarKeyframe(const nlohmann::json &value,
                                         const std::string &context) {
  RequireOnlyCurrentJsonFields(value,
                               {"offset", "value", "tangent_in", "tangent_out",
                                "bezier", "bezier_time_in", "bezier_time_out"},
                               context);
  Keyframe keyframe;
  keyframe.offset = OptionalCurrentJsonNumber(value, "offset", 0.0);
  keyframe.value = OptionalCurrentJsonNumber(value, "value", 0.0);
  keyframe.tangentIn = OptionalCurrentJsonNumber(value, "tangent_in", 0.0);
  keyframe.tangentOut = OptionalCurrentJsonNumber(value, "tangent_out", 0.0);
  keyframe.cubicBezier = OptionalCurrentJsonBool(value, "bezier", false);
  keyframe.bezierTimeIn =
      OptionalCurrentJsonNumber(value, "bezier_time_in", 0.0);
  keyframe.bezierTimeOut =
      OptionalCurrentJsonNumber(value, "bezier_time_out", 0.0);
  return keyframe;
}

void DecodeCurrentTextKeyframeArray(
    const nlohmann::json &value, const char *field, const std::string &context,
    std::vector<videocut::text::TextKeyframe> &output) {
  const auto found = value.find(field);
  if (found == value.end())
    return;
  if (!found->is_array())
    throw std::invalid_argument(context + "." + field + " must be an array");
  output.reserve(found->size());
  for (std::size_t index = 0U; index < found->size(); ++index) {
    output.push_back(
        DecodeCurrentTextScalarKeyframe<videocut::text::TextKeyframe>(
            (*found)[index],
            context + "." + field + "[" + std::to_string(index) + "]"));
  }
}

[[nodiscard]] bool DecodeTextEffectProgramDocument(
    const nlohmann::json &document,
    videocut::text::TextEffectProgramLibrary &library, std::string &error) {
  library.clear();
  if (!ValidateTextEffectProgramDocument(document, error))
    return false;
  try {
    library.reserve(document["programs"].size());
    for (const auto &source : document["programs"]) {
      videocut::text::TextEffectProgramIR program;
      program.programId = source["program_id"].get<std::string>();
      DecodeTextPackageUint64(source["random_seed"], program.randomSeed);
      program.stages.reserve(source["stages"].size());
      for (const auto &stageSource : source["stages"]) {
        videocut::text::TextEffectProgramStageIR stage;
        stage.stageId = stageSource["stage_id"].get<std::string>();
        stage.kind = *videocut::text::ParseTextEffectStageKind(
            stageSource["kind"].get_ref<const std::string &>());
        DecodeTextPackageUint32(stageSource["register_count"],
                                stage.registerCount);
        stage.instructions.reserve(stageSource["instructions"].size());
        for (const auto &instructionSource : stageSource["instructions"]) {
          videocut::text::TextEffectInstruction instruction;
          instruction.opcode = *videocut::text::ParseTextEffectOpcode(
              instructionSource["opcode"].get_ref<const std::string &>());
          DecodeTextPackageUint32(instructionSource["output_register"],
                                  instruction.outputRegister);
          for (const auto &input : instructionSource["input_registers"]) {
            std::uint32_t registerIndex = 0U;
            DecodeTextPackageUint32(input, registerIndex);
            instruction.inputRegisters.push_back(registerIndex);
          }
          for (const auto &immediate : instructionSource["immediates"])
            instruction.immediates.push_back(immediate.get<double>());
          if (!instructionSource["input"].is_null()) {
            instruction.input = *videocut::text::ParseTextEffectInput(
                instructionSource["input"].get_ref<const std::string &>());
          }
          stage.instructions.push_back(std::move(instruction));
        }
        stage.outputs.reserve(stageSource["outputs"].size());
        for (const auto &outputSource : stageSource["outputs"]) {
          videocut::text::TextEffectOutputBinding binding;
          binding.output = *videocut::text::ParseTextEffectOutput(
              outputSource["output"].get_ref<const std::string &>());
          DecodeTextPackageUint32(outputSource["register_index"],
                                  binding.registerIndex);
          DecodeTextPackageUint32(outputSource["transform_index"],
                                  binding.transformIndex);
          stage.outputs.push_back(binding);
        }
        if (const auto bindings =
                stageSource.find("execution_parameter_bindings");
            bindings != stageSource.end()) {
          stage.executionParameterBindings.reserve(bindings->size());
          for (const auto &bindingSource : *bindings) {
            videocut::text::TextEffectExecutionParameterBinding binding;
            binding.nodeId = bindingSource["node_id"].get<std::string>();
            binding.parameter =
                *videocut::text::ParseTextEffectExecutionParameterKind(
                    bindingSource["parameter"]
                        .get_ref<const std::string &>());
            binding.domain =
                *videocut::text::ParseTextEffectExecutionParameterDomain(
                    bindingSource["domain"].get_ref<const std::string &>());
            binding.valueSpace =
                *videocut::text::ParseTextEffectExecutionParameterSpace(
                    bindingSource["value_space"]
                        .get_ref<const std::string &>());
            DecodeTextPackageUint32(bindingSource["slot"], binding.slot);
            binding.registerIndices.reserve(
                bindingSource["register_indices"].size());
            for (const auto &source : bindingSource["register_indices"]) {
              std::uint32_t registerIndex = 0U;
              DecodeTextPackageUint32(source, registerIndex);
              binding.registerIndices.push_back(registerIndex);
            }
            stage.executionParameterBindings.push_back(std::move(binding));
          }
        }
        program.stages.push_back(std::move(stage));
      }
      std::string validationError;
      if (!videocut::text::ValidateTextEffectProgramIR(program,
                                                       &validationError)) {
        error = "builtin text effect program failed current validation: " +
                validationError;
        return false;
      }
      library.push_back(std::move(program));
    }
  } catch (const nlohmann::json::exception &) {
    library.clear();
    error = "builtin text effect-program entry changed while decoding";
    return false;
  }
  error.clear();
  return true;
}

videocut::text::TextAnimationPlaybackMode
DecodeCurrentAnimationPlayback(const std::string_view value) {
  using Value = videocut::text::TextAnimationPlaybackMode;
  if (value == "once")
    return Value::Once;
  if (value == "loop")
    return Value::Loop;
  if (value == "ping_pong")
    return Value::PingPong;
  if (value == "hold")
    return Value::Hold;
  throw std::invalid_argument("unsupported text animation playback");
}

videocut::text::TextSelectorShape
DecodeCurrentSelectorShape(const std::string_view value) {
  using Value = videocut::text::TextSelectorShape;
  if (value == "linear")
    return Value::Linear;
  if (value == "ramp_up")
    return Value::RampUp;
  if (value == "ramp_down")
    return Value::RampDown;
  if (value == "triangle")
    return Value::Triangle;
  if (value == "round")
    return Value::Round;
  if (value == "smooth")
    return Value::Smooth;
  if (value == "custom_cubic")
    return Value::CustomCubic;
  if (value == "square")
    return Value::Square;
  throw std::invalid_argument("unsupported text selector shape");
}

videocut::text::TextAnimatedProperty
DecodeCurrentAnimatedProperty(const std::string_view value) {
  using Value = videocut::text::TextAnimatedProperty;
  if (value == "opacity")
    return Value::Opacity;
  if (value == "position_x")
    return Value::PositionX;
  if (value == "position_y")
    return Value::PositionY;
  if (value == "scale_x")
    return Value::ScaleX;
  if (value == "scale_y")
    return Value::ScaleY;
  if (value == "position_z")
    return Value::PositionZ;
  if (value == "rotation_x")
    return Value::RotationX;
  if (value == "rotation_y")
    return Value::RotationY;
  if (value == "rotation_z")
    return Value::RotationZ;
  if (value == "shear_x")
    return Value::ShearX;
  if (value == "shear_y")
    return Value::ShearY;
  if (value == "tracking")
    return Value::Tracking;
  if (value == "blur_radius")
    return Value::BlurRadius;
  if (value == "distance_from_center")
    return Value::DistanceFromCenter;
  throw std::invalid_argument("unsupported text animated property");
}

videocut::text::TextPropertyCombineMode
DecodeCurrentTextCombineMode(const std::string_view value) {
  using Value = videocut::text::TextPropertyCombineMode;
  if (value == "replace")
    return Value::Replace;
  if (value == "add")
    return Value::Add;
  if (value == "multiply")
    return Value::Multiply;
  if (value == "matrix_concat")
    return Value::MatrixConcat;
  if (value == "color_mix")
    return Value::ColorMix;
  throw std::invalid_argument("unsupported text property combine mode");
}

videocut::text::TextPropertyDisposition
DecodeCurrentTextPropertyDisposition(const std::string_view value,
                                     const std::string &context) {
  using Value = videocut::text::TextPropertyDisposition;
  if (value == "set")
    return Value::Set;
  if (value == "clear")
    return Value::Clear;
  if (value == "inherit")
    return Value::Inherit;
  throw std::invalid_argument(context + ".disposition is unsupported");
}

videocut::text::TextPropertyValue DecodeCurrentTimedTextPropertyValue(
    const nlohmann::json &value,
    const videocut::text::TextPropertyDescriptor &descriptor,
    const std::string &context) {
  using Kind = videocut::text::TextPropertyValueKind;
  switch (descriptor.valueKind) {
  case Kind::Scalar:
    if (!value.is_number())
      throw std::invalid_argument(context + " must be numeric");
    if (const auto decoded = value.get<double>(); std::isfinite(decoded))
      return decoded;
    throw std::invalid_argument(context + " must be finite");
  case Kind::Color:
    return DecodeCurrentTextColor(value, context);
  case Kind::Material:
    return DecodeCurrentTextMaterial(value, context);
  case Kind::Boolean:
  case Kind::Integer:
  case Kind::String:
  case Kind::Font:
  case Kind::FontAxes:
  case Kind::FontFeatures:
  case Kind::MaterialStack:
  case Kind::StrokeStack:
  case Kind::ShadowStack:
  case Kind::GlowStack:
  case Kind::InlineDecoration:
  case Kind::BoxBackground:
  case Kind::ParagraphStyle:
  case Kind::TabStops:
  case Kind::LayoutBox:
  case Kind::Insets:
  case Kind::WritingMode:
  case Kind::BackdropLayer:
  case Kind::BackdropStack:
  case Kind::Bend:
  case Kind::Path:
  case Kind::SdfMaterial:
  case Kind::Enumeration:
    break;
  }
  throw std::invalid_argument(
      context + " uses a value kind that is not range-animatable");
}

videocut::text::TextPropertyPatch
DecodeCurrentTimedTextPropertyPatch(const nlohmann::json &value,
                                    const std::string &context) {
  RequireOnlyCurrentJsonFields(value, {"id", "assignments"}, context);
  videocut::text::TextPropertyPatch patch;
  patch.patchId = RequireCurrentJsonString(value, "id", context);
  const auto &assignments =
      RequireCurrentJsonMember(value, "assignments", context);
  if (!assignments.is_array() || assignments.empty() ||
      assignments.size() > 4096U) {
    throw std::invalid_argument(context +
                                ".assignments exceeds the current budget");
  }

  constexpr std::string_view dynamicRunId{"timed.dynamic-range"};
  constexpr std::uint64_t dynamicRangeBegin = 0U;
  constexpr std::uint64_t dynamicRangeEnd = 1U;
  const auto rangeScopeBit = videocut::text::TextPropertyScopeBit(
      videocut::text::TextPropertyScope::Utf8Range);
  const auto layerScopeBit = videocut::text::TextPropertyScopeBit(
      videocut::text::TextPropertyScope::GlyphMaterialLayer);
  patch.assignments.reserve(assignments.size());
  for (std::size_t index = 0U; index < assignments.size(); ++index) {
    const auto assignmentContext =
        context + ".assignments[" + std::to_string(index) + "]";
    const auto &source = assignments[index];
    RequireOnlyCurrentJsonFields(
        source, {"property", "disposition", "combine", "layer_id", "value"},
        assignmentContext);
    const auto propertyName =
        RequireCurrentJsonString(source, "property", assignmentContext);
    const auto property = videocut::text::ParseTextPropertyId(propertyName);
    const auto *descriptor =
        property ? videocut::text::DescribeTextProperty(*property) : nullptr;
    if (!descriptor) {
      throw std::invalid_argument(assignmentContext +
                                  ".property is unsupported");
    }
    if (!descriptor->animatable ||
        (descriptor->legalScopeMask & (rangeScopeBit | layerScopeBit)) == 0U) {
      throw std::invalid_argument(
          assignmentContext +
          ".property is not an animatable timed-range property");
    }

    videocut::text::TextPropertyAssignment assignment;
    assignment.address.property = *property;
    assignment.disposition = DecodeCurrentTextPropertyDisposition(
        RequireCurrentJsonString(source, "disposition", assignmentContext),
        assignmentContext);
    assignment.combineMode = DecodeCurrentTextCombineMode(
        RequireCurrentJsonString(source, "combine", assignmentContext));
    if ((descriptor->legalCombineModeMask &
         videocut::text::TextPropertyCombineModeBit(assignment.combineMode)) ==
        0U) {
      throw std::invalid_argument(assignmentContext +
                                  ".combine is illegal for its property");
    }

    const auto layer = source.find("layer_id");
    if (layer != source.end()) {
      if (!layer->is_string() || layer->get_ref<const std::string &>().empty())
        throw std::invalid_argument(assignmentContext +
                                    ".layer_id must be non-empty");
      if ((descriptor->legalScopeMask & layerScopeBit) == 0U)
        throw std::invalid_argument(assignmentContext +
                                    ".layer_id is illegal for its property");
      assignment.address.target.scope =
          videocut::text::TextPropertyScope::GlyphMaterialLayer;
      assignment.address.target.layerId = layer->get<std::string>();
    } else if ((descriptor->legalScopeMask & rangeScopeBit) != 0U) {
      assignment.address.target.scope =
          videocut::text::TextPropertyScope::Utf8Range;
    } else {
      throw std::invalid_argument(assignmentContext +
                                  ".layer_id is required for its property");
    }
    assignment.address.target.runIds = {std::string(dynamicRunId)};
    assignment.address.target.range =
        videocut::text::TextUtf8Range{dynamicRangeBegin, dynamicRangeEnd};

    const auto authoredValue = source.find("value");
    if (assignment.disposition ==
        videocut::text::TextPropertyDisposition::Set) {
      if (authoredValue == source.end())
        throw std::invalid_argument(assignmentContext + ".value is required");
      assignment.value = DecodeCurrentTimedTextPropertyValue(
          *authoredValue, *descriptor, assignmentContext + ".value");
    } else {
      if (authoredValue != source.end())
        throw std::invalid_argument(
            assignmentContext +
            ".value is forbidden for clear or inherit disposition");
      if (assignment.combineMode !=
          videocut::text::TextPropertyCombineMode::Replace) {
        throw std::invalid_argument(
            assignmentContext +
            ".combine must be replace for clear or inherit disposition");
      }
    }
    patch.assignments.push_back(std::move(assignment));
  }

  const auto validation = videocut::text::ValidateTextPropertyPatch(patch);
  if (!validation.valid) {
    std::string message = context + " failed TextPropertyPatch validation";
    if (!validation.diagnostics.empty()) {
      message += ": " + validation.diagnostics.front().message;
    }
    throw std::invalid_argument(std::move(message));
  }
  return patch;
}

videocut::text::TextPropertyTarget
DecodeCurrentTextAnimationTarget(const nlohmann::json &value,
                                 const std::string &context) {
  RequireOnlyCurrentJsonFields(value,
                               {"scope", "content_slot_id", "paragraph_ids",
                                "run_ids", "range", "layer_id"},
                               context);
  videocut::text::TextPropertyTarget target;
  const auto scope = OptionalCurrentJsonString(value, "scope", "composition");
  using Scope = videocut::text::TextPropertyScope;
  if (scope == "composition")
    target.scope = Scope::Composition;
  else if (scope == "content_slot")
    target.scope = Scope::ContentSlot;
  else if (scope == "paragraph")
    target.scope = Scope::Paragraph;
  else if (scope == "run")
    target.scope = Scope::Run;
  else if (scope == "utf8_range")
    target.scope = Scope::Utf8Range;
  else if (scope == "glyph_material_layer")
    target.scope = Scope::GlyphMaterialLayer;
  else if (scope == "backdrop_layer")
    target.scope = Scope::BackdropLayer;
  else
    throw std::invalid_argument(context + ".scope is unsupported");
  target.contentSlotId = OptionalCurrentJsonString(value, "content_slot_id");
  target.paragraphIds = OptionalCurrentJsonStringArray(value, "paragraph_ids");
  target.runIds = OptionalCurrentJsonStringArray(value, "run_ids");
  target.layerId = OptionalCurrentJsonString(value, "layer_id");
  if (const auto range = value.find("range"); range != value.end()) {
    RequireOnlyCurrentJsonFields(*range, {"begin", "end"}, context + ".range");
    const auto begin = OptionalCurrentJsonInteger(*range, "begin", -1);
    const auto end = OptionalCurrentJsonInteger(*range, "end", -1);
    const auto maximumUtf8Bytes = static_cast<std::int64_t>(
        videocut::text::RichTextLimits{}.maximumUtf8Bytes);
    if (begin < 0 || end <= begin || end > maximumUtf8Bytes)
      throw std::invalid_argument(context + ".range is invalid");
    target.range = videocut::text::TextUtf8Range{
        static_cast<std::uint64_t>(begin), static_cast<std::uint64_t>(end)};
  }
  return target;
}

videocut::text::TextRenderGroupTimeRange
DecodeCurrentRenderGroupTimeRange(const nlohmann::json &value,
                                  const std::string &context) {
  RequireOnlyCurrentJsonFields(value, {"start_time_us", "end_time_us"},
                               context);
  return {OptionalCurrentJsonInteger(value, "start_time_us", 0),
          OptionalCurrentJsonInteger(value, "end_time_us", 0)};
}

videocut::text::TextRenderGroupSpec
DecodeCurrentTextRenderGroup(const nlohmann::json &value,
                             const std::string &context) {
  RequireOnlyCurrentJsonFields(value,
                               {"expand_ratio_x", "expand_ratio_y", "mode",
                                "offset", "duration", "priority", "random_seed",
                                "random_sort", "shape", "custom_ranges"},
                               context);
  videocut::text::TextRenderGroupSpec result;
  result.expandRatioX = static_cast<float>(
      OptionalCurrentJsonNumber(value, "expand_ratio_x", 1.0));
  result.expandRatioY = static_cast<float>(
      OptionalCurrentJsonNumber(value, "expand_ratio_y", 1.0));
  const auto mode = OptionalCurrentJsonString(value, "mode", "page");
  using Mode = videocut::text::TextRenderGroupMode;
  if (mode == "page")
    result.mode = Mode::Page;
  else if (mode == "per_letter")
    result.mode = Mode::PerLetter;
  else if (mode == "per_line")
    result.mode = Mode::PerLine;
  else if (mode == "per_word")
    result.mode = Mode::PerWord;
  else if (mode == "custom")
    result.mode = Mode::Custom;
  else
    throw std::invalid_argument(context + ".mode is unsupported");
  result.offset =
      static_cast<float>(OptionalCurrentJsonNumber(value, "offset", 0.0));
  if (const auto duration = value.find("duration"); duration != value.end())
    result.duration =
        DecodeCurrentRenderGroupTimeRange(*duration, context + ".duration");
  const auto priority = OptionalCurrentJsonInteger(value, "priority", 0);
  const auto randomSeed = OptionalCurrentJsonInteger(value, "random_seed", 0);
  if (priority < std::numeric_limits<std::int32_t>::min() ||
      priority > std::numeric_limits<std::int32_t>::max() || randomSeed < 0 ||
      static_cast<std::uint64_t>(randomSeed) >
          std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument(context + " integer is out of range");
  }
  result.priority = static_cast<std::int32_t>(priority);
  result.randomSeed = static_cast<std::uint32_t>(randomSeed);
  result.randomSort = OptionalCurrentJsonBool(value, "random_sort", false);
  result.shape = DecodeCurrentSelectorShape(
      OptionalCurrentJsonString(value, "shape", "square"));
  if (const auto ranges = value.find("custom_ranges"); ranges != value.end()) {
    if (!ranges->is_array())
      throw std::invalid_argument(context + ".custom_ranges must be an array");
    result.customRanges.reserve(ranges->size());
    for (std::size_t index = 0U; index < ranges->size(); ++index) {
      const auto &source = (*ranges)[index];
      const auto rangeContext =
          context + ".custom_ranges[" + std::to_string(index) + "]";
      RequireOnlyCurrentJsonFields(
          source, {"start_index", "end_index", "local_time", "intensity"},
          rangeContext);
      videocut::text::TextRenderGroupCustomRange range;
      range.startIndex = OptionalCurrentJsonInteger(source, "start_index", 0);
      range.endIndex = OptionalCurrentJsonInteger(source, "end_index", 0);
      range.intensity = static_cast<float>(
          OptionalCurrentJsonNumber(source, "intensity", 1.0));
      if (const auto localTime = source.find("local_time");
          localTime != source.end()) {
        range.localTime = DecodeCurrentRenderGroupTimeRange(
            *localTime, rangeContext + ".local_time");
      }
      result.customRanges.push_back(std::move(range));
    }
  }
  return result;
}

videocut::text::TextLayerAnimationClip
DecodeCurrentTextLayerTrack(const nlohmann::json &value,
                            const std::string &context) {
  RequireOnlyCurrentJsonFields(
      value, {"clip_id", "phase", "duration_us", "tracks"}, context);
  videocut::text::TextLayerAnimationClip result;
  result.clipId = RequireCurrentJsonString(value, "clip_id", context);
  const auto phase = RequireCurrentJsonString(value, "phase", context);
  using Phase = videocut::text::TextLayerAnimationPhase;
  if (phase == "enter")
    result.phase = Phase::Enter;
  else if (phase == "loop")
    result.phase = Phase::Loop;
  else if (phase == "exit")
    result.phase = Phase::Exit;
  else if (phase == "caption")
    result.phase = Phase::Caption;
  else
    throw std::invalid_argument(context + ".phase is unsupported");
  result.durationUs = OptionalCurrentJsonInteger(value, "duration_us", 0);
  const auto &tracks = RequireCurrentJsonMember(value, "tracks", context);
  if (result.durationUs <= 0 || !tracks.is_array())
    throw std::invalid_argument(context + " has invalid duration or tracks");
  for (std::size_t index = 0U; index < tracks.size(); ++index) {
    const auto &trackSource = tracks[index];
    const auto trackContext =
        context + ".tracks[" + std::to_string(index) + "]";
    RequireOnlyCurrentJsonFields(trackSource, {"property", "keyframes"},
                                 trackContext);
    videocut::text::TextLayerAnimationTrack track;
    const auto property =
        RequireCurrentJsonString(trackSource, "property", trackContext);
    using Property = videocut::text::TextLayerAnimationProperty;
    if (property == "opacity")
      track.property = Property::Opacity;
    else if (property == "position_x")
      track.property = Property::PositionX;
    else if (property == "position_y")
      track.property = Property::PositionY;
    else if (property == "scale_x")
      track.property = Property::ScaleX;
    else if (property == "scale_y")
      track.property = Property::ScaleY;
    else if (property == "rotation_degrees")
      track.property = Property::RotationDegrees;
    else
      throw std::invalid_argument(trackContext + ".property is unsupported");
    const auto &keyframes =
        RequireCurrentJsonMember(trackSource, "keyframes", trackContext);
    if (!keyframes.is_array() || keyframes.empty())
      throw std::invalid_argument(trackContext + ".keyframes is invalid");
    for (std::size_t keyIndex = 0U; keyIndex < keyframes.size(); ++keyIndex) {
      const auto &keySource = keyframes[keyIndex];
      const auto keyContext =
          trackContext + ".keyframes[" + std::to_string(keyIndex) + "]";
      RequireOnlyCurrentJsonFields(keySource, {"offset", "value", "easing"},
                                   keyContext);
      videocut::text::TextLayerAnimationKeyframe keyframe;
      keyframe.offset = static_cast<float>(
          OptionalCurrentJsonNumber(keySource, "offset", 0.0));
      keyframe.value = static_cast<float>(
          OptionalCurrentJsonNumber(keySource, "value", 0.0));
      const auto easing =
          OptionalCurrentJsonString(keySource, "easing", "linear");
      using Easing = videocut::text::TextAnimationEasing;
      if (easing == "linear")
        keyframe.easing = Easing::Linear;
      else if (easing == "ease_in")
        keyframe.easing = Easing::EaseIn;
      else if (easing == "ease_out")
        keyframe.easing = Easing::EaseOut;
      else if (easing == "ease_in_out")
        keyframe.easing = Easing::EaseInOut;
      else
        throw std::invalid_argument(keyContext + ".easing is unsupported");
      track.keyframes.push_back(keyframe);
    }
    result.tracks.push_back(std::move(track));
  }
  return result;
}

videocut::text::TextAnimationTimeDriver
DecodeCurrentTextTimeDriver(const nlohmann::json &value,
                            const std::string &context) {
  RequireOnlyCurrentJsonFields(value,
                               {"kind", "start_offset_us", "duration_us",
                                "playback", "timed_mode", "span_ids",
                                "active_patch"},
                               context);
  videocut::text::TextAnimationTimeDriver result;
  const auto kind = OptionalCurrentJsonString(value, "kind", "clip_local");
  using Kind = videocut::text::TextAnimationTimeDriverKind;
  if (kind == "enter_phase")
    result.kind = Kind::EnterPhase;
  else if (kind == "exit_phase")
    result.kind = Kind::ExitPhase;
  else if (kind == "loop_phase")
    result.kind = Kind::LoopPhase;
  else if (kind == "caption_phase")
    result.kind = Kind::CaptionPhase;
  else if (kind == "clip_local")
    result.kind = Kind::ClipLocal;
  else if (kind == "timed_ranges")
    result.kind = Kind::TimedRanges;
  else
    throw std::invalid_argument(context + ".kind is unsupported");
  result.startOffsetUs =
      OptionalCurrentJsonInteger(value, "start_offset_us", 0);
  result.durationUs =
      OptionalCurrentJsonInteger(value, "duration_us", 1'000'000);
  result.playback = DecodeCurrentAnimationPlayback(
      OptionalCurrentJsonString(value, "playback", "once"));
  if (result.kind != Kind::TimedRanges) {
    if (value.contains("timed_mode") || value.contains("span_ids") ||
        value.contains("active_patch")) {
      throw std::invalid_argument(
          context + " contains timed-range fields for a non-timed driver");
    }
    return result;
  }

  videocut::text::TextTimedRangeDriverSpec timed;
  const auto mode =
      OptionalCurrentJsonString(value, "timed_mode", "span_progress");
  using TimedMode = videocut::text::TextTimedDriverMode;
  if (mode == "span_step")
    timed.mode = TimedMode::SpanStep;
  else if (mode == "span_progress")
    timed.mode = TimedMode::SpanProgress;
  else if (mode == "grapheme_sweep")
    timed.mode = TimedMode::GraphemeSweep;
  else if (mode == "active_hold")
    timed.mode = TimedMode::ActiveHold;
  else
    throw std::invalid_argument(context + ".timed_mode is unsupported");
  timed.spanIds = OptionalCurrentJsonStringArray(value, "span_ids");
  if (const auto authored = value.find("active_patch");
      authored != value.end()) {
    timed.activePatch = DecodeCurrentTimedTextPropertyPatch(
        *authored, context + ".active_patch");
  }
  result.timedRanges = std::move(timed);
  return result;
}

videocut::text::TextAnimatorSpec
DecodeCurrentTextAnimator(const nlohmann::json &value,
                          const std::string &context) {
  RequireOnlyCurrentJsonFields(
      value,
      {"id", "paragraph_ids", "run_ids", "position_mode", "anchor",
       "anchor_basis", "anchor_mode", "anchor_offset_x", "anchor_offset_y",
       "presentation", "fade_fraction", "effect_program_id", "active_rgba",
       "projection", "selectors", "tracks", "fill_color_keyframes"},
      context);
  videocut::text::TextAnimatorSpec result;
  result.animatorId = RequireCurrentJsonString(value, "id", context);
  result.paragraphIds = OptionalCurrentJsonStringArray(value, "paragraph_ids");
  result.runIds = OptionalCurrentJsonStringArray(value, "run_ids");
  const auto positionMode =
      OptionalCurrentJsonString(value, "position_mode", "absolute_pixels");
  using Position = videocut::text::TextPositionMode;
  if (positionMode == "absolute_pixels")
    result.positionMode = Position::AbsolutePixels;
  else if (positionMode == "text_bounds_offset")
    result.positionMode = Position::TextBoundsOffset;
  else if (positionMode == "distance_from_center")
    result.positionMode = Position::DistanceFromCenter;
  else if (positionMode == "space_x")
    result.positionMode = Position::SpaceX;
  else if (positionMode == "space_y")
    result.positionMode = Position::SpaceY;
  else
    throw std::invalid_argument(context + ".position_mode is unsupported");
  const auto anchor =
      OptionalCurrentJsonString(value, "anchor", "glyph_center");
  using Anchor = videocut::text::TextUnitAnchor;
  if (anchor == "glyph_center")
    result.anchor = Anchor::GlyphCenter;
  else if (anchor == "baseline")
    result.anchor = Anchor::Baseline;
  else if (anchor == "line_box")
    result.anchor = Anchor::LineBox;
  else
    throw std::invalid_argument(context + ".anchor is unsupported");
  const auto anchorBasis =
      OptionalCurrentJsonString(value, "anchor_basis", "letter");
  using Basis = videocut::text::TextAnimatorAnchorBasis;
  if (anchorBasis == "letter")
    result.anchorBasis = Basis::Letter;
  else if (anchorBasis == "line")
    result.anchorBasis = Basis::Line;
  else if (anchorBasis == "word")
    result.anchorBasis = Basis::Word;
  else if (anchorBasis == "page")
    result.anchorBasis = Basis::Page;
  else
    throw std::invalid_argument(context + ".anchor_basis is unsupported");
  const auto anchorMode =
      OptionalCurrentJsonString(value, "anchor_mode", "fixed");
  using AnchorMode = videocut::text::TextAnimatorAnchorMode;
  if (anchorMode == "fixed")
    result.anchorMode = AnchorMode::Fixed;
  else if (anchorMode == "selector_influenced")
    result.anchorMode = AnchorMode::SelectorInfluenced;
  else
    throw std::invalid_argument(context + ".anchor_mode is unsupported");
  result.anchorOffsetX = static_cast<float>(
      OptionalCurrentJsonNumber(value, "anchor_offset_x", 0.0));
  result.anchorOffsetY = static_cast<float>(
      OptionalCurrentJsonNumber(value, "anchor_offset_y", 0.0));
  const auto presentation =
      OptionalCurrentJsonString(value, "presentation", "transform");
  using Presentation = videocut::text::TextUnitPresentation;
  if (presentation == "transform")
    result.presentation = Presentation::Transform;
  else if (presentation == "reveal")
    result.presentation = Presentation::Reveal;
  else if (presentation == "active_fill")
    result.presentation = Presentation::ActiveFill;
  else
    throw std::invalid_argument(context + ".presentation is unsupported");
  result.fadeFraction = static_cast<float>(
      OptionalCurrentJsonNumber(value, "fade_fraction", 0.0));
  result.effectProgramId =
      OptionalCurrentJsonString(value, "effect_program_id");
  if (const auto color = value.find("active_rgba"); color != value.end())
    result.activeColor =
        DecodeCurrentTextColor(*color, context + ".active_rgba");
  if (const auto projection = value.find("projection");
      projection != value.end()) {
    RequireOnlyCurrentJsonFields(*projection,
                                 {"kind", "field_of_view_degrees",
                                  "vanishing_point_x", "vanishing_point_y"},
                                 context + ".projection");
    const auto kind =
        OptionalCurrentJsonString(*projection, "kind", "planar_2d");
    if (kind == "planar_2d")
      result.projection.kind = videocut::text::TextProjectionKind::Planar2D;
    else if (kind == "perspective_3d")
      result.projection.kind =
          videocut::text::TextProjectionKind::Perspective3D;
    else
      throw std::invalid_argument(context + ".projection.kind is unsupported");
    result.projection.fieldOfViewDegrees = static_cast<float>(
        OptionalCurrentJsonNumber(*projection, "field_of_view_degrees", 45.0));
    result.projection.vanishingPointX = static_cast<float>(
        OptionalCurrentJsonNumber(*projection, "vanishing_point_x", 0.5));
    result.projection.vanishingPointY = static_cast<float>(
        OptionalCurrentJsonNumber(*projection, "vanishing_point_y", 0.5));
  }

  const auto &selectors = RequireCurrentJsonMember(value, "selectors", context);
  if (!selectors.is_array())
    throw std::invalid_argument(context + ".selectors must be an array");
  for (std::size_t index = 0U; index < selectors.size(); ++index) {
    const auto &source = selectors[index];
    const auto selectorContext =
        context + ".selectors[" + std::to_string(index) + "]";
    RequireOnlyCurrentJsonFields(source,
                                 {"kind",
                                  "based_on",
                                  "range_start",
                                  "range_end",
                                  "offset",
                                  "stagger",
                                  "edge_smooth",
                                  "constrained",
                                  "shape",
                                  "order",
                                  "random_seed",
                                  "intensity",
                                  "intensity_start",
                                  "intensity_end",
                                  "time_start_1",
                                  "time_start_2",
                                  "time_end_1",
                                  "time_end_2",
                                  "time_cycle",
                                  "range_start_keyframes",
                                  "range_end_keyframes",
                                  "offset_keyframes",
                                  "intensity_keyframes"},
                                 selectorContext);
    videocut::text::TextUnitSelector selector;
    const auto kind = OptionalCurrentJsonString(source, "kind", "range");
    if (kind == "range")
      selector.kind = videocut::text::TextSelectorKind::Range;
    else if (kind == "time")
      selector.kind = videocut::text::TextSelectorKind::Time;
    else
      throw std::invalid_argument(selectorContext + ".kind is unsupported");
    const auto basis =
        OptionalCurrentJsonString(source, "based_on", "grapheme");
    using UnitBasis = videocut::text::TextUnitBasis;
    if (basis == "grapheme")
      selector.basedOn = UnitBasis::Grapheme;
    else if (basis == "word")
      selector.basedOn = UnitBasis::Word;
    else if (basis == "line")
      selector.basedOn = UnitBasis::Line;
    else if (basis == "all")
      selector.basedOn = UnitBasis::All;
    else
      throw std::invalid_argument(selectorContext + ".based_on is unsupported");
    selector.rangeStart = OptionalCurrentJsonNumber(source, "range_start", 0.0);
    selector.rangeEnd = OptionalCurrentJsonNumber(source, "range_end", 1.0);
    selector.offset = OptionalCurrentJsonNumber(source, "offset", 0.0);
    selector.stagger = OptionalCurrentJsonNumber(source, "stagger", 0.0);
    selector.edgeSmooth = OptionalCurrentJsonNumber(source, "edge_smooth", 0.0);
    selector.constrained = OptionalCurrentJsonBool(source, "constrained", true);
    selector.shape = DecodeCurrentSelectorShape(
        OptionalCurrentJsonString(source, "shape", "linear"));
    const auto order = OptionalCurrentJsonString(source, "order", "forward");
    using Order = videocut::text::TextUnitOrder;
    if (order == "forward")
      selector.order = Order::Forward;
    else if (order == "backward")
      selector.order = Order::Backward;
    else if (order == "center_out")
      selector.order = Order::CenterOut;
    else if (order == "random")
      selector.order = Order::Random;
    else
      throw std::invalid_argument(selectorContext + ".order is unsupported");
    selector.randomSeed = OptionalCurrentJsonNumber(source, "random_seed", 0.0);
    selector.intensity = OptionalCurrentJsonNumber(source, "intensity", 1.0);
    selector.intensityStart =
        OptionalCurrentJsonNumber(source, "intensity_start", 0.0);
    selector.intensityEnd =
        OptionalCurrentJsonNumber(source, "intensity_end", 1.0);
    selector.timeStart1 =
        OptionalCurrentJsonNumber(source, "time_start_1", 0.0);
    selector.timeStart2 =
        OptionalCurrentJsonNumber(source, "time_start_2", 0.0);
    selector.timeEnd1 = OptionalCurrentJsonNumber(source, "time_end_1", 1.0);
    selector.timeEnd2 = OptionalCurrentJsonNumber(source, "time_end_2", 1.0);
    selector.timeCycle = OptionalCurrentJsonBool(source, "time_cycle", false);
    const auto decodeSelectorFrames = [&](const char *field, auto &output) {
      const auto frames = source.find(field);
      if (frames == source.end())
        return;
      if (!frames->is_array())
        throw std::invalid_argument(selectorContext + "." + field +
                                    " must be an array");
      output.reserve(frames->size());
      for (std::size_t keyIndex = 0U; keyIndex < frames->size(); ++keyIndex) {
        output.push_back(DecodeCurrentTextScalarKeyframe<
                         videocut::text::TextSelectorKeyframe>(
            (*frames)[keyIndex], selectorContext + "." + field + "[" +
                                     std::to_string(keyIndex) + "]"));
      }
    };
    decodeSelectorFrames("range_start_keyframes", selector.rangeStartKeyframes);
    decodeSelectorFrames("range_end_keyframes", selector.rangeEndKeyframes);
    decodeSelectorFrames("offset_keyframes", selector.offsetKeyframes);
    decodeSelectorFrames("intensity_keyframes", selector.intensityKeyframes);
    result.selectors.push_back(std::move(selector));
  }

  const auto &tracks = RequireCurrentJsonMember(value, "tracks", context);
  if (!tracks.is_array())
    throw std::invalid_argument(context + ".tracks must be an array");
  for (std::size_t index = 0U; index < tracks.size(); ++index) {
    const auto &source = tracks[index];
    const auto trackContext =
        context + ".tracks[" + std::to_string(index) + "]";
    RequireOnlyCurrentJsonFields(source, {"property", "keyframes"},
                                 trackContext);
    videocut::text::TextAnimatorTrack track;
    track.property = DecodeCurrentAnimatedProperty(
        RequireCurrentJsonString(source, "property", trackContext));
    const auto &keyframes =
        RequireCurrentJsonMember(source, "keyframes", trackContext);
    if (!keyframes.is_array())
      throw std::invalid_argument(trackContext + ".keyframes is invalid");
    for (std::size_t keyIndex = 0U; keyIndex < keyframes.size(); ++keyIndex) {
      track.keyframes.push_back(
          DecodeCurrentTextScalarKeyframe<videocut::text::TextKeyframe>(
              keyframes[keyIndex],
              trackContext + ".keyframes[" + std::to_string(keyIndex) + "]"));
    }
    result.tracks.push_back(std::move(track));
  }
  if (const auto keyframes = value.find("fill_color_keyframes");
      keyframes != value.end()) {
    if (!keyframes->is_array())
      throw std::invalid_argument(context +
                                  ".fill_color_keyframes must be an array");
    for (std::size_t index = 0U; index < keyframes->size(); ++index) {
      const auto &source = (*keyframes)[index];
      const auto keyContext =
          context + ".fill_color_keyframes[" + std::to_string(index) + "]";
      RequireOnlyCurrentJsonFields(source,
                                   {"offset", "rgba", "tangent_in",
                                    "tangent_out", "bezier", "bezier_time_in",
                                    "bezier_time_out"},
                                   keyContext);
      videocut::text::TextColorKeyframe keyframe;
      keyframe.offset =
          static_cast<float>(OptionalCurrentJsonNumber(source, "offset", 0.0));
      keyframe.value = DecodeCurrentTextColor(
          RequireCurrentJsonMember(source, "rgba", keyContext),
          keyContext + ".rgba");
      if (const auto tangent = source.find("tangent_in");
          tangent != source.end())
        keyframe.tangentIn =
            DecodeCurrentTextColor(*tangent, keyContext + ".tangent_in");
      if (const auto tangent = source.find("tangent_out");
          tangent != source.end())
        keyframe.tangentOut =
            DecodeCurrentTextColor(*tangent, keyContext + ".tangent_out");
      keyframe.cubicBezier = OptionalCurrentJsonBool(source, "bezier", false);
      keyframe.bezierTimeIn = static_cast<float>(
          OptionalCurrentJsonNumber(source, "bezier_time_in", 0.0));
      keyframe.bezierTimeOut = static_cast<float>(
          OptionalCurrentJsonNumber(source, "bezier_time_out", 0.0));
      result.fillColorKeyframes.push_back(std::move(keyframe));
    }
  }
  return result;
}

videocut::text::TextPostEffectSpec
DecodeCurrentTextPostEffect(const nlohmann::json &value,
                            const std::string &context) {
  RequireOnlyCurrentJsonFields(value,
                               {"id", "node_kind", "amount", "padding_px",
                                "amount_keyframes", "input_ids", "parameters"},
                               context);
  videocut::text::TextPostEffectSpec result;
  result.effectId = RequireCurrentJsonString(value, "id", context);
  const auto nodeKind = RequireCurrentJsonString(value, "node_kind", context);
  const auto kind = videocut::text::ParseTextPostEffectKind(nodeKind);
  if (!kind)
    throw std::invalid_argument(context + ".node_kind is unsupported");
  result.kind = *kind;
  if (const auto inputs = value.find("input_ids"); inputs != value.end()) {
    if (!inputs->is_array())
      throw std::invalid_argument(context + ".input_ids must be an array");
    result.inputIds.reserve(inputs->size());
    for (std::size_t index = 0U; index < inputs->size(); ++index) {
      const auto &input = (*inputs)[index];
      if (!input.is_string() || input.get_ref<const std::string &>().empty())
        throw std::invalid_argument(
            context + ".input_ids must contain non-empty strings");
      result.inputIds.push_back(input.get<std::string>());
    }
  }
  result.amount =
      static_cast<float>(OptionalCurrentJsonNumber(value, "amount", 0.0));
  result.paddingPx =
      static_cast<float>(OptionalCurrentJsonNumber(value, "padding_px", 0.0));
  DecodeCurrentTextKeyframeArray(value, "amount_keyframes", context,
                                 result.amountKeyframes);
  if (const auto parameters = value.find("parameters");
      parameters != value.end()) {
    if (!parameters->is_array())
      throw std::invalid_argument(context + ".parameters must be an array");
    result.parameters.reserve(parameters->size());
    for (std::size_t index = 0U; index < parameters->size(); ++index) {
      const auto &source = (*parameters)[index];
      const auto parameterContext =
          context + ".parameters[" + std::to_string(index) + "]";
      RequireOnlyCurrentJsonFields(source, {"name", "values", "keyframes"},
                                   parameterContext);
      videocut::text::TextPostEffectParameter parameter;
      parameter.name =
          RequireCurrentJsonString(source, "name", parameterContext);
      const auto &values =
          RequireCurrentJsonMember(source, "values", parameterContext);
      if (!values.is_array())
        throw std::invalid_argument(parameterContext + ".values is invalid");
      for (const auto &raw : values) {
        if (!raw.is_number() || !std::isfinite(raw.get<double>()))
          throw std::invalid_argument(parameterContext +
                                      ".values must be finite numbers");
        parameter.values.push_back(raw.get<float>());
      }
      DecodeCurrentTextKeyframeArray(source, "keyframes", parameterContext,
                                     parameter.keyframes);
      result.parameters.push_back(std::move(parameter));
    }
  }
  return result;
}

videocut::text::TextAnimatedDecorationAnchor
DecodeCurrentAnimatedDecorationAnchor(const std::string_view value) {
  using Value = videocut::text::TextAnimatedDecorationAnchor;
  if (value == "bbox_center")
    return Value::TextBoundsCenter;
  if (value == "canvas_center")
    return Value::CanvasCenter;
  throw std::invalid_argument("unsupported text decoration animation anchor");
}

videocut::text::TextAnimatedDecorationFit
DecodeCurrentAnimatedDecorationFit(const std::string_view value) {
  using Value = videocut::text::TextAnimatedDecorationFit;
  if (value == "inherit")
    return Value::Inherit;
  if (value == "contain")
    return Value::Contain;
  if (value == "cover")
    return Value::Cover;
  if (value == "stretch")
    return Value::Stretch;
  if (value == "native")
    return Value::Native;
  if (value == "fit_width")
    return Value::FitWidth;
  if (value == "fit_height")
    return Value::FitHeight;
  if (value == "fit_long_side")
    return Value::FitLongSide;
  if (value == "fit_short_side")
    return Value::FitShortSide;
  throw std::invalid_argument("unsupported text decoration animation fit");
}

videocut::text::TextEffectExecutionNodeKind
DecodeCurrentTextExecutionNodeKind(const std::string_view value,
                                   const std::string &context) {
  using Kind = videocut::text::TextEffectExecutionNodeKind;
  if (value == "scene")
    return Kind::Scene;
  if (value == "material_pass" || value == "material-pass")
    return Kind::MaterialPass;
  if (value == "post_effect_pass" || value == "post-effect-pass")
    return Kind::PostEffectPass;
  if (value == "render_target" || value == "render-target")
    return Kind::RenderTarget;
  if (value == "history")
    return Kind::History;
  if (value == "media_input" || value == "media-input")
    return Kind::MediaInput;
  if (value == "state")
    return Kind::State;
  if (value == "composite")
    return Kind::Composite;
  if (value == "layout")
    return Kind::Layout;
  if (value == "selector")
    return Kind::Selector;
  if (value == "operator")
    return Kind::Operator;
  throw std::invalid_argument(context + ".node_kind is unsupported");
}

videocut::text::TextEffectExecutionCapability
DecodeCurrentTextExecutionCapability(const std::string_view value,
                                     const std::string &context) {
  using Capability = videocut::text::TextEffectExecutionCapability;
  static const std::map<std::string_view, Capability> values{
      {"none", Capability::None},
      {"layout_glyph_run", Capability::LayoutGlyphRun},
      {"layout:glyph-run", Capability::LayoutGlyphRun},
      {"layout_caption_module", Capability::LayoutCaptionModule},
      {"layout:caption-module", Capability::LayoutCaptionModule},
      {"layout_paged_text", Capability::LayoutPagedText},
      {"layout:paged-text", Capability::LayoutPagedText},
      {"layout_timed_lyric", Capability::LayoutTimedLyric},
      {"layout:timed-lyric", Capability::LayoutTimedLyric},
      {"selector_base", Capability::SelectorBase},
      {"selector:base", Capability::SelectorBase},
      {"selector_time", Capability::SelectorTime},
      {"selector:time", Capability::SelectorTime},
      {"operator_program", Capability::OperatorProgram},
      {"operator:program", Capability::OperatorProgram},
      {"operator_keyframe", Capability::OperatorKeyframe},
      {"operator:keyframe", Capability::OperatorKeyframe},
      {"operator_stagger", Capability::OperatorStagger},
      {"operator:stagger", Capability::OperatorStagger},
      {"operator_wipe", Capability::OperatorWipe},
      {"operator:wipe", Capability::OperatorWipe},
      {"operator_typewriter", Capability::OperatorTypewriter},
      {"operator:typewriter", Capability::OperatorTypewriter},
      {"operator_reveal", Capability::OperatorReveal},
      {"operator:reveal", Capability::OperatorReveal},
      {"operator_clone", Capability::OperatorClone},
      {"operator:clone", Capability::OperatorClone},
      {"operator_transform", Capability::OperatorTransform},
      {"operator:transform", Capability::OperatorTransform},
      {"operator_material", Capability::OperatorMaterial},
      {"operator:material", Capability::OperatorMaterial},
      {"operator_scene_lookup", Capability::OperatorSceneLookup},
      {"operator:scene-lookup", Capability::OperatorSceneLookup},
      {"operator_resource", Capability::OperatorResource},
      {"operator:resource", Capability::OperatorResource},
      {"operator_command", Capability::OperatorCommand},
      {"operator:command", Capability::OperatorCommand},
      {"operator_event", Capability::OperatorEvent},
      {"operator:event", Capability::OperatorEvent},
      {"operator_glyph_substitution", Capability::OperatorGlyphSubstitution},
      {"operator:glyph-substitution", Capability::OperatorGlyphSubstitution},
      {"operator_lifecycle", Capability::OperatorLifecycle},
      {"operator:lifecycle", Capability::OperatorLifecycle},
      {"scene_prefab", Capability::ScenePrefab},
      {"scene:prefab", Capability::ScenePrefab},
      {"scene_entity", Capability::SceneEntity},
      {"scene:entity", Capability::SceneEntity},
      {"scene_mesh", Capability::SceneMesh},
      {"scene:mesh", Capability::SceneMesh},
      {"scene_camera", Capability::SceneCamera},
      {"scene:camera", Capability::SceneCamera},
      {"scene_projection", Capability::SceneProjection},
      {"scene:projection", Capability::SceneProjection},
      {"scene_clone", Capability::SceneClone},
      {"scene:clone", Capability::SceneClone},
      {"material_color_pass", Capability::MaterialColorPass},
      {"material:color-pass", Capability::MaterialColorPass},
      {"material_depth_pass", Capability::MaterialDepthPass},
      {"material:depth-pass", Capability::MaterialDepthPass},
      {"material_program_pass", Capability::MaterialProgramPass},
      {"material:program-pass", Capability::MaterialProgramPass},
      {"render_target_offscreen", Capability::RenderTargetOffscreen},
      {"render-target:offscreen", Capability::RenderTargetOffscreen},
      {"render_target_expanded", Capability::RenderTargetExpanded},
      {"render-target:expanded", Capability::RenderTargetExpanded},
      {"history_feedback", Capability::HistoryFeedback},
      {"history:feedback", Capability::HistoryFeedback},
      {"media_font", Capability::MediaFont},
      {"media:font", Capability::MediaFont},
      {"media_image", Capability::MediaImage},
      {"media:image", Capability::MediaImage},
      {"media_image_sequence", Capability::MediaImageSequence},
      {"media:image-sequence", Capability::MediaImageSequence},
      {"media_video", Capability::MediaVideo},
      {"media:video", Capability::MediaVideo},
      {"media_mesh", Capability::MediaMesh},
      {"media:mesh", Capability::MediaMesh},
      {"media_particle", Capability::MediaParticle},
      {"media:particle", Capability::MediaParticle},
      {"state_deterministic_random", Capability::StateDeterministicRandom},
      {"state:deterministic-random", Capability::StateDeterministicRandom},
      {"state_physics", Capability::StatePhysics},
      {"state:physics", Capability::StatePhysics},
      {"state_collision", Capability::StateCollision},
      {"state:collision", Capability::StateCollision},
      {"state_particle", Capability::StateParticle},
      {"state:particle", Capability::StateParticle},
      {"composite_source_over", Capability::CompositeSourceOver},
      {"composite:source-over", Capability::CompositeSourceOver},
      {"material_alpha_modulate", Capability::MaterialAlphaModulate},
      {"material:alpha-modulate", Capability::MaterialAlphaModulate},
      {"material_color_modulate", Capability::MaterialColorModulate},
      {"material:color-modulate", Capability::MaterialColorModulate},
      {"material_threshold_reveal_blur",
       Capability::MaterialThresholdRevealBlur},
      {"material:threshold-reveal-blur",
       Capability::MaterialThresholdRevealBlur},
      {"material_threshold_transition_blur",
       Capability::MaterialThresholdTransitionBlur},
      {"material:threshold-transition-blur",
       Capability::MaterialThresholdTransitionBlur},
      {"material_glyph_uv_ballistic_echo_composite",
       Capability::MaterialGlyphUvBallisticEchoComposite},
      {"material:glyph-uv-ballistic-echo-composite",
       Capability::MaterialGlyphUvBallisticEchoComposite},
      {"material_directional_box_blur",
       Capability::MaterialDirectionalBoxBlur},
      {"material:directional-box-blur",
       Capability::MaterialDirectionalBoxBlur},
      {"material_separable_gaussian_blur",
       Capability::MaterialSeparableGaussianBlur},
      {"material:separable-gaussian-blur",
       Capability::MaterialSeparableGaussianBlur},
      {"material_weighted_axis_box_blur",
       Capability::MaterialWeightedAxisBoxBlur},
      {"material:weighted-axis-box-blur",
       Capability::MaterialWeightedAxisBoxBlur},
      {"material_turbulence_displacement",
       Capability::MaterialTurbulenceDisplacement},
      {"material:turbulence-displacement",
       Capability::MaterialTurbulenceDisplacement},
      {"material_deep_glow_composite",
       Capability::MaterialDeepGlowComposite},
      {"material:deep-glow-composite",
       Capability::MaterialDeepGlowComposite},
      {"material_sdf_alpha_outline", Capability::MaterialSdfAlphaOutline},
      {"material:sdf-alpha-outline", Capability::MaterialSdfAlphaOutline},
      {"material_sdf_light_sweep", Capability::MaterialSdfLightSweep},
      {"material:sdf-light-sweep", Capability::MaterialSdfLightSweep},
      {"material_sdf_cut_glow", Capability::MaterialSdfCutGlow},
      {"material:sdf-cut-glow", Capability::MaterialSdfCutGlow},
      {"material_shaped_ramp_mask", Capability::MaterialShapedRampMask},
      {"material:shaped-ramp-mask", Capability::MaterialShapedRampMask},
      {"material_masked_dual_offset_crossfade",
       Capability::MaterialMaskedDualOffsetCrossfade},
      {"material:masked-dual-offset-crossfade",
       Capability::MaterialMaskedDualOffsetCrossfade},
      {"material_line_column_noise_trail",
       Capability::MaterialLineColumnNoiseTrail},
      {"material:line-column-noise-trail",
       Capability::MaterialLineColumnNoiseTrail},
      {"material_particle_scatter_composite",
       Capability::MaterialParticleScatterComposite},
      {"material:particle-scatter-composite",
       Capability::MaterialParticleScatterComposite},
      {"material_sdf_wave_energy", Capability::MaterialSdfWaveEnergy},
      {"material:sdf-wave-energy", Capability::MaterialSdfWaveEnergy},
      {"material_mask_motion_noise_glow",
       Capability::MaterialMaskMotionNoiseGlow},
      {"material:mask-motion-noise-glow",
       Capability::MaterialMaskMotionNoiseGlow},
      {"material_face_noise_shadow", Capability::MaterialFaceNoiseShadow},
      {"material:face-noise-shadow", Capability::MaterialFaceNoiseShadow},
      {"material_projection_mesh_composite",
       Capability::MaterialProjectionMeshComposite},
      {"material:projection-mesh-composite",
       Capability::MaterialProjectionMeshComposite},
      {"material_vat_rbd_mesh", Capability::MaterialVatRbdMesh},
      {"material:vat-rbd-mesh", Capability::MaterialVatRbdMesh},
      {"material_multimesh_text_composite",
       Capability::MaterialMultimeshTextComposite},
      {"material:multimesh-text-composite",
       Capability::MaterialMultimeshTextComposite},
      {"material_radial_media_composite",
       Capability::MaterialRadialMediaComposite},
      {"material:radial-media-composite",
       Capability::MaterialRadialMediaComposite},
      {"material_spiral_sdf_warp", Capability::MaterialSpiralSdfWarp},
      {"material:spiral-sdf-warp", Capability::MaterialSpiralSdfWarp},
      {"material_flux_turbulent_blend",
       Capability::MaterialFluxTurbulentBlend},
      {"material:flux-turbulent-blend",
       Capability::MaterialFluxTurbulentBlend},
      {"material_cube_projection_composite",
       Capability::MaterialCubeProjectionComposite},
      {"material:cube-projection-composite",
       Capability::MaterialCubeProjectionComposite},
      {"material_cylinder_projection_composite",
       Capability::MaterialCylinderProjectionComposite},
      {"material:cylinder-projection-composite",
       Capability::MaterialCylinderProjectionComposite},
      {"material_masked_cut_line", Capability::MaterialMaskedCutLine},
      {"material:masked-cut-line", Capability::MaterialMaskedCutLine},
      {"material_cut_line_reveal", Capability::MaterialCutLineReveal},
      {"material:cut-line-reveal", Capability::MaterialCutLineReveal},
      {"material_packed_multiscale_gaussian_blur",
       Capability::MaterialPackedMultiscaleGaussianBlur},
      {"material:packed-multiscale-gaussian-blur",
       Capability::MaterialPackedMultiscaleGaussianBlur},
      {"material_dual_packed_glow_composite",
       Capability::MaterialDualPackedGlowComposite},
      {"material:dual-packed-glow-composite",
       Capability::MaterialDualPackedGlowComposite},
      {"material_encoded_alpha_distance_blur",
       Capability::MaterialEncodedAlphaDistanceBlur},
      {"material:encoded-alpha-distance-blur",
       Capability::MaterialEncodedAlphaDistanceBlur},
      {"material_sdf_normal_light_sweep",
       Capability::MaterialSdfNormalLightSweep},
      {"material:sdf-normal-light-sweep",
       Capability::MaterialSdfNormalLightSweep},
      {"material_light_sweep_packed_glow_composite",
       Capability::MaterialLightSweepPackedGlowComposite},
      {"material:light-sweep-packed-glow-composite",
       Capability::MaterialLightSweepPackedGlowComposite},
      {"material_radial_decay_hsv_glow",
       Capability::MaterialRadialDecayHsvGlow},
      {"material:radial-decay-hsv-glow",
       Capability::MaterialRadialDecayHsvGlow},
      {"material_fixed_nine_tap_axis_blur",
       Capability::MaterialFixedNineTapAxisBlur},
      {"material:fixed-nine-tap-axis-blur",
       Capability::MaterialFixedNineTapAxisBlur},
      {"material_hsv_original_over_blur",
       Capability::MaterialHsvOriginalOverBlur},
      {"material:hsv-original-over-blur",
       Capability::MaterialHsvOriginalOverBlur},
      {"material_noise_threshold_dissolve",
       Capability::MaterialNoiseThresholdDissolve},
      {"material:noise-threshold-dissolve",
       Capability::MaterialNoiseThresholdDissolve},
  };
  const auto found = values.find(value);
  if (found == values.end())
    throw std::invalid_argument(context + ".capability is unsupported");
  return found->second;
}

videocut::text::TextEffectPhysicsSpec DecodeCurrentTextPhysicsSpec(
    const nlohmann::json &value, const std::string &context) {
  RequireOnlyCurrentJsonFields(
      value,
      {"fixed_step_hz", "gravity_x", "gravity_y", "angular_damping",
       "explosion_delay_us", "explosion_force_duration_us",
       "explosion_ramp_up_us", "explosion_acceleration_min",
       "explosion_acceleration_max", "explosion_origin_y_shift_factor",
       "explosion_angle_range_degrees", "initial_angular_velocity_scale",
       "random_seed", "random_phase", "random_scale", "angle_seed_stride",
       "speed_seed_stride", "rotation_seed_stride"},
      context);
  using Spec = videocut::text::TextEffectPhysicsSpec;
  Spec result;
  if (!DecodeTextPackageUint32(
          RequireCurrentJsonMember(value, "fixed_step_hz", context),
          result.fixedStepHz) ||
      !DecodeTextPackageUint64(
          RequireCurrentJsonMember(value, "random_seed", context),
          result.randomSeed) ||
      !DecodeTextPackageUint32(
          RequireCurrentJsonMember(value, "angle_seed_stride", context),
          result.angleSeedStride) ||
      !DecodeTextPackageUint32(
          RequireCurrentJsonMember(value, "speed_seed_stride", context),
          result.speedSeedStride) ||
      !DecodeTextPackageUint32(
          RequireCurrentJsonMember(value, "rotation_seed_stride", context),
          result.rotationSeedStride)) {
    throw std::invalid_argument(context + " contains an invalid integer");
  }
  const auto number = [&](const char *field) {
    RequireCurrentJsonMember(value, field, context);
    return static_cast<float>(OptionalCurrentJsonNumber(value, field, 0.0));
  };
  const auto integer = [&](const char *field) {
    RequireCurrentJsonMember(value, field, context);
    return OptionalCurrentJsonInteger(value, field, 0);
  };
  result.gravityX = number("gravity_x");
  result.gravityY = number("gravity_y");
  result.angularDamping = number("angular_damping");
  result.explosionDelayUs = integer("explosion_delay_us");
  result.explosionForceDurationUs = integer("explosion_force_duration_us");
  result.explosionRampUpUs = integer("explosion_ramp_up_us");
  result.explosionAccelerationMin = number("explosion_acceleration_min");
  result.explosionAccelerationMax = number("explosion_acceleration_max");
  result.explosionOriginYShiftFactor =
      number("explosion_origin_y_shift_factor");
  result.explosionAngleRangeDegrees = number("explosion_angle_range_degrees");
  result.initialAngularVelocityScale =
      number("initial_angular_velocity_scale");
  result.randomPhase = number("random_phase");
  result.randomScale = number("random_scale");
  return result;
}

videocut::text::TextEffectCollisionSpec DecodeCurrentTextCollisionSpec(
    const nlohmann::json &value, const std::string &context) {
  RequireOnlyCurrentJsonFields(
      value,
      {"fixed_box_width", "fixed_box_height", "box_center_x_scale",
       "wall_damping", "side_wall_velocity_scale", "top_wall_velocity_scale",
       "bottom_wall_damping", "wall_torque_scale",
       "collision_boundary_scale", "collision_restitution",
       "collision_torque_scale", "minimum_cell_size", "cell_size_scale",
       "expand_horizontal_to_content", "expand_top_to_content",
       "collide_left", "collide_right", "collide_top", "collide_bottom"},
      context);
  videocut::text::TextEffectCollisionSpec result;
  const auto number = [&](const char *field) {
    RequireCurrentJsonMember(value, field, context);
    return static_cast<float>(OptionalCurrentJsonNumber(value, field, 0.0));
  };
  const auto boolean = [&](const char *field) {
    RequireCurrentJsonMember(value, field, context);
    return OptionalCurrentJsonBool(value, field, false);
  };
  result.fixedBoxWidth = number("fixed_box_width");
  result.fixedBoxHeight = number("fixed_box_height");
  result.boxCenterXScale = number("box_center_x_scale");
  result.wallDamping = number("wall_damping");
  result.sideWallVelocityScale = number("side_wall_velocity_scale");
  result.topWallVelocityScale = number("top_wall_velocity_scale");
  result.bottomWallDamping = number("bottom_wall_damping");
  result.wallTorqueScale = number("wall_torque_scale");
  result.collisionBoundaryScale = number("collision_boundary_scale");
  result.collisionRestitution = number("collision_restitution");
  result.collisionTorqueScale = number("collision_torque_scale");
  result.minimumCellSize = number("minimum_cell_size");
  result.cellSizeScale = number("cell_size_scale");
  result.expandHorizontalToContent = boolean("expand_horizontal_to_content");
  result.expandTopToContent = boolean("expand_top_to_content");
  result.collideLeft = boolean("collide_left");
  result.collideRight = boolean("collide_right");
  result.collideTop = boolean("collide_top");
  result.collideBottom = boolean("collide_bottom");
  return result;
}

videocut::text::TextEffectExecutionStaticAffine
DecodeCurrentTextExecutionStaticAffine(const nlohmann::json &value,
                                       const std::string &context) {
  RequireOnlyCurrentJsonFields(
      value,
      {"translation_x", "translation_y", "scale_x", "scale_y",
       "rotation_degrees", "pivot_x", "pivot_y"},
      context);
  const auto number = [&](const char *field) {
    RequireCurrentJsonMember(value, field, context);
    return static_cast<float>(OptionalCurrentJsonNumber(value, field, 0.0));
  };
  videocut::text::TextEffectExecutionStaticAffine result;
  result.translationX = number("translation_x");
  result.translationY = number("translation_y");
  result.scaleX = number("scale_x");
  result.scaleY = number("scale_y");
  result.rotationDegrees = number("rotation_degrees");
  result.pivotX = number("pivot_x");
  result.pivotY = number("pivot_y");
  return result;
}

videocut::text::TextEffectExecutionCamera
DecodeCurrentTextExecutionCamera(const nlohmann::json &value,
                                 const std::string &context) {
  RequireOnlyCurrentJsonFields(value, {"world_to_clip", "viewport"}, context);
  const auto &worldToClip =
      RequireCurrentJsonMember(value, "world_to_clip", context);
  const auto &viewport = RequireCurrentJsonMember(value, "viewport", context);
  if (!worldToClip.is_array() || worldToClip.size() != 16U)
    throw std::invalid_argument(context +
                                ".world_to_clip must contain 16 floats");
  if (!viewport.is_array() || viewport.size() != 4U)
    throw std::invalid_argument(context + ".viewport must contain 4 floats");
  videocut::text::TextEffectExecutionCamera result;
  for (std::size_t index = 0U; index < result.worldToClip.size(); ++index) {
    result.worldToClip[index] = DecodeCurrentTextFloat(
        worldToClip[index], context + ".world_to_clip[" +
                                std::to_string(index) + "]");
  }
  for (std::size_t index = 0U; index < result.viewport.size(); ++index) {
    result.viewport[index] = DecodeCurrentTextFloat(
        viewport[index],
        context + ".viewport[" + std::to_string(index) + "]");
  }
  return result;
}

videocut::text::TextEffectExecutionNode DecodeCurrentTextExecutionNode(
    const nlohmann::json &value, const std::string &context,
    const std::size_t depth) {
  if (depth > 32U)
    throw std::invalid_argument(context + " exceeds the nesting limit");
  RequireOnlyCurrentJsonFields(
      value,
      {"id", "node_kind", "capability", "input_ids", "resource_ids",
       "post_effect_kind", "state_id", "random_seed", "history_id", "time_driver",
       "physics_spec", "collision_spec", "static_affine", "camera", "children",
       "owner_layer_id"},
      context);
  videocut::text::TextEffectExecutionNode result;
  result.nodeId = RequireCurrentJsonString(value, "id", context);
  if (value.contains("owner_layer_id")) {
    result.ownerLayerId =
        RequireCurrentJsonString(value, "owner_layer_id", context);
  }
  result.kind = DecodeCurrentTextExecutionNodeKind(
      RequireCurrentJsonString(value, "node_kind", context), context);
  result.capability = DecodeCurrentTextExecutionCapability(
      RequireCurrentJsonString(value, "capability", context), context);
  result.inputIds = OptionalCurrentJsonStringArray(value, "input_ids");
  result.resourceIds = OptionalCurrentJsonStringArray(value, "resource_ids");
  if (const auto postKind = value.find("post_effect_kind");
      postKind != value.end()) {
    if (!postKind->is_string())
      throw std::invalid_argument(context +
                                  ".post_effect_kind must be a string");
    result.postEffectKind = videocut::text::ParseTextPostEffectKind(
        postKind->get_ref<const std::string &>());
    if (!result.postEffectKind)
      throw std::invalid_argument(context +
                                  ".post_effect_kind is unsupported");
  }
  if (const auto stateId = value.find("state_id"); stateId != value.end())
    result.stateId = RequireCurrentJsonString(value, "state_id", context);
  if (const auto randomSeed = value.find("random_seed");
      randomSeed != value.end() &&
      !DecodeTextPackageUint64(*randomSeed, result.randomSeed)) {
    throw std::invalid_argument(context +
                                ".random_seed must be an unsigned integer");
  }
  if (const auto historyId = value.find("history_id");
      historyId != value.end()) {
    result.historyId =
        RequireCurrentJsonString(value, "history_id", context);
  }
  if (const auto driver = value.find("time_driver"); driver != value.end()) {
    result.timeDriver =
        DecodeCurrentTextTimeDriver(*driver, context + ".time_driver");
  }
  if (const auto physics = value.find("physics_spec");
      physics != value.end()) {
    result.physicsSpec =
        DecodeCurrentTextPhysicsSpec(*physics, context + ".physics_spec");
  }
  if (const auto collision = value.find("collision_spec");
      collision != value.end()) {
    result.collisionSpec = DecodeCurrentTextCollisionSpec(
        *collision, context + ".collision_spec");
  }
  if (const auto affine = value.find("static_affine");
      affine != value.end()) {
    result.staticAffine = DecodeCurrentTextExecutionStaticAffine(
        *affine, context + ".static_affine");
  }
  if (const auto camera = value.find("camera"); camera != value.end()) {
    result.camera =
        DecodeCurrentTextExecutionCamera(*camera, context + ".camera");
  }
  if (const auto children = value.find("children"); children != value.end()) {
    if (!children->is_array())
      throw std::invalid_argument(context + ".children must be an array");
    result.children.reserve(children->size());
    for (std::size_t index = 0U; index < children->size(); ++index) {
      result.children.push_back(DecodeCurrentTextExecutionNode(
          (*children)[index],
          context + ".children[" + std::to_string(index) + "]", depth + 1U));
    }
  }
  return result;
}

videocut::text::TextEffectExecutionGraph DecodeCurrentTextExecutionGraph(
    const nlohmann::json &value, const std::string &context) {
  RequireOnlyCurrentJsonFields(value, {"nodes"}, context);
  const auto &nodes = RequireCurrentJsonMember(value, "nodes", context);
  if (!nodes.is_array())
    throw std::invalid_argument(context + ".nodes must be an array");
  videocut::text::TextEffectExecutionGraph result;
  result.nodes.reserve(nodes.size());
  for (std::size_t index = 0U; index < nodes.size(); ++index) {
    result.nodes.push_back(DecodeCurrentTextExecutionNode(
        nodes[index], context + ".nodes[" + std::to_string(index) + "]", 0U));
  }
  return result;
}

videocut::text::TextDecorationAnimationSpec
DecodeCurrentTextDecorationAnimation(const nlohmann::json &value,
                                     const std::string &context) {
  RequireOnlyCurrentJsonFields(value,
                               {"id",
                                "asset_id",
                                "anchor",
                                "anchor_keyframes",
                                "fit",
                                "fit_keyframes",
                                "extent_space",
                                "inherit",
                                "playback",
                                "expand_x",
                                "expand_y",
                                "source_outsets",
                                "pivot_x",
                                "pivot_y",
                                "offset_x",
                                "offset_y",
                                "relative_offset_x",
                                "relative_offset_y",
                                "scale_x",
                                "scale_y",
                                "rotation_x_degrees",
                                "rotation_y_degrees",
                                "rotation_degrees",
                                "opacity",
                                "pivot_x_keyframes",
                                "pivot_y_keyframes",
                                "offset_x_keyframes",
                                "offset_y_keyframes",
                                "relative_offset_x_keyframes",
                                "relative_offset_y_keyframes",
                                "scale_x_keyframes",
                                "scale_y_keyframes",
                                "rotation_x_keyframes",
                                "rotation_y_keyframes",
                                "rotation_keyframes",
                                "opacity_keyframes",
                                "asset_progress_keyframes"},
                               context);
  videocut::text::TextDecorationAnimationSpec result;
  result.decorationId = RequireCurrentJsonString(value, "id", context);
  result.assetId = RequireCurrentJsonString(value, "asset_id", context);
  result.anchor = DecodeCurrentAnimatedDecorationAnchor(
      OptionalCurrentJsonString(value, "anchor", "bbox_center"));
  result.fit = DecodeCurrentAnimatedDecorationFit(
      OptionalCurrentJsonString(value, "fit", "inherit"));
  const auto extent =
      OptionalCurrentJsonString(value, "extent_space", "text_local");
  using Extent = videocut::text::TextDecorationExtentSpace;
  if (extent == "text_local")
    result.extentSpace = Extent::TextLocal;
  else if (extent == "canvas_width")
    result.extentSpace = Extent::CanvasWidth;
  else if (extent == "canvas_height")
    result.extentSpace = Extent::CanvasHeight;
  else if (extent == "canvas_full")
    result.extentSpace = Extent::CanvasFull;
  else
    throw std::invalid_argument(context + ".extent_space is unsupported");
  const auto inherit = OptionalCurrentJsonString(value, "inherit", "full");
  using Inherit = videocut::text::TextDecorationTransformInherit;
  if (inherit == "full")
    result.inherit = Inherit::Full;
  else if (inherit == "translate_only")
    result.inherit = Inherit::TranslateOnly;
  else
    throw std::invalid_argument(context + ".inherit is unsupported");
  result.playback = DecodeCurrentAnimationPlayback(
      OptionalCurrentJsonString(value, "playback", "once"));
  result.expandRatioX =
      static_cast<float>(OptionalCurrentJsonNumber(value, "expand_x", 1.0));
  result.expandRatioY =
      static_cast<float>(OptionalCurrentJsonNumber(value, "expand_y", 1.0));
  if (const auto sourceOutsets = value.find("source_outsets");
      sourceOutsets != value.end()) {
    RequireOnlyCurrentJsonFields(*sourceOutsets,
                                 {"left", "top", "right", "bottom"},
                                 context + ".source_outsets");
    result.sourceOutsets.left = static_cast<float>(
        OptionalCurrentJsonNumber(*sourceOutsets, "left", 0.0));
    result.sourceOutsets.top = static_cast<float>(
        OptionalCurrentJsonNumber(*sourceOutsets, "top", 0.0));
    result.sourceOutsets.right = static_cast<float>(
        OptionalCurrentJsonNumber(*sourceOutsets, "right", 0.0));
    result.sourceOutsets.bottom = static_cast<float>(
        OptionalCurrentJsonNumber(*sourceOutsets, "bottom", 0.0));
  }
  result.pivotX =
      static_cast<float>(OptionalCurrentJsonNumber(value, "pivot_x", 0.0));
  result.pivotY =
      static_cast<float>(OptionalCurrentJsonNumber(value, "pivot_y", 0.0));
  result.offsetX =
      static_cast<float>(OptionalCurrentJsonNumber(value, "offset_x", 0.0));
  result.offsetY =
      static_cast<float>(OptionalCurrentJsonNumber(value, "offset_y", 0.0));
  result.relativeOffsetX = static_cast<float>(
      OptionalCurrentJsonNumber(value, "relative_offset_x", 0.0));
  result.relativeOffsetY = static_cast<float>(
      OptionalCurrentJsonNumber(value, "relative_offset_y", 0.0));
  result.scaleX =
      static_cast<float>(OptionalCurrentJsonNumber(value, "scale_x", 1.0));
  result.scaleY =
      static_cast<float>(OptionalCurrentJsonNumber(value, "scale_y", 1.0));
  result.rotationXDegrees = static_cast<float>(
      OptionalCurrentJsonNumber(value, "rotation_x_degrees", 0.0));
  result.rotationYDegrees = static_cast<float>(
      OptionalCurrentJsonNumber(value, "rotation_y_degrees", 0.0));
  result.rotationDegrees = static_cast<float>(
      OptionalCurrentJsonNumber(value, "rotation_degrees", 0.0));
  result.opacity =
      static_cast<float>(OptionalCurrentJsonNumber(value, "opacity", 1.0));
  if (const auto keyframes = value.find("anchor_keyframes");
      keyframes != value.end()) {
    if (!keyframes->is_array())
      throw std::invalid_argument(context + ".anchor_keyframes is invalid");
    for (std::size_t index = 0U; index < keyframes->size(); ++index) {
      const auto &source = (*keyframes)[index];
      RequireOnlyCurrentJsonFields(source, {"offset", "value"}, context);
      result.anchorKeyframes.push_back(
          {static_cast<float>(OptionalCurrentJsonNumber(source, "offset", 0.0)),
           DecodeCurrentAnimatedDecorationAnchor(
               RequireCurrentJsonString(source, "value", context))});
    }
  }
  if (const auto keyframes = value.find("fit_keyframes");
      keyframes != value.end()) {
    if (!keyframes->is_array())
      throw std::invalid_argument(context + ".fit_keyframes is invalid");
    for (std::size_t index = 0U; index < keyframes->size(); ++index) {
      const auto &source = (*keyframes)[index];
      RequireOnlyCurrentJsonFields(source, {"offset", "value"}, context);
      result.fitKeyframes.push_back(
          {static_cast<float>(OptionalCurrentJsonNumber(source, "offset", 0.0)),
           DecodeCurrentAnimatedDecorationFit(
               RequireCurrentJsonString(source, "value", context))});
    }
  }
  DecodeCurrentTextKeyframeArray(value, "pivot_x_keyframes", context,
                                 result.pivotXKeyframes);
  DecodeCurrentTextKeyframeArray(value, "pivot_y_keyframes", context,
                                 result.pivotYKeyframes);
  DecodeCurrentTextKeyframeArray(value, "offset_x_keyframes", context,
                                 result.offsetXKeyframes);
  DecodeCurrentTextKeyframeArray(value, "offset_y_keyframes", context,
                                 result.offsetYKeyframes);
  DecodeCurrentTextKeyframeArray(value, "relative_offset_x_keyframes", context,
                                 result.relativeOffsetXKeyframes);
  DecodeCurrentTextKeyframeArray(value, "relative_offset_y_keyframes", context,
                                 result.relativeOffsetYKeyframes);
  DecodeCurrentTextKeyframeArray(value, "scale_x_keyframes", context,
                                 result.scaleXKeyframes);
  DecodeCurrentTextKeyframeArray(value, "scale_y_keyframes", context,
                                 result.scaleYKeyframes);
  DecodeCurrentTextKeyframeArray(value, "rotation_x_keyframes", context,
                                 result.rotationXKeyframes);
  DecodeCurrentTextKeyframeArray(value, "rotation_y_keyframes", context,
                                 result.rotationYKeyframes);
  DecodeCurrentTextKeyframeArray(value, "rotation_keyframes", context,
                                 result.rotationKeyframes);
  DecodeCurrentTextKeyframeArray(value, "opacity_keyframes", context,
                                 result.opacityKeyframes);
  DecodeCurrentTextKeyframeArray(value, "asset_progress_keyframes", context,
                                 result.assetProgressKeyframes);
  return result;
}

std::uint64_t ResolveCurrentTextRunEnd(
    const videocut::text_composition::TextCompositionDocument &document,
    const std::string &paragraphId, const std::string &runId,
    const std::string &context) {
  const auto paragraph = std::find_if(
      document.content.begin(), document.content.end(),
      [&](const auto &item) { return item.paragraphId == paragraphId; });
  if (paragraph == document.content.end())
    throw std::invalid_argument(context + " paragraph is unavailable");
  const auto run =
      std::find_if(paragraph->runs.begin(), paragraph->runs.end(),
                   [&](const auto &item) { return item.runId == runId; });
  if (run == paragraph->runs.end())
    throw std::invalid_argument(context + " run is unavailable");
  return run->utf8Text.size();
}

void DecodeCurrentTimedText(
    const nlohmann::json &animation,
    videocut::text_composition::TextCompositionDocument &document) {
  const auto found = animation.find("timed_spans");
  if (found == animation.end())
    return;
  if (!found->is_array())
    throw std::invalid_argument("animation.timed_spans must be an array");
  videocut::text_composition::TimedTextTrack track;
  track.spans.reserve(found->size());
  for (std::size_t index = 0U; index < found->size(); ++index) {
    const auto &source = (*found)[index];
    const auto context = "animation.timed_spans[" + std::to_string(index) + "]";
    RequireOnlyCurrentJsonFields(source,
                                 {"id", "paragraph_id", "run_id", "utf8_begin",
                                  "utf8_end", "start_offset_us",
                                  "end_offset_us", "transition_end_offset_us",
                                  "semantic", "progress"},
                                 context);
    videocut::text_composition::TimedTextSpan span;
    span.spanId = RequireCurrentJsonString(source, "id", context);
    span.paragraphId =
        RequireCurrentJsonString(source, "paragraph_id", context);
    span.runId = RequireCurrentJsonString(source, "run_id", context);
    const auto begin = OptionalCurrentJsonInteger(source, "utf8_begin", -1);
    const auto &endValue =
        RequireCurrentJsonMember(source, "utf8_end", context);
    std::uint64_t end = 0U;
    if (endValue.is_string() &&
        endValue.get_ref<const std::string &>() == "run_end") {
      end = ResolveCurrentTextRunEnd(document, span.paragraphId, span.runId,
                                     context);
    } else if (endValue.is_number_integer() || endValue.is_number_unsigned()) {
      const auto decoded = endValue.get<std::int64_t>();
      if (decoded < 0)
        throw std::invalid_argument(context + ".utf8_end is invalid");
      end = static_cast<std::uint64_t>(decoded);
    } else {
      throw std::invalid_argument(context + ".utf8_end is invalid");
    }
    if (begin < 0 || static_cast<std::uint64_t>(begin) > end)
      throw std::invalid_argument(context + " UTF-8 range is invalid");
    span.range = {static_cast<std::uint64_t>(begin), end};
    span.startOffsetUs =
        OptionalCurrentJsonInteger(source, "start_offset_us", 0);
    span.endOffsetUs = OptionalCurrentJsonInteger(source, "end_offset_us", 0);
    span.transitionEndOffsetUs = OptionalCurrentJsonInteger(
        source, "transition_end_offset_us", span.endOffsetUs);
    span.semantic = OptionalCurrentJsonString(source, "semantic", "word");
    const auto progress = OptionalCurrentJsonString(source, "progress", "step");
    if (progress == "step")
      span.progressMode = videocut::text::TimedTextProgressMode::Step;
    else if (progress == "grapheme_sweep")
      span.progressMode = videocut::text::TimedTextProgressMode::GraphemeSweep;
    else
      throw std::invalid_argument(context + ".progress is unsupported");
    track.spans.push_back(std::move(span));
  }
  document.timedText = std::move(track);
}

[[nodiscard]] bool DecodeTextAnimationDocument(
    const nlohmann::json &document,
    videocut::text::TextEffectProgramLibrary effectPrograms,
    videocut::text_composition::TextCompositionDocument &composition,
    std::string &error) {
  try {
    if (!HasExactJsonFields(document, {"format", "animation"}) ||
        !document["format"].is_string() ||
        document["format"].get_ref<const std::string &>() !=
            "videocut.text-animation") {
      error = "builtin text animation entry is malformed";
      return false;
    }
    const auto &animation = document["animation"];
    RequireOnlyCurrentJsonFields(
        animation, {"layers", "timed_spans", "execution_graph"},
        "animation");
    DecodeCurrentTimedText(animation, composition);

    const auto &layers =
        RequireCurrentJsonMember(animation, "layers", "animation");
    if (!layers.is_array())
      throw std::invalid_argument("animation.layers must be an array");
    videocut::text::TextAnimationStack stack;
    stack.effectPrograms = std::move(effectPrograms);
    if (const auto executionGraph = animation.find("execution_graph");
        executionGraph != animation.end()) {
      stack.executionGraph = DecodeCurrentTextExecutionGraph(
          *executionGraph, "animation.execution_graph");
    }
    stack.layers.reserve(layers.size());
    for (std::size_t index = 0U; index < layers.size(); ++index) {
      const auto &source = layers[index];
      const auto context = "animation.layers[" + std::to_string(index) + "]";
      RequireOnlyCurrentJsonFields(source,
                                   {"id", "enabled", "target", "combine_mode",
                                    "requires_timed_text", "render_group",
                                    "time_driver", "layer_track", "animators",
                                    "post_effects", "decorations"},
                                   context);
      videocut::text::TextAnimationLayerSpec layer;
      layer.layerId = RequireCurrentJsonString(source, "id", context);
      layer.enabled = OptionalCurrentJsonBool(source, "enabled", true);
      layer.requiresTimedText =
          OptionalCurrentJsonBool(source, "requires_timed_text", false);
      layer.combineMode = DecodeCurrentTextCombineMode(
          OptionalCurrentJsonString(source, "combine_mode", "replace"));
      if (const auto target = source.find("target"); target != source.end())
        layer.target =
            DecodeCurrentTextAnimationTarget(*target, context + ".target");
      if (const auto renderGroup = source.find("render_group");
          renderGroup != source.end()) {
        layer.renderGroup = DecodeCurrentTextRenderGroup(
            *renderGroup, context + ".render_group");
      }
      if (const auto driver = source.find("time_driver");
          driver != source.end()) {
        layer.timeDriver =
            DecodeCurrentTextTimeDriver(*driver, context + ".time_driver");
      }
      if (const auto layerTrack = source.find("layer_track");
          layerTrack != source.end()) {
        layer.layerTrack =
            DecodeCurrentTextLayerTrack(*layerTrack, context + ".layer_track");
        using Driver = videocut::text::TextAnimationTimeDriverKind;
        using Phase = videocut::text::TextLayerAnimationPhase;
        const auto expected =
            layer.layerTrack->phase == Phase::Enter  ? Driver::EnterPhase
            : layer.layerTrack->phase == Phase::Loop ? Driver::LoopPhase
            : layer.layerTrack->phase == Phase::Exit ? Driver::ExitPhase
                                                     : Driver::CaptionPhase;
        if (layer.timeDriver.kind != expected ||
            layer.layerTrack->durationUs != layer.timeDriver.durationUs) {
          throw std::invalid_argument(
              context + ".layer_track does not match its time driver");
        }
      }
      if (const auto animators = source.find("animators");
          animators != source.end()) {
        if (!animators->is_array())
          throw std::invalid_argument(context + ".animators must be an array");
        layer.animators.reserve(animators->size());
        for (std::size_t animatorIndex = 0U; animatorIndex < animators->size();
             ++animatorIndex) {
          layer.animators.push_back(DecodeCurrentTextAnimator(
              (*animators)[animatorIndex],
              context + ".animators[" + std::to_string(animatorIndex) + "]"));
        }
      }
      if (const auto effects = source.find("post_effects");
          effects != source.end()) {
        if (!effects->is_array())
          throw std::invalid_argument(context +
                                      ".post_effects must be an array");
        layer.postEffects.reserve(effects->size());
        for (std::size_t effectIndex = 0U; effectIndex < effects->size();
             ++effectIndex) {
          layer.postEffects.push_back(DecodeCurrentTextPostEffect(
              (*effects)[effectIndex],
              context + ".post_effects[" + std::to_string(effectIndex) + "]"));
        }
      }
      if (const auto decorations = source.find("decorations");
          decorations != source.end()) {
        if (!decorations->is_array())
          throw std::invalid_argument(context +
                                      ".decorations must be an array");
        layer.decorations.reserve(decorations->size());
        for (std::size_t decorationIndex = 0U;
             decorationIndex < decorations->size(); ++decorationIndex) {
          layer.decorations.push_back(DecodeCurrentTextDecorationAnimation(
              (*decorations)[decorationIndex],
              context + ".decorations[" + std::to_string(decorationIndex) +
                  "]"));
        }
      }
      stack.layers.push_back(std::move(layer));
    }

    if (stack.layers.size() == 1U) {
      const auto &ownerLayerId = stack.layers.front().layerId;
      const auto bindOwner = [&](const auto &self, auto &nodes) -> void {
        for (auto &node : nodes) {
          if (node.ownerLayerId.empty()) node.ownerLayerId = ownerLayerId;
          self(self, node.children);
        }
      };
      bindOwner(bindOwner, stack.executionGraph.nodes);
    }

    const bool hasTimedText =
        composition.timedText && !composition.timedText->spans.empty();
    std::string validationError;
    if (!videocut::text::ValidateTextAnimationStack(stack, hasTimedText,
                                                    &validationError)) {
      error = "builtin text animation failed current validation: " +
              validationError;
      return false;
    }
    composition.animations = std::move(stack);
  } catch (const std::exception &exception) {
    error = "builtin text animation entry is invalid: " +
            std::string(exception.what());
    return false;
  }
  error.clear();
  return true;
}


}
bool DecodePackageAnimation(const nlohmann::json& effect, const nlohmann::json& animation,
    text_composition::TextCompositionDocument& composition, std::string& error) {
  text::TextEffectProgramLibrary programs;
  return DecodeTextEffectProgramDocument(effect, programs, error) &&
      DecodeTextAnimationDocument(animation, std::move(programs), composition, error);
}
}
