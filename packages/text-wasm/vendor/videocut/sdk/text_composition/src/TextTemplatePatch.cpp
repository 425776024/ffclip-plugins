#include "videocut/text_composition/TextTemplatePatch.h"

#include "CanonicalTextIdentity.h"
#include "videocut/base/Sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace videocut::text_composition {
namespace {

using Json = nlohmann::json;

void Add(std::vector<Diagnostic> &diagnostics, std::string code,
         std::string subject, std::string message,
         const DiagnosticSeverity severity = DiagnosticSeverity::Error) {
  diagnostics.push_back({std::move(code), severity, "template-patch",
                         std::move(subject), std::move(message)});
}

[[noreturn]] void Invalid(const std::string &context,
                          const std::string &message) {
  throw std::invalid_argument(context + " " + message);
}

void OnlyKeys(const Json &value,
              const std::initializer_list<std::string_view> keys,
              const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be an object");
  for (const auto &[key, ignored] : value.items()) {
    (void)ignored;
    if (std::find(keys.begin(), keys.end(), std::string_view(key)) ==
        keys.end())
      Invalid(context + "." + key, "is not a current-schema field");
  }
}

const Json &Object(const Json &parent, const char *key,
                   const std::string &context) {
  const auto found = parent.find(key);
  if (found == parent.end() || !found->is_object())
    Invalid(context + "." + key, "must be an object");
  return *found;
}

const Json &Array(const Json &parent, const char *key,
                  const std::string &context) {
  const auto found = parent.find(key);
  if (found == parent.end() || !found->is_array())
    Invalid(context + "." + key, "must be an array");
  return *found;
}

std::string String(const Json &parent, const char *key,
                   const std::string &context, const bool allowEmpty = false) {
  const auto found = parent.find(key);
  if (found == parent.end() || !found->is_string() ||
      (!allowEmpty && found->get_ref<const std::string &>().empty()))
    Invalid(context + "." + key, "must be a string");
  return found->get<std::string>();
}

std::string OptionalString(const Json &parent, const char *key,
                           std::string fallback = {}) {
  const auto found = parent.find(key);
  if (found == parent.end())
    return fallback;
  if (!found->is_string())
    Invalid(key, "must be a string");
  return found->get<std::string>();
}

bool OptionalBool(const Json &parent, const char *key,
                  const bool fallback) {
  const auto found = parent.find(key);
  if (found == parent.end())
    return fallback;
  if (!found->is_boolean())
    Invalid(key, "must be boolean");
  return found->get<bool>();
}

double Number(const Json &value, const std::string &context) {
  if (!value.is_number())
    Invalid(context, "must be numeric");
  const auto result = value.get<double>();
  if (!std::isfinite(result))
    Invalid(context, "must be finite");
  return result;
}

double OptionalNumber(const Json &parent, const char *key,
                      const double fallback) {
  const auto found = parent.find(key);
  return found == parent.end() ? fallback
                               : Number(*found, std::string(key));
}

float Float(const Json &value, const std::string &context) {
  const auto result = Number(value, context);
  if (result < -std::numeric_limits<float>::max() ||
      result > std::numeric_limits<float>::max())
    Invalid(context, "is outside float range");
  return static_cast<float>(result);
}

float OptionalFloat(const Json &parent, const char *key,
                    const float fallback) {
  const auto found = parent.find(key);
  return found == parent.end() ? fallback
                               : Float(*found, std::string(key));
}

std::int64_t Integer(const Json &value, const std::string &context) {
  if (!value.is_number_integer())
    Invalid(context, "must be an integer");
  return value.get<std::int64_t>();
}

std::int64_t OptionalInteger(const Json &parent, const char *key,
                             const std::int64_t fallback) {
  const auto found = parent.find(key);
  return found == parent.end() ? fallback
                               : Integer(*found, std::string(key));
}

std::uint64_t Unsigned(const Json &value, const std::string &context) {
  if (!value.is_number_unsigned() && !value.is_number_integer())
    Invalid(context, "must be an unsigned integer");
  const auto signedValue = value.get<std::int64_t>();
  if (signedValue < 0)
    Invalid(context, "must be non-negative");
  return static_cast<std::uint64_t>(signedValue);
}

std::uint64_t OptionalUnsigned(const Json &parent, const char *key,
                               const std::uint64_t fallback) {
  const auto found = parent.find(key);
  return found == parent.end() ? fallback
                               : Unsigned(*found, std::string(key));
}

template <typename Enum>
Enum EnumString(
    const Json &value, const std::string &context,
    const std::initializer_list<std::pair<std::string_view, Enum>> values) {
  if (!value.is_string())
    Invalid(context, "must be a named enum");
  const auto &name = value.get_ref<const std::string &>();
  const auto found = std::find_if(
      values.begin(), values.end(),
      [&](const auto &entry) { return entry.first == name; });
  if (found == values.end())
    Invalid(context, "contains an unsupported enum value");
  return found->second;
}

template <typename Enum>
Enum OptionalEnum(
    const Json &parent, const char *key, const Enum fallback,
    const std::initializer_list<std::pair<std::string_view, Enum>> values) {
  const auto found = parent.find(key);
  return found == parent.end()
             ? fallback
             : EnumString<Enum>(*found, std::string(key), values);
}

std::vector<std::string> StringArray(const Json &parent, const char *key,
                                     const std::string &context,
                                     const bool optional = false) {
  const auto found = parent.find(key);
  if (found == parent.end() && optional)
    return {};
  if (found == parent.end() || !found->is_array())
    Invalid(context + "." + key, "must be an array");
  std::vector<std::string> result;
  result.reserve(found->size());
  for (std::size_t index = 0U; index < found->size(); ++index) {
    if (!(*found)[index].is_string())
      Invalid(context + "." + key, "contains a non-string");
    result.push_back((*found)[index].get<std::string>());
  }
  return result;
}

text::Color ParseColor(const Json &value, const std::string &context) {
  if (!value.is_array() || value.size() != 4U)
    Invalid(context, "must be an RGBA array");
  return {Float(value[0], context), Float(value[1], context),
          Float(value[2], context), Float(value[3], context)};
}

std::pair<float, float> ParsePoint(const Json &value,
                                   const std::string &context) {
  if (!value.is_array() || value.size() != 2U)
    Invalid(context, "must be a two-component array");
  return {Float(value[0], context), Float(value[1], context)};
}

text::Insets ParseInsets(const Json &value, const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be an inset object");
  OnlyKeys(value, {"left", "top", "right", "bottom"}, context);
  return {OptionalFloat(value, "left", 0.0F),
          OptionalFloat(value, "top", 0.0F),
          OptionalFloat(value, "right", 0.0F),
          OptionalFloat(value, "bottom", 0.0F)};
}

text::TextureReference ParseTexture(const Json &value,
                                    const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be a texture object");
  OnlyKeys(value,
           {"source_kind", "asset_id", "digest", "media_type",
            "color_space", "orientation"},
           context);
  text::TextureReference result;
  result.sourceKind = OptionalEnum<text::TextureSourceKind>(
      value, "source_kind", result.sourceKind,
      {{"builtin", text::TextureSourceKind::Builtin},
       {"project_managed", text::TextureSourceKind::ProjectManaged}});
  result.assetId = String(value, "asset_id", context);
  result.digest = OptionalString(value, "digest");
  if (result.sourceKind == text::TextureSourceKind::ProjectManaged &&
      result.digest.empty()) {
    Invalid(context + ".digest",
            "is required for project-managed texture resources");
  }
  result.mediaType = String(value, "media_type", context);
  result.colorSpace = OptionalString(value, "color_space", "srgb");
  result.orientation = OptionalEnum<text::TextureOrientation>(
      value, "orientation", result.orientation,
      {{"up", text::TextureOrientation::Up},
       {"right", text::TextureOrientation::Right},
       {"down", text::TextureOrientation::Down},
       {"left", text::TextureOrientation::Left}});
  return result;
}

std::vector<text::GradientStop> ParseStops(const Json &value,
                                           const std::string &context) {
  if (!value.is_array())
    Invalid(context, "must be an array");
  std::vector<text::GradientStop> result;
  result.reserve(value.size());
  for (std::size_t index = 0U; index < value.size(); ++index) {
    const auto &source = value[index];
    if (!source.is_object())
      Invalid(context, "contains a non-object stop");
    OnlyKeys(source, {"offset", "color"},
             context + "[" + std::to_string(index) + "]");
    result.push_back({OptionalFloat(source, "offset", 0.0F),
                      ParseColor(source.at("color"), context + ".color")});
  }
  return result;
}

text::TextMaterialCoordinates ParseCoordinates(const Json &value,
                                               const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be an object");
  OnlyKeys(value, {"space", "outset", "scale"}, context);
  text::TextMaterialCoordinates result;
  result.coordinateSpace = OptionalEnum<text::PaintCoordinateSpace>(
      value, "space", result.coordinateSpace,
      {{"layout_box", text::PaintCoordinateSpace::LayoutBox},
       {"text", text::PaintCoordinateSpace::TextBounds},
       {"grapheme", text::PaintCoordinateSpace::Grapheme}});
  result.coordinateOutset =
      OptionalFloat(value, "outset", result.coordinateOutset);
  result.coordinateScale =
      OptionalFloat(value, "scale", result.coordinateScale);
  return result;
}

text::TextMaterial ParseMaterial(const Json &value,
                                 const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be a material object");
  const auto kind = String(value, "kind", context);
  if (kind == "solid") {
    OnlyKeys(value, {"kind", "color"}, context);
    text::SolidTextMaterial result;
    result.color = ParseColor(value.at("color"), context + ".color");
    return result;
  }
  if (kind == "linear_gradient") {
    OnlyKeys(value,
             {"kind", "stops", "start", "end", "spread", "sampling",
              "coordinates"},
             context);
    text::LinearGradientTextMaterial result;
    result.stops = ParseStops(value.at("stops"), context + ".stops");
    if (const auto found = value.find("start"); found != value.end()) {
      const auto [x, y] = ParsePoint(*found, context + ".start");
      result.startX = x;
      result.startY = y;
    }
    if (const auto found = value.find("end"); found != value.end()) {
      const auto [x, y] = ParsePoint(*found, context + ".end");
      result.endX = x;
      result.endY = y;
    }
    result.spread = OptionalEnum<text::PaintSpread>(
        value, "spread", result.spread,
        {{"clamp", text::PaintSpread::Clamp},
         {"repeat", text::PaintSpread::Repeat},
         {"mirror", text::PaintSpread::Mirror}});
    result.sampling = OptionalEnum<text::GradientSampling>(
        value, "sampling", result.sampling,
        {{"continuous", text::GradientSampling::Continuous},
         {"rgba8_lut_256", text::GradientSampling::Rgba8Lut256}});
    if (const auto found = value.find("coordinates"); found != value.end())
      result.coordinates = ParseCoordinates(*found, context + ".coordinates");
    return result;
  }
  if (kind == "radial_gradient") {
    OnlyKeys(value,
             {"kind", "stops", "center", "radius", "spread", "sampling",
              "coordinates"},
             context);
    text::RadialGradientTextMaterial result;
    result.stops = ParseStops(value.at("stops"), context + ".stops");
    if (const auto found = value.find("center"); found != value.end()) {
      const auto [x, y] = ParsePoint(*found, context + ".center");
      result.centerX = x;
      result.centerY = y;
    }
    result.radius = OptionalFloat(value, "radius", result.radius);
    result.spread = OptionalEnum<text::PaintSpread>(
        value, "spread", result.spread,
        {{"clamp", text::PaintSpread::Clamp},
         {"repeat", text::PaintSpread::Repeat},
         {"mirror", text::PaintSpread::Mirror}});
    result.sampling = OptionalEnum<text::GradientSampling>(
        value, "sampling", result.sampling,
        {{"continuous", text::GradientSampling::Continuous},
         {"rgba8_lut_256", text::GradientSampling::Rgba8Lut256}});
    if (const auto found = value.find("coordinates"); found != value.end())
      result.coordinates = ParseCoordinates(*found, context + ".coordinates");
    return result;
  }
  if (kind == "texture") {
    OnlyKeys(value,
             {"kind", "texture", "fit", "mapping", "coordinates", "scale",
              "rotation_degrees", "offset_x", "offset_y", "flip_x",
              "flip_y", "atlas_columns", "atlas_rows", "texture_opacity",
              "opacity", "source_alpha", "underlay_color",
              "underlay_gradient", "underlay_gradient_projection"},
             context);
    text::TextureTextMaterial result;
    result.texture = ParseTexture(Object(value, "texture", context),
                                  context + ".texture");
    result.fit = OptionalEnum<text::TextureFit>(
        value, "fit", result.fit,
        {{"cover", text::TextureFit::Cover},
         {"contain", text::TextureFit::Contain},
         {"stretch", text::TextureFit::Stretch},
         {"tile", text::TextureFit::Tile}});
    result.mapping = OptionalEnum<text::TextureMapping>(
        value, "mapping", result.mapping,
        {{"scope_bounds", text::TextureMapping::ScopeBounds},
         {"glyph_distance_field", text::TextureMapping::GlyphDistanceField}});
    if (const auto found = value.find("coordinates"); found != value.end())
      result.coordinates = ParseCoordinates(*found, context + ".coordinates");
    result.scale = OptionalFloat(value, "scale", result.scale);
    result.rotationDegrees =
        OptionalFloat(value, "rotation_degrees", result.rotationDegrees);
    result.offsetX = OptionalFloat(value, "offset_x", result.offsetX);
    result.offsetY = OptionalFloat(value, "offset_y", result.offsetY);
    result.flipX = OptionalBool(value, "flip_x", result.flipX);
    result.flipY = OptionalBool(value, "flip_y", result.flipY);
    result.atlasColumns = static_cast<std::uint16_t>(
        OptionalUnsigned(value, "atlas_columns", result.atlasColumns));
    result.atlasRows = static_cast<std::uint16_t>(
        OptionalUnsigned(value, "atlas_rows", result.atlasRows));
    result.textureOpacity =
        OptionalFloat(value, "texture_opacity", result.textureOpacity);
    result.opacity = OptionalFloat(value, "opacity", result.opacity);
    result.sourceAlpha =
        OptionalBool(value, "source_alpha", result.sourceAlpha);
    if (const auto found = value.find("underlay_color");
        found != value.end() && !found->is_null())
      result.underlayColor = ParseColor(*found, context + ".underlay_color");
    if (const auto found = value.find("underlay_gradient");
        found != value.end())
      result.underlayGradient =
          ParseStops(*found, context + ".underlay_gradient");
    if (const auto found = value.find("underlay_gradient_projection");
        found != value.end()) {
      const auto projectionContext = context + ".underlay_gradient_projection";
      OnlyKeys(*found, {"start", "end", "spread", "sampling"},
               projectionContext);
      const auto [startX, startY] =
          ParsePoint(found->at("start"), projectionContext + ".start");
      const auto [endX, endY] =
          ParsePoint(found->at("end"), projectionContext + ".end");
      result.underlayGradientProjection.startX = startX;
      result.underlayGradientProjection.startY = startY;
      result.underlayGradientProjection.endX = endX;
      result.underlayGradientProjection.endY = endY;
      result.underlayGradientProjection.spread =
          OptionalEnum<text::PaintSpread>(
              *found, "spread", result.underlayGradientProjection.spread,
              {{"clamp", text::PaintSpread::Clamp},
               {"repeat", text::PaintSpread::Repeat},
               {"mirror", text::PaintSpread::Mirror}});
      result.underlayGradientProjection.sampling =
          OptionalEnum<text::GradientSampling>(
              *found, "sampling",
              result.underlayGradientProjection.sampling,
              {{"continuous", text::GradientSampling::Continuous},
               {"rgba8_lut_256", text::GradientSampling::Rgba8Lut256}});
    }
    return result;
  }
  Invalid(context, "contains an unsupported material kind");
}

text::TextMaterialBinding ParseMaterialBinding(const Json &value,
                                               const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be a material binding object");
  const auto binding = String(value, "kind", context);
  if (binding == "literal") {
    OnlyKeys(value, {"kind", "material"}, context);
    return text::LiteralTextMaterial{
        ParseMaterial(Object(value, "material", context),
                      context + ".material")};
  }
  if (binding == "editable_slot") {
    OnlyKeys(value,
             {"kind", "semantic_role", "fallback", "replacement_mask"},
             context);
    text::EditableTextStyleSlot result;
    result.semanticRole = String(value, "semantic_role", context);
    result.fallback = ParseMaterial(Object(value, "fallback", context),
                                    context + ".fallback");
    if (const auto found = value.find("replacement_mask");
        found != value.end())
      result.replacementMask =
          ParseTexture(*found, context + ".replacement_mask");
    return result;
  }
  Invalid(context, "contains an unsupported binding kind");
}

text::TextBlendMode ParseBlend(const Json &parent,
                               const text::TextBlendMode fallback) {
  return OptionalEnum<text::TextBlendMode>(
      parent, "blend", fallback,
      {{"source_over", text::TextBlendMode::SourceOver},
       {"multiply", text::TextBlendMode::Multiply},
       {"screen", text::TextBlendMode::Screen},
       {"overlay", text::TextBlendMode::Overlay},
       {"add", text::TextBlendMode::Add},
       {"darken", text::TextBlendMode::Darken},
       {"lighten", text::TextBlendMode::Lighten}});
}

text::TextGlyphMaterialLayer ParseGlyphLayer(const Json &value,
                                             const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be a glyph layer object");
  const auto kind = String(value, "type", context);
  const auto layerId = String(value, "layer_id", context);
  const auto zOrder = static_cast<std::int32_t>(
      OptionalInteger(value, "z_order", kind == "fill" ? 0 : -1));
  const auto blend = ParseBlend(value, kind == "glow"
                                           ? text::TextBlendMode::Add
                                           : text::TextBlendMode::SourceOver);
  const auto material = ParseMaterialBinding(
      Object(value, "material", context), context + ".material");
  const auto parsePolarOffset = [&]()
      -> std::optional<text::TextNormalizedPolarOffset> {
    const auto found = value.find("offset_polar");
    if (found == value.end())
      return std::nullopt;
    const auto polarContext = context + ".offset_polar";
    if (!found->is_object())
      Invalid(polarContext, "must be an object");
    OnlyKeys(*found, {"radius_normalized", "angle_radians"}, polarContext);
    const auto radius = found->find("radius_normalized");
    const auto angle = found->find("angle_radians");
    if (radius == found->end())
      Invalid(polarContext + ".radius_normalized", "must be numeric");
    if (angle == found->end())
      Invalid(polarContext + ".angle_radians", "must be numeric");
    text::TextNormalizedPolarOffset result;
    result.radius = Float(*radius, polarContext + ".radius_normalized");
    result.angleRadians = Float(*angle, polarContext + ".angle_radians");
    return result;
  };
  if (kind == "fill") {
    OnlyKeys(value,
             {"type", "layer_id", "z_order", "blend", "material",
              "offset_x", "offset_y", "offset_polar"},
             context);
    text::TextFillLayer result;
    result.layerId = layerId;
    result.zOrder = zOrder;
    result.blend = blend;
    result.material = material;
    result.offsetX = OptionalFloat(value, "offset_x", result.offsetX);
    result.offsetY = OptionalFloat(value, "offset_y", result.offsetY);
    result.normalizedPolarOffset = parsePolarOffset();
    return result;
  }
  if (kind == "stroke") {
    OnlyKeys(value,
             {"type", "layer_id", "z_order", "blend", "material", "width",
              "inner_ring_width", "offset_x", "offset_y", "blur_radius",
              "spread", "offset_polar", "signed_start_width"},
             context);
    text::TextStrokeLayer result;
    result.layerId = layerId;
    result.zOrder = zOrder;
    result.blend = blend;
    result.material = material;
    result.width = OptionalFloat(value, "width", result.width);
    result.innerRingWidth =
        OptionalFloat(value, "inner_ring_width", result.innerRingWidth);
    if (const auto found = value.find("signed_start_width");
        found != value.end())
      result.signedStartWidth =
          Float(*found, context + ".signed_start_width");
    result.offsetX = OptionalFloat(value, "offset_x", result.offsetX);
    result.offsetY = OptionalFloat(value, "offset_y", result.offsetY);
    result.normalizedPolarOffset = parsePolarOffset();
    result.blurRadius =
        OptionalFloat(value, "blur_radius", result.blurRadius);
    result.spread = OptionalFloat(value, "spread", result.spread);
    return result;
  }
  if (kind == "shadow") {
    OnlyKeys(value,
             {"type", "layer_id", "z_order", "blend", "material",
              "shadow_kind", "offset_x", "offset_y", "blur_radius",
              "spread", "thickness_angle_degrees", "thickness_distance",
              "smoothing", "round_mask_intensity", "offset_polar",
              "offset_uv", "sdf_blur_scale", "strokes"},
             context);
    text::TextShadowLayer result;
    result.layerId = layerId;
    result.zOrder = zOrder;
    result.blend = blend;
    result.material = material;
    result.kind = OptionalEnum<text::TextShadowKind>(
        value, "shadow_kind", result.kind,
        {{"outer", text::TextShadowKind::Outer},
         {"inner", text::TextShadowKind::Inner}});
    result.offsetX = OptionalFloat(value, "offset_x", result.offsetX);
    result.offsetY = OptionalFloat(value, "offset_y", result.offsetY);
    result.normalizedPolarOffset = parsePolarOffset();
    if (const auto found = value.find("offset_uv"); found != value.end()) {
      const auto uvContext = context + ".offset_uv";
      if (!found->is_object())
        Invalid(uvContext, "must be an object");
      OnlyKeys(*found, {"x", "y"}, uvContext);
      const auto x = found->find("x");
      const auto y = found->find("y");
      if (x == found->end() || y == found->end())
        Invalid(uvContext, "must contain numeric x and y");
      result.normalizedUvOffset = text::TextNormalizedUvOffset{
          Float(*x, uvContext + ".x"), Float(*y, uvContext + ".y")};
    }
    result.blurRadius =
        OptionalFloat(value, "blur_radius", result.blurRadius);
    result.spread = OptionalFloat(value, "spread", result.spread);
    result.thicknessAngleDegrees = OptionalFloat(
        value, "thickness_angle_degrees", result.thicknessAngleDegrees);
    result.thicknessDistance = OptionalFloat(
        value, "thickness_distance", result.thicknessDistance);
    result.smoothing = OptionalEnum<text::TextOuterShadowSmoothingMode>(
        value, "smoothing", result.smoothing,
        {{"auto", text::TextOuterShadowSmoothingMode::Auto},
         {"feather", text::TextOuterShadowSmoothingMode::Feather},
         {"diffuse_round_mask",
          text::TextOuterShadowSmoothingMode::DiffuseRoundMask}});
    result.roundMaskIntensity = OptionalFloat(
        value, "round_mask_intensity", result.roundMaskIntensity);
    result.sdfBlurScale =
        OptionalFloat(value, "sdf_blur_scale", result.sdfBlurScale);
    if (const auto found = value.find("strokes"); found != value.end()) {
      if (!found->is_array())
        Invalid(context + ".strokes", "must be an array");
      result.strokes.reserve(found->size());
      for (std::size_t index = 0U; index < found->size(); ++index) {
        const auto layer = ParseGlyphLayer(
            (*found)[index], context + ".strokes[" +
                                 std::to_string(index) + "]");
        const auto *stroke = std::get_if<text::TextStrokeLayer>(&layer);
        if (!stroke)
          Invalid(context + ".strokes", "must contain stroke layers");
        result.strokes.push_back(*stroke);
      }
    }
    return result;
  }
  if (kind == "glow") {
    OnlyKeys(value,
             {"type", "layer_id", "z_order", "blend", "material", "radius",
              "spread", "direction_x", "direction_y", "offset_polar"},
             context);
    text::TextGlowLayer result;
    result.layerId = layerId;
    result.zOrder = zOrder;
    result.blend = blend;
    result.material = material;
    result.radius = OptionalFloat(value, "radius", result.radius);
    result.spread = OptionalFloat(value, "spread", result.spread);
    result.directionX =
        OptionalFloat(value, "direction_x", result.directionX);
    result.directionY =
        OptionalFloat(value, "direction_y", result.directionY);
    result.normalizedPolarOffset = parsePolarOffset();
    return result;
  }
  Invalid(context, "contains an unsupported glyph layer kind");
}

text::TextBoxBackground ParseBoxBackground(const Json &value,
                                           const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be an object");
  OnlyKeys(value, {"enabled", "color", "padding", "corner_radius"}, context);
  text::TextBoxBackground result;
  result.enabled = OptionalBool(value, "enabled", result.enabled);
  if (const auto found = value.find("color"); found != value.end())
    result.color = ParseColor(*found, context + ".color");
  if (const auto found = value.find("padding"); found != value.end())
    result.padding = ParseInsets(*found, context + ".padding");
  result.cornerRadius =
      OptionalFloat(value, "corner_radius", result.cornerRadius);
  return result;
}

text::TextDecorationLine ParseDecorationLine(const Json &value,
                                             const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be an object");
  OnlyKeys(value,
           {"enabled", "material", "thickness", "offset", "style",
            "skip_ink"},
           context);
  text::TextDecorationLine result;
  result.enabled = OptionalBool(value, "enabled", result.enabled);
  if (const auto found = value.find("material"); found != value.end())
    result.material = ParseMaterialBinding(*found, context + ".material");
  result.thickness = OptionalFloat(value, "thickness", result.thickness);
  result.offset = OptionalFloat(value, "offset", result.offset);
  result.style = OptionalEnum<text::TextDecorationLineStyle>(
      value, "style", result.style,
      {{"solid", text::TextDecorationLineStyle::Solid},
       {"double", text::TextDecorationLineStyle::Double},
       {"dotted", text::TextDecorationLineStyle::Dotted},
       {"dashed", text::TextDecorationLineStyle::Dashed},
       {"wavy", text::TextDecorationLineStyle::Wavy}});
  result.skipInk = OptionalBool(value, "skip_ink", result.skipInk);
  return result;
}

text::FontReference ParseFontReference(const Json &value,
                                       const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be a font reference");
  OnlyKeys(value,
           {"kind", "family", "postscript_name", "weight", "width", "slant",
            "face_index", "variation_axes", "asset_id", "platform", "digest",
            "face_fingerprint", "allow_system_glyph_fallback"},
           context);
  text::FontReference result;
  result.kind = OptionalEnum<text::FontSourceKind>(
      value, "kind", result.kind,
      {{"builtin", text::FontSourceKind::Builtin},
       {"project_managed", text::FontSourceKind::ProjectManaged},
       {"system", text::FontSourceKind::System}});
  result.family = OptionalString(value, "family");
  result.postscriptName = OptionalString(value, "postscript_name");
  result.weight = static_cast<std::int32_t>(
      OptionalInteger(value, "weight", result.weight));
  result.width = static_cast<std::int32_t>(
      OptionalInteger(value, "width", result.width));
  result.slant = OptionalEnum<text::FontSlant>(
      value, "slant", result.slant,
      {{"upright", text::FontSlant::Upright},
       {"italic", text::FontSlant::Italic},
       {"oblique", text::FontSlant::Oblique}});
  result.faceIndex = static_cast<std::uint32_t>(
      OptionalUnsigned(value, "face_index", result.faceIndex));
  result.assetId = OptionalString(value, "asset_id");
  result.platform = OptionalString(value, "platform");
  result.digest = OptionalString(value, "digest");
  result.faceFingerprint = OptionalString(value, "face_fingerprint");
  result.allowSystemGlyphFallback = OptionalBool(
      value, "allow_system_glyph_fallback", result.allowSystemGlyphFallback);
  if (const auto found = value.find("variation_axes"); found != value.end()) {
    if (!found->is_array())
      Invalid(context + ".variation_axes", "must be an array");
    for (const auto &axis : *found) {
      OnlyKeys(axis, {"tag", "value"}, context + ".variation_axes");
      result.variationAxes.push_back(
          {String(axis, "tag", context),
           OptionalFloat(axis, "value", 0.0F)});
    }
  }
  return result;
}

text::FontSpec ParseFontSpec(const Json &value, const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be an object");
  OnlyKeys(value,
           {"primary", "fallbacks", "family", "postscript_name", "weight",
            "width", "slant", "face_index", "variation_axes", "features",
            "allow_system_glyph_fallback"},
           context);
  text::FontSpec result;
  result.primary =
      ParseFontReference(Object(value, "primary", context), context + ".primary");
  if (const auto found = value.find("fallbacks"); found != value.end()) {
    if (!found->is_array())
      Invalid(context + ".fallbacks", "must be an array");
    for (std::size_t index = 0U; index < found->size(); ++index)
      result.fallbacks.push_back(ParseFontReference(
          (*found)[index], context + ".fallbacks[" + std::to_string(index) + "]"));
  }
  result.family = OptionalString(value, "family");
  result.postscriptName = OptionalString(value, "postscript_name");
  result.weight = static_cast<std::int32_t>(
      OptionalInteger(value, "weight", result.weight));
  result.width = static_cast<std::int32_t>(
      OptionalInteger(value, "width", result.width));
  result.slant = OptionalEnum<text::FontSlant>(
      value, "slant", result.slant,
      {{"upright", text::FontSlant::Upright},
       {"italic", text::FontSlant::Italic},
       {"oblique", text::FontSlant::Oblique}});
  result.faceIndex = static_cast<std::uint32_t>(
      OptionalUnsigned(value, "face_index", result.faceIndex));
  result.allowSystemGlyphFallback = OptionalBool(
      value, "allow_system_glyph_fallback", result.allowSystemGlyphFallback);
  if (const auto found = value.find("variation_axes"); found != value.end()) {
    if (!found->is_array())
      Invalid(context + ".variation_axes", "must be an array");
    for (const auto &axis : *found) {
      OnlyKeys(axis, {"tag", "value"}, context + ".variation_axes");
      result.variationAxes.push_back(
          {String(axis, "tag", context), OptionalFloat(axis, "value", 0.0F)});
    }
  }
  if (const auto found = value.find("features"); found != value.end()) {
    if (!found->is_array())
      Invalid(context + ".features", "must be an array");
    for (const auto &feature : *found) {
      OnlyKeys(feature, {"tag", "value"}, context + ".features");
      result.features.push_back(
          {String(feature, "tag", context),
           static_cast<std::uint32_t>(
               OptionalUnsigned(feature, "value", 1U))});
    }
  }
  return result;
}

text::TextStyle ParseTextStyle(const Json &value,
                               const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be an object");
  OnlyKeys(value,
           {"font", "font_size", "letter_spacing", "word_spacing",
            "baseline_shift", "materials", "background", "decoration"},
           context);
  text::TextStyle result;
  result.font = ParseFontSpec(Object(value, "font", context),
                              context + ".font");
  result.fontSize = OptionalFloat(value, "font_size", result.fontSize);
  result.letterSpacing =
      OptionalFloat(value, "letter_spacing", result.letterSpacing);
  result.wordSpacing =
      OptionalFloat(value, "word_spacing", result.wordSpacing);
  result.baselineShift =
      OptionalFloat(value, "baseline_shift", result.baselineShift);
  if (const auto found = value.find("materials"); found != value.end()) {
    OnlyKeys(*found, {"layers"}, context + ".materials");
    const auto &layers = Array(*found, "layers", context + ".materials");
    result.materials.layers.clear();
    for (std::size_t index = 0U; index < layers.size(); ++index)
      result.materials.layers.push_back(ParseGlyphLayer(
          layers[index], context + ".materials.layers[" +
                             std::to_string(index) + "]"));
  }
  if (const auto found = value.find("background"); found != value.end())
    result.background = ParseBoxBackground(*found, context + ".background");
  if (const auto found = value.find("decoration"); found != value.end()) {
    if (!found->is_object())
      Invalid(context + ".decoration", "must be an object");
    OnlyKeys(*found, {"underline", "strike_through"},
             context + ".decoration");
    if (const auto underline = found->find("underline");
        underline != found->end())
      result.decoration.underline = ParseDecorationLine(
          *underline, context + ".decoration.underline");
    if (const auto strike = found->find("strike_through");
        strike != found->end())
      result.decoration.strikeThrough = ParseDecorationLine(
          *strike, context + ".decoration.strike_through");
  }
  return result;
}

text::ParagraphStyle ParseParagraphStyle(const Json &value,
                                         const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be an object");
  OnlyKeys(value,
           {"alignment", "direction", "locale", "max_lines", "overflow",
            "line_height", "wrap", "line_break_policy", "hyphenation",
            "first_line_indent", "start_indent", "end_indent",
            "spacing_before", "spacing_after", "hanging_punctuation",
            "tab_stops", "background"},
           context);
  text::ParagraphStyle result;
  result.alignment = OptionalEnum<text::TextAlignment>(
      value, "alignment", result.alignment,
      {{"start", text::TextAlignment::Start},
       {"center", text::TextAlignment::Center},
       {"end", text::TextAlignment::End},
       {"justify", text::TextAlignment::Justify}});
  result.direction = OptionalEnum<text::TextDirection>(
      value, "direction", result.direction,
      {{"auto", text::TextDirection::Auto},
       {"left_to_right", text::TextDirection::LeftToRight},
       {"right_to_left", text::TextDirection::RightToLeft}});
  result.locale = OptionalString(value, "locale", result.locale);
  if (const auto found = value.find("max_lines"); found != value.end())
    result.maximumLines = static_cast<std::uint32_t>(
        Unsigned(*found, context + ".max_lines"));
  result.overflow = OptionalEnum<text::TextOverflow>(
      value, "overflow", result.overflow,
      {{"clip", text::TextOverflow::Clip},
       {"ellipsis", text::TextOverflow::Ellipsis},
       {"visible", text::TextOverflow::Visible}});
  result.lineHeight = OptionalFloat(value, "line_height", result.lineHeight);
  result.wrap = OptionalEnum<text::TextWrap>(
      value, "wrap", result.wrap,
      {{"word", text::TextWrap::Word},
       {"character", text::TextWrap::Character},
       {"none", text::TextWrap::None}});
  result.lineBreakPolicy = OptionalEnum<text::TextLineBreakPolicy>(
      value, "line_break_policy", result.lineBreakPolicy,
      {{"unicode", text::TextLineBreakPolicy::Unicode},
       {"cjk_strict", text::TextLineBreakPolicy::CjkStrict},
       {"cjk_loose", text::TextLineBreakPolicy::CjkLoose},
       {"anywhere", text::TextLineBreakPolicy::Anywhere}});
  result.hyphenation = OptionalEnum<text::TextHyphenation>(
      value, "hyphenation", result.hyphenation,
      {{"none", text::TextHyphenation::None},
       {"automatic", text::TextHyphenation::Automatic}});
  result.firstLineIndent =
      OptionalFloat(value, "first_line_indent", result.firstLineIndent);
  result.startIndent =
      OptionalFloat(value, "start_indent", result.startIndent);
  result.endIndent = OptionalFloat(value, "end_indent", result.endIndent);
  result.spacingBefore =
      OptionalFloat(value, "spacing_before", result.spacingBefore);
  result.spacingAfter =
      OptionalFloat(value, "spacing_after", result.spacingAfter);
  result.hangingPunctuation = OptionalBool(
      value, "hanging_punctuation", result.hangingPunctuation);
  if (const auto found = value.find("tab_stops"); found != value.end()) {
    if (!found->is_array())
      Invalid(context + ".tab_stops", "must be an array");
    for (const auto &tab : *found) {
      OnlyKeys(tab, {"position", "alignment", "decimal_character"},
               context + ".tab_stops");
      text::TextTabStop decoded;
      decoded.position = OptionalFloat(tab, "position", decoded.position);
      decoded.alignment = OptionalEnum<text::TextAlignment>(
          tab, "alignment", decoded.alignment,
          {{"start", text::TextAlignment::Start},
           {"center", text::TextAlignment::Center},
           {"end", text::TextAlignment::End},
           {"justify", text::TextAlignment::Justify}});
      const auto decimal = OptionalString(tab, "decimal_character", ".");
      if (decimal.size() != 1U)
        Invalid(context + ".tab_stops.decimal_character",
                "must be one ASCII character");
      decoded.decimalCharacter =
          static_cast<char32_t>(static_cast<unsigned char>(decimal.front()));
      result.tabStops.push_back(decoded);
    }
  }
  if (const auto found = value.find("background"); found != value.end())
    result.background = ParseBoxBackground(*found, context + ".background");
  return result;
}

text::LayoutBox ParseLayoutBox(const Json &value,
                               const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be an object");
  OnlyKeys(value,
           {"x", "y", "width", "height", "sizing_mode", "minimum_width",
            "minimum_height", "maximum_width", "maximum_height",
            "minimum_fit_font_size", "maximum_fit_font_size",
            "fit_measurement_outsets", "fit_origin_x", "padding",
            "vertical_alignment", "clip_overflow", "pixel_snap"},
           context);
  text::LayoutBox result;
  result.x = OptionalFloat(value, "x", result.x);
  result.y = OptionalFloat(value, "y", result.y);
  result.width = OptionalFloat(value, "width", result.width);
  result.height = OptionalFloat(value, "height", result.height);
  result.sizingMode = OptionalEnum<text::TextLayoutSizingMode>(
      value, "sizing_mode", result.sizingMode,
      {{"auto_width", text::TextLayoutSizingMode::AutoWidth},
       {"auto_height", text::TextLayoutSizingMode::AutoHeight},
       {"fixed", text::TextLayoutSizingMode::Fixed},
       {"fit_text", text::TextLayoutSizingMode::FitText}});
  result.minimumWidth =
      OptionalFloat(value, "minimum_width", result.minimumWidth);
  result.minimumHeight =
      OptionalFloat(value, "minimum_height", result.minimumHeight);
  if (const auto found = value.find("maximum_width"); found != value.end())
    result.maximumWidth = Float(*found, context + ".maximum_width");
  if (const auto found = value.find("maximum_height"); found != value.end())
    result.maximumHeight = Float(*found, context + ".maximum_height");
  result.minimumFitFontSize = OptionalFloat(
      value, "minimum_fit_font_size", result.minimumFitFontSize);
  result.maximumFitFontSize = OptionalFloat(
      value, "maximum_fit_font_size", result.maximumFitFontSize);
  if (const auto found = value.find("fit_measurement_outsets");
      found != value.end()) {
    result.fitMeasurementOutsets =
        ParseInsets(*found, context + ".fit_measurement_outsets");
  }
  if (const auto found = value.find("fit_origin_x"); found != value.end())
    result.fitOriginX = Float(*found, context + ".fit_origin_x");
  if (const auto found = value.find("padding"); found != value.end())
    result.padding = ParseInsets(*found, context + ".padding");
  result.verticalAlignment = OptionalEnum<text::VerticalAlignment>(
      value, "vertical_alignment", result.verticalAlignment,
      {{"top", text::VerticalAlignment::Top},
       {"center", text::VerticalAlignment::Center},
       {"bottom", text::VerticalAlignment::Bottom}});
  result.clipOverflow =
      OptionalBool(value, "clip_overflow", result.clipOverflow);
  result.pixelSnap = OptionalBool(value, "pixel_snap", result.pixelSnap);
  return result;
}

text::TextLayerAnimationClip ParseLayerClip(const Json &value,
                                            const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be an object");
  OnlyKeys(value, {"clip_id", "preset_id", "phase", "duration_us", "tracks"},
           context);
  text::TextLayerAnimationClip result;
  result.clipId = String(value, "clip_id", context);
  result.presetId = OptionalString(value, "preset_id");
  result.phase = OptionalEnum<text::TextLayerAnimationPhase>(
      value, "phase", result.phase,
      {{"enter", text::TextLayerAnimationPhase::Enter},
       {"loop", text::TextLayerAnimationPhase::Loop},
       {"exit", text::TextLayerAnimationPhase::Exit},
       {"caption", text::TextLayerAnimationPhase::Caption}});
  result.durationUs = OptionalInteger(value, "duration_us", result.durationUs);
  if (const auto found = value.find("tracks"); found != value.end()) {
    if (!found->is_array())
      Invalid(context + ".tracks", "must be an array");
    for (const auto &track : *found) {
      OnlyKeys(track, {"property", "keyframes"}, context + ".tracks");
      text::TextLayerAnimationTrack decoded;
      decoded.property = OptionalEnum<text::TextLayerAnimationProperty>(
          track, "property", decoded.property,
          {{"opacity", text::TextLayerAnimationProperty::Opacity},
           {"position_x", text::TextLayerAnimationProperty::PositionX},
           {"position_y", text::TextLayerAnimationProperty::PositionY},
           {"scale_x", text::TextLayerAnimationProperty::ScaleX},
           {"scale_y", text::TextLayerAnimationProperty::ScaleY},
           {"rotation_degrees",
            text::TextLayerAnimationProperty::RotationDegrees}});
      for (const auto &keyframe : Array(track, "keyframes", context)) {
        OnlyKeys(keyframe, {"offset", "value", "easing"},
                 context + ".keyframes");
        text::TextLayerAnimationKeyframe key;
        key.offset = OptionalFloat(keyframe, "offset", key.offset);
        key.value = OptionalFloat(keyframe, "value", key.value);
        key.easing = OptionalEnum<text::TextAnimationEasing>(
            keyframe, "easing", key.easing,
            {{"linear", text::TextAnimationEasing::Linear},
             {"ease_in", text::TextAnimationEasing::EaseIn},
             {"ease_out", text::TextAnimationEasing::EaseOut},
             {"ease_in_out", text::TextAnimationEasing::EaseInOut}});
        decoded.keyframes.push_back(key);
      }
      result.tracks.push_back(std::move(decoded));
    }
  }
  return result;
}

text::TextBackdropFitPolicy ParseBackdropFit(
    const Json &parent, const char *key,
    const text::TextBackdropFitPolicy fallback) {
  return OptionalEnum<text::TextBackdropFitPolicy>(
      parent, key, fallback,
      {{"ink_bounds", text::TextBackdropFitPolicy::InkBounds},
       {"line_union", text::TextBackdropFitPolicy::LineUnion},
       {"layout_bounds", text::TextBackdropFitPolicy::LayoutBounds}});
}

text::TextBackdropSource ParseBackdropSource(const Json &value,
                                             const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be an object");
  const auto type = String(value, "kind", context);
  if (type == "rounded_rect") {
    OnlyKeys(value,
             {"kind", "family", "fill", "strokes", "corner_radius", "tail",
              "authored_width", "authored_height"},
             context);
    text::RoundedRectBackdrop result;
    result.family = OptionalEnum<text::BubbleFamily>(
        value, "family", result.family,
        {{"round_rect", text::BubbleFamily::RoundRect},
         {"pill", text::BubbleFamily::Pill},
         {"speech", text::BubbleFamily::Speech},
         {"cloud", text::BubbleFamily::Cloud},
         {"spike", text::BubbleFamily::Spike},
         {"caption_bar", text::BubbleFamily::CaptionBar}});
    result.fill = ParseMaterialBinding(Object(value, "fill", context),
                                       context + ".fill");
    if (const auto found = value.find("strokes"); found != value.end()) {
      if (!found->is_array())
        Invalid(context + ".strokes", "must be an array");
      for (std::size_t index = 0U; index < found->size(); ++index) {
        const auto layer = ParseGlyphLayer(
            (*found)[index], context + ".strokes[" +
                                 std::to_string(index) + "]");
        const auto *stroke = std::get_if<text::TextStrokeLayer>(&layer);
        if (!stroke)
          Invalid(context + ".strokes", "must contain stroke layers");
        result.strokes.push_back(*stroke);
      }
    }
    result.cornerRadius =
        OptionalFloat(value, "corner_radius", result.cornerRadius);
    if (const auto found = value.find("tail"); found != value.end()) {
      OnlyKeys(*found, {"edge", "position", "width", "length"},
               context + ".tail");
      result.tail.edge = OptionalEnum<text::BubbleTailEdge>(
          *found, "edge", result.tail.edge,
          {{"none", text::BubbleTailEdge::None},
           {"top", text::BubbleTailEdge::Top},
           {"bottom", text::BubbleTailEdge::Bottom},
           {"left", text::BubbleTailEdge::Left},
           {"right", text::BubbleTailEdge::Right}});
      result.tail.position =
          OptionalFloat(*found, "position", result.tail.position);
      result.tail.width = OptionalFloat(*found, "width", result.tail.width);
      result.tail.length = OptionalFloat(*found, "length", result.tail.length);
    }
    if (const auto found = value.find("authored_width"); found != value.end())
      result.authoredWidth = Float(*found, context + ".authored_width");
    if (const auto found = value.find("authored_height"); found != value.end())
      result.authoredHeight = Float(*found, context + ".authored_height");
    return result;
  }
  if (type == "nine_slice") {
    OnlyKeys(value,
             {"kind", "asset", "cap_insets", "content_insets",
              "minimum_content_width", "minimum_content_height",
              "stretch_mode", "fallback_color"},
             context);
    text::NineSliceBackdrop result;
    result.asset = ParseTexture(Object(value, "asset", context),
                                context + ".asset");
    if (const auto found = value.find("cap_insets"); found != value.end())
      result.capInsets = ParseInsets(*found, context + ".cap_insets");
    if (const auto found = value.find("content_insets"); found != value.end())
      result.contentInsets = ParseInsets(*found, context + ".content_insets");
    result.minimumContentWidth = OptionalFloat(
        value, "minimum_content_width", result.minimumContentWidth);
    result.minimumContentHeight = OptionalFloat(
        value, "minimum_content_height", result.minimumContentHeight);
    result.stretchMode = OptionalEnum<text::TextBackdropStretchMode>(
        value, "stretch_mode", result.stretchMode,
        {{"nine_slice", text::TextBackdropStretchMode::NineSlice},
         {"stretch", text::TextBackdropStretchMode::Stretch},
         {"tile_center", text::TextBackdropStretchMode::TileCenter}});
    if (const auto found = value.find("fallback_color"); found != value.end())
      result.fallbackColor =
          ParseColor(*found, context + ".fallback_color");
    return result;
  }
  if (type == "vector") {
    OnlyKeys(value, {"kind", "asset", "fit", "fallback_color"}, context);
    text::VectorBackdrop result;
    result.asset = ParseTexture(Object(value, "asset", context),
                                context + ".asset");
    result.fit = ParseBackdropFit(value, "fit", result.fit);
    if (const auto found = value.find("fallback_color"); found != value.end())
      result.fallbackColor =
          ParseColor(*found, context + ".fallback_color");
    return result;
  }
  if (type == "animated") {
    OnlyKeys(value,
             {"kind", "asset", "content_insets", "time_source", "playback",
              "source_in_us", "source_out_us", "phase_us", "fallback_color"},
             context);
    text::AnimatedBackdrop result;
    result.asset = ParseTexture(Object(value, "asset", context),
                                context + ".asset");
    if (const auto found = value.find("content_insets"); found != value.end())
      result.contentInsets = ParseInsets(*found, context + ".content_insets");
    result.timeSource = OptionalEnum<text::TextBackdropTimeSource>(
        value, "time_source", result.timeSource,
        {{"composition", text::TextBackdropTimeSource::Composition},
         {"animation_layer", text::TextBackdropTimeSource::AnimationLayer},
         {"source", text::TextBackdropTimeSource::Source}});
    result.playback = OptionalEnum<text::TextBackdropPlaybackMode>(
        value, "playback", result.playback,
        {{"once", text::TextBackdropPlaybackMode::Once},
         {"loop", text::TextBackdropPlaybackMode::Loop},
         {"ping_pong", text::TextBackdropPlaybackMode::PingPong},
         {"hold", text::TextBackdropPlaybackMode::Hold}});
    result.sourceInUs =
        OptionalInteger(value, "source_in_us", result.sourceInUs);
    result.sourceOutUs =
        OptionalInteger(value, "source_out_us", result.sourceOutUs);
    result.phaseUs = OptionalInteger(value, "phase_us", result.phaseUs);
    if (const auto found = value.find("fallback_color"); found != value.end())
      result.fallbackColor =
          ParseColor(*found, context + ".fallback_color");
    return result;
  }
  Invalid(context, "contains an unsupported backdrop type");
}

text::TextBackdropLayer ParseBackdropLayer(const Json &value,
                                           const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be an object");
  OnlyKeys(value,
           {"layer_id", "channel", "enabled", "source", "material_override",
            "padding", "fit", "source_intrinsic_width",
            "source_intrinsic_height", "fit_mode", "source_pivot_x",
            "source_pivot_y", "expand", "source_outsets", "transform",
            "z_order", "animation_clips"},
           context);
  text::TextBackdropLayer result;
  result.layerId = String(value, "layer_id", context);
  result.channel = OptionalEnum<text::TextBackdropChannel>(
      value, "channel", result.channel,
      {{"frame", text::TextBackdropChannel::Frame},
       {"bubble", text::TextBackdropChannel::Bubble},
       {"custom", text::TextBackdropChannel::Custom}});
  result.enabled = OptionalBool(value, "enabled", result.enabled);
  result.source = ParseBackdropSource(Object(value, "source", context),
                                      context + ".source");
  if (const auto found = value.find("material_override");
      found != value.end())
    result.materialOverride =
        ParseMaterialBinding(*found, context + ".material_override");
  if (const auto found = value.find("padding"); found != value.end())
    result.padding = ParseInsets(*found, context + ".padding");
  result.fit = ParseBackdropFit(value, "fit", result.fit);
  if (const auto found = value.find("source_intrinsic_width");
      found != value.end()) {
    result.sourceIntrinsicWidth = OptionalFloat(
        value, "source_intrinsic_width", 0.0F);
  }
  if (const auto found = value.find("source_intrinsic_height");
      found != value.end()) {
    result.sourceIntrinsicHeight = OptionalFloat(
        value, "source_intrinsic_height", 0.0F);
  }
  result.fitMode = OptionalEnum<text::TextBackdropFitMode>(
      value, "fit_mode", result.fitMode,
      {{"width", text::TextBackdropFitMode::Width},
       {"height", text::TextBackdropFitMode::Height},
       {"long_side", text::TextBackdropFitMode::LongSide},
       {"short_side", text::TextBackdropFitMode::ShortSide},
       {"stretch", text::TextBackdropFitMode::Stretch}});
  result.sourcePivotX =
      OptionalFloat(value, "source_pivot_x", result.sourcePivotX);
  result.sourcePivotY =
      OptionalFloat(value, "source_pivot_y", result.sourcePivotY);
  if (const auto found = value.find("expand"); found != value.end())
    result.expand = ParseInsets(*found, context + ".expand");
  if (const auto found = value.find("source_outsets"); found != value.end())
    result.sourceOutsets = ParseInsets(*found, context + ".source_outsets");
  if (const auto found = value.find("transform"); found != value.end()) {
    OnlyKeys(*found,
             {"offset_x", "offset_y", "scale_x", "scale_y",
              "rotation_degrees", "opacity"},
             context + ".transform");
    result.transform.offsetX =
        OptionalFloat(*found, "offset_x", result.transform.offsetX);
    result.transform.offsetY =
        OptionalFloat(*found, "offset_y", result.transform.offsetY);
    result.transform.scaleX =
        OptionalFloat(*found, "scale_x", result.transform.scaleX);
    result.transform.scaleY =
        OptionalFloat(*found, "scale_y", result.transform.scaleY);
    result.transform.rotationDegrees = OptionalFloat(
        *found, "rotation_degrees", result.transform.rotationDegrees);
    result.transform.opacity =
        OptionalFloat(*found, "opacity", result.transform.opacity);
  }
  result.zOrder = static_cast<std::int32_t>(
      OptionalInteger(value, "z_order", result.zOrder));
  if (const auto found = value.find("animation_clips"); found != value.end()) {
    if (!found->is_array())
      Invalid(context + ".animation_clips", "must be an array");
    for (std::size_t index = 0U; index < found->size(); ++index)
      result.animationClips.push_back(ParseLayerClip(
          (*found)[index], context + ".animation_clips[" +
                               std::to_string(index) + "]"));
  }
  return result;
}

text::TextLayerAppearance ParseAppearance(const Json &value,
                                          const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be an object");
  OnlyKeys(value, {"backdrops", "sdf_material", "bend", "path",
                   "visual_extent", "global_alpha"},
           context);
  text::TextLayerAppearance result;
  const auto &backdrops = Object(value, "backdrops", context);
  OnlyKeys(backdrops, {"layers"}, context + ".backdrops");
  for (std::size_t index = 0U;
       index < Array(backdrops, "layers", context + ".backdrops").size();
       ++index) {
    const auto &layers = Array(backdrops, "layers", context + ".backdrops");
    result.backdrops.layers.push_back(ParseBackdropLayer(
        layers[index], context + ".backdrops.layers[" +
                           std::to_string(index) + "]"));
  }
  const auto &sdf = Object(value, "sdf_material", context);
  OnlyKeys(sdf,
           {"enabled", "distance_range", "raster_distance_range",
            "smoothing_scale", "source_design_width", "source_design_height",
            "source_creation_component"},
           context + ".sdf_material");
  result.sdfMaterial.enabled =
      OptionalBool(sdf, "enabled", result.sdfMaterial.enabled);
  const auto sourceCreationComponent = OptionalString(
      sdf, "source_creation_component", "legacy_text");
  if (sourceCreationComponent == "legacy_text") {
    result.sdfMaterial.sourceCreationComponent =
        text::TextSourceCreationComponent::LegacyText;
  } else if (sourceCreationComponent == "sdf_text") {
    result.sdfMaterial.sourceCreationComponent =
        text::TextSourceCreationComponent::SdfText;
  } else {
    Invalid(context + ".sdf_material.source_creation_component",
            "must be legacy_text or sdf_text");
  }
  result.sdfMaterial.distanceRange = OptionalFloat(
      sdf, "distance_range", result.sdfMaterial.distanceRange);
  result.sdfMaterial.rasterDistanceRange = OptionalFloat(
      sdf, "raster_distance_range", result.sdfMaterial.rasterDistanceRange);
  result.sdfMaterial.smoothingScale = OptionalFloat(
      sdf, "smoothing_scale", result.sdfMaterial.smoothingScale);
  result.sdfMaterial.sourceDesignWidth = OptionalFloat(
      sdf, "source_design_width", result.sdfMaterial.sourceDesignWidth);
  result.sdfMaterial.sourceDesignHeight = OptionalFloat(
      sdf, "source_design_height", result.sdfMaterial.sourceDesignHeight);
  const auto &bend = Object(value, "bend", context);
  OnlyKeys(bend, {"enabled", "amount"}, context + ".bend");
  result.bend.enabled = OptionalBool(bend, "enabled", result.bend.enabled);
  result.bend.amount = OptionalFloat(bend, "amount", result.bend.amount);
  const auto &path = Object(value, "path", context);
  OnlyKeys(path,
           {"enabled", "geometry_id", "preset_id", "commands",
            "start_offset", "baseline_offset", "overflow", "loop",
            "rotate_to_tangent", "keep_upright"},
           context + ".path");
  result.path.enabled = OptionalBool(path, "enabled", result.path.enabled);
  result.path.geometryId = OptionalString(path, "geometry_id");
  result.path.presetId = OptionalString(path, "preset_id");
  if (const auto found = path.find("commands"); found != path.end()) {
    if (!found->is_array())
      Invalid(context + ".path.commands", "must be an array");
    for (const auto &command : *found) {
      OnlyKeys(command, {"kind", "control1", "control2", "end"},
               context + ".path.commands");
      text::TextPathCommand decoded;
      decoded.kind = OptionalEnum<text::TextPathCommandKind>(
          command, "kind", decoded.kind,
          {{"move_to", text::TextPathCommandKind::MoveTo},
           {"line_to", text::TextPathCommandKind::LineTo},
           {"quadratic_to", text::TextPathCommandKind::QuadraticTo},
           {"cubic_to", text::TextPathCommandKind::CubicTo},
           {"close", text::TextPathCommandKind::Close}});
      const auto point = [&](const char *key, text::TextPathPoint &target) {
        if (const auto item = command.find(key); item != command.end()) {
          const auto [x, y] = ParsePoint(*item, context + ".path.commands");
          target = {x, y};
        }
      };
      point("control1", decoded.control1);
      point("control2", decoded.control2);
      point("end", decoded.end);
      result.path.commands.push_back(decoded);
    }
  }
  result.path.startOffset =
      OptionalFloat(path, "start_offset", result.path.startOffset);
  result.path.baselineOffset =
      OptionalFloat(path, "baseline_offset", result.path.baselineOffset);
  result.path.overflow = OptionalEnum<text::TextPathOverflow>(
      path, "overflow", result.path.overflow,
      {{"clip", text::TextPathOverflow::Clip},
       {"visible", text::TextPathOverflow::Visible},
       {"scale_to_fit", text::TextPathOverflow::ScaleToFit}});
  result.path.loop = OptionalBool(path, "loop", result.path.loop);
  result.path.rotateToTangent = OptionalBool(
      path, "rotate_to_tangent", result.path.rotateToTangent);
  result.path.keepUpright =
      OptionalBool(path, "keep_upright", result.path.keepUpright);
  const auto &extent = Object(value, "visual_extent", context);
  OnlyKeys(extent,
           {"allow_control_overflow", "maximum_extent_width",
            "maximum_extent_height"},
           context + ".visual_extent");
  result.visualExtent.allowControlOverflow = OptionalBool(
      extent, "allow_control_overflow",
      result.visualExtent.allowControlOverflow);
  if (const auto found = extent.find("maximum_extent_width");
      found != extent.end())
    result.visualExtent.maximumExtentWidth =
        Float(*found, context + ".visual_extent.maximum_extent_width");
  if (const auto found = extent.find("maximum_extent_height");
      found != extent.end())
    result.visualExtent.maximumExtentHeight =
        Float(*found, context + ".visual_extent.maximum_extent_height");
  result.globalAlpha =
      OptionalFloat(value, "global_alpha", result.globalAlpha);
  return result;
}

vector::VectorFieldOverride ParseVectorOverride(const Json &value,
                                                const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be an object");
  OnlyKeys(value, {"field_id", "kind", "value"}, context);
  vector::VectorFieldOverride result;
  result.fieldId = String(value, "field_id", context);
  const auto kind = String(value, "kind", context);
  if (kind == "color") {
    result.value.kind = vector::VectorFieldKind::Color;
    const auto color = ParseColor(value.at("value"), context + ".value");
    result.value.color = {color.red, color.green, color.blue, color.alpha};
  } else if (kind == "text") {
    result.value.kind = vector::VectorFieldKind::Text;
    result.value.text = String(value, "value", context, true);
  } else if (kind == "number") {
    result.value.kind = vector::VectorFieldKind::Number;
    result.value.number = Number(value.at("value"), context + ".value");
  } else {
    Invalid(context, "contains an unsupported vector field kind");
  }
  return result;
}

DecorationAssetReference ParseDecorationAsset(const Json &value,
                                               const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be an object");
  OnlyKeys(value,
           {"asset_id", "digest", "media_type", "intrinsic_width",
            "intrinsic_height", "estimated_resource_bytes",
            "static_field_overrides"},
           context);
  DecorationAssetReference result;
  result.assetId = String(value, "asset_id", context);
  // A decoration's owning resource closes provenance below. Bundled resources
  // may omit a digest; project-managed providers remain digest-required by
  // TextCompositionDocument admission.
  result.digest = OptionalString(value, "digest");
  result.mediaType = String(value, "media_type", context);
  result.intrinsicWidth =
      OptionalFloat(value, "intrinsic_width", result.intrinsicWidth);
  result.intrinsicHeight =
      OptionalFloat(value, "intrinsic_height", result.intrinsicHeight);
  result.estimatedResourceBytes = OptionalUnsigned(
      value, "estimated_resource_bytes", result.estimatedResourceBytes);
  if (const auto found = value.find("static_field_overrides");
      found != value.end()) {
    if (!found->is_array())
      Invalid(context + ".static_field_overrides", "must be an array");
    for (std::size_t index = 0U; index < found->size(); ++index)
      result.staticFieldOverrides.push_back(ParseVectorOverride(
          (*found)[index], context + ".static_field_overrides[" +
                               std::to_string(index) + "]"));
  }
  return result;
}

VectorDecorationBinding ParseVectorDecoration(const Json &value,
                                              const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be an object");
  OnlyKeys(value,
           {"decoration_id", "enabled", "asset", "instance_pattern",
            "mode", "target", "placement_driver", "anchor", "fit",
            "padding", "local_transform", "asset_playback", "z_order",
            "transform_inherit", "sampling", "effect_scope", "fallback",
            "fallback_asset_id"},
           context);
  VectorDecorationBinding result;
  result.decorationId = String(value, "decoration_id", context);
  result.enabled = OptionalBool(value, "enabled", result.enabled);
  result.asset =
      ParseDecorationAsset(Object(value, "asset", context), context + ".asset");
  if (const auto found = value.find("instance_pattern"); found != value.end()) {
    OnlyKeys(*found,
             {"asset_variants", "selection_policy", "explicit_pattern",
              "random_seed", "offset_x_step", "offset_y_step",
              "scale_step", "rotation_step_degrees", "phase_policy",
              "phase_step_us"},
             context + ".instance_pattern");
    DecorationInstancePattern pattern;
    const auto &variants = Array(*found, "asset_variants", context);
    for (std::size_t index = 0U; index < variants.size(); ++index) {
      OnlyKeys(variants[index], {"variant_id", "asset"},
               context + ".instance_pattern.asset_variants[" +
                   std::to_string(index) + "]");
      DecorationAssetVariant variant;
      variant.variantId = String(variants[index], "variant_id", context);
      variant.asset = ParseDecorationAsset(
          Object(variants[index], "asset", context),
          context + ".instance_pattern.asset_variants[" +
              std::to_string(index) + "].asset");
      pattern.assetVariants.push_back(std::move(variant));
    }
    pattern.selectionPolicy =
        OptionalEnum<DecorationVariantSelectionPolicy>(
            *found, "selection_policy", pattern.selectionPolicy,
            {{"repeat_one", DecorationVariantSelectionPolicy::RepeatOne},
             {"cycle", DecorationVariantSelectionPolicy::Cycle},
             {"deterministic_hash",
              DecorationVariantSelectionPolicy::DeterministicHash},
             {"explicit_pattern",
              DecorationVariantSelectionPolicy::ExplicitPattern}});
    if (const auto sequence = found->find("explicit_pattern");
        sequence != found->end()) {
      if (!sequence->is_array())
        Invalid(context + ".instance_pattern.explicit_pattern",
                "must be an array");
      for (const auto &index : *sequence)
        pattern.explicitPattern.push_back(static_cast<std::uint32_t>(
            Unsigned(index, context + ".explicit_pattern")));
    }
    pattern.randomSeed = static_cast<std::uint32_t>(
        OptionalUnsigned(*found, "random_seed", pattern.randomSeed));
    pattern.offsetXStep =
        OptionalFloat(*found, "offset_x_step", pattern.offsetXStep);
    pattern.offsetYStep =
        OptionalFloat(*found, "offset_y_step", pattern.offsetYStep);
    pattern.scaleStep =
        OptionalFloat(*found, "scale_step", pattern.scaleStep);
    pattern.rotationStepDegrees = OptionalFloat(
        *found, "rotation_step_degrees", pattern.rotationStepDegrees);
    pattern.phasePolicy = OptionalEnum<DecorationPerIndexPhasePolicy>(
        *found, "phase_policy", pattern.phasePolicy,
        {{"shared", DecorationPerIndexPhasePolicy::Shared},
         {"stagger", DecorationPerIndexPhasePolicy::Stagger},
         {"deterministic_seed",
          DecorationPerIndexPhasePolicy::DeterministicSeed}});
    pattern.phaseStepUs =
        OptionalInteger(*found, "phase_step_us", pattern.phaseStepUs);
    result.instancePattern = std::move(pattern);
  }
  result.mode = OptionalEnum<VectorDecorationMode>(
      value, "mode", result.mode,
      {{"tracked_grapheme", VectorDecorationMode::TrackedGrapheme},
       {"sentence_envelope", VectorDecorationMode::SentenceEnvelope},
       {"per_grapheme_background",
        VectorDecorationMode::PerGraphemeBackground},
       {"composition_overlay", VectorDecorationMode::CompositionOverlay}});
  if (const auto found = value.find("target"); found != value.end()) {
    OnlyKeys(*found, {"scope", "paragraph_id", "run_id", "range"},
             context + ".target");
    result.target.scope = OptionalEnum<DecorationTargetScope>(
        *found, "scope", result.target.scope,
        {{"all_text", DecorationTargetScope::AllText},
         {"utf8_range", DecorationTargetScope::Utf8Range}});
    result.target.paragraphId = OptionalString(*found, "paragraph_id");
    result.target.runId = OptionalString(*found, "run_id");
    if (const auto range = found->find("range"); range != found->end()) {
      OnlyKeys(*range, {"begin", "end"}, context + ".target.range");
      result.target.range.begin =
          OptionalUnsigned(*range, "begin", result.target.range.begin);
      result.target.range.end =
          OptionalUnsigned(*range, "end", result.target.range.end);
    }
  }
  result.placementDriver = OptionalEnum<DecorationPlacementDriver>(
      value, "placement_driver", result.placementDriver,
      {{"sentence_uniform_progress",
        DecorationPlacementDriver::SentenceUniformProgress},
       {"geometry_only", DecorationPlacementDriver::GeometryOnly}});
  result.anchor = OptionalEnum<DecorationAnchor>(
      value, "anchor", result.anchor,
      {{"above", DecorationAnchor::Above},
       {"below", DecorationAnchor::Below},
       {"center", DecorationAnchor::Center},
       {"caret_path", DecorationAnchor::CaretPath}});
  result.fit = OptionalEnum<DecorationFit>(
      value, "fit", result.fit,
      {{"contain", DecorationFit::Contain},
       {"cover", DecorationFit::Cover},
       {"stretch", DecorationFit::Stretch},
       {"native", DecorationFit::Native}});
  if (const auto found = value.find("padding"); found != value.end())
    result.padding = ParseInsets(*found, context + ".padding");
  if (const auto found = value.find("local_transform"); found != value.end()) {
    OnlyKeys(*found,
             {"offset_x", "offset_y", "scale_x", "scale_y",
              "rotation_degrees", "opacity"},
             context + ".local_transform");
    result.localTransform.offsetX =
        OptionalFloat(*found, "offset_x", result.localTransform.offsetX);
    result.localTransform.offsetY =
        OptionalFloat(*found, "offset_y", result.localTransform.offsetY);
    result.localTransform.scaleX =
        OptionalFloat(*found, "scale_x", result.localTransform.scaleX);
    result.localTransform.scaleY =
        OptionalFloat(*found, "scale_y", result.localTransform.scaleY);
    result.localTransform.rotationDegrees = OptionalFloat(
        *found, "rotation_degrees", result.localTransform.rotationDegrees);
    result.localTransform.opacity =
        OptionalFloat(*found, "opacity", result.localTransform.opacity);
  }
  if (const auto found = value.find("asset_playback"); found != value.end()) {
    OnlyKeys(*found,
             {"clock", "mode", "source_in_us", "source_out_us", "speed",
              "phase_us"},
             context + ".asset_playback");
    result.assetPlayback.clock = OptionalEnum<DecorationAssetClock>(
        *found, "clock", result.assetPlayback.clock,
        {{"composition", DecorationAssetClock::Composition},
         {"source", DecorationAssetClock::Source},
         {"target", DecorationAssetClock::Target}});
    result.assetPlayback.mode = OptionalEnum<DecorationPlaybackMode>(
        *found, "mode", result.assetPlayback.mode,
        {{"loop", DecorationPlaybackMode::Loop},
         {"ping_pong", DecorationPlaybackMode::PingPong},
         {"once", DecorationPlaybackMode::Once},
         {"hold", DecorationPlaybackMode::Hold}});
    result.assetPlayback.sourceInUs = OptionalInteger(
        *found, "source_in_us", result.assetPlayback.sourceInUs);
    result.assetPlayback.sourceOutUs = OptionalInteger(
        *found, "source_out_us", result.assetPlayback.sourceOutUs);
    result.assetPlayback.speed =
        OptionalNumber(*found, "speed", result.assetPlayback.speed);
    result.assetPlayback.phaseUs =
        OptionalInteger(*found, "phase_us", result.assetPlayback.phaseUs);
  }
  result.zOrder = static_cast<std::int32_t>(
      OptionalInteger(value, "z_order", result.zOrder));
  result.transformInherit = OptionalEnum<DecorationTransformInherit>(
      value, "transform_inherit", result.transformInherit,
      {{"full", DecorationTransformInherit::Full},
       {"translate_only", DecorationTransformInherit::TranslateOnly}});
  result.sampling = OptionalEnum<DecorationSamplingPolicy>(
      value, "sampling", result.sampling,
      {{"automatic", DecorationSamplingPolicy::Automatic},
       {"linear_clamp", DecorationSamplingPolicy::LinearClamp}});
  result.effectScope = OptionalEnum<DecorationEffectScope>(
      value, "effect_scope", result.effectScope,
      {{"inside_group_behind_text",
        DecorationEffectScope::InsideGroupBehindText},
       {"inside_group_in_front_of_text",
        DecorationEffectScope::InsideGroupInFrontOfText},
       {"after_group_effect", DecorationEffectScope::AfterGroupEffect}});
  result.fallback = OptionalEnum<DecorationFallbackPolicy>(
      value, "fallback", result.fallback,
      {{"disable_decoration",
        DecorationFallbackPolicy::DisableDecoration},
       {"static_first_frame", DecorationFallbackPolicy::StaticFirstFrame},
       {"native_background", DecorationFallbackPolicy::NativeBackground},
       {"clip_visible_instances",
        DecorationFallbackPolicy::ClipVisibleInstances}});
  result.fallbackAssetId = OptionalString(value, "fallback_asset_id");
  return result;
}

TextResourceReference ParseResource(const Json &value,
                                    const std::string &context) {
  if (!value.is_object())
    Invalid(context, "must be an object");
  OnlyKeys(value,
           {"resource_id", "kind", "ownership", "asset_id", "digest",
            "media_type"},
           context);
  TextResourceReference result;
  result.resourceId = String(value, "resource_id", context);
  result.kind = OptionalEnum<TextResourceKind>(
      value, "kind", result.kind,
      {{"font", TextResourceKind::Font},
       {"texture", TextResourceKind::Texture},
       {"vector", TextResourceKind::Vector},
       {"animated", TextResourceKind::Animated},
       {"mesh", TextResourceKind::Mesh},
       {"float_texture", TextResourceKind::FloatTexture}});
  result.ownership = OptionalEnum<TextResourceOwnership>(
      value, "ownership", result.ownership,
      {{"builtin", TextResourceOwnership::Builtin},
       {"project_managed", TextResourceOwnership::ProjectManaged},
       {"system", TextResourceOwnership::System}});
  result.assetId = String(value, "asset_id", context);
  result.digest = OptionalString(value, "digest");
  if (result.ownership == TextResourceOwnership::ProjectManaged &&
      result.digest.empty()) {
    Invalid(context + ".digest",
            "is required for project-managed resources");
  }
  result.mediaType = String(value, "media_type", context);
  return result;
}

TextTemplateChannel ParseChannel(const Json &value,
                                 const std::string &context) {
  return EnumString<TextTemplateChannel>(
      value, context,
      {{"content", TextTemplateChannel::Content},
       {"typography", TextTemplateChannel::Typography},
       {"glyph.fill", TextTemplateChannel::GlyphFills},
       {"glyph.strokes", TextTemplateChannel::GlyphStrokes},
       {"glyph.shadows", TextTemplateChannel::GlyphShadows},
       {"glyph.glows", TextTemplateChannel::GlyphGlows},
       {"inline.decorations", TextTemplateChannel::InlineDecorations},
       {"paragraph", TextTemplateChannel::Paragraph},
       {"layout", TextTemplateChannel::Layout},
       {"bubble", TextTemplateChannel::Bubble},
       {"frame", TextTemplateChannel::Frame},
       {"custom.backdrops", TextTemplateChannel::CustomBackdrops},
       {"bend", TextTemplateChannel::Bend},
       {"path", TextTemplateChannel::Path},
       {"sdf.material", TextTemplateChannel::SdfMaterial},
       {"animations", TextTemplateChannel::Animations},
       {"decorations", TextTemplateChannel::VectorDecorations},
       {"timed.text", TextTemplateChannel::TimedText},
       {"resources", TextTemplateChannel::Resources}});
}

TextTemplateChannelMode ParseChannelMode(const Json &value,
                                         const std::string &context) {
  return EnumString<TextTemplateChannelMode>(
      value, context,
      {{"keep", TextTemplateChannelMode::Keep},
       {"merge", TextTemplateChannelMode::Merge},
       {"replace", TextTemplateChannelMode::Replace},
       {"clear", TextTemplateChannelMode::Clear}});
}

TextCompositionDocument ParseTemplateDocument(const Json &root) {
  TextCompositionDocument result;
  result.schemaRevision = text::kTextSchemaRevision;
  const auto &placement = Object(root, "placement", "template");
  OnlyKeys(placement,
           {"default_duration_ticks", "role", "reference_width",
            "reference_height", "scale_policy"},
           "template.placement");
  result.durationTicks = OptionalInteger(
      placement, "default_duration_ticks", 600'000);
  result.role = OptionalEnum<TextRole>(
      placement, "role", result.role,
      {{"normal", TextRole::Normal},
       {"caption", TextRole::Caption},
       {"lyrics", TextRole::Lyrics},
       {"title", TextRole::Title}});
  result.presentation.referenceCanvas.width = OptionalFloat(
      placement, "reference_width", result.presentation.referenceCanvas.width);
  result.presentation.referenceCanvas.height = OptionalFloat(
      placement, "reference_height",
      result.presentation.referenceCanvas.height);
  result.presentation.referenceCanvas.scalePolicy =
      OptionalEnum<text::CanvasScalePolicy>(
          placement, "scale_policy",
          result.presentation.referenceCanvas.scalePolicy,
          {{"fit", text::CanvasScalePolicy::Fit},
           {"fill", text::CanvasScalePolicy::Fill},
           {"none", text::CanvasScalePolicy::None}});

  const auto &document = Object(root, "document", "template");
  OnlyKeys(document,
           {"layout_box", "writing_mode", "content_slots", "paragraphs"},
           "template.document");
  result.presentation.authoredLayoutFrame = ParseLayoutBox(
      Object(document, "layout_box", "template.document"),
      "template.document.layout_box");
  result.presentation.writingMode = OptionalEnum<text::TextWritingMode>(
      document, "writing_mode", result.presentation.writingMode,
      {{"horizontal", text::TextWritingMode::Horizontal},
       {"vertical_right_to_left",
        text::TextWritingMode::VerticalRightToLeft},
       {"vertical_left_to_right",
        text::TextWritingMode::VerticalLeftToRight}});
  std::unordered_map<std::string, std::string> bindingDefaults;
  for (const auto &binding : Array(root, "bindings", "template"))
    bindingDefaults.emplace(String(binding, "id", "template.binding"),
                            String(binding, "default", "template.binding", true));
  for (const auto &slot :
       Array(document, "content_slots", "template.document")) {
    OnlyKeys(slot, {"slot_id", "semantic_role", "paragraph_ids", "run_ids"},
             "template.content_slot");
    text::TextContentSlot decoded;
    decoded.slotId = String(slot, "slot_id", "template.content_slot");
    decoded.semanticRole =
        String(slot, "semantic_role", "template.content_slot");
    decoded.paragraphIds = StringArray(
        slot, "paragraph_ids", "template.content_slot");
    decoded.runIds =
        StringArray(slot, "run_ids", "template.content_slot");
    result.contentSlots.push_back(std::move(decoded));
  }
  for (const auto &paragraph :
       Array(document, "paragraphs", "template.document")) {
    OnlyKeys(paragraph, {"id", "style", "runs"}, "template.paragraph");
    text::RichTextParagraph decoded;
    decoded.paragraphId = String(paragraph, "id", "template.paragraph");
    decoded.style = ParseParagraphStyle(
        Object(paragraph, "style", "template.paragraph"),
        "template.paragraph.style");
    for (const auto &run : Array(paragraph, "runs", "template.paragraph")) {
      OnlyKeys(run, {"id", "text_binding", "locale", "style"},
               "template.run");
      text::RichTextRun decodedRun;
      decodedRun.runId = String(run, "id", "template.run");
      const auto bindingId = String(run, "text_binding", "template.run");
      const auto binding = bindingDefaults.find(bindingId);
      if (binding == bindingDefaults.end())
        Invalid("template.run.text_binding",
                "must reference one current text binding");
      decodedRun.utf8Text = binding->second;
      decodedRun.locale =
          OptionalString(run, "locale", decoded.style.locale);
      decodedRun.style = ParseTextStyle(
          Object(run, "style", "template.run"), "template.run.style");
      decoded.runs.push_back(std::move(decodedRun));
    }
    result.content.push_back(std::move(decoded));
  }
  result.presentation.appearance =
      ParseAppearance(Object(root, "appearance", "template"),
                      "template.appearance");
  for (std::size_t index = 0U;
       index < Array(root, "decorations", "template").size(); ++index) {
    const auto &decorations = Array(root, "decorations", "template");
    result.decorations.push_back(ParseVectorDecoration(
        decorations[index],
        "template.decorations[" + std::to_string(index) + "]"));
  }
  for (std::size_t index = 0U;
       index < Array(root, "resources", "template").size(); ++index) {
    const auto &resources = Array(root, "resources", "template");
    result.resources.push_back(ParseResource(
        resources[index],
        "template.resources[" + std::to_string(index) + "]"));
  }
  return result;
}

bool ValidIdentity(const std::string &value, const std::size_t maximum,
                   const bool emptyAllowed = false) noexcept {
  return (emptyAllowed || !value.empty()) && value.size() <= maximum &&
         text::IsValidUtf8(value);
}

bool ChannelAllowsContentSlot(const TextTemplateChannel channel) noexcept {
  switch (channel) {
  case TextTemplateChannel::Content:
  case TextTemplateChannel::Typography:
  case TextTemplateChannel::GlyphFills:
  case TextTemplateChannel::GlyphStrokes:
  case TextTemplateChannel::GlyphShadows:
  case TextTemplateChannel::GlyphGlows:
  case TextTemplateChannel::InlineDecorations:
  case TextTemplateChannel::Paragraph:
  case TextTemplateChannel::Layout:
  case TextTemplateChannel::Bubble:
  case TextTemplateChannel::Frame:
  case TextTemplateChannel::CustomBackdrops:
  case TextTemplateChannel::Bend:
  case TextTemplateChannel::Path:
  case TextTemplateChannel::SdfMaterial:
  case TextTemplateChannel::Animations:
  case TextTemplateChannel::VectorDecorations:
  case TextTemplateChannel::TimedText:
    return true;
  case TextTemplateChannel::Resources:
    return false;
  }
  return false;
}

bool ChannelAllowsLayer(const TextTemplateChannel channel) noexcept {
  switch (channel) {
  case TextTemplateChannel::GlyphFills:
  case TextTemplateChannel::GlyphStrokes:
  case TextTemplateChannel::GlyphShadows:
  case TextTemplateChannel::GlyphGlows:
  case TextTemplateChannel::Bubble:
  case TextTemplateChannel::Frame:
  case TextTemplateChannel::CustomBackdrops:
  case TextTemplateChannel::Animations:
  case TextTemplateChannel::VectorDecorations:
    return true;
  default:
    return false;
  }
}

bool ChannelAllowsSemanticRole(const TextTemplateChannel channel) noexcept {
  switch (channel) {
  case TextTemplateChannel::Content:
  case TextTemplateChannel::Typography:
  case TextTemplateChannel::GlyphFills:
  case TextTemplateChannel::GlyphStrokes:
  case TextTemplateChannel::GlyphShadows:
  case TextTemplateChannel::GlyphGlows:
  case TextTemplateChannel::InlineDecorations:
  case TextTemplateChannel::Paragraph:
  case TextTemplateChannel::Bubble:
  case TextTemplateChannel::Frame:
  case TextTemplateChannel::CustomBackdrops:
  case TextTemplateChannel::Animations:
  case TextTemplateChannel::VectorDecorations:
  case TextTemplateChannel::TimedText:
    return true;
  default:
    return false;
  }
}

bool MaterialChannel(const TextTemplateChannel channel) noexcept {
  return channel == TextTemplateChannel::GlyphFills ||
         channel == TextTemplateChannel::GlyphStrokes ||
         channel == TextTemplateChannel::GlyphShadows ||
         channel == TextTemplateChannel::GlyphGlows;
}

bool BackdropChannel(const TextTemplateChannel channel) noexcept {
  return channel == TextTemplateChannel::Bubble ||
         channel == TextTemplateChannel::Frame ||
         channel == TextTemplateChannel::CustomBackdrops;
}

bool ValidatePatchMetadata(const TextTemplatePatch &patch,
                           const TextCompositionLimits &limits,
                           std::vector<Diagnostic> &diagnostics,
                           const bool requirePackageDigest = true) {
  bool valid = true;
  const auto fail = [&](std::string code, std::string subject,
                        std::string message) {
    valid = false;
    Add(diagnostics, std::move(code), std::move(subject), std::move(message));
  };
  if (!ValidIdentity(patch.templateId, limits.maximumIdentityBytes))
    fail("text_template.id_invalid", patch.templateId,
         "template identity must be bounded valid UTF-8");
  if (requirePackageDigest &&
      !base::Sha256::IsCanonicalDigest(patch.packageDigest))
    fail("text_template.digest_invalid", patch.templateId,
         "application requires the admitted package digest");
  std::unordered_set<std::string> selectors;
  for (const auto &rule : patch.rules) {
    if (rule.channel > TextTemplateChannel::Resources ||
        rule.mode > TextTemplateChannelMode::Clear ||
        !ValidIdentity(rule.contentSlotId, limits.maximumIdentityBytes, true) ||
        !ValidIdentity(rule.targetLayerId, limits.maximumIdentityBytes, true) ||
        !ValidIdentity(rule.semanticRole, limits.maximumIdentityBytes, true)) {
      fail("text_template.rule_invalid", patch.templateId,
           "channel rule contains an invalid current selector");
      continue;
    }
    if ((!rule.contentSlotId.empty() &&
         !ChannelAllowsContentSlot(rule.channel)) ||
        (!rule.targetLayerId.empty() && !ChannelAllowsLayer(rule.channel)) ||
        (!rule.semanticRole.empty() &&
         !ChannelAllowsSemanticRole(rule.channel))) {
      fail("text_template.rule_selector_inapplicable", patch.templateId,
           "channel selector is not applicable to its semantic channel");
      continue;
    }
    const auto key = std::to_string(static_cast<unsigned>(rule.channel)) +
                     "\n" + rule.contentSlotId + "\n" +
                     rule.targetLayerId + "\n" + rule.semanticRole;
    if (!selectors.insert(key).second)
      fail("text_template.rule_duplicate", patch.templateId,
           "channel selectors must be unique");
  }
  std::unordered_set<std::string> bindingIds;
  std::unordered_set<std::string> bindingSlots;
  for (const auto &binding : patch.textBindings) {
    const auto slot = std::find_if(
        patch.value.contentSlots.begin(), patch.value.contentSlots.end(),
        [&](const auto &value) {
          return value.slotId == binding.contentSlotId;
        });
    if (!ValidIdentity(binding.bindingId, limits.maximumIdentityBytes) ||
        !ValidIdentity(binding.contentSlotId, limits.maximumIdentityBytes) ||
        !text::IsValidUtf8(binding.defaultText) ||
        !bindingIds.insert(binding.bindingId).second ||
        !bindingSlots.insert(binding.contentSlotId).second ||
        slot == patch.value.contentSlots.end()) {
      fail("text_template.binding_invalid", binding.bindingId,
           "bindings require unique IDs, unique current slots and UTF-8 text");
    }
  }
  return valid;
}

const text::TextContentSlot *FindSlot(const TextCompositionDocument &document,
                                     const std::string &slotId) noexcept {
  const auto found = std::find_if(
      document.contentSlots.begin(), document.contentSlots.end(),
      [&](const auto &slot) { return slot.slotId == slotId; });
  return found == document.contentSlots.end() ? nullptr : &*found;
}

text::TextContentSlot *FindSlot(TextCompositionDocument &document,
                               const std::string &slotId) noexcept {
  const auto found = std::find_if(
      document.contentSlots.begin(), document.contentSlots.end(),
      [&](const auto &slot) { return slot.slotId == slotId; });
  return found == document.contentSlots.end() ? nullptr : &*found;
}

const text::RichTextParagraph *
FindParagraph(const TextCompositionDocument &document,
              const std::string &paragraphId) noexcept {
  const auto found = std::find_if(
      document.content.begin(), document.content.end(), [&](const auto &value) {
        return value.paragraphId == paragraphId;
      });
  return found == document.content.end() ? nullptr : &*found;
}

text::RichTextParagraph *FindParagraph(TextCompositionDocument &document,
                                      const std::string &paragraphId) noexcept {
  const auto found = std::find_if(
      document.content.begin(), document.content.end(), [&](const auto &value) {
        return value.paragraphId == paragraphId;
      });
  return found == document.content.end() ? nullptr : &*found;
}

const text::RichTextRun *FindRun(const TextCompositionDocument &document,
                                const std::string &runId) noexcept {
  for (const auto &paragraph : document.content) {
    const auto found = std::find_if(
        paragraph.runs.begin(), paragraph.runs.end(),
        [&](const auto &value) { return value.runId == runId; });
    if (found != paragraph.runs.end())
      return &*found;
  }
  return nullptr;
}

std::vector<const text::RichTextRun *>
RunsForSlot(const TextCompositionDocument &document,
            const text::TextContentSlot &slot) {
  std::vector<const text::RichTextRun *> result;
  result.reserve(slot.runIds.size());
  for (const auto &runId : slot.runIds)
    if (const auto *run = FindRun(document, runId))
      result.push_back(run);
  return result;
}

std::vector<text::RichTextRun *>
RunsForSlot(TextCompositionDocument &document,
            const text::TextContentSlot &slot) {
  std::vector<text::RichTextRun *> result;
  result.reserve(slot.runIds.size());
  for (const auto &runId : slot.runIds) {
    for (auto &paragraph : document.content) {
      const auto found = std::find_if(
          paragraph.runs.begin(), paragraph.runs.end(),
          [&](const auto &run) { return run.runId == runId; });
      if (found != paragraph.runs.end()) {
        result.push_back(&*found);
        break;
      }
    }
  }
  return result;
}

std::string SlotText(const TextCompositionDocument &document,
                     const text::TextContentSlot &slot) {
  std::string result;
  for (std::size_t paragraphIndex = 0U;
       paragraphIndex < slot.paragraphIds.size(); ++paragraphIndex) {
    if (paragraphIndex != 0U)
      result.push_back('\n');
    const auto *paragraph =
        FindParagraph(document, slot.paragraphIds[paragraphIndex]);
    if (!paragraph)
      continue;
    for (const auto &run : paragraph->runs)
      result.append(run.utf8Text);
  }
  return result;
}

std::vector<std::string> SplitLines(const std::string &textValue) {
  std::vector<std::string> result;
  std::size_t begin = 0U;
  while (true) {
    const auto end = textValue.find('\n', begin);
    result.push_back(textValue.substr(begin, end - begin));
    if (end == std::string::npos)
      break;
    begin = end + 1U;
  }
  return result;
}

void AssignSlotText(TextCompositionDocument &document,
                    const std::string &slotId, const std::string &textValue) {
  auto *slot = FindSlot(document, slotId);
  if (!slot)
    return;
  const auto lines = SplitLines(textValue);
  if (slot->paragraphIds.empty())
    return;
  std::unordered_set<std::string> paragraphIds;
  std::unordered_set<std::string> runIds;
  for (const auto &paragraph : document.content) {
    paragraphIds.insert(paragraph.paragraphId);
    for (const auto &run : paragraph.runs)
      runIds.insert(run.runId);
  }
  const auto uniqueIdentity = [](const std::string &prefix,
                                 std::unordered_set<std::string> &identities) {
    for (std::uint64_t suffix = 1U;; ++suffix) {
      auto candidate = prefix + std::to_string(suffix);
      if (identities.insert(candidate).second)
        return candidate;
    }
  };
  while (slot->paragraphIds.size() < lines.size()) {
    const auto *prototype =
        FindParagraph(document, slot->paragraphIds.back());
    if (!prototype || prototype->runs.empty())
      break;
    auto paragraph = *prototype;
    paragraph.paragraphId = uniqueIdentity(slot->slotId + ".paragraph.",
                                            paragraphIds);
    paragraph.runs.resize(1U);
    paragraph.runs.front().runId =
        uniqueIdentity(slot->slotId + ".run.", runIds);
    paragraph.runs.front().utf8Text.clear();
    slot->paragraphIds.push_back(paragraph.paragraphId);
    slot->runIds.push_back(paragraph.runs.front().runId);
    document.content.push_back(std::move(paragraph));
    slot = FindSlot(document, slotId);
  }
  if (slot->paragraphIds.size() > lines.size()) {
    std::unordered_set<std::string> removed(
        slot->paragraphIds.begin() + static_cast<std::ptrdiff_t>(lines.size()),
        slot->paragraphIds.end());
    document.content.erase(
        std::remove_if(document.content.begin(), document.content.end(),
                       [&](const auto &paragraph) {
                         return removed.count(paragraph.paragraphId) != 0U;
                       }),
        document.content.end());
    slot = FindSlot(document, slotId);
    slot->paragraphIds.resize(lines.size());
    slot->runIds.clear();
    for (const auto &paragraphId : slot->paragraphIds)
      if (const auto *paragraph = FindParagraph(document, paragraphId))
        for (const auto &run : paragraph->runs)
          slot->runIds.push_back(run.runId);
  }
  for (std::size_t index = 0U; index < slot->paragraphIds.size(); ++index) {
    auto *paragraph = FindParagraph(document, slot->paragraphIds[index]);
    if (!paragraph || paragraph->runs.empty())
      continue;
    auto &primaryRun = paragraph->runs.front();
    const auto oldByteLength = primaryRun.utf8Text.size();
    const auto replacement = index < lines.size() ? lines[index] : "";
    if (document.timedText) {
      for (auto &span : document.timedText->spans) {
        if (span.paragraphId == paragraph->paragraphId &&
            span.runId == primaryRun.runId && span.range.begin == 0U &&
            span.range.end == oldByteLength) {
          span.range.end = replacement.size();
        }
      }
    }
    primaryRun.utf8Text = replacement;
    for (std::size_t run = 1U; run < paragraph->runs.size(); ++run)
      paragraph->runs[run].utf8Text.clear();
  }
}

void RemapTemplateTextOwners(TextCompositionDocument &source,
                             const TextCompositionDocument &destination) {
  std::unordered_map<std::string, std::string> slotMap;
  std::unordered_map<std::string, std::string> paragraphMap;
  std::unordered_map<std::string, std::string> runMap;
  for (const auto &sourceSlot : source.contentSlots) {
    auto destinationSlot = std::find_if(
        destination.contentSlots.begin(), destination.contentSlots.end(),
        [&](const auto &candidate) {
          return candidate.slotId == sourceSlot.slotId;
        });
    if (destinationSlot == destination.contentSlots.end()) {
      const auto firstSemantic = std::find_if(
          destination.contentSlots.begin(), destination.contentSlots.end(),
          [&](const auto &candidate) {
            return !sourceSlot.semanticRole.empty() &&
                   candidate.semanticRole == sourceSlot.semanticRole;
          });
      if (firstSemantic != destination.contentSlots.end() &&
          std::find_if(std::next(firstSemantic), destination.contentSlots.end(),
                       [&](const auto &candidate) {
                         return candidate.semanticRole ==
                                sourceSlot.semanticRole;
                       }) == destination.contentSlots.end()) {
        destinationSlot = firstSemantic;
      }
    }
    if (destinationSlot == destination.contentSlots.end())
      continue;
    slotMap.emplace(sourceSlot.slotId, destinationSlot->slotId);
    for (std::size_t index = 0U;
         index < std::min(sourceSlot.paragraphIds.size(),
                          destinationSlot->paragraphIds.size());
         ++index)
      paragraphMap.emplace(sourceSlot.paragraphIds[index],
                           destinationSlot->paragraphIds[index]);
    for (std::size_t index = 0U;
         index < std::min(sourceSlot.runIds.size(),
                          destinationSlot->runIds.size());
         ++index)
      runMap.emplace(sourceSlot.runIds[index], destinationSlot->runIds[index]);
  }
  const auto remap = [](std::string &identity, const auto &mapping) {
    if (const auto found = mapping.find(identity); found != mapping.end())
      identity = found->second;
  };
  const auto remapList = [&](std::vector<std::string> &identities,
                             const auto &mapping) {
    for (auto &identity : identities)
      remap(identity, mapping);
    std::vector<std::string> unique;
    unique.reserve(identities.size());
    for (auto &identity : identities)
      if (std::find(unique.begin(), unique.end(), identity) == unique.end())
        unique.push_back(std::move(identity));
    identities = std::move(unique);
  };
  for (auto &slot : source.contentSlots) {
    remap(slot.slotId, slotMap);
    remapList(slot.paragraphIds, paragraphMap);
    remapList(slot.runIds, runMap);
  }
  for (auto &paragraph : source.content) {
    remap(paragraph.paragraphId, paragraphMap);
    for (auto &run : paragraph.runs)
      remap(run.runId, runMap);
  }
  if (source.timedText) {
    for (auto &span : source.timedText->spans) {
      remap(span.paragraphId, paragraphMap);
      remap(span.runId, runMap);
    }
  }
  for (auto &decoration : source.decorations) {
    remap(decoration.target.paragraphId, paragraphMap);
    remap(decoration.target.runId, runMap);
  }
  for (auto &layer : source.animations.layers) {
    remap(layer.target.contentSlotId, slotMap);
    remapList(layer.target.paragraphIds, paragraphMap);
    remapList(layer.target.runIds, runMap);
    for (auto &animator : layer.animators) {
      remapList(animator.paragraphIds, paragraphMap);
      remapList(animator.runIds, runMap);
    }
  }
}

TextTemplateChannelRule ResolveTemplateRuleForTarget(
    const TextTemplateChannelRule &rule,
    const TextCompositionDocument &source,
    const TextCompositionDocument &destination) {
  auto resolved = rule;
  if (rule.contentSlotId.empty() || FindSlot(destination, rule.contentSlotId))
    return resolved;
  const auto *sourceSlot = FindSlot(source, rule.contentSlotId);
  if (!sourceSlot || sourceSlot->semanticRole.empty())
    return resolved;
  const auto first = std::find_if(
      destination.contentSlots.begin(), destination.contentSlots.end(),
      [&](const auto &candidate) {
        return candidate.semanticRole == sourceSlot->semanticRole;
      });
  if (first == destination.contentSlots.end() ||
      std::find_if(std::next(first), destination.contentSlots.end(),
                   [&](const auto &candidate) {
                     return candidate.semanticRole == sourceSlot->semanticRole;
                   }) != destination.contentSlots.end()) {
    return resolved;
  }
  resolved.contentSlotId = first->slotId;
  return resolved;
}

bool Utf8Boundary(const std::string &value, const std::uint64_t offset) {
  return offset <= value.size() &&
         (offset == 0U || offset == value.size() ||
          (static_cast<unsigned char>(value[static_cast<std::size_t>(offset)]) &
           0xc0U) != 0x80U);
}

std::optional<text::TextUtf8Range>
FitRangeToRun(const text::TextUtf8Range range,
              const std::string &utf8Text) {
  if (utf8Text.empty())
    return std::nullopt;
  auto begin = std::min<std::uint64_t>(range.begin, utf8Text.size());
  auto end = std::min<std::uint64_t>(range.end, utf8Text.size());
  while (begin != 0U && !Utf8Boundary(utf8Text, begin))
    --begin;
  while (end < utf8Text.size() && !Utf8Boundary(utf8Text, end))
    ++end;
  if (begin >= end) {
    begin = 0U;
    end = utf8Text.size();
  }
  return text::TextUtf8Range{begin, end};
}

const text::RichTextParagraph *ParagraphOwningRun(
    const TextCompositionDocument &document, const std::string &runId) {
  const auto found = std::find_if(
      document.content.begin(), document.content.end(), [&](const auto &paragraph) {
        return std::any_of(paragraph.runs.begin(), paragraph.runs.end(),
                           [&](const auto &run) { return run.runId == runId; });
      });
  return found == document.content.end() ? nullptr : &*found;
}

void FitTemplateReferencesToTarget(TextCompositionDocument &source,
                                   const TextCompositionDocument &target) {
  const auto resolveRun = [&](const std::string &runId) {
    const auto *run = FindRun(target, runId);
    return run ? run : FindRun(source, runId);
  };
  const auto resolveParagraph = [&](const std::string &runId) {
    const auto *paragraph = ParagraphOwningRun(target, runId);
    return paragraph ? paragraph : ParagraphOwningRun(source, runId);
  };
  if (source.timedText) {
    source.timedText->spans.erase(
        std::remove_if(source.timedText->spans.begin(),
                       source.timedText->spans.end(), [&](auto &span) {
                         const auto *run = resolveRun(span.runId);
                         const auto *paragraph = resolveParagraph(span.runId);
                         const auto range =
                             run ? FitRangeToRun(span.range, run->utf8Text)
                                 : std::nullopt;
                         if (!paragraph || !range)
                           return true;
                         span.paragraphId = paragraph->paragraphId;
                         span.range = *range;
                         return false;
                       }),
        source.timedText->spans.end());
    if (source.timedText->spans.empty())
      source.timedText.reset();
  }
  for (auto &decoration : source.decorations) {
    if (decoration.target.scope != DecorationTargetScope::Utf8Range)
      continue;
    const auto *run = resolveRun(decoration.target.runId);
    const auto *paragraph = resolveParagraph(decoration.target.runId);
    const auto range = run
                           ? FitRangeToRun(decoration.target.range, run->utf8Text)
                           : std::nullopt;
    if (!paragraph || !range) {
      decoration.enabled = false;
      decoration.target = {};
      continue;
    }
    decoration.target.paragraphId = paragraph->paragraphId;
    decoration.target.range = *range;
  }
  std::unordered_set<std::string> paragraphIds;
  std::unordered_set<std::string> runIds;
  for (const auto &paragraph : target.content) {
    paragraphIds.insert(paragraph.paragraphId);
    for (const auto &run : paragraph.runs)
      runIds.insert(run.runId);
  }
  for (const auto &paragraph : source.content) {
    paragraphIds.insert(paragraph.paragraphId);
    for (const auto &run : paragraph.runs)
      runIds.insert(run.runId);
  }
  for (auto &layer : source.animations.layers) {
    auto &animationTarget = layer.target;
    animationTarget.paragraphIds.erase(
        std::remove_if(animationTarget.paragraphIds.begin(),
                       animationTarget.paragraphIds.end(), [&](const auto &id) {
                         return paragraphIds.count(id) == 0U;
                       }),
        animationTarget.paragraphIds.end());
    animationTarget.runIds.erase(
        std::remove_if(animationTarget.runIds.begin(),
                       animationTarget.runIds.end(),
                       [&](const auto &id) { return runIds.count(id) == 0U; }),
        animationTarget.runIds.end());
    if (animationTarget.scope == text::TextPropertyScope::Utf8Range) {
      const auto *run = animationTarget.runIds.size() == 1U
                            ? resolveRun(animationTarget.runIds.front())
                            : nullptr;
      const auto range = run && animationTarget.range
                             ? FitRangeToRun(*animationTarget.range,
                                             run->utf8Text)
                             : std::nullopt;
      if (!range) {
        animationTarget.scope = text::TextPropertyScope::Composition;
        animationTarget.contentSlotId.clear();
        animationTarget.paragraphIds.clear();
        animationTarget.runIds.clear();
        animationTarget.range.reset();
      } else {
        animationTarget.range = *range;
      }
    }
    for (auto &animator : layer.animators) {
      animator.paragraphIds.erase(
          std::remove_if(animator.paragraphIds.begin(),
                         animator.paragraphIds.end(), [&](const auto &id) {
                           return paragraphIds.count(id) == 0U;
                         }),
          animator.paragraphIds.end());
      animator.runIds.erase(
          std::remove_if(animator.runIds.begin(), animator.runIds.end(),
                         [&](const auto &id) {
                           return runIds.count(id) == 0U;
                         }),
          animator.runIds.end());
    }
  }
}

bool SlotMatches(const text::TextContentSlot &slot,
                 const TextTemplateChannelRule &rule,
                 const bool semanticSelectsSlot) noexcept {
  return (rule.contentSlotId.empty() ||
          slot.slotId == rule.contentSlotId) &&
         (!semanticSelectsSlot || rule.semanticRole.empty() ||
          slot.semanticRole == rule.semanticRole);
}

const text::TextContentSlot *SourceSlotFor(
    const TextCompositionDocument &source,
    const text::TextContentSlot &destination,
    const TextTemplateChannelRule &rule) noexcept {
  if (!rule.contentSlotId.empty())
    if (const auto *exact = FindSlot(source, rule.contentSlotId))
      return exact;
  if (const auto *exact = FindSlot(source, destination.slotId))
    return exact;
  const auto semantic = std::find_if(
      source.contentSlots.begin(), source.contentSlots.end(),
      [&](const auto &slot) {
        return slot.semanticRole == destination.semanticRole;
      });
  return semantic == source.contentSlots.end()
             ? (source.contentSlots.empty() ? nullptr
                                            : &source.contentSlots.front())
             : &*semantic;
}

template <typename Callback>
void ForEachRunPair(TextCompositionDocument &destination,
                    const TextCompositionDocument &source,
                    const TextTemplateChannelRule &rule,
                    const bool semanticSelectsSlot, Callback &&callback) {
  for (const auto &slotValue : destination.contentSlots) {
    if (!SlotMatches(slotValue, rule, semanticSelectsSlot))
      continue;
    const auto *sourceSlot = SourceSlotFor(source, slotValue, rule);
    if (!sourceSlot)
      continue;
    auto destinationRuns = RunsForSlot(destination, slotValue);
    const auto sourceRuns = RunsForSlot(source, *sourceSlot);
    if (sourceRuns.empty())
      continue;
    for (std::size_t index = 0U; index < destinationRuns.size(); ++index)
      callback(*destinationRuns[index],
               *sourceRuns[std::min(index, sourceRuns.size() - 1U)]);
  }
}

enum class GlyphLayerFamily : std::uint8_t { Fill, Stroke, Shadow, Glow };

GlyphLayerFamily Family(const text::TextGlyphMaterialLayer &layer) noexcept {
  if (std::holds_alternative<text::TextFillLayer>(layer))
    return GlyphLayerFamily::Fill;
  if (std::holds_alternative<text::TextStrokeLayer>(layer))
    return GlyphLayerFamily::Stroke;
  if (std::holds_alternative<text::TextShadowLayer>(layer))
    return GlyphLayerFamily::Shadow;
  return GlyphLayerFamily::Glow;
}

GlyphLayerFamily Family(const TextTemplateChannel channel) noexcept {
  switch (channel) {
  case TextTemplateChannel::GlyphFills:
    return GlyphLayerFamily::Fill;
  case TextTemplateChannel::GlyphStrokes:
    return GlyphLayerFamily::Stroke;
  case TextTemplateChannel::GlyphShadows:
    return GlyphLayerFamily::Shadow;
  case TextTemplateChannel::GlyphGlows:
    return GlyphLayerFamily::Glow;
  default:
    return GlyphLayerFamily::Fill;
  }
}

const std::string &LayerId(const text::TextGlyphMaterialLayer &layer) noexcept {
  return std::visit([](const auto &value) -> const std::string & {
    return value.layerId;
  }, layer);
}

const text::TextMaterialBinding &
LayerMaterial(const text::TextGlyphMaterialLayer &layer) noexcept {
  return std::visit([](const auto &value) -> const text::TextMaterialBinding & {
    return value.material;
  }, layer);
}

bool BindingHasSemanticRole(const text::TextMaterialBinding &binding,
                            const std::string &role) noexcept {
  if (role.empty())
    return true;
  const auto *slot = std::get_if<text::EditableTextStyleSlot>(&binding);
  return slot && slot->semanticRole == role;
}

bool GlyphLayerMatches(const text::TextGlyphMaterialLayer &layer,
                       const TextTemplateChannelRule &rule) noexcept {
  return Family(layer) == Family(rule.channel) &&
         (rule.targetLayerId.empty() || LayerId(layer) == rule.targetLayerId) &&
         BindingHasSemanticRole(LayerMaterial(layer), rule.semanticRole);
}

void EnsureMaterialStack(text::TextGlyphMaterialStack &stack) {
  if (!stack.layers.empty())
    return;
  text::TextFillLayer fallback;
  fallback.layerId = "glyph.primaryFill";
  fallback.material = text::LiteralTextMaterial{text::SolidTextMaterial{
      {0.0F, 0.0F, 0.0F, 0.0F}}};
  stack.layers.emplace_back(std::move(fallback));
}

void ApplyGlyphLayers(text::TextGlyphMaterialStack &destination,
                      const text::TextGlyphMaterialStack &source,
                      const TextTemplateChannelRule &rule) {
  if (rule.mode == TextTemplateChannelMode::Keep)
    return;
  const auto sourceMatches = [&]() {
    std::vector<text::TextGlyphMaterialLayer> result;
    for (const auto &layer : source.layers)
      if (GlyphLayerMatches(layer, rule))
        result.push_back(layer);
    return result;
  }();
  if (rule.mode == TextTemplateChannelMode::Clear ||
      rule.mode == TextTemplateChannelMode::Replace) {
    destination.layers.erase(
        std::remove_if(destination.layers.begin(), destination.layers.end(),
                       [&](const auto &layer) {
                         return GlyphLayerMatches(layer, rule);
                       }),
        destination.layers.end());
  }
  if (rule.mode == TextTemplateChannelMode::Replace) {
    destination.layers.insert(destination.layers.end(), sourceMatches.begin(),
                              sourceMatches.end());
  } else if (rule.mode == TextTemplateChannelMode::Merge) {
    for (const auto &layer : sourceMatches) {
      const auto found = std::find_if(
          destination.layers.begin(), destination.layers.end(),
          [&](const auto &current) {
            return LayerId(current) == LayerId(layer) &&
                   GlyphLayerMatches(current, rule);
          });
      if (found == destination.layers.end())
        destination.layers.push_back(layer);
      else
        *found = layer;
    }
  }
  EnsureMaterialStack(destination);
}

bool BackdropMaterialHasRole(const text::TextBackdropLayer &layer,
                             const std::string &role) noexcept {
  if (role.empty())
    return true;
  if (layer.materialOverride &&
      BindingHasSemanticRole(*layer.materialOverride, role))
    return true;
  if (const auto *rounded =
          std::get_if<text::RoundedRectBackdrop>(&layer.source)) {
    if (BindingHasSemanticRole(rounded->fill, role))
      return true;
    return std::any_of(rounded->strokes.begin(), rounded->strokes.end(),
                       [&](const auto &stroke) {
                         return BindingHasSemanticRole(stroke.material, role);
                       });
  }
  return false;
}

text::TextBackdropChannel BackdropFamily(
    const TextTemplateChannel channel) noexcept {
  if (channel == TextTemplateChannel::Bubble)
    return text::TextBackdropChannel::Bubble;
  if (channel == TextTemplateChannel::CustomBackdrops)
    return text::TextBackdropChannel::Custom;
  return text::TextBackdropChannel::Frame;
}

bool BackdropMatches(const text::TextBackdropLayer &layer,
                     const TextTemplateChannelRule &rule) noexcept {
  return layer.channel == BackdropFamily(rule.channel) &&
         (rule.targetLayerId.empty() || layer.layerId == rule.targetLayerId) &&
         BackdropMaterialHasRole(layer, rule.semanticRole);
}

void ApplyBackdrops(text::TextBackdropStack &destination,
                    const text::TextBackdropStack &source,
                    const TextTemplateChannelRule &rule) {
  if (rule.mode == TextTemplateChannelMode::Keep)
    return;
  std::vector<text::TextBackdropLayer> sourceMatches;
  for (const auto &layer : source.layers)
    if (BackdropMatches(layer, rule))
      sourceMatches.push_back(layer);
  if (rule.mode == TextTemplateChannelMode::Clear ||
      rule.mode == TextTemplateChannelMode::Replace) {
    destination.layers.erase(
        std::remove_if(destination.layers.begin(), destination.layers.end(),
                       [&](const auto &layer) {
                         return BackdropMatches(layer, rule);
                       }),
        destination.layers.end());
  }
  if (rule.mode == TextTemplateChannelMode::Replace) {
    destination.layers.insert(destination.layers.end(), sourceMatches.begin(),
                              sourceMatches.end());
  } else if (rule.mode == TextTemplateChannelMode::Merge) {
    for (const auto &layer : sourceMatches) {
      const auto found = std::find_if(
          destination.layers.begin(), destination.layers.end(),
          [&](const auto &current) {
            return current.layerId == layer.layerId &&
                   BackdropMatches(current, rule);
          });
      if (found == destination.layers.end())
        destination.layers.push_back(layer);
      else
        *found = layer;
    }
  }
}

void CopyTypography(text::TextStyle &destination,
                    const text::TextStyle &source) {
  destination.font = source.font;
  destination.fontSize = source.fontSize;
  destination.letterSpacing = source.letterSpacing;
  destination.wordSpacing = source.wordSpacing;
  destination.baselineShift = source.baselineShift;
}

void ClearTypography(text::TextStyle &style) {
  const text::TextStyle defaults;
  style.font = defaults.font;
  style.fontSize = defaults.fontSize;
  style.letterSpacing = defaults.letterSpacing;
  style.wordSpacing = defaults.wordSpacing;
  style.baselineShift = defaults.baselineShift;
}

void ApplyRunChannel(TextCompositionDocument &destination,
                     const TextCompositionDocument &source,
                     const TextTemplateChannelRule &rule) {
  if (rule.mode == TextTemplateChannelMode::Keep)
    return;
  const bool semanticSelectsSlot = !MaterialChannel(rule.channel);
  ForEachRunPair(
      destination, source, rule, semanticSelectsSlot,
      [&](text::RichTextRun &target, const text::RichTextRun &prototype) {
        switch (rule.channel) {
        case TextTemplateChannel::Typography:
          if (rule.mode == TextTemplateChannelMode::Clear)
            ClearTypography(target.style);
          else
            CopyTypography(target.style, prototype.style);
          break;
        case TextTemplateChannel::InlineDecorations:
          target.style.decoration =
              rule.mode == TextTemplateChannelMode::Clear
                  ? text::InlineTextDecoration{}
                  : prototype.style.decoration;
          break;
        case TextTemplateChannel::GlyphFills:
        case TextTemplateChannel::GlyphStrokes:
        case TextTemplateChannel::GlyphShadows:
        case TextTemplateChannel::GlyphGlows:
          ApplyGlyphLayers(target.style.materials,
                           prototype.style.materials, rule);
          break;
        default:
          break;
        }
      });
}

void ApplyParagraphChannel(TextCompositionDocument &destination,
                           const TextCompositionDocument &source,
                           const TextTemplateChannelRule &rule) {
  if (rule.mode == TextTemplateChannelMode::Keep)
    return;
  for (const auto &destinationSlot : destination.contentSlots) {
    if (!SlotMatches(destinationSlot, rule, true))
      continue;
    const auto *sourceSlot = SourceSlotFor(source, destinationSlot, rule);
    if (!sourceSlot || sourceSlot->paragraphIds.empty())
      continue;
    for (std::size_t index = 0U;
         index < destinationSlot.paragraphIds.size(); ++index) {
      auto *paragraph =
          FindParagraph(destination, destinationSlot.paragraphIds[index]);
      const auto *prototype = FindParagraph(
          source, sourceSlot->paragraphIds[std::min(
                      index, sourceSlot->paragraphIds.size() - 1U)]);
      if (!paragraph || !prototype)
        continue;
      paragraph->style = rule.mode == TextTemplateChannelMode::Clear
                             ? text::ParagraphStyle{}
                             : prototype->style;
    }
  }
}

void RemoveSlotContent(TextCompositionDocument &document,
                       const text::TextContentSlot &slot) {
  std::unordered_set<std::string> paragraphs(slot.paragraphIds.begin(),
                                             slot.paragraphIds.end());
  document.content.erase(
      std::remove_if(document.content.begin(), document.content.end(),
                     [&](const auto &paragraph) {
                       return paragraphs.count(paragraph.paragraphId) != 0U;
                     }),
      document.content.end());
}

void AppendSlotContent(TextCompositionDocument &destination,
                       const TextCompositionDocument &source,
                       const text::TextContentSlot &slot) {
  destination.contentSlots.push_back(slot);
  for (const auto &paragraphId : slot.paragraphIds)
    if (const auto *paragraph = FindParagraph(source, paragraphId))
      destination.content.push_back(*paragraph);
}

void ApplyContentChannel(TextCompositionDocument &destination,
                         const TextCompositionDocument &source,
                         const TextTemplateChannelRule &rule) {
  if (rule.mode == TextTemplateChannelMode::Keep)
    return;
  if (rule.mode == TextTemplateChannelMode::Clear) {
    for (const auto &slot : destination.contentSlots)
      if (SlotMatches(slot, rule, true))
        AssignSlotText(destination, slot.slotId, "");
    return;
  }
  std::vector<text::TextContentSlot> sourceSlots;
  for (const auto &slot : source.contentSlots)
    if (SlotMatches(slot, rule, true))
      sourceSlots.push_back(slot);
  if (rule.mode == TextTemplateChannelMode::Merge) {
    for (const auto &slot : sourceSlots)
      if (!FindSlot(destination, slot.slotId))
        AppendSlotContent(destination, source, slot);
    return;
  }
  std::vector<text::TextContentSlot> retained;
  retained.reserve(destination.contentSlots.size() + sourceSlots.size());
  for (const auto &slot : destination.contentSlots) {
    if (SlotMatches(slot, rule, true))
      RemoveSlotContent(destination, slot);
    else
      retained.push_back(slot);
  }
  destination.contentSlots = std::move(retained);
  for (const auto &slot : sourceSlots)
    AppendSlotContent(destination, source, slot);
}

bool AnimationMatches(const text::TextAnimationLayerSpec &layer,
                      const TextCompositionDocument &document,
                      const TextTemplateChannelRule &rule) noexcept {
  if (!rule.targetLayerId.empty() && layer.layerId != rule.targetLayerId)
    return false;
  if (rule.contentSlotId.empty() && rule.semanticRole.empty())
    return true;
  return std::any_of(
      document.contentSlots.begin(), document.contentSlots.end(),
      [&](const auto &slot) {
        if ((!rule.contentSlotId.empty() &&
             slot.slotId != rule.contentSlotId) ||
            (!rule.semanticRole.empty() &&
             slot.semanticRole != rule.semanticRole))
          return false;
        switch (layer.target.scope) {
        case text::TextPropertyScope::Composition:
        case text::TextPropertyScope::BackdropLayer:
          return true;
        case text::TextPropertyScope::ContentSlot:
          return layer.target.contentSlotId == slot.slotId;
        case text::TextPropertyScope::Paragraph:
          return std::any_of(
              layer.target.paragraphIds.begin(),
              layer.target.paragraphIds.end(), [&](const auto &id) {
                return std::find(slot.paragraphIds.begin(),
                                 slot.paragraphIds.end(), id) !=
                       slot.paragraphIds.end();
              });
        case text::TextPropertyScope::Run:
        case text::TextPropertyScope::Utf8Range:
        case text::TextPropertyScope::GlyphMaterialLayer:
          return layer.target.runIds.empty() ||
                 std::any_of(layer.target.runIds.begin(),
                             layer.target.runIds.end(), [&](const auto &id) {
                               return std::find(slot.runIds.begin(),
                                                slot.runIds.end(), id) !=
                                      slot.runIds.end();
                             });
        }
        return false;
      });
}

void RebuildEffectProgramClosure(
    text::TextAnimationStack &destination,
    const text::TextAnimationStack &source,
    const std::unordered_set<std::string> &preferSource) {
  std::vector<std::string> required;
  for (const auto &layer : destination.layers)
    for (const auto &animator : layer.animators)
      if (!animator.effectProgramId.empty() &&
          std::find(required.begin(), required.end(),
                    animator.effectProgramId) == required.end())
        required.push_back(animator.effectProgramId);
  text::TextEffectProgramLibrary closed;
  closed.reserve(required.size());
  for (const auto &programId : required) {
    const auto *fromDestination =
        text::FindTextEffectProgram(destination.effectPrograms, programId);
    const auto *fromSource =
        text::FindTextEffectProgram(source.effectPrograms, programId);
    const auto *selected =
        preferSource.count(programId) != 0U && fromSource ? fromSource
                                                         : fromDestination;
    if (!selected)
      selected = fromSource;
    if (selected)
      closed.push_back(*selected);
  }
  destination.effectPrograms = std::move(closed);
}

bool NamespaceAnimationClosure(TextCompositionDocument &sourceDocument,
                               const std::string &prefix,
                               std::vector<Diagnostic> &diagnostics) {
  auto &source = sourceDocument.animations;
  std::unordered_map<std::string, std::string> layerIds;
  for (auto &layer : source.layers) {
    const auto original = layer.layerId;
    layer.layerId = prefix + original;
    layerIds.emplace(original, layer.layerId);
  }
  std::unordered_map<std::string, std::string> resourceIds;
  for (auto &resource : sourceDocument.resources) {
    const auto original = resource.resourceId;
    resource.resourceId = prefix + original;
    resourceIds.emplace(original, resource.resourceId);
  }
  std::unordered_map<std::string, std::string> nodeIds;
  const auto collectNodes = [&](const auto &self, const auto &nodes) -> void {
    for (const auto &node : nodes) {
      nodeIds.emplace(node.nodeId, prefix + node.nodeId);
      self(self, node.children);
    }
  };
  collectNodes(collectNodes, source.executionGraph.nodes);
  const auto remapNodeId = [&](std::string &id) {
    const auto found = nodeIds.find(id);
    if (found != nodeIds.end()) id = found->second;
  };
  std::unordered_map<std::string, std::string> postEffectIds;
  for (const auto &layer : source.layers)
    for (const auto &effect : layer.postEffects)
      postEffectIds.emplace(effect.effectId, prefix + effect.effectId);
  const auto remapNodes = [&](const auto &self, auto &nodes) -> bool {
    for (auto &node : nodes) {
      const auto originalOwner = node.ownerLayerId.empty() &&
                                         source.layers.size() == 1U
                                     ? layerIds.begin()->first
                                     : node.ownerLayerId;
      const auto owner = layerIds.find(originalOwner);
      if (owner == layerIds.end()) {
        Add(diagnostics, "text_template.animation_graph_unowned",
            node.nodeId,
            "execution graph node must identify an authored source layer");
        return false;
      }
      remapNodeId(node.nodeId);
      for (auto &input : node.inputIds) remapNodeId(input);
      for (auto &resourceId : node.resourceIds) {
        const auto found = resourceIds.find(resourceId);
        if (found != resourceIds.end()) resourceId = found->second;
      }
      if (!node.stateId.empty()) node.stateId = prefix + node.stateId;
      if (!node.historyId.empty()) node.historyId = prefix + node.historyId;
      node.ownerLayerId = owner->second;
      if (!self(self, node.children)) return false;
    }
    return true;
  };
  if (!remapNodes(remapNodes, source.executionGraph.nodes)) return false;
  for (auto &layer : source.layers)
    for (auto &effect : layer.postEffects) {
      const auto effectNode = nodeIds.find(effect.effectId);
      effect.effectId = effectNode != nodeIds.end()
                            ? effectNode->second
                            : postEffectIds.at(effect.effectId);
      for (auto &input : effect.inputIds) {
        const auto inputNode = nodeIds.find(input);
        if (inputNode != nodeIds.end()) {
          input = inputNode->second;
        } else if (const auto priorEffect = postEffectIds.find(input);
                   priorEffect != postEffectIds.end()) {
          input = priorEffect->second;
        }
      }
    }
  for (auto &decoration : sourceDocument.decorations) {
    const auto found = resourceIds.find(decoration.fallbackAssetId);
    if (found != resourceIds.end())
      decoration.fallbackAssetId = found->second;
  }
  for (auto &program : source.effectPrograms) {
    const auto original = program.programId;
    program.programId = prefix + original;
    for (auto &layer : source.layers)
      for (auto &animator : layer.animators)
        if (animator.effectProgramId == original)
          animator.effectProgramId = program.programId;
    for (auto &stage : program.stages)
      for (auto &binding : stage.executionParameterBindings)
        remapNodeId(binding.nodeId);
  }
  return true;
}

bool ComposeAnimationPhase(text::TextAnimationStack &destination,
                           TextCompositionDocument &sourceDocument,
                           const TextTemplateChannelRule &rule,
                           const std::string &templateId,
                           const std::string &packageDigest,
                           std::vector<Diagnostic> &diagnostics) {
  auto &source = sourceDocument.animations;
  const auto sourceLayer = std::find_if(
      source.layers.begin(), source.layers.end(), [&](const auto &layer) {
        return AnimationMatches(layer, sourceDocument, rule);
      });
  if (sourceLayer == source.layers.end() || source.layers.size() != 1U) {
    Add(diagnostics, "text_template.animation_phase_unresolved", templateId,
        "phase template must resolve exactly one authored animation layer");
    return false;
  }
  const auto phase = sourceLayer->timeDriver.kind;
  std::unordered_set<std::string> removedLayerIds;
  for (const auto &layer : destination.layers)
    if (layer.timeDriver.kind == phase)
      removedLayerIds.insert(layer.layerId);
  const auto graphIsOwned = [&](const auto &self,
                                const auto &nodes) -> bool {
    for (const auto &node : nodes) {
      if (node.ownerLayerId.empty() ||
          !self(self, node.children))
        return false;
    }
    return true;
  };
  if (!graphIsOwned(graphIsOwned, destination.executionGraph.nodes)) {
    Add(diagnostics, "text_template.animation_graph_unowned", templateId,
        "phase composition requires layer ownership for retained graph nodes");
    return false;
  }

  const std::string prefix = "text-template." + templateId + "." +
                             packageDigest + ".animation.";
  if (!NamespaceAnimationClosure(sourceDocument, prefix, diagnostics))
    return false;

  const auto erasePhaseNodes = [&](const auto &self, auto &nodes) -> void {
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                               [&](auto &node) {
                                 if (removedLayerIds.count(node.ownerLayerId))
                                   return true;
                                 self(self, node.children);
                                 return false;
                               }),
                nodes.end());
  };
  erasePhaseNodes(erasePhaseNodes, destination.executionGraph.nodes);
  destination.layers.erase(
      std::remove_if(destination.layers.begin(), destination.layers.end(),
                     [&](const auto &layer) {
                       return layer.timeDriver.kind == phase;
                     }),
      destination.layers.end());
  destination.layers.push_back(source.layers.front());
  destination.executionGraph.nodes.insert(
      destination.executionGraph.nodes.end(),
      source.executionGraph.nodes.begin(), source.executionGraph.nodes.end());
  std::unordered_set<std::string> sourceProgramIds;
  for (const auto &animator : source.layers.front().animators)
    if (!animator.effectProgramId.empty())
      sourceProgramIds.insert(animator.effectProgramId);
  RebuildEffectProgramClosure(destination, source, sourceProgramIds);
  return true;
}

bool MergeOwnedAnimationGraphs(
    text::TextAnimationStack &destination,
    TextCompositionDocument &sourceDocument,
    const TextTemplateChannelRule &rule,
    const std::string &templateId,
    const std::string &packageDigest,
    std::unordered_set<std::string> &removedDecorations,
    std::vector<Diagnostic> &diagnostics) {
  auto &source = sourceDocument.animations;
  std::unordered_set<std::string> selectedLayerIds;
  for (const auto &layer : source.layers)
    if (AnimationMatches(layer, sourceDocument, rule))
      selectedLayerIds.insert(layer.layerId);
  if (selectedLayerIds.empty()) {
    Add(diagnostics, "text_template.animation_merge_unresolved", templateId,
        "animation graph merge has no selected source layer");
    return false;
  }
  const auto destinationOwned = [&](const auto &self,
                                    const auto &nodes) -> bool {
    for (const auto &node : nodes)
      if (node.ownerLayerId.empty() || !self(self, node.children))
        return false;
    return true;
  };
  if (!destinationOwned(destinationOwned, destination.executionGraph.nodes)) {
    Add(diagnostics, "text_template.animation_graph_unowned", templateId,
        "animation graph merge requires ownership for retained nodes");
    return false;
  }
  const std::string prefix = "text-template." + templateId + "." +
                             packageDigest + ".animation.";
  if (!NamespaceAnimationClosure(sourceDocument, prefix, diagnostics))
    return false;
  std::unordered_set<std::string> selectedNamespacedIds;
  for (const auto &layerId : selectedLayerIds)
    selectedNamespacedIds.insert(prefix + layerId);
  std::unordered_set<std::string> selectedNodeIds;
  const auto collectSelectedNodes = [&](const auto &self,
                                        const auto &nodes) -> void {
    for (const auto &node : nodes) {
      if (selectedNamespacedIds.count(node.ownerLayerId) == 0U)
        continue;
      selectedNodeIds.insert(node.nodeId);
      self(self, node.children);
    }
  };
  collectSelectedNodes(collectSelectedNodes, source.executionGraph.nodes);
  std::unordered_set<std::string> sourceProgramIds;
  for (const auto &layer : source.layers) {
    if (selectedNamespacedIds.count(layer.layerId) == 0U) continue;
    for (const auto &animator : layer.animators)
      if (!animator.effectProgramId.empty())
        sourceProgramIds.insert(animator.effectProgramId);
  }
  for (const auto &programId : sourceProgramIds) {
    const auto *program =
        text::FindTextEffectProgram(source.effectPrograms, programId);
    if (!program) continue;
    for (const auto &stage : program->stages)
      for (const auto &binding : stage.executionParameterBindings)
        if (selectedNodeIds.count(binding.nodeId) == 0U) {
          Add(diagnostics, "text_template.animation_program_cross_owner",
              programId,
              "selected effect program binds a node outside the selected graph closure");
          return false;
        }
  }
  for (const auto &layer : destination.layers)
    if (selectedNamespacedIds.count(layer.layerId) != 0U)
      for (const auto &decoration : layer.decorations)
        removedDecorations.insert(decoration.decorationId);
  const auto eraseSelectedNodes = [&](const auto &self,
                                      auto &nodes) -> void {
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                               [&](auto &node) {
                                 if (selectedNamespacedIds.count(
                                         node.ownerLayerId) != 0U)
                                   return true;
                                 self(self, node.children);
                                 return false;
                               }),
                nodes.end());
  };
  eraseSelectedNodes(eraseSelectedNodes, destination.executionGraph.nodes);
  destination.layers.erase(
      std::remove_if(destination.layers.begin(), destination.layers.end(),
                     [&](const auto &layer) {
                       return selectedNamespacedIds.count(layer.layerId) != 0U;
                     }),
      destination.layers.end());
  for (const auto &layer : source.layers)
    if (selectedNamespacedIds.count(layer.layerId) != 0U)
      destination.layers.push_back(layer);
  for (const auto &node : source.executionGraph.nodes)
    if (selectedNamespacedIds.count(node.ownerLayerId) != 0U)
      destination.executionGraph.nodes.push_back(node);
  RebuildEffectProgramClosure(destination, source, sourceProgramIds);
  return true;
}

void ApplyAnimations(text::TextAnimationStack &destination,
                     const text::TextAnimationStack &source,
                     const TextCompositionDocument &destinationDocument,
                     const TextCompositionDocument &sourceDocument,
                     const TextTemplateChannelRule &rule) {
  if (rule.mode == TextTemplateChannelMode::Keep)
    return;
  std::vector<text::TextAnimationLayerSpec> sourceLayers;
  std::unordered_set<std::string> sourceProgramIds;
  for (const auto &layer : source.layers) {
    if (!AnimationMatches(layer, sourceDocument, rule))
      continue;
    sourceLayers.push_back(layer);
    for (const auto &animator : layer.animators)
      if (!animator.effectProgramId.empty())
        sourceProgramIds.insert(animator.effectProgramId);
  }
  if (rule.mode == TextTemplateChannelMode::Clear ||
      rule.mode == TextTemplateChannelMode::Replace) {
    std::unordered_set<std::string> removedLayerIds;
    for (const auto &layer : destination.layers)
      if (AnimationMatches(layer, destinationDocument, rule))
        removedLayerIds.insert(layer.layerId);
    destination.layers.erase(
        std::remove_if(destination.layers.begin(), destination.layers.end(),
                       [&](const auto &layer) {
                         return AnimationMatches(layer, destinationDocument,
                                                 rule);
                       }),
        destination.layers.end());
    if (rule.mode == TextTemplateChannelMode::Clear) {
      const bool fullChannelClear = rule.targetLayerId.empty() &&
                                    rule.contentSlotId.empty() &&
                                    rule.semanticRole.empty();
      if (destination.layers.empty() &&
          (!removedLayerIds.empty() || fullChannelClear)) {
        destination.executionGraph = {};
      } else if (!removedLayerIds.empty()) {
        const auto eraseOwnedNodes = [&](const auto &self,
                                         auto &nodes) -> void {
          nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                                     [&](auto &node) {
                                       if (removedLayerIds.count(
                                               node.ownerLayerId) != 0U)
                                         return true;
                                       self(self, node.children);
                                       return false;
                                     }),
                      nodes.end());
        };
        eraseOwnedNodes(eraseOwnedNodes, destination.executionGraph.nodes);
      }
    }
  }
  if (rule.mode == TextTemplateChannelMode::Replace) {
    destination.layers.insert(destination.layers.end(), sourceLayers.begin(),
                              sourceLayers.end());
  } else if (rule.mode == TextTemplateChannelMode::Merge) {
    for (const auto &layer : sourceLayers) {
      const auto found = std::find_if(
          destination.layers.begin(), destination.layers.end(),
          [&](const auto &current) {
            return current.layerId == layer.layerId &&
                   AnimationMatches(current, destinationDocument, rule);
          });
      if (found == destination.layers.end())
        destination.layers.push_back(layer);
      else
        *found = layer;
    }
  }
  // Full channel replacement consumes the authored graph as one closure.
  // Graph-bearing Merge rules are handled by MergeOwnedAnimationGraphs before
  // reaching this fallback; graphless Merge leaves the destination graph.
  if (rule.mode == TextTemplateChannelMode::Replace) {
    destination.executionGraph = source.executionGraph;
  }
  RebuildEffectProgramClosure(destination, source, sourceProgramIds);
}

bool DecorationMatches(const VectorDecorationBinding &decoration,
                       const TextCompositionDocument &document,
                       const TextTemplateChannelRule &rule) noexcept {
  if (!rule.targetLayerId.empty() &&
      decoration.decorationId != rule.targetLayerId)
    return false;
  if (rule.contentSlotId.empty() && rule.semanticRole.empty())
    return true;
  if (decoration.target.scope == DecorationTargetScope::AllText)
    return std::any_of(
        document.contentSlots.begin(), document.contentSlots.end(),
        [&](const auto &slot) {
          return (rule.contentSlotId.empty() ||
                  slot.slotId == rule.contentSlotId) &&
                 (rule.semanticRole.empty() ||
                  slot.semanticRole == rule.semanticRole);
        });
  const auto slot = std::find_if(
      document.contentSlots.begin(), document.contentSlots.end(),
      [&](const auto &value) {
        return std::find(value.runIds.begin(), value.runIds.end(),
                         decoration.target.runId) != value.runIds.end();
      });
  if (slot == document.contentSlots.end())
    return false;
  return (rule.contentSlotId.empty() ||
          slot->slotId == rule.contentSlotId) &&
         (rule.semanticRole.empty() ||
          slot->semanticRole == rule.semanticRole);
}

void ApplyVectorDecorations(TextCompositionDocument &destination,
                            const TextCompositionDocument &source,
                            const TextTemplateChannelRule &rule,
                            const std::string_view templateSlot) {
  if (rule.mode == TextTemplateChannelMode::Keep)
    return;
  std::vector<VectorDecorationBinding> sourceDecorations;
  for (const auto &decoration : source.decorations)
    if (DecorationMatches(decoration, source, rule))
      sourceDecorations.push_back(decoration);
  // Decoration identities are document-scoped, not selector-scoped.  A
  // template replacement can legitimately carry the same stable id as the
  // previous template while its target/slot metadata has been remapped to
  // the current composition.  Remove those identities before applying the
  // replacement so a stale selector cannot leave two declarations with the
  // same Product registry identity.
  std::unordered_set<std::string> sourceIds;
  for (const auto &decoration : sourceDecorations)
    sourceIds.insert(decoration.decorationId);
  const auto slotMarker = templateSlot.empty()
                              ? std::string{}
                              : "." + std::string(templateSlot) + ".";
  if (rule.mode == TextTemplateChannelMode::Clear ||
      rule.mode == TextTemplateChannelMode::Replace) {
    destination.decorations.erase(
        std::remove_if(destination.decorations.begin(),
                       destination.decorations.end(), [&](const auto &value) {
                         const bool sameTemplateSlot =
                             (rule.mode == TextTemplateChannelMode::Replace ||
                              rule.mode == TextTemplateChannelMode::Clear) &&
                             !slotMarker.empty() &&
                             value.decorationId.find(slotMarker) !=
                                 std::string::npos;
                         const bool selected =
                             slotMarker.empty()
                                 ? DecorationMatches(value, destination, rule)
                                 : sameTemplateSlot;
                         return selected ||
                                (rule.mode == TextTemplateChannelMode::Replace &&
                                 sourceIds.count(value.decorationId) != 0U);
                       }),
        destination.decorations.end());
  }
  if (rule.mode == TextTemplateChannelMode::Replace) {
    destination.decorations.insert(destination.decorations.end(),
                                   sourceDecorations.begin(),
                                   sourceDecorations.end());
  } else if (rule.mode == TextTemplateChannelMode::Merge) {
    for (const auto &decoration : sourceDecorations) {
      const auto found = std::find_if(
          destination.decorations.begin(), destination.decorations.end(),
          [&](const auto &current) {
            return current.decorationId == decoration.decorationId;
          });
      if (found == destination.decorations.end())
        destination.decorations.push_back(decoration);
      else
        *found = decoration;
    }
  }
}

bool TimedSpanMatches(const TimedTextSpan &span,
                      const TextCompositionDocument &document,
                      const TextTemplateChannelRule &rule) noexcept {
  if (rule.contentSlotId.empty() && rule.semanticRole.empty())
    return true;
  const auto slot = std::find_if(
      document.contentSlots.begin(), document.contentSlots.end(),
      [&](const auto &value) {
        return std::find(value.runIds.begin(), value.runIds.end(), span.runId) !=
               value.runIds.end();
      });
  return slot != document.contentSlots.end() &&
         (rule.contentSlotId.empty() ||
          slot->slotId == rule.contentSlotId) &&
         (rule.semanticRole.empty() ||
          slot->semanticRole == rule.semanticRole);
}

void ApplyTimedText(TextCompositionDocument &destination,
                    const TextCompositionDocument &source,
                    const TextTemplateChannelRule &rule) {
  if (rule.mode == TextTemplateChannelMode::Keep)
    return;
  std::vector<TimedTextSpan> sourceSpans;
  if (source.timedText)
    for (const auto &span : source.timedText->spans)
      if (TimedSpanMatches(span, source, rule))
        sourceSpans.push_back(span);
  if (!destination.timedText)
    destination.timedText = TimedTextTrack{};
  auto &spans = destination.timedText->spans;
  if (rule.mode == TextTemplateChannelMode::Clear ||
      rule.mode == TextTemplateChannelMode::Replace) {
    spans.erase(std::remove_if(spans.begin(), spans.end(), [&](const auto &span) {
                  return TimedSpanMatches(span, destination, rule);
                }),
                spans.end());
  }
  if (rule.mode == TextTemplateChannelMode::Replace) {
    spans.insert(spans.end(), sourceSpans.begin(), sourceSpans.end());
  } else if (rule.mode == TextTemplateChannelMode::Merge) {
    for (const auto &span : sourceSpans) {
      const auto found = std::find_if(
          spans.begin(), spans.end(),
          [&](const auto &current) {
            return current.spanId == span.spanId &&
                   TimedSpanMatches(current, destination, rule);
          });
      if (found == spans.end())
        spans.push_back(span);
      else
        *found = span;
    }
  }
  if (spans.empty())
    destination.timedText.reset();
}

void ApplyResources(TextCompositionDocument &destination,
                    const TextCompositionDocument &source,
                    const TextTemplateChannelRule &rule) {
  if (rule.mode == TextTemplateChannelMode::Keep)
    return;
  if (rule.mode == TextTemplateChannelMode::Clear)
    destination.resources.clear();
  else if (rule.mode == TextTemplateChannelMode::Replace)
    destination.resources = source.resources;
  else {
    for (const auto &resource : source.resources) {
      const auto found = std::find_if(
          destination.resources.begin(), destination.resources.end(),
          [&](const auto &current) {
            return current.resourceId == resource.resourceId;
          });
      if (found == destination.resources.end())
        destination.resources.push_back(resource);
      else
        *found = resource;
    }
  }
}

template <typename MaterialCallback, typename TextureCallback>
void VisitBindingResources(const text::TextMaterialBinding &binding,
                           MaterialCallback &&materialCallback,
                           TextureCallback &&textureCallback) {
  const auto visit = [&](const text::TextMaterial &material) {
    materialCallback(material);
    if (const auto *texture =
            std::get_if<text::TextureTextMaterial>(&material))
      textureCallback(texture->texture, TextResourceKind::Texture);
  };
  if (const auto *literal =
          std::get_if<text::LiteralTextMaterial>(&binding)) {
    visit(literal->material);
  } else if (const auto *slot =
                 std::get_if<text::EditableTextStyleSlot>(&binding)) {
    visit(slot->fallback);
    if (slot->replacementMask)
      textureCallback(*slot->replacementMask, TextResourceKind::Texture);
  }
}

struct ResourceNeed final {
  std::string assetId;
  std::string digest;
  std::string mediaType;
  TextResourceKind kind{TextResourceKind::Texture};
  std::string resourceId;
  std::optional<TextResourceOwnership> ownership;
};

std::vector<ResourceNeed>
CollectResourceNeeds(const TextCompositionDocument &document) {
  std::vector<ResourceNeed> result;
  const auto texture = [&](const text::TextureReference &value,
                           const TextResourceKind kind) {
    result.push_back({
        value.assetId, value.digest, value.mediaType, kind, {},
        value.sourceKind == text::TextureSourceKind::Builtin
            ? TextResourceOwnership::Builtin
            : TextResourceOwnership::ProjectManaged});
  };
  const auto material = [](const text::TextMaterial &) {};
  const auto binding = [&](const text::TextMaterialBinding &value) {
    VisitBindingResources(value, material, texture);
  };
  for (const auto &paragraph : document.content) {
    for (const auto &run : paragraph.runs) {
      const auto font = [&](const text::FontReference &value) {
        if (value.kind == text::FontSourceKind::ProjectManaged)
          result.push_back({value.assetId, value.digest, {},
                            TextResourceKind::Font, {},
                            TextResourceOwnership::ProjectManaged});
      };
      font(run.style.font.primary);
      for (const auto &fallback : run.style.font.fallbacks)
        font(fallback);
      for (const auto &layer : run.style.materials.layers) {
        std::visit(
            [&](const auto &typed) {
              binding(typed.material);
              using Layer = std::decay_t<decltype(typed)>;
              if constexpr (std::is_same_v<Layer, text::TextShadowLayer>) {
                for (const auto &stroke : typed.strokes)
                  binding(stroke.material);
              }
            },
            layer);
      }
      binding(run.style.decoration.underline.material);
      binding(run.style.decoration.strikeThrough.material);
    }
  }
  for (const auto &layer : document.presentation.appearance.backdrops.layers) {
    if (layer.materialOverride)
      binding(*layer.materialOverride);
    std::visit(
        [&](const auto &source) {
          using Source = std::decay_t<decltype(source)>;
          if constexpr (std::is_same_v<Source, text::RoundedRectBackdrop>) {
            binding(source.fill);
            for (const auto &stroke : source.strokes)
              binding(stroke.material);
          } else if constexpr (std::is_same_v<Source,
                                              text::AnimatedBackdrop>) {
            texture(source.asset, TextResourceKind::Animated);
          } else {
            texture(source.asset, TextResourceKind::Texture);
          }
        },
        layer.source);
  }
  for (const auto &decoration : document.decorations) {
    const auto asset = [&](const DecorationAssetReference &value) {
      const auto animated = value.mediaType.rfind("video/", 0U) == 0U ||
                            value.mediaType == "image/gif" ||
                            value.mediaType == "image/apng" ||
                            value.mediaType == "image/webp+animated";
      const auto vector = value.mediaType == "application/json" ||
                          value.mediaType == "application/lottie+json" ||
                          value.mediaType == "image/svg+xml";
      result.push_back({value.assetId, value.digest, value.mediaType,
                        vector ? TextResourceKind::Vector
                               : animated ? TextResourceKind::Animated
                                          : TextResourceKind::Texture,
                        {}, std::nullopt});
    };
    asset(decoration.asset);
    if (decoration.instancePattern)
      for (const auto &variant : decoration.instancePattern->assetVariants)
        asset(variant.asset);
    if (!decoration.fallbackAssetId.empty())
      result.push_back({{}, {}, {}, TextResourceKind::Texture,
                        decoration.fallbackAssetId, std::nullopt});
  }
  const auto executionResources =
      [&](const auto &self,
          const std::vector<text::TextEffectExecutionNode> &nodes) -> void {
    for (const auto &node : nodes) {
      for (const auto &resourceId : node.resourceIds)
        result.push_back({{}, {}, {}, TextResourceKind::Texture, resourceId,
                          std::nullopt});
      self(self, node.children);
    }
  };
  executionResources(executionResources,
                     document.animations.executionGraph.nodes);
  return result;
}

bool ResourceSatisfies(const TextResourceReference &resource,
                       const ResourceNeed &need) noexcept {
  if (!need.resourceId.empty())
    return (resource.resourceId == need.resourceId ||
            resource.assetId == need.resourceId) &&
           resource.kind != TextResourceKind::Font;
  if (resource.assetId != need.assetId || resource.digest != need.digest ||
      (!need.mediaType.empty() && resource.mediaType != need.mediaType) ||
      (need.ownership && resource.ownership != *need.ownership))
    return false;
  return resource.kind == need.kind ||
         (need.kind == TextResourceKind::Texture &&
          resource.kind == TextResourceKind::Animated);
}

bool CloseResources(TextCompositionDocument &destination,
                    const TextCompositionDocument &source,
                    const TextCompositionDocument &retainedSource,
                    std::vector<Diagnostic> &diagnostics) {
  bool closed = true;
  for (const auto &need : CollectResourceNeeds(destination)) {
    if (std::any_of(destination.resources.begin(), destination.resources.end(),
                    [&](const auto &resource) {
                      return ResourceSatisfies(resource, need);
                    }))
      continue;
    const TextResourceReference *provider = nullptr;
    bool identityCollision = false;
    const auto findProvider = [&](const auto &resources) {
      for (const auto &resource : resources) {
        if (!ResourceSatisfies(resource, need))
          continue;
        const auto collision = std::find_if(
            destination.resources.begin(), destination.resources.end(),
            [&](const auto &current) {
              return current.resourceId == resource.resourceId;
            });
        if (collision == destination.resources.end()) {
          provider = &resource;
          return;
        }
        identityCollision = true;
      }
    };
    findProvider(source.resources);
    if (!provider)
      findProvider(retainedSource.resources);
    if (!provider) {
      closed = false;
      Add(diagnostics,
          identityCollision ? "text_template.resource_identity_collision"
                            : "text_template.resource_closure_missing",
          need.resourceId.empty() ? need.assetId : need.resourceId,
          identityCollision
              ? "required resource identity conflicts with an unaffected resource"
              : "applied channels reference a resource absent from both admitted closures");
      continue;
    }
    destination.resources.push_back(*provider);
  }
  return closed;
}

void ApplyScalarChannel(TextCompositionDocument &destination,
                        const TextCompositionDocument &source,
                        const TextTemplateChannelRule &rule) {
  if (rule.mode == TextTemplateChannelMode::Keep)
    return;
  const bool clear = rule.mode == TextTemplateChannelMode::Clear;
  switch (rule.channel) {
  case TextTemplateChannel::Layout:
    if (clear) {
      destination.presentation.referenceCanvas = {};
      destination.presentation.authoredLayoutFrame = {};
      destination.presentation.writingMode = text::TextWritingMode::Horizontal;
      destination.presentation.appearance.visualExtent = {};
      destination.presentation.appearance.globalAlpha = 1.0F;
    } else {
      destination.presentation.referenceCanvas =
          source.presentation.referenceCanvas;
      destination.presentation.authoredLayoutFrame =
          source.presentation.authoredLayoutFrame;
      destination.presentation.writingMode = source.presentation.writingMode;
      destination.presentation.appearance.visualExtent =
          source.presentation.appearance.visualExtent;
      destination.presentation.appearance.globalAlpha =
          source.presentation.appearance.globalAlpha;
    }
    break;
  case TextTemplateChannel::Bend:
    destination.presentation.appearance.bend =
        clear ? text::TextBend{} : source.presentation.appearance.bend;
    break;
  case TextTemplateChannel::Path:
    destination.presentation.appearance.path =
        clear ? text::TextPath{} : source.presentation.appearance.path;
    break;
  case TextTemplateChannel::SdfMaterial:
    destination.presentation.appearance.sdfMaterial =
        clear ? text::TextSdfMaterial{}
              : source.presentation.appearance.sdfMaterial;
    break;
  default:
    break;
  }
}

bool ChannelWritesProperty(const TextTemplateChannel channel,
                           const text::TextPropertyId property) {
  using P = text::TextPropertyId;
  switch (channel) {
  case TextTemplateChannel::Content:
    return property <= P::ParagraphTabStops ||
           property == P::SemanticMaterialChannel;
  case TextTemplateChannel::Typography:
    return property >= P::FontReference && property <= P::BaselineShift;
  case TextTemplateChannel::InlineDecorations:
    return property >= P::UnderlineEnabled &&
           property <= P::StrikeThroughOffset;
  case TextTemplateChannel::GlyphFills:
    return (property >= P::GlyphFill &&
            property <= P::GlyphFillGradientRadius) ||
           property == P::SemanticMaterialChannel;
  case TextTemplateChannel::GlyphStrokes:
    return (property >= P::GlyphStrokeStack &&
            property <= P::GlyphStrokeOpacity) ||
           property == P::SemanticMaterialChannel;
  case TextTemplateChannel::GlyphShadows:
    return (property >= P::GlyphShadowStack &&
            property <= P::GlyphShadowAngle) ||
           property == P::SemanticMaterialChannel;
  case TextTemplateChannel::GlyphGlows:
    return (property >= P::GlyphGlowStack &&
            property <= P::GlyphGlowDirectionY) ||
           property == P::SemanticMaterialChannel;
  case TextTemplateChannel::Paragraph:
    return property >= P::ParagraphBackground &&
           property <= P::ParagraphTabStops;
  case TextTemplateChannel::Layout:
    return (property >= P::LayoutFrame && property <= P::WritingMode) ||
           property == P::GlobalAlpha;
  case TextTemplateChannel::Bubble:
  case TextTemplateChannel::Frame:
  case TextTemplateChannel::CustomBackdrops:
    return (property >= P::BackdropStack && property <= P::CustomBackdrop) ||
           property == P::BackdropEnabled;
  case TextTemplateChannel::Bend:
    return property == P::Bend;
  case TextTemplateChannel::Path:
    return property == P::Path;
  case TextTemplateChannel::SdfMaterial:
    return property == P::SdfMaterial;
  default:
    return false;
  }
}

struct TemplatePropertyCoverage {
  text::TextPropertyAddress address;
  std::unordered_set<std::string> targets;
  std::unordered_set<std::string> overwritten;
};

TemplatePropertyCoverage
PropertyCoverage(const TextCompositionDocument &document,
                 const text::TextPropertyAddress &address) {
  using P = text::TextPropertyId;
  TemplatePropertyCoverage result{address, {}, {}};
  const auto property = address.property;
  if (property >= P::ParagraphBackground && property <= P::ParagraphTabStops) {
    result.targets = text::ResolveTextPropertyParagraphIds(
        address.target, document.contentSlots, document.content);
  } else if (property < P::ParagraphBackground ||
             property == P::SemanticMaterialChannel) {
    result.targets = text::ResolveTextPropertyRunIds(
        address.target, document.contentSlots, document.content);
  } else if ((property >= P::BackdropStack && property <= P::CustomBackdrop) ||
             property == P::BackdropEnabled) {
    for (const auto &layer :
         document.presentation.appearance.backdrops.layers) {
      if (!address.target.layerId.empty() &&
          address.target.layerId != layer.layerId)
        continue;
      if (property == P::FrameBackdrop &&
          layer.channel != text::TextBackdropChannel::Frame)
        continue;
      if (property == P::BubbleBackdrop &&
          layer.channel != text::TextBackdropChannel::Bubble)
        continue;
      if (property == P::CustomBackdrop &&
          layer.channel != text::TextBackdropChannel::Custom)
        continue;
      result.targets.insert(layer.layerId);
    }
  } else {
    result.targets.insert("");
  }
  return result;
}

void RecordPropertyCoverage(TemplatePropertyCoverage &coverage,
                            TextCompositionDocument &destination,
                            const TextCompositionDocument &source,
                            const TextTemplateChannelRule &rule) {
  if (rule.mode == TextTemplateChannelMode::Keep ||
      !ChannelWritesProperty(rule.channel, coverage.address.property))
    return;
  const auto record = [&](const std::string &id) {
    if (coverage.targets.count(id))
      coverage.overwritten.insert(id);
  };
  if (rule.channel == TextTemplateChannel::Content) {
    if (rule.mode != TextTemplateChannelMode::Replace)
      return;
    for (const auto &slot : destination.contentSlots) {
      if (!SlotMatches(slot, rule, true))
        continue;
      for (const auto &id : slot.runIds)
        record(id);
      for (const auto &id : slot.paragraphIds)
        record(id);
    }
  } else if (BackdropChannel(rule.channel)) {
    for (const auto &layer :
         destination.presentation.appearance.backdrops.layers) {
      if (!BackdropMatches(layer, rule))
        continue;
      if (rule.mode == TextTemplateChannelMode::Merge &&
          std::none_of(source.presentation.appearance.backdrops.layers.begin(),
                       source.presentation.appearance.backdrops.layers.end(),
                       [&](const auto &value) {
                         return value.layerId == layer.layerId &&
                                BackdropMatches(value, rule);
                       }))
        continue;
      record(layer.layerId);
    }
  } else if (rule.channel == TextTemplateChannel::Paragraph) {
    for (const auto &slot : destination.contentSlots) {
      if (!SlotMatches(slot, rule, true))
        continue;
      const auto *prototype = SourceSlotFor(source, slot, rule);
      if (!prototype || prototype->paragraphIds.empty())
        continue;
      for (const auto &id : slot.paragraphIds)
        record(id);
    }
  } else if (MaterialChannel(rule.channel) ||
             rule.channel == TextTemplateChannel::Typography ||
             rule.channel == TextTemplateChannel::InlineDecorations) {
    ForEachRunPair(
        destination, source, rule, !MaterialChannel(rule.channel),
        [&](const auto &run, const auto &prototype) {
          if (!MaterialChannel(rule.channel)) {
            record(run.runId);
            return;
          }
          const auto &layers = run.style.materials.layers;
          const auto layer = std::find_if(
              layers.begin(), layers.end(), [&](const auto &value) {
                return coverage.address.target.layerId.empty()
                           ? Family(value) == Family(rule.channel)
                           : LayerId(value) == coverage.address.target.layerId;
              });
          if (layer == layers.end() || !GlyphLayerMatches(*layer, rule))
            return;
          if (rule.mode == TextTemplateChannelMode::Merge &&
              std::none_of(prototype.style.materials.layers.begin(),
                           prototype.style.materials.layers.end(),
                           [&](const auto &value) {
                             return LayerId(value) == LayerId(*layer) &&
                                    GlyphLayerMatches(value, rule);
                           }))
            return;
          record(run.runId);
        });
  } else {
    record("");
  }
}

void ApplyRule(TextCompositionDocument &destination,
               const TextCompositionDocument &source,
               const TextTemplateChannelRule &rule,
               const std::string_view templateDecorationSlot) {
  switch (rule.channel) {
  case TextTemplateChannel::Content:
    ApplyContentChannel(destination, source, rule);
    break;
  case TextTemplateChannel::Typography:
  case TextTemplateChannel::GlyphFills:
  case TextTemplateChannel::GlyphStrokes:
  case TextTemplateChannel::GlyphShadows:
  case TextTemplateChannel::GlyphGlows:
  case TextTemplateChannel::InlineDecorations:
    ApplyRunChannel(destination, source, rule);
    break;
  case TextTemplateChannel::Paragraph:
    ApplyParagraphChannel(destination, source, rule);
    break;
  case TextTemplateChannel::Layout:
  case TextTemplateChannel::Bend:
  case TextTemplateChannel::Path:
  case TextTemplateChannel::SdfMaterial:
    ApplyScalarChannel(destination, source, rule);
    break;
  case TextTemplateChannel::Bubble:
  case TextTemplateChannel::Frame:
  case TextTemplateChannel::CustomBackdrops:
    ApplyBackdrops(destination.presentation.appearance.backdrops,
                   source.presentation.appearance.backdrops, rule);
    break;
  case TextTemplateChannel::Animations:
    ApplyAnimations(destination.animations, source.animations, destination,
                    source, rule);
    break;
  case TextTemplateChannel::VectorDecorations:
    ApplyVectorDecorations(destination, source, rule,
                           templateDecorationSlot);
    break;
  case TextTemplateChannel::TimedText:
    ApplyTimedText(destination, source, rule);
    break;
  case TextTemplateChannel::Resources:
    ApplyResources(destination, source, rule);
    break;
  }
}

bool HasBackdropSelector(const TextCompositionDocument &document,
                         const TextTemplateChannelRule &rule) noexcept {
  return std::any_of(
      document.presentation.appearance.backdrops.layers.begin(),
      document.presentation.appearance.backdrops.layers.end(),
      [&](const auto &layer) { return BackdropMatches(layer, rule); });
}

bool HasGlyphSelector(const TextCompositionDocument &document,
                      const TextTemplateChannelRule &rule) noexcept {
  for (const auto &slot : document.contentSlots) {
    if (!SlotMatches(slot, rule, false))
      continue;
    for (const auto *run : RunsForSlot(document, slot))
      if (std::any_of(run->style.materials.layers.begin(),
                      run->style.materials.layers.end(), [&](const auto &layer) {
                        return GlyphLayerMatches(layer, rule);
                      }))
        return true;
  }
  return false;
}

bool RuleHasMatch(const TextCompositionDocument &document,
                  const TextTemplateChannelRule &rule) noexcept {
  if (rule.contentSlotId.empty() && rule.targetLayerId.empty() &&
      rule.semanticRole.empty())
    return true;
  if (rule.targetLayerId.empty() &&
      (!rule.contentSlotId.empty() || !rule.semanticRole.empty()) &&
      std::any_of(document.contentSlots.begin(), document.contentSlots.end(),
                  [&](const auto &slot) {
                    return SlotMatches(slot, rule, true);
                  }))
    return true;
  if (MaterialChannel(rule.channel))
    return HasGlyphSelector(document, rule);
  if (BackdropChannel(rule.channel)) {
    if (!rule.contentSlotId.empty() &&
        !FindSlot(document, rule.contentSlotId))
      return false;
    return HasBackdropSelector(document, rule);
  }
  if (rule.channel == TextTemplateChannel::Animations)
    return std::any_of(document.animations.layers.begin(),
                       document.animations.layers.end(), [&](const auto &layer) {
                         return AnimationMatches(layer, document, rule);
                       });
  if (rule.channel == TextTemplateChannel::VectorDecorations)
    return std::any_of(document.decorations.begin(), document.decorations.end(),
                       [&](const auto &decoration) {
                         return DecorationMatches(decoration, document, rule);
                       });
  if (rule.channel == TextTemplateChannel::TimedText)
    return document.timedText &&
           std::any_of(document.timedText->spans.begin(),
                       document.timedText->spans.end(), [&](const auto &span) {
                         return TimedSpanMatches(span, document, rule);
                       });
  return std::any_of(document.contentSlots.begin(), document.contentSlots.end(),
                     [&](const auto &slot) {
                       return SlotMatches(slot, rule, true);
                     });
}

void AppendChanged(std::vector<TextTemplateChannel> &channels,
                   const TextTemplateChannel channel) {
  if (std::find(channels.begin(), channels.end(), channel) == channels.end())
    channels.push_back(channel);
}

bool RequiresTimedText(
    const text::TextAnimationStack &animations) noexcept {
  if (std::any_of(animations.layers.begin(), animations.layers.end(),
                  [](const auto &layer) {
                    return layer.requiresTimedText ||
                           layer.timeDriver.kind ==
                               text::TextAnimationTimeDriverKind::TimedRanges;
                  })) {
    return true;
  }
  const auto graphRequiresTimedText =
      [&](const auto &self,
          const std::vector<text::TextEffectExecutionNode> &nodes) -> bool {
    return std::any_of(nodes.begin(), nodes.end(), [&](const auto &node) {
      return (node.timeDriver &&
              node.timeDriver->kind ==
                  text::TextAnimationTimeDriverKind::TimedRanges) ||
             self(self, node.children);
    });
  };
  return graphRequiresTimedText(graphRequiresTimedText,
                                animations.executionGraph.nodes);
}

bool HasContentAuthority(
    const std::vector<TextTemplateChannelRule> &rules) noexcept {
  return std::any_of(rules.begin(), rules.end(), [](const auto &rule) {
    return rule.channel == TextTemplateChannel::Content &&
           rule.mode != TextTemplateChannelMode::Keep;
  });
}

TextTemplateTextBinding ParseTextBinding(const Json &value,
                                         const std::string &context) {
  OnlyKeys(value,
           {"id", "kind", "default", "target", "content_slot_id"},
           context);
  if (String(value, "kind", context) != "plain_text")
    Invalid(context + ".kind", "must be plain_text");
  const auto &target = Object(value, "target", context);
  OnlyKeys(target, {"paragraph_id", "run_id"}, context + ".target");
  (void)String(target, "paragraph_id", context + ".target");
  (void)String(target, "run_id", context + ".target");
  TextTemplateTextBinding result;
  result.bindingId = String(value, "id", context);
  result.contentSlotId = String(value, "content_slot_id", context);
  result.defaultText = String(value, "default", context, true);
  return result;
}

TextTemplateChannelRule ParseRule(const Json &value,
                                  const std::string &context) {
  OnlyKeys(value,
           {"channel", "mode", "content_slot_id", "target_layer_id",
            "semantic_role"},
           context);
  TextTemplateChannelRule result;
  result.channel = ParseChannel(value.at("channel"), context + ".channel");
  result.mode = ParseChannelMode(value.at("mode"), context + ".mode");
  result.contentSlotId = String(value, "content_slot_id", context, true);
  result.targetLayerId = String(value, "target_layer_id", context, true);
  result.semanticRole = String(value, "semantic_role", context, true);
  return result;
}

} // namespace

void PruneResourcesNoLongerRequired(
    TextCompositionDocument &document,
    const TextCompositionDocument &before) {
  const auto beforeNeeds = CollectResourceNeeds(before);
  const auto afterNeeds = CollectResourceNeeds(document);
  document.resources.erase(
      std::remove_if(document.resources.begin(), document.resources.end(),
                     [&](const auto &resource) {
                       return std::any_of(
                                  beforeNeeds.begin(), beforeNeeds.end(),
                                  [&](const auto &need) {
                                    return ResourceSatisfies(resource, need);
                                  }) &&
                              std::none_of(afterNeeds.begin(),
                                           afterNeeds.end(),
                                           [&](const auto &need) {
                                             return ResourceSatisfies(resource,
                                                                      need);
                                           });
                     }),
      document.resources.end());
}

void RecordTextTemplateSlotOrigins(TextCompositionDocument &document,
                                   const TextTemplatePatch &patch) {
  const TextTemplateOrigin origin{patch.templateId, patch.packageDigest};
  for (const auto &rule : patch.rules) {
    if (rule.mode == TextTemplateChannelMode::Keep) continue;
    switch (rule.channel) {
    case TextTemplateChannel::Bubble:
      document.bubbleTemplateOrigin = origin;
      break;
    case TextTemplateChannel::Animations:
      document.animationTemplateOrigin = origin;
      break;
    case TextTemplateChannel::Typography:
    case TextTemplateChannel::GlyphFills:
    case TextTemplateChannel::GlyphStrokes:
    case TextTemplateChannel::GlyphShadows:
    case TextTemplateChannel::GlyphGlows:
    case TextTemplateChannel::InlineDecorations:
    case TextTemplateChannel::Paragraph:
    case TextTemplateChannel::Layout:
    case TextTemplateChannel::SdfMaterial:
      document.flowerTemplateOrigin = origin;
      break;
    default:
      break;
    }
  }
}

TextTemplateApplicationResult ClearTextTemplateAppearanceSlot(
    TextCompositionDocument &document, const TextTemplateAppearanceSlot slot,
    const std::vector<text::TextPropertyAddress> &animatedProperties) {
  TextTemplateApplicationResult result;
  TextCompositionDocument candidate = document;
  const auto beforeNeeds = CollectResourceNeeds(document);
  std::unordered_set<std::string> affectedLayerIds;
  std::unordered_set<std::string> retainedLayerIds;
  for (const auto &animation : candidate.animations.layers) {
    if (slot == TextTemplateAppearanceSlot::Bubble &&
        animation.target.scope == text::TextPropertyScope::BackdropLayer)
      retainedLayerIds.insert(animation.target.layerId);
    if (slot == TextTemplateAppearanceSlot::Flower &&
        animation.target.scope == text::TextPropertyScope::GlyphMaterialLayer)
      retainedLayerIds.insert(animation.target.layerId);
  }

  if (slot == TextTemplateAppearanceSlot::Bubble) {
    auto &layers = candidate.presentation.appearance.backdrops.layers;
    layers.erase(std::remove_if(layers.begin(), layers.end(),
        [&](auto &layer) {
          if (layer.channel != text::TextBackdropChannel::Bubble)
            return false;
          affectedLayerIds.insert(layer.layerId);
          if (retainedLayerIds.count(layer.layerId) != 0U) {
            layer.enabled = false;
            layer.source = text::RoundedRectBackdrop{};
            std::get<text::RoundedRectBackdrop>(layer.source).fill =
                text::LiteralTextMaterial{text::SolidTextMaterial{
                    text::Color{0.0F, 0.0F, 0.0F, 0.0F}}};
            layer.materialOverride.reset();
            layer.animationClips.clear();
            return false;
          }
          return true;
        }), layers.end());
    candidate.bubbleTemplateOrigin.reset();
  } else {
    const text::TextMaterialBinding transparent =
        text::LiteralTextMaterial{text::SolidTextMaterial{
            text::Color{0.0F, 0.0F, 0.0F, 0.0F}}};
    const text::TextMaterialBinding white =
        text::LiteralTextMaterial{text::SolidTextMaterial{
            text::Color{1.0F, 1.0F, 1.0F, 1.0F}}};
    for (auto &paragraph : candidate.content) {
      for (auto &run : paragraph.runs) {
        std::vector<text::TextGlyphMaterialLayer> retained;
        bool hasFill = false;
        for (auto layer : run.style.materials.layers) {
          const auto id = LayerId(layer);
          affectedLayerIds.insert(id);
          if (retainedLayerIds.count(id) == 0U) continue;
          std::visit([&](auto &typed) {
            using Layer = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Layer, text::TextFillLayer>) {
              typed.material = white;
              hasFill = true;
            } else {
              typed.material = transparent;
              if constexpr (std::is_same_v<Layer, text::TextShadowLayer>)
                typed.strokes.clear();
            }
          }, layer);
          retained.push_back(std::move(layer));
        }
        if (!hasFill)
          retained.insert(retained.begin(), text::TextFillLayer{});
        run.style.materials.layers = std::move(retained);
      }
    }
    candidate.presentation.appearance.sdfMaterial = text::TextSdfMaterial{};
    candidate.flowerTemplateOrigin.reset();
  }

  const auto &slotOrigin = slot == TextTemplateAppearanceSlot::Bubble
      ? document.bubbleTemplateOrigin : document.flowerTemplateOrigin;
  if (document.templateOrigin && slotOrigin &&
      document.templateOrigin->templateId == slotOrigin->templateId &&
      document.templateOrigin->packageDigest == slotOrigin->packageDigest)
    candidate.templateOrigin.reset();

  const auto needs = CollectResourceNeeds(candidate);
  auto &resources = candidate.resources;
  resources.erase(std::remove_if(resources.begin(), resources.end(),
      [&](const auto &resource) {
        const bool wasRequired = std::any_of(beforeNeeds.begin(), beforeNeeds.end(),
            [&](const auto &need) { return ResourceSatisfies(resource, need); });
        return wasRequired && std::none_of(needs.begin(), needs.end(),
            [&](const auto &need) { return ResourceSatisfies(resource, need); });
      }), resources.end());

  const auto validation = ValidateTextCompositionDocument(candidate);
  if (!validation.valid) {
    result.diagnostics = validation.diagnostics;
    return result;
  }
  for (const auto &address : animatedProperties) {
    bool overwritten = slot == TextTemplateAppearanceSlot::Bubble
        ? address.property == text::TextPropertyId::BubbleBackdrop ||
          (!address.target.layerId.empty() &&
           affectedLayerIds.count(address.target.layerId) != 0U)
        : ChannelWritesProperty(TextTemplateChannel::GlyphFills, address.property) ||
          ChannelWritesProperty(TextTemplateChannel::GlyphStrokes, address.property) ||
          ChannelWritesProperty(TextTemplateChannel::GlyphShadows, address.property) ||
          ChannelWritesProperty(TextTemplateChannel::GlyphGlows, address.property) ||
          ChannelWritesProperty(TextTemplateChannel::SdfMaterial, address.property);
    if (slot == TextTemplateAppearanceSlot::Flower &&
        address.property == text::TextPropertyId::SemanticMaterialChannel)
      overwritten = !address.target.layerId.empty() &&
          affectedLayerIds.count(address.target.layerId) != 0U;
    if (overwritten) result.overwrittenProperties.push_back(address);
  }
  result.changed = ComputeTextCompositionDocumentIdentity(candidate) !=
                   ComputeTextCompositionDocumentIdentity(document);
  result.changedChannels.push_back(slot == TextTemplateAppearanceSlot::Bubble
      ? TextTemplateChannel::Bubble : TextTemplateChannel::GlyphFills);
  document = std::move(candidate);
  result.valid = true;
  return result;
}

std::string_view TextTemplateDecorationIdentitySlot(
    const TextTemplatePatch &patch) noexcept {
  // Content-authoritative patches replace the document identity closure and
  // therefore do not need a composable Inspector-slot namespace.  This is a
  // rule/schema property, independent of package or template identity.
  if (HasContentAuthority(patch.rules))
    return {};
  const bool animationTemplate =
      std::any_of(patch.rules.begin(), patch.rules.end(), [](const auto &rule) {
        return rule.channel == TextTemplateChannel::Animations;
      });
  return animationTemplate ? std::string_view{"animation"}
                           : std::string_view{"flower"};
}

TextTemplateParseResult ParseTextTemplateJson(
    const std::string &json, const TextCompositionLimits &limits) {
  TextTemplateParseResult result;
  if (json.size() > 8U * 1024U * 1024U) {
    Add(result.diagnostics, "text_template.byte_limit", {},
        "template entry exceeds the bounded parser budget");
    return result;
  }
  try {
    const auto root = Json::parse(json);
    OnlyKeys(root,
             {"format", "id", "display", "placement", "bindings", "rules",
              "document", "appearance", "decorations", "resources"},
             "template");
    if (String(root, "format", "template") != "videocut.text-template")
      Invalid("template.format", "must identify the current template entry");
    const auto &display = Object(root, "display", "template");
    OnlyKeys(display, {"name_key", "fallback_name", "category", "tags"},
             "template.display");
    (void)String(display, "name_key", "template.display");
    (void)String(display, "fallback_name", "template.display", true);
    (void)String(display, "category", "template.display");
    (void)StringArray(display, "tags", "template.display");
    result.value.templateId = String(root, "id", "template");
    result.value.value = ParseTemplateDocument(root);
    const auto &bindings = Array(root, "bindings", "template");
    result.value.textBindings.reserve(bindings.size());
    for (std::size_t index = 0U; index < bindings.size(); ++index) {
      const auto context =
          "template.bindings[" + std::to_string(index) + "]";
      auto binding = ParseTextBinding(bindings[index], context);
      const auto &target = Object(bindings[index], "target", context);
      const auto paragraphId = String(target, "paragraph_id", context);
      const auto runId = String(target, "run_id", context);
      const auto *slot = FindSlot(result.value.value, binding.contentSlotId);
      const auto *paragraph = FindParagraph(result.value.value, paragraphId);
      const auto *run = FindRun(result.value.value, runId);
      if (!slot || !paragraph || !run ||
          std::find(slot->paragraphIds.begin(), slot->paragraphIds.end(),
                    paragraphId) == slot->paragraphIds.end() ||
          std::find(slot->runIds.begin(), slot->runIds.end(), runId) ==
              slot->runIds.end())
        Invalid(context + ".target",
                "must resolve inside its declared content slot");
      result.value.textBindings.push_back(std::move(binding));
    }
    const auto &rules = Array(root, "rules", "template");
    result.value.rules.reserve(rules.size());
    for (std::size_t index = 0U; index < rules.size(); ++index)
      result.value.rules.push_back(ParseRule(
          rules[index], "template.rules[" + std::to_string(index) + "]"));

    if (!ValidatePatchMetadata(result.value, limits, result.diagnostics,
                               false))
      return result;
    const auto validation =
        ValidateTextCompositionDocument(result.value.value, limits);
    result.diagnostics.insert(result.diagnostics.end(),
                              validation.diagnostics.begin(),
                              validation.diagnostics.end());
    result.valid = validation.valid;
  } catch (const Json::exception &exception) {
    Add(result.diagnostics, "text_template.json_invalid", {},
        exception.what());
  } catch (const std::exception &exception) {
    Add(result.diagnostics, "text_template.schema_invalid", {},
        exception.what());
  }
  return result;
}

TextTemplateApplicationResult ApplyTextTemplatePatch(
    TextCompositionDocument &document, const TextTemplatePatch &patch,
    const std::map<std::string, std::string> &textOverrides,
    const TextCompositionLimits &limits,
    const std::string_view decorationIdentityNamespace,
    const std::vector<text::TextPropertyAddress> &animatedProperties) {
  TextTemplateApplicationResult result;
  if (!ValidatePatchMetadata(patch, limits, result.diagnostics))
    return result;
  const auto targetValidation = ValidateTextCompositionDocument(document, limits);
  result.diagnostics.insert(result.diagnostics.end(),
                            targetValidation.diagnostics.begin(),
                            targetValidation.diagnostics.end());
  if (!targetValidation.valid)
    return result;
  const auto sourceValidation =
      ValidateTextCompositionDocument(patch.value, limits);
  result.diagnostics.insert(result.diagnostics.end(),
                            sourceValidation.diagnostics.begin(),
                            sourceValidation.diagnostics.end());
  if (!sourceValidation.valid)
    return result;
  for (const auto &overrideEntry : textOverrides) {
    const auto &bindingId = overrideEntry.first;
    const auto &textValue = overrideEntry.second;
    if (!text::IsValidUtf8(textValue) ||
        std::none_of(patch.textBindings.begin(), patch.textBindings.end(),
                     [&](const auto &binding) {
                       return binding.bindingId == bindingId;
                     })) {
      Add(result.diagnostics, "text_template.override_invalid", bindingId,
          "text overrides require a declared binding and valid UTF-8");
      return result;
    }
  }
  for (const auto &rule : patch.rules) {
    if (!RuleHasMatch(document, rule) && !RuleHasMatch(patch.value, rule)) {
      Add(result.diagnostics, "text_template.selector_unresolved",
          rule.targetLayerId.empty() ? rule.contentSlotId : rule.targetLayerId,
          "channel selector does not resolve in the target or template");
      return result;
    }
  }
  if (!patch.value.animations.executionGraph.nodes.empty() &&
      std::count_if(patch.rules.begin(), patch.rules.end(),
                    [](const auto &rule) {
                      return rule.channel == TextTemplateChannel::Animations &&
                             rule.mode != TextTemplateChannelMode::Keep;
                    }) > 1) {
    Add(result.diagnostics, "text_template.animation_rules_ambiguous",
        patch.templateId,
        "one authored execution graph requires one animation channel rule");
    return result;
  }

  TextCompositionDocument source = patch.value;
  std::vector<TextTemplateChannelRule> resolvedRules;
  resolvedRules.reserve(patch.rules.size());
  for (const auto &rule : patch.rules)
    resolvedRules.push_back(
        ResolveTemplateRuleForTarget(rule, patch.value, document));
  const auto templateDecorationSlot =
      TextTemplateDecorationIdentitySlot(patch);
  const bool composableSlotPatch =
      !HasContentAuthority(patch.rules) &&
      !document.contentSlots.empty() && !document.content.empty() &&
      std::any_of(patch.rules.begin(), patch.rules.end(), [](const auto &rule) {
        switch (rule.channel) {
        case TextTemplateChannel::GlyphFills:
        case TextTemplateChannel::GlyphStrokes:
        case TextTemplateChannel::GlyphShadows:
        case TextTemplateChannel::GlyphGlows:
        case TextTemplateChannel::Bubble:
        case TextTemplateChannel::Frame:
        case TextTemplateChannel::CustomBackdrops:
        case TextTemplateChannel::Bend:
        case TextTemplateChannel::Path:
        case TextTemplateChannel::SdfMaterial:
        case TextTemplateChannel::Animations:
        case TextTemplateChannel::VectorDecorations:
        case TextTemplateChannel::TimedText:
          return true;
        case TextTemplateChannel::Content:
        case TextTemplateChannel::Typography:
        case TextTemplateChannel::InlineDecorations:
        case TextTemplateChannel::Paragraph:
        case TextTemplateChannel::Layout:
        case TextTemplateChannel::Resources:
          return false;
        }
        return false;
      });
  if (!templateDecorationSlot.empty()) {
    std::string identityNamespace(decorationIdentityNamespace);
    if (templateDecorationSlot == "animation") {
      if (!identityNamespace.empty()) identityNamespace += ".";
      identityNamespace += patch.templateId + "." + patch.packageDigest;
    }
    if (!identityNamespace.empty())
      NamespaceTextTemplateDecorationIdentities(
          source, identityNamespace, templateDecorationSlot);
  }
  for (const auto &binding : patch.textBindings) {
    const auto override = textOverrides.find(binding.bindingId);
    const auto *sourceSlot = FindSlot(source, binding.contentSlotId);
    const auto *currentSlot = FindSlot(document, binding.contentSlotId);
    if (!currentSlot && sourceSlot) {
      const auto semantic = std::find_if(
          document.contentSlots.begin(), document.contentSlots.end(),
          [&](const auto &candidate) {
            return candidate.semanticRole == sourceSlot->semanticRole;
          });
      if (semantic != document.contentSlots.end())
        currentSlot = &*semantic;
    }
    const auto value =
        override != textOverrides.end()
            ? override->second
            : (currentSlot ? SlotText(document, *currentSlot)
                           : binding.defaultText);
    AssignSlotText(source, binding.contentSlotId, value);
  }
  RemapTemplateTextOwners(source, document);
  FitTemplateReferencesToTarget(source, document);

  TextCompositionDocument candidate = document;
  std::vector<TemplatePropertyCoverage> propertyCoverage;
  propertyCoverage.reserve(animatedProperties.size());
  for (const auto &address : animatedProperties)
    propertyCoverage.push_back(PropertyCoverage(document, address));
  bool composedAnimationPhase = false;
  const auto removeUnreferencedDecorations =
      [&](const std::unordered_set<std::string> &removed) {
        std::unordered_set<std::string> retained;
        for (const auto &layer : candidate.animations.layers)
          for (const auto &decoration : layer.decorations)
            retained.insert(decoration.decorationId);
        candidate.decorations.erase(
            std::remove_if(candidate.decorations.begin(),
                           candidate.decorations.end(),
                           [&](const auto &decoration) {
                             return removed.count(decoration.decorationId) !=
                                        0U &&
                                    retained.count(decoration.decorationId) ==
                                        0U;
                           }),
            candidate.decorations.end());
      };
  for (const auto &rule : resolvedRules) {
    // Bubble/flower/animation packages are composable Inspector slots. Their
    // prototype typography and layout close standalone preview/placement, but
    // applying one to an existing Text composition must not overwrite the
    // user's font metrics or resized authoring box. A full content/document
    // replacement remains authoritative over those channels.
    if (composableSlotPatch &&
        (rule.channel == TextTemplateChannel::Typography ||
         rule.channel == TextTemplateChannel::Layout)) {
      continue;
    }
    const auto before = ComputeTextCompositionDocumentIdentity(candidate);
    for (auto &coverage : propertyCoverage)
      RecordPropertyCoverage(coverage, candidate, source, rule);
    if (composableSlotPatch &&
        rule.channel == TextTemplateChannel::Animations &&
        (rule.mode == TextTemplateChannelMode::Replace ||
         rule.mode == TextTemplateChannelMode::Merge) &&
        source.animations.layers.size() == 1U &&
        (source.animations.layers.front().timeDriver.kind ==
             text::TextAnimationTimeDriverKind::EnterPhase ||
         source.animations.layers.front().timeDriver.kind ==
             text::TextAnimationTimeDriverKind::LoopPhase ||
         source.animations.layers.front().timeDriver.kind ==
             text::TextAnimationTimeDriverKind::ExitPhase)) {
      std::unordered_set<std::string> removedDecorations;
      const auto phase = source.animations.layers.front().timeDriver.kind;
      for (const auto &layer : candidate.animations.layers)
        if (layer.timeDriver.kind == phase) {
          for (const auto &decoration : layer.decorations)
            removedDecorations.insert(decoration.decorationId);
        }
      if (!ComposeAnimationPhase(candidate.animations, source, rule,
                                 patch.templateId, patch.packageDigest,
                                 result.diagnostics))
        return result;
      removeUnreferencedDecorations(removedDecorations);
      composedAnimationPhase = true;
    } else if (rule.channel == TextTemplateChannel::Animations &&
               rule.mode == TextTemplateChannelMode::Merge &&
               !source.animations.executionGraph.nodes.empty()) {
      std::unordered_set<std::string> removedDecorations;
      if (!MergeOwnedAnimationGraphs(
              candidate.animations, source, rule, patch.templateId,
              patch.packageDigest, removedDecorations, result.diagnostics))
        return result;
      removeUnreferencedDecorations(removedDecorations);
      composedAnimationPhase = true;
    } else if (composedAnimationPhase &&
               rule.channel == TextTemplateChannel::VectorDecorations &&
               rule.mode == TextTemplateChannelMode::Replace) {
      auto mergeRule = rule;
      mergeRule.mode = TextTemplateChannelMode::Merge;
      ApplyRule(candidate, source, mergeRule, templateDecorationSlot);
    } else {
      ApplyRule(candidate, source, rule, templateDecorationSlot);
    }
    if (ComputeTextCompositionDocumentIdentity(candidate) != before)
      AppendChanged(result.changedChannels, rule.channel);
  }
  // Timed animation packages carry their canonical seed spans in the
  // animation artifact. They are a required dependency of the admitted
  // animation layer, not an independent style channel. Preserve authored
  // caption/lyric timing when present; otherwise close the dependency from
  // the remapped template source before final validation.
  if (RequiresTimedText(candidate.animations) &&
      (!candidate.timedText || candidate.timedText->spans.empty()) &&
      source.timedText && !source.timedText->spans.empty()) {
    candidate.timedText = source.timedText;
    AppendChanged(result.changedChannels, TextTemplateChannel::TimedText);
  }
  for (const auto &binding : patch.textBindings) {
    const auto override = textOverrides.find(binding.bindingId);
    if (override == textOverrides.end())
      continue;
    std::string targetSlotId = binding.contentSlotId;
    if (!FindSlot(candidate, targetSlotId)) {
      const auto *sourceSlot = FindSlot(patch.value, binding.contentSlotId);
      const auto semantic = sourceSlot
                                ? std::find_if(
                                      candidate.contentSlots.begin(),
                                      candidate.contentSlots.end(),
                                      [&](const auto &slot) {
                                        return slot.semanticRole ==
                                               sourceSlot->semanticRole;
                                      })
                                : candidate.contentSlots.end();
      if (semantic == candidate.contentSlots.end())
        continue;
      targetSlotId = semantic->slotId;
    }
    const auto before = ComputeTextCompositionDocumentIdentity(candidate);
    AssignSlotText(candidate, targetSlotId, override->second);
    if (ComputeTextCompositionDocumentIdentity(candidate) != before)
      AppendChanged(result.changedChannels, TextTemplateChannel::Content);
  }
  const auto resourcesBefore =
      detail::MakeIdentity("videocut.text-template.resources-before",
                           [&](auto &writer) {
                             detail::EncodeResourceIdentity(writer, candidate);
                           });
  if (!CloseResources(candidate, source, document, result.diagnostics))
    return result;
  PruneResourcesNoLongerRequired(candidate, document);
  const auto resourcesAfter =
      detail::MakeIdentity("videocut.text-template.resources-before",
                           [&](auto &writer) {
                             detail::EncodeResourceIdentity(writer, candidate);
                           });
  if (resourcesBefore != resourcesAfter)
    AppendChanged(result.changedChannels, TextTemplateChannel::Resources);

  if (HasContentAuthority(patch.rules)) {
    candidate.templateOrigin = patch.value.templateOrigin;
    candidate.bubbleTemplateOrigin = patch.value.bubbleTemplateOrigin;
    candidate.flowerTemplateOrigin = patch.value.flowerTemplateOrigin;
    candidate.animationTemplateOrigin = patch.value.animationTemplateOrigin;
  } else {
    candidate.templateOrigin =
        TextTemplateOrigin{patch.templateId, patch.packageDigest};
    RecordTextTemplateSlotOrigins(candidate, patch);
  }
  for (const auto &coverage : propertyCoverage) {
    if (coverage.overwritten.empty()) {
      const auto finalTargets =
          PropertyCoverage(candidate, coverage.address).targets;
      if (std::any_of(
              finalTargets.begin(), finalTargets.end(),
              [&](const auto &id) { return !coverage.targets.count(id); })) {
        Add(result.diagnostics, "template.expanded_property_animation",
            text::DescribeTextProperty(coverage.address.property)->name,
            "template would extend a retained property animation to new "
            "targets");
        return result;
      }
      continue;
    }
    if (coverage.overwritten.size() != coverage.targets.size()) {
      Add(result.diagnostics, "template.partial_property_animation",
          text::DescribeTextProperty(coverage.address.property)->name,
          "template coverage intersects only part of a property animation "
          "target");
      return result;
    }
    result.overwrittenProperties.push_back(coverage.address);
  }
  const auto finalValidation =
      ValidateTextCompositionDocument(candidate, limits);
  result.diagnostics.insert(result.diagnostics.end(),
                            finalValidation.diagnostics.begin(),
                            finalValidation.diagnostics.end());
  if (!finalValidation.valid)
    return result;

  result.changed = ComputeTextCompositionDocumentIdentity(candidate) !=
                   ComputeTextCompositionDocumentIdentity(document);
  if (result.changed)
    document = std::move(candidate);
  result.valid = true;
  return result;
}

void NamespaceTextTemplateDecorationIdentities(
    TextCompositionDocument &document, const std::string_view identityNamespace,
    const std::string_view slot) {
  if (identityNamespace.empty() || slot.empty() ||
      document.decorations.empty()) {
    return;
  }
  const std::string prefix = "text-template." +
                             std::string(identityNamespace) + "." +
                             std::string(slot) + ".";
  std::unordered_map<std::string, std::string> identities;
  identities.reserve(document.decorations.size());
  for (auto &decoration : document.decorations) {
    if (decoration.decorationId.rfind("text-template.", 0U) == 0U)
      continue;
    const auto sourceId = decoration.decorationId;
    decoration.decorationId = prefix + sourceId;
    identities.emplace(sourceId, decoration.decorationId);
  }
  if (identities.empty())
    return;
  for (auto &layer : document.animations.layers) {
    for (auto &animation : layer.decorations) {
      const auto found = identities.find(animation.decorationId);
      if (found != identities.end())
        animation.decorationId = found->second;
    }
  }
}

} // namespace videocut::text_composition
