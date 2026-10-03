#include "videocut/text/TextTemplate.h"

#include "videocut/text/TextAnimation.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace videocut::text {
namespace {

using Json = nlohmann::json;

void Add(std::vector<Diagnostic> &diagnostics, std::string code,
         std::string stage, std::string subject, std::string message) {
  diagnostics.push_back({
      std::move(code),
      DiagnosticSeverity::Error,
      std::move(stage),
      std::move(subject),
      std::move(message),
  });
}

void RequireOnlyKeys(const Json &value,
                     const std::initializer_list<std::string_view> allowedKeys,
                     const std::string &context) {
  if (!value.is_object())
    throw std::invalid_argument(context + " must be an object");
  for (const auto &[key, ignored] : value.items()) {
    (void)ignored;
    if (std::find(allowedKeys.begin(), allowedKeys.end(),
                  std::string_view(key)) == allowedKeys.end()) {
      throw std::invalid_argument(context + "." + key + " is unsupported");
    }
  }
}

const Json &RequiredObject(const Json &parent, const char *key,
                           const std::string &context) {
  const auto found = parent.find(key);
  if (found == parent.end() || !found->is_object())
    throw std::invalid_argument(context + "." + key + " must be an object");
  return *found;
}

const Json &RequiredArray(const Json &parent, const char *key,
                          const std::string &context) {
  const auto found = parent.find(key);
  if (found == parent.end() || !found->is_array())
    throw std::invalid_argument(context + "." + key + " must be an array");
  return *found;
}

std::string RequiredString(const Json &parent, const char *key,
                           const std::string &context) {
  const auto found = parent.find(key);
  if (found == parent.end() || !found->is_string() ||
      found->get_ref<const std::string &>().empty())
    throw std::invalid_argument(context + "." + key +
                                " must be a non-empty string");
  return found->get<std::string>();
}

std::string OptionalString(const Json &parent, const char *key,
                           std::string fallback = {}) {
  const auto found = parent.find(key);
  if (found == parent.end())
    return fallback;
  if (!found->is_string())
    throw std::invalid_argument(std::string(key) + " must be a string");
  return found->get<std::string>();
}

std::int64_t RequiredInteger(const Json &parent, const char *key,
                             const std::string &context) {
  const auto found = parent.find(key);
  if (found == parent.end() || !found->is_number_integer())
    throw std::invalid_argument(context + "." + key + " must be an integer");
  return found->get<std::int64_t>();
}

float OptionalFloat(const Json &parent, const char *key, float fallback) {
  const auto found = parent.find(key);
  if (found == parent.end())
    return fallback;
  if (!found->is_number())
    throw std::invalid_argument(std::string(key) + " must be numeric");
  const double value = found->get<double>();
  if (!std::isfinite(value) || value < -std::numeric_limits<float>::max() ||
      value > std::numeric_limits<float>::max())
    throw std::invalid_argument(std::string(key) + " must be a finite float");
  return static_cast<float>(value);
}

double OptionalDouble(const Json &parent, const char *key, double fallback) {
  const auto found = parent.find(key);
  if (found == parent.end())
    return fallback;
  if (!found->is_number())
    throw std::invalid_argument(std::string(key) + " must be numeric");
  const double value = found->get<double>();
  if (!std::isfinite(value))
    throw std::invalid_argument(std::string(key) + " must be finite");
  return value;
}

bool OptionalBool(const Json &parent, const char *key, bool fallback) {
  const auto found = parent.find(key);
  if (found == parent.end())
    return fallback;
  if (!found->is_boolean())
    throw std::invalid_argument(std::string(key) + " must be boolean");
  return found->get<bool>();
}

std::uint16_t OptionalUint16(const Json &parent, const char *key,
                             const std::uint16_t fallback) {
  const auto found = parent.find(key);
  if (found == parent.end())
    return fallback;
  if (!found->is_number_integer())
    throw std::invalid_argument(std::string(key) + " must be an integer");
  const auto value = found->get<std::int64_t>();
  if (value < 0 || value > std::numeric_limits<std::uint16_t>::max())
    throw std::invalid_argument(std::string(key) + " is outside uint16");
  return static_cast<std::uint16_t>(value);
}

Color ParseColor(const Json &value, const std::string &context) {
  if (!value.is_array() || value.size() != 4)
    throw std::invalid_argument(context + " must be an RGBA array");
  Color result;
  float *channels[] = {&result.red, &result.green, &result.blue, &result.alpha};
  for (std::size_t index = 0; index < 4; ++index) {
    if (!value[index].is_number())
      throw std::invalid_argument(context + " contains a non-number");
    const double channel = value[index].get<double>();
    if (!std::isfinite(channel) || channel < 0.0 || channel > 1.0)
      throw std::invalid_argument(context + " channel is outside [0, 1]");
    *channels[index] = static_cast<float>(channel);
  }
  return result;
}

Color ParseColorTangent(const Json &value, const std::string &context) {
  if (!value.is_array() || value.size() != 4)
    throw std::invalid_argument(context + " must be a four-channel array");
  Color result{0.0F, 0.0F, 0.0F, 0.0F};
  float *channels[] = {&result.red, &result.green, &result.blue, &result.alpha};
  for (std::size_t index = 0; index < 4; ++index) {
    if (!value[index].is_number())
      throw std::invalid_argument(context + " contains a non-number");
    const double channel = value[index].get<double>();
    if (!std::isfinite(channel) || std::fabs(channel) > 65'536.0)
      throw std::invalid_argument(context + " channel exceeds native bound");
    *channels[index] = static_cast<float>(channel);
  }
  return result;
}

std::pair<float, float> ParsePoint(const Json &value,
                                   const std::string &context) {
  if (!value.is_array() || value.size() != 2 || !value[0].is_number() ||
      !value[1].is_number()) {
    throw std::invalid_argument(context + " must be a two-number array");
  }
  const double x = value[0].get<double>();
  const double y = value[1].get<double>();
  if (!std::isfinite(x) || !std::isfinite(y) ||
      x < -std::numeric_limits<float>::max() ||
      x > std::numeric_limits<float>::max() ||
      y < -std::numeric_limits<float>::max() ||
      y > std::numeric_limits<float>::max()) {
    throw std::invalid_argument(context + " must contain finite floats");
  }
  return {
      static_cast<float>(x),
      static_cast<float>(y),
  };
}

PaintKind ParsePaintKind(const std::string &value) {
  if (value == "solid")
    return PaintKind::Solid;
  if (value == "linear_gradient")
    return PaintKind::LinearGradient;
  if (value == "radial_gradient")
    return PaintKind::RadialGradient;
  if (value == "texture")
    return PaintKind::Texture;
  throw std::invalid_argument("unknown text paint kind: " + value);
}

PaintSpread ParsePaintSpread(const std::string &value) {
  if (value == "clamp")
    return PaintSpread::Clamp;
  if (value == "repeat")
    return PaintSpread::Repeat;
  if (value == "mirror")
    return PaintSpread::Mirror;
  throw std::invalid_argument("unknown text paint spread: " + value);
}

TextureSourceKind ParseTextureSourceKind(const std::string &value) {
  if (value == "builtin")
    return TextureSourceKind::Builtin;
  if (value == "project_managed")
    return TextureSourceKind::ProjectManaged;
  if (value == "external_file")
    return TextureSourceKind::ExternalFile;
  throw std::invalid_argument("unknown text texture source kind: " + value);
}

TextureFit ParseTextureFit(const std::string &value) {
  if (value == "cover")
    return TextureFit::Cover;
  if (value == "contain")
    return TextureFit::Contain;
  if (value == "stretch")
    return TextureFit::Stretch;
  if (value == "tile")
    return TextureFit::Tile;
  throw std::invalid_argument("unknown text texture fit: " + value);
}

TextureBlendMode ParseTextureBlendMode(const std::string &value) {
  if (value == "none")
    return TextureBlendMode::None;
  if (value == "solid")
    return TextureBlendMode::Solid;
  if (value == "gradient")
    return TextureBlendMode::Gradient;
  throw std::invalid_argument("unknown text texture blend mode: " + value);
}

TextSourceCreationComponent
ParseTextSourceCreationComponent(const std::string &value) {
  if (value == "legacy_text")
    return TextSourceCreationComponent::LegacyText;
  if (value == "sdf_text")
    return TextSourceCreationComponent::SdfText;
  throw std::invalid_argument("unknown text source creation component: " +
                              value);
}

TextureMapping ParseTextureMapping(const std::string &value) {
  if (value == "scope_bounds")
    return TextureMapping::ScopeBounds;
  if (value == "glyph_distance_field")
    return TextureMapping::GlyphDistanceField;
  throw std::invalid_argument("unknown text texture mapping: " + value);
}

PaintCoordinateSpace ParsePaintCoordinateSpace(const std::string &value) {
  if (value == "layout")
    return PaintCoordinateSpace::LayoutBox;
  if (value == "text")
    return PaintCoordinateSpace::TextBounds;
  if (value == "grapheme")
    return PaintCoordinateSpace::Grapheme;
  throw std::invalid_argument("unknown text paint coordinate space: " + value);
}

GradientSampling ParseGradientSampling(const std::string &value) {
  if (value == "continuous")
    return GradientSampling::Continuous;
  if (value == "rgba8_lut_256")
    return GradientSampling::Rgba8Lut256;
  throw std::invalid_argument("unknown text gradient sampling: " + value);
}

TextPaint ParseTextPaint(const Json &value, const std::string &context) {
  if (!value.is_object())
    throw std::invalid_argument(context + " must be an object");
  TextPaint result;
  result.kind = ParsePaintKind(OptionalString(value, "kind", "solid"));
  result.coordinateSpace = ParsePaintCoordinateSpace(
      OptionalString(value, "coordinate_space", "layout"));
  result.gradientSampling = ParseGradientSampling(
      OptionalString(value, "gradient_sampling", "continuous"));
  result.coordinateOutset = OptionalFloat(value, "coordinate_outset", 0.0F);
  result.gradientCoordinateScale =
      OptionalFloat(value, "gradient_coordinate_scale", 1.0F);
  if (const auto color = value.find("rgba"); color != value.end())
    result.color = ParseColor(*color, context + ".rgba");

  if (result.kind == PaintKind::LinearGradient ||
      result.kind == PaintKind::RadialGradient) {
    const auto &stops = RequiredArray(value, "stops", context);
    for (const auto &stop : stops) {
      if (!stop.is_object())
        throw std::invalid_argument(context + ".stops entry must be an object");
      result.stops.push_back({
          OptionalFloat(stop, "offset", 0.0F),
          ParseColor(stop.at("rgba"), context + ".stops.rgba"),
      });
    }
    result.spread = ParsePaintSpread(OptionalString(value, "spread", "clamp"));
  }
  if (result.kind == PaintKind::LinearGradient) {
    if (const auto start = value.find("start"); start != value.end()) {
      const auto point = ParsePoint(*start, context + ".start");
      result.startX = point.first;
      result.startY = point.second;
    } else {
      result.startX = OptionalFloat(value, "start_x", 0.0F);
      result.startY = OptionalFloat(value, "start_y", 0.5F);
    }
    if (const auto end = value.find("end"); end != value.end()) {
      const auto point = ParsePoint(*end, context + ".end");
      result.endX = point.first;
      result.endY = point.second;
    } else {
      result.endX = OptionalFloat(value, "end_x", 1.0F);
      result.endY = OptionalFloat(value, "end_y", 0.5F);
    }
  } else if (result.kind == PaintKind::RadialGradient) {
    if (const auto center = value.find("center"); center != value.end()) {
      const auto point = ParsePoint(*center, context + ".center");
      result.centerX = point.first;
      result.centerY = point.second;
    } else {
      result.centerX = OptionalFloat(value, "center_x", 0.5F);
      result.centerY = OptionalFloat(value, "center_y", 0.5F);
    }
    result.radius = OptionalFloat(value, "radius", 0.5F);
  } else if (result.kind == PaintKind::Texture) {
    const auto &texture = RequiredObject(value, "texture", context);
    const auto source = texture.find("source");
    const Json &identity =
        source == texture.end()
            ? texture
            : RequiredObject(texture, "source", context + ".texture");
    result.texture.sourceKind = ParseTextureSourceKind(OptionalString(
        identity, source == texture.end() ? "source_kind" : "kind", "builtin"));
    result.texture.assetId = OptionalString(identity, "asset_id");
    result.texture.locatorId = OptionalString(identity, "locator_id");
    result.texture.displayPath = OptionalString(identity, "display_path");
    result.texture.digest = OptionalString(identity, "digest");
    result.texture.mediaType =
        OptionalString(texture, "media_type", "image/png");
    result.texture.colorSpace = OptionalString(texture, "color_space", "srgb");
    result.textureFit =
        ParseTextureFit(OptionalString(value, "texture_fit", "cover"));
    result.textureBlendMode =
        ParseTextureBlendMode(OptionalString(value, "texture_blend", "none"));
    result.textureMapping = ParseTextureMapping(
        OptionalString(value, "texture_mapping", "scope_bounds"));
    result.textureScale = OptionalFloat(value, "texture_scale", 1.0F);
    result.textureRotationDegrees =
        OptionalFloat(value, "texture_rotation_degrees", 0.0F);
    result.textureOffsetX = OptionalFloat(value, "texture_offset_x", 0.0F);
    result.textureOffsetY = OptionalFloat(value, "texture_offset_y", 0.0F);
    result.textureFlipX = OptionalBool(value, "texture_flip_x", false);
    result.textureFlipY = OptionalBool(value, "texture_flip_y", false);
    result.textureAtlasColumns =
        OptionalUint16(value, "texture_atlas_columns", 1);
    result.textureAtlasRows = OptionalUint16(value, "texture_atlas_rows", 1);
    result.textureOpacity = OptionalFloat(value, "texture_opacity", 1.0F);
    result.textureSourceAlpha =
        OptionalBool(value, "texture_source_alpha", false);
    if (result.textureBlendMode == TextureBlendMode::Gradient) {
      const auto &stops = RequiredArray(value, "stops", context);
      for (const auto &stop : stops) {
        if (!stop.is_object())
          throw std::invalid_argument(context +
                                      ".stops entry must be an object");
        result.stops.push_back({
            OptionalFloat(stop, "offset", 0.0F),
            ParseColor(stop.at("rgba"), context + ".stops.rgba"),
        });
      }
      result.spread =
          ParsePaintSpread(OptionalString(value, "spread", "clamp"));
      if (const auto start = value.find("start"); start != value.end()) {
        const auto point = ParsePoint(*start, context + ".start");
        result.startX = point.first;
        result.startY = point.second;
      }
      if (const auto end = value.find("end"); end != value.end()) {
        const auto point = ParsePoint(*end, context + ".end");
        result.endX = point.first;
        result.endY = point.second;
      }
    }
  }
  return result;
}

TextBoxBackground ParseBoxBackground(const Json &value,
                                     const std::string &context) {
  if (!value.is_object())
    throw std::invalid_argument(context + " must be an object");
  TextBoxBackground result;
  result.enabled = OptionalBool(value, "enabled", true);
  if (const auto color = value.find("rgba"); color != value.end())
    result.color = ParseColor(*color, context + ".rgba");
  result.cornerRadius = OptionalFloat(value, "corner_radius", 0.0F);
  if (const auto padding = value.find("padding"); padding != value.end()) {
    if (!padding->is_object())
      throw std::invalid_argument(context + ".padding must be an object");
    result.padding = {
        OptionalFloat(*padding, "left", 0.0F),
        OptionalFloat(*padding, "top", 0.0F),
        OptionalFloat(*padding, "right", 0.0F),
        OptionalFloat(*padding, "bottom", 0.0F),
    };
  }
  return result;
}

Insets ParseInsets(const Json &value, const Insets fallback,
                   const std::string &context) {
  if (!value.is_object())
    throw std::invalid_argument(context + " must be an object");
  return {
      OptionalFloat(value, "left", fallback.left),
      OptionalFloat(value, "top", fallback.top),
      OptionalFloat(value, "right", fallback.right),
      OptionalFloat(value, "bottom", fallback.bottom),
  };
}

TextStroke ParseStroke(const Json &value, const std::string &context) {
  if (!value.is_object())
    throw std::invalid_argument(context + " must be an object");
  TextStroke result;
  result.width = OptionalFloat(value, "width", 0.0F);
  if (const auto color = value.find("rgba"); color != value.end())
    result.color = ParseColor(*color, context + ".rgba");
  if (const auto paint = value.find("paint"); paint != value.end()) {
    result.usePaint = true;
    result.paint = ParseTextPaint(*paint, context + ".paint");
  }
  return result;
}

BubbleFamily ParseBubbleFamily(const std::string &value) {
  if (value == "round_rect")
    return BubbleFamily::RoundRect;
  if (value == "pill")
    return BubbleFamily::Pill;
  if (value == "speech")
    return BubbleFamily::Speech;
  if (value == "cloud")
    return BubbleFamily::Cloud;
  if (value == "spike")
    return BubbleFamily::Spike;
  if (value == "caption_bar")
    return BubbleFamily::CaptionBar;
  throw std::invalid_argument("unknown text bubble family: " + value);
}

BubbleTailEdge ParseBubbleTailEdge(const std::string &value) {
  if (value == "none")
    return BubbleTailEdge::None;
  if (value == "top")
    return BubbleTailEdge::Top;
  if (value == "bottom")
    return BubbleTailEdge::Bottom;
  if (value == "left")
    return BubbleTailEdge::Left;
  if (value == "right")
    return BubbleTailEdge::Right;
  throw std::invalid_argument("unknown text bubble tail edge: " + value);
}

TextureReference ParseBubbleTexture(const Json &value,
                                    const std::string &context) {
  Json paint = Json::object();
  paint["kind"] = "texture";
  paint["texture"] = value;
  return ParseTextPaint(paint, context).texture;
}

TextLayerAnimationClip ParseLayerAnimationClip(const Json &source,
                                               const std::string &context);

TextBubble ParseTextBubble(const Json &value, const std::string &context) {
  if (!value.is_object())
    throw std::invalid_argument(context + " must be an object");
  TextBubble result;
  result.enabled = OptionalBool(value, "enabled", true);
  if (const auto padding = value.find("padding"); padding != value.end())
    result.padding =
        ParseInsets(*padding, result.padding, context + ".padding");
  const auto fit = OptionalString(value, "fit_policy", "ink_bounds");
  if (fit == "ink_bounds")
    result.fitPolicy = BubbleFitPolicy::InkBounds;
  else if (fit == "line_union")
    result.fitPolicy = BubbleFitPolicy::LineUnion;
  else if (fit == "layout_bounds")
    result.fitPolicy = BubbleFitPolicy::LayoutBounds;
  else
    throw std::invalid_argument(context + ".fit_policy is unsupported");
  const auto &source = RequiredObject(value, "source", context);
  const auto kind = OptionalString(source, "kind", "parametric");
  if (kind == "parametric") {
    ParametricBubbleSource decoded;
    decoded.family =
        ParseBubbleFamily(OptionalString(source, "family", "round_rect"));
    if (const auto fill = source.find("fill"); fill != source.end())
      decoded.fill = ParseTextPaint(*fill, context + ".source.fill");
    if (const auto strokes = source.find("strokes"); strokes != source.end()) {
      if (!strokes->is_array())
        throw std::invalid_argument(context +
                                    ".source.strokes must be an array");
      for (std::size_t index = 0; index < strokes->size(); ++index)
        decoded.strokes.push_back(
            ParseStroke((*strokes)[index], context + ".source.strokes[" +
                                               std::to_string(index) + "]"));
    }
    decoded.cornerRadius =
        OptionalFloat(source, "corner_radius", decoded.cornerRadius);
    if (const auto tail = source.find("tail"); tail != source.end()) {
      if (!tail->is_object())
        throw std::invalid_argument(context + ".source.tail must be an object");
      decoded.tail.edge =
          ParseBubbleTailEdge(OptionalString(*tail, "edge", "none"));
      decoded.tail.position = OptionalFloat(*tail, "position", 0.5F);
      decoded.tail.width = OptionalFloat(*tail, "width", decoded.tail.width);
      decoded.tail.length = OptionalFloat(*tail, "length", decoded.tail.length);
    }
    result.source = std::move(decoded);
  } else if (kind == "stretchable") {
    StretchableBubbleSource decoded;
    decoded.asset = ParseBubbleTexture(
        RequiredObject(source, "texture", context + ".source"),
        context + ".source.texture");
    if (const auto capInsets = source.find("cap_insets");
        capInsets != source.end()) {
      decoded.capInsets = ParseInsets(*capInsets, decoded.capInsets,
                                      context + ".source.cap_insets");
    }
    if (const auto contentInsets = source.find("content_insets");
        contentInsets != source.end()) {
      decoded.contentInsets = ParseInsets(*contentInsets, decoded.contentInsets,
                                          context + ".source.content_insets");
    }
    decoded.minimumContentWidth = OptionalFloat(source, "minimum_content_width",
                                                decoded.minimumContentWidth);
    decoded.minimumContentHeight = OptionalFloat(
        source, "minimum_content_height", decoded.minimumContentHeight);
    const auto stretch = OptionalString(source, "stretch_mode", "nine_slice");
    if (stretch == "nine_slice")
      decoded.stretchMode = BubbleStretchMode::NineSlice;
    else if (stretch == "stretch")
      decoded.stretchMode = BubbleStretchMode::Stretch;
    else if (stretch == "tile_center")
      decoded.stretchMode = BubbleStretchMode::TileCenter;
    else
      throw std::invalid_argument(context +
                                  ".source.stretch_mode is unsupported");
    if (const auto color = source.find("fallback_rgba"); color != source.end())
      decoded.fallbackColor =
          ParseColor(*color, context + ".source.fallback_rgba");
    result.source = std::move(decoded);
  } else if (kind == "vector_envelope") {
    VectorEnvelopeBubbleSource decoded;
    decoded.assetId = RequiredString(source, "asset_id", context + ".source");
    decoded.fitProfile = OptionalString(source, "fit_profile", "contain");
    if (const auto color = source.find("fallback_rgba"); color != source.end())
      decoded.fallbackColor =
          ParseColor(*color, context + ".source.fallback_rgba");
    result.source = std::move(decoded);
  } else if (kind == "animated") {
    AnimatedBubbleSource decoded;
    decoded.assetId = RequiredString(source, "asset_id", context + ".source");
    if (const auto contentInsets = source.find("content_insets");
        contentInsets != source.end()) {
      decoded.contentInsets = ParseInsets(*contentInsets, decoded.contentInsets,
                                          context + ".source.content_insets");
    }
    const auto clock = OptionalString(source, "time_binding", "composition");
    if (clock == "composition")
      decoded.timeBinding = BubbleTimeBinding::Composition;
    else if (clock == "animation_layer")
      decoded.timeBinding = BubbleTimeBinding::AnimationLayer;
    else if (clock == "source")
      decoded.timeBinding = BubbleTimeBinding::Source;
    else
      throw std::invalid_argument(context +
                                  ".source.time_binding is unsupported");
    if (const auto color = source.find("fallback_rgba"); color != source.end())
      decoded.fallbackColor =
          ParseColor(*color, context + ".source.fallback_rgba");
    result.source = std::move(decoded);
  } else {
    throw std::invalid_argument(context + ".source.kind is unsupported");
  }
  if (const auto animations = value.find("animations");
      animations != value.end()) {
    if (!animations->is_array() || animations->size() > 3U)
      throw std::invalid_argument(context +
                                  ".animations must contain at most 3 clips");
    result.animationClips.reserve(animations->size());
    for (std::size_t index = 0U; index < animations->size(); ++index) {
      result.animationClips.push_back(ParseLayerAnimationClip(
          (*animations)[index],
          context + ".animations[" + std::to_string(index) + "]"));
    }
  }
  return result;
}

TextFlowerLayerKind ParseFlowerLayerKind(const std::string &value) {
  if (value == "shadow")
    return TextFlowerLayerKind::Shadow;
  if (value == "outer_stroke")
    return TextFlowerLayerKind::OuterStroke;
  if (value == "fill")
    return TextFlowerLayerKind::Fill;
  if (value == "inner_shadow")
    return TextFlowerLayerKind::InnerShadow;
  if (value == "glow")
    return TextFlowerLayerKind::Glow;
  throw std::invalid_argument("unknown flower text layer kind: " + value);
}

TextFlowerLayerPaintSource
ParseFlowerLayerPaintSource(const std::string &value,
                            const std::string &context) {
  if (value == "literal")
    return TextFlowerLayerPaintSource::Literal;
  if (value == "editable_glyph_fill")
    return TextFlowerLayerPaintSource::EditableGlyphFill;
  throw std::invalid_argument(context +
                              " must be literal or editable_glyph_fill");
}

TextOuterShadowSmoothingMode
ParseOuterShadowSmoothingMode(const std::string &value,
                              const std::string &context) {
  if (value == "auto")
    return TextOuterShadowSmoothingMode::Auto;
  if (value == "legacy_feather")
    return TextOuterShadowSmoothingMode::LegacyFeather;
  if (value == "diffuse_round_mask")
    return TextOuterShadowSmoothingMode::DiffuseRoundMask;
  throw std::invalid_argument(context + " must be auto, legacy_feather, or "
                                        "diffuse_round_mask");
}

TextFlowerAppearance ParseFlowerAppearance(const Json &value,
                                           const std::string &context) {
  if (!value.is_object())
    throw std::invalid_argument(context + " must be an object");
  TextFlowerAppearance result;
  result.enabled = OptionalBool(value, "enabled", true);
  result.revision = value.value("revision", std::uint32_t{1});
  const auto &layers = RequiredArray(value, "layers", context);
  for (std::size_t index = 0; index < layers.size(); ++index) {
    const auto &source = layers[index];
    const auto layerContext =
        context + ".layers[" + std::to_string(index) + "]";
    if (!source.is_object())
      throw std::invalid_argument(layerContext + " must be an object");
    TextFlowerLayer layer;
    layer.layerId = RequiredString(source, "id", layerContext);
    layer.kind =
        ParseFlowerLayerKind(RequiredString(source, "kind", layerContext));
    layer.paintSource = ParseFlowerLayerPaintSource(
        OptionalString(source, "paint_source", "literal"),
        layerContext + ".paint_source");
    layer.enabled = OptionalBool(source, "enabled", true);
    layer.paint = ParseTextPaint(RequiredObject(source, "paint", layerContext),
                                 layerContext + ".paint");
    layer.width = OptionalFloat(source, "width", 0.0F);
    layer.offsetX = OptionalFloat(source, "offset_x", 0.0F);
    layer.offsetY = OptionalFloat(source, "offset_y", 0.0F);
    layer.blurRadius = OptionalFloat(source, "blur", 0.0F);
    layer.spread = OptionalFloat(source, "spread", 0.0F);
    layer.thicknessProjectionEnabled =
        OptionalBool(source, "thickness_projection", false);
    layer.thicknessAngleDegrees =
        OptionalFloat(source, "thickness_angle_degrees", 45.0F);
    layer.thicknessDistance = OptionalFloat(source, "thickness_distance", 0.0F);
    layer.outerShadowSmoothingMode = ParseOuterShadowSmoothingMode(
        OptionalString(source, "outer_shadow_smoothing", "auto"),
        layerContext + ".outer_shadow_smoothing");
    layer.roundMaskIntensity =
        OptionalFloat(source, "round_mask_intensity", 0.0F);
    result.layers.push_back(std::move(layer));
  }
  return result;
}

TextLayerAppearance ParseAppearance(const Json &root) {
  TextLayerAppearance result;
  const auto appearance = root.find("appearance");
  if (appearance == root.end())
    return result;
  if (!appearance->is_object())
    throw std::invalid_argument("template.appearance must be an object");

  result.presetId = OptionalString(*appearance, "preset_id");
  result.revision = appearance->value("revision", std::uint32_t{2});
  if (const auto background = appearance->find("background");
      background != appearance->end()) {
    if (!background->is_object())
      throw std::invalid_argument(
          "template.appearance.background must be an object");
    result.background.enabled = OptionalBool(*background, "enabled", false);
    if (const auto color = background->find("rgba");
        color != background->end()) {
      result.background.color =
          ParseColor(*color, "template.appearance.background.rgba");
    }
    result.background.cornerRadius = OptionalFloat(
        *background, "corner_radius", result.background.cornerRadius);
    if (const auto padding = background->find("padding");
        padding != background->end()) {
      if (!padding->is_object())
        throw std::invalid_argument("template.appearance.background.padding "
                                    "must be an object");
      result.background.padding = {
          OptionalFloat(*padding, "left", result.background.padding.left),
          OptionalFloat(*padding, "top", result.background.padding.top),
          OptionalFloat(*padding, "right", result.background.padding.right),
          OptionalFloat(*padding, "bottom", result.background.padding.bottom),
      };
    }
  }
  if (const auto bubble = appearance->find("bubble");
      bubble != appearance->end()) {
    result.bubble = ParseTextBubble(*bubble, "template.appearance.bubble");
  }
  if (const auto flower = appearance->find("flower");
      flower != appearance->end()) {
    result.flower =
        ParseFlowerAppearance(*flower, "template.appearance.flower");
  }
  if (const auto sdfMaterial = appearance->find("sdf_material");
      sdfMaterial != appearance->end()) {
    RequireOnlyKeys(*sdfMaterial,
                    {"enabled", "distance_range", "raster_distance_range",
                     "smoothing_scale", "source_design_width",
                     "source_design_height", "source_creation_component"},
                    "template.appearance.sdf_material");
    result.sdfMaterial.enabled = OptionalBool(*sdfMaterial, "enabled", true);
    result.sdfMaterial.sourceCreationComponent =
        ParseTextSourceCreationComponent(OptionalString(
            *sdfMaterial, "source_creation_component", "legacy_text"));
    result.sdfMaterial.distanceRange = OptionalFloat(
        *sdfMaterial, "distance_range", result.sdfMaterial.distanceRange);
    result.sdfMaterial.rasterDistanceRange =
        OptionalFloat(*sdfMaterial, "raster_distance_range",
                      result.sdfMaterial.rasterDistanceRange);
    result.sdfMaterial.smoothingScale = OptionalFloat(
        *sdfMaterial, "smoothing_scale", result.sdfMaterial.smoothingScale);
    result.sdfMaterial.sourceDesignWidth =
        OptionalFloat(*sdfMaterial, "source_design_width",
                      result.sdfMaterial.sourceDesignWidth);
    result.sdfMaterial.sourceDesignHeight =
        OptionalFloat(*sdfMaterial, "source_design_height",
                      result.sdfMaterial.sourceDesignHeight);
  }
  if (const auto glow = appearance->find("glow"); glow != appearance->end()) {
    if (!glow->is_object())
      throw std::invalid_argument("template.appearance.glow must be an object");
    result.glow.enabled = OptionalBool(*glow, "enabled", false);
    if (const auto color = glow->find("rgba"); color != glow->end()) {
      result.glow.color = ParseColor(*color, "template.appearance.glow.rgba");
    }
    result.glow.radius = OptionalFloat(*glow, "radius", result.glow.radius);
    result.glow.spread = OptionalFloat(*glow, "spread", result.glow.spread);
  }
  if (const auto bend = appearance->find("bend"); bend != appearance->end()) {
    if (!bend->is_object())
      throw std::invalid_argument("template.appearance.bend must be an object");
    result.bend.enabled = OptionalBool(*bend, "enabled", false);
    result.bend.amount = OptionalFloat(*bend, "amount", result.bend.amount);
  }
  if (const auto authoredPath = appearance->find("path");
      authoredPath != appearance->end()) {
    if (!authoredPath->is_object())
      throw std::invalid_argument("template.appearance.path must be an object");
    const auto presetId = OptionalString(*authoredPath, "preset_id");
    if (!presetId.empty()) {
      std::string presetError;
      if (!ConfigureTextPathPreset(result.path, presetId, &presetError)) {
        throw std::invalid_argument(presetError);
      }
    }
    result.path.enabled =
        OptionalBool(*authoredPath, "enabled", result.path.enabled);
    result.path.geometryId =
        OptionalString(*authoredPath, "geometry_id", result.path.geometryId);
    result.path.presetId =
        presetId.empty()
            ? OptionalString(*authoredPath, "preset_id", result.path.presetId)
            : presetId;
    result.path.startOffset =
        OptionalFloat(*authoredPath, "start_offset", result.path.startOffset);
    result.path.baselineOffset = OptionalFloat(*authoredPath, "baseline_offset",
                                               result.path.baselineOffset);
    result.path.loop = OptionalBool(*authoredPath, "loop", result.path.loop);
    result.path.rotateToTangent = OptionalBool(
        *authoredPath, "rotate_to_tangent", result.path.rotateToTangent);
    result.path.keepUpright =
        OptionalBool(*authoredPath, "keep_upright", result.path.keepUpright);
    const auto overflow = OptionalString(
        *authoredPath, "overflow",
        result.path.overflow == TextPathOverflow::Visible      ? "visible"
        : result.path.overflow == TextPathOverflow::ScaleToFit ? "scale_to_fit"
                                                               : "clip");
    if (overflow == "clip")
      result.path.overflow = TextPathOverflow::Clip;
    else if (overflow == "visible")
      result.path.overflow = TextPathOverflow::Visible;
    else if (overflow == "scale_to_fit")
      result.path.overflow = TextPathOverflow::ScaleToFit;
    else
      throw std::invalid_argument(
          "template.appearance.path.overflow is unsupported");

    if (const auto commands = authoredPath->find("commands");
        commands != authoredPath->end()) {
      if (!commands->is_array())
        throw std::invalid_argument(
            "template.appearance.path.commands must be an array");
      result.path.commands.clear();
      result.path.commands.reserve(commands->size());
      for (std::size_t index = 0; index < commands->size(); ++index) {
        const auto &source = (*commands)[index];
        const auto context =
            "template.appearance.path.commands[" + std::to_string(index) + "]";
        if (!source.is_object())
          throw std::invalid_argument(context + " must be an object");
        TextPathCommand command;
        const auto kind = RequiredString(source, "kind", context);
        if (kind == "move_to")
          command.kind = TextPathCommandKind::MoveTo;
        else if (kind == "line_to")
          command.kind = TextPathCommandKind::LineTo;
        else if (kind == "quadratic_to")
          command.kind = TextPathCommandKind::QuadraticTo;
        else if (kind == "cubic_to")
          command.kind = TextPathCommandKind::CubicTo;
        else if (kind == "close")
          command.kind = TextPathCommandKind::Close;
        else
          throw std::invalid_argument(context + ".kind is unsupported");
        const auto readPoint = [&](const char *key, TextPathPoint &target) {
          const auto found = source.find(key);
          if (found == source.end())
            throw std::invalid_argument(context + "." + key + " is required");
          const auto [x, y] = ParsePoint(*found, context + "." + key);
          target = {x, y};
        };
        if (command.kind == TextPathCommandKind::MoveTo ||
            command.kind == TextPathCommandKind::LineTo) {
          readPoint("end", command.end);
        } else if (command.kind == TextPathCommandKind::QuadraticTo) {
          readPoint("control1", command.control1);
          readPoint("end", command.end);
        } else if (command.kind == TextPathCommandKind::CubicTo) {
          readPoint("control1", command.control1);
          readPoint("control2", command.control2);
          readPoint("end", command.end);
        }
        result.path.commands.push_back(std::move(command));
      }
    }
  }
  if (const auto extent = appearance->find("visual_extent");
      extent != appearance->end()) {
    if (!extent->is_object())
      throw std::invalid_argument(
          "template.appearance.visual_extent must be an object");
    result.visualExtent.allowControlOverflow =
        OptionalBool(*extent, "allow_control_overflow", true);
    if (extent->contains("maximum_width"))
      result.visualExtent.maximumExtentWidth =
          OptionalFloat(*extent, "maximum_width", 0.0F);
    if (extent->contains("maximum_height"))
      result.visualExtent.maximumExtentHeight =
          OptionalFloat(*extent, "maximum_height", 0.0F);
  }
  std::string error;
  if (!ValidateTextLayerAppearance(result, &error))
    throw std::invalid_argument(error);
  return result;
}

FontSourceKind ParseFontSourceKind(const std::string &value) {
  if (value == "builtin")
    return FontSourceKind::Builtin;
  if (value == "project_managed")
    return FontSourceKind::ProjectManaged;
  if (value == "system")
    return FontSourceKind::System;
  if (value == "external_file")
    return FontSourceKind::ExternalFile;
  throw std::invalid_argument("unknown font source kind: " + value);
}

FontSlant ParseFontSlant(const std::string &value) {
  if (value == "upright")
    return FontSlant::Upright;
  if (value == "italic")
    return FontSlant::Italic;
  if (value == "oblique")
    return FontSlant::Oblique;
  throw std::invalid_argument("unknown font slant: " + value);
}

FontReference ParseFontReference(const Json &source, const Json &font,
                                 const std::string &context) {
  FontReference result;
  result.kind =
      ParseFontSourceKind(RequiredString(source, "kind", context + ".source"));
  result.family = RequiredString(font, "family", context);
  result.postscriptName = OptionalString(font, "postscript_name");
  if (const auto weight = font.find("weight"); weight != font.end()) {
    if (!weight->is_number_integer())
      throw std::invalid_argument(context + ".weight must be an integer");
    result.weight = weight->get<std::int32_t>();
  }
  if (const auto width = font.find("width"); width != font.end()) {
    if (!width->is_number_integer())
      throw std::invalid_argument(context + ".width must be an integer");
    result.width = width->get<std::int32_t>();
  }
  result.slant = ParseFontSlant(OptionalString(font, "slant", "upright"));
  const auto faceIndex = font.value("face_index", 0);
  if (faceIndex < 0)
    throw std::invalid_argument(context + ".face_index must be non-negative");
  result.faceIndex = static_cast<std::uint32_t>(faceIndex);
  result.assetId = OptionalString(source, "asset_id");
  result.relativePath = OptionalString(source, "relative_path");
  result.platform = OptionalString(source, "platform");
  result.locatorId = OptionalString(source, "locator_id");
  result.displayPath = OptionalString(source, "display_path");
  result.digest = OptionalString(source, "digest");
  result.faceFingerprint = OptionalString(source, "face_fingerprint");
  result.allowSystemGlyphFallback =
      OptionalBool(font, "allow_system_glyph_fallback", false);
  if (const auto axes = font.find("variation_axes"); axes != font.end()) {
    if (!axes->is_array())
      throw std::invalid_argument(context + ".variation_axes must be an array");
    for (const auto &axis : *axes) {
      if (!axis.is_object())
        throw std::invalid_argument(context +
                                    ".variation_axes entry must be an object");
      result.variationAxes.push_back({
          RequiredString(axis, "tag", context + ".variation_axes"),
          OptionalFloat(axis, "value", 0.0F),
      });
    }
  }
  return result;
}

TextStyle ParseTextStyle(const Json &value, const std::string &context) {
  TextStyle result;
  const auto &font = RequiredObject(value, "font", context);
  const auto &source = RequiredObject(font, "source", context + ".font");
  result.font.primary = ParseFontReference(source, font, context + ".font");
  result.font.family = result.font.primary.family;
  result.font.postscriptName = result.font.primary.postscriptName;
  result.font.weight = result.font.primary.weight;
  result.font.width = result.font.primary.width;
  result.font.slant = result.font.primary.slant;
  result.font.faceIndex = result.font.primary.faceIndex;
  result.font.variationAxes = result.font.primary.variationAxes;
  result.font.allowSystemGlyphFallback =
      result.font.primary.allowSystemGlyphFallback;
  if (const auto fallbacks = font.find("fallbacks"); fallbacks != font.end()) {
    if (!fallbacks->is_array())
      throw std::invalid_argument(context + ".font.fallbacks must be an array");
    for (const auto &fallback : *fallbacks) {
      if (!fallback.is_object())
        throw std::invalid_argument(context +
                                    ".font.fallback entry must be an object");
      const auto &fallbackSource =
          RequiredObject(fallback, "source", context + ".font.fallback");
      result.font.fallbacks.push_back(ParseFontReference(
          fallbackSource, fallback, context + ".font.fallback"));
    }
  }
  result.fontSize = OptionalFloat(value, "font_size", 64.0F);
  result.letterSpacing = OptionalFloat(value, "letter_spacing", 0.0F);
  result.wordSpacing = OptionalFloat(value, "word_spacing", 0.0F);
  result.baselineShift = OptionalFloat(value, "baseline_shift", 0.0F);
  if (const auto fill = value.find("fill"); fill != value.end())
    result.fill = ParseTextPaint(*fill, context + ".fill");
  if (const auto strokes = value.find("strokes"); strokes != value.end()) {
    if (!strokes->is_array())
      throw std::invalid_argument(context + ".strokes must be an array");
    for (const auto &stroke : *strokes) {
      result.strokes.push_back(ParseStroke(stroke, context + ".strokes"));
    }
  }
  if (result.strokes.empty()) {
    if (const auto stroke = value.find("stroke"); stroke != value.end()) {
      if (!stroke->is_object())
        throw std::invalid_argument(context + ".stroke must be an object");
      TextStroke decoded;
      decoded = ParseStroke(*stroke, context + ".stroke");
      if (decoded.width > 0.0F &&
          (decoded.usePaint || decoded.color.alpha > 0.0F))
        result.strokes.push_back(decoded);
    }
  }
  if (const auto shadows = value.find("shadows"); shadows != value.end()) {
    if (!shadows->is_array())
      throw std::invalid_argument(context + ".shadows must be an array");
    for (const auto &shadow : *shadows) {
      if (!shadow.is_object())
        throw std::invalid_argument(context + ".shadow must be an object");
      TextShadow decoded;
      decoded.offsetX = OptionalFloat(shadow, "offset_x", 0.0F);
      decoded.offsetY = OptionalFloat(shadow, "offset_y", 0.0F);
      decoded.blurRadius = OptionalFloat(shadow, "blur", 0.0F);
      decoded.spread = OptionalFloat(shadow, "spread", 0.0F);
      const auto kind = OptionalString(shadow, "kind", "outer");
      if (kind == "outer")
        decoded.kind = TextShadowKind::Outer;
      else if (kind == "inner")
        decoded.kind = TextShadowKind::Inner;
      else
        throw std::invalid_argument(context +
                                    ".shadow.kind must be outer or inner");
      if (const auto color = shadow.find("rgba"); color != shadow.end())
        decoded.color = ParseColor(*color, context + ".shadow.rgba");
      if (const auto paint = shadow.find("paint"); paint != shadow.end()) {
        decoded.usePaint = true;
        decoded.paint = ParseTextPaint(*paint, context + ".shadow.paint");
      }
      decoded.thicknessProjectionEnabled =
          OptionalBool(shadow, "thickness_projection", false);
      decoded.thicknessAngleDegrees =
          OptionalFloat(shadow, "thickness_angle_degrees", 45.0F);
      decoded.thicknessDistance =
          OptionalFloat(shadow, "thickness_distance", 0.0F);
      decoded.outerShadowSmoothingMode = ParseOuterShadowSmoothingMode(
          OptionalString(shadow, "outer_shadow_smoothing", "auto"),
          context + ".shadow.outer_shadow_smoothing");
      decoded.roundMaskIntensity =
          OptionalFloat(shadow, "round_mask_intensity", 0.0F);
      result.shadows.push_back(decoded);
    }
  }
  if (const auto background = value.find("background");
      background != value.end()) {
    result.background =
        ParseBoxBackground(*background, context + ".background");
  }
  if (const auto decoration = value.find("decoration");
      decoration != value.end()) {
    if (!decoration->is_object())
      throw std::invalid_argument(context + ".decoration must be an object");
    result.decoration.underline = OptionalBool(*decoration, "underline", false);
    result.decoration.strikeThrough =
        OptionalBool(*decoration, "strike_through", false);
    result.decoration.thickness = OptionalFloat(*decoration, "thickness", 1.0F);
    if (const auto color = decoration->find("rgba"); color != decoration->end())
      result.decoration.color =
          ParseColor(*color, context + ".decoration.rgba");
  }
  return result;
}

TextAlignment ParseAlignment(const std::string &value) {
  if (value == "start")
    return TextAlignment::Start;
  if (value == "center")
    return TextAlignment::Center;
  if (value == "end")
    return TextAlignment::End;
  if (value == "justify")
    return TextAlignment::Justify;
  throw std::invalid_argument("unknown text alignment: " + value);
}

TextDirection ParseDirection(const std::string &value) {
  if (value == "auto")
    return TextDirection::Auto;
  if (value == "ltr")
    return TextDirection::LeftToRight;
  if (value == "rtl")
    return TextDirection::RightToLeft;
  throw std::invalid_argument("unknown text direction: " + value);
}

TextOverflow ParseOverflow(const std::string &value) {
  if (value == "clip")
    return TextOverflow::Clip;
  if (value == "ellipsis")
    return TextOverflow::Ellipsis;
  if (value == "visible")
    return TextOverflow::Visible;
  throw std::invalid_argument("unknown text overflow: " + value);
}

TextWrap ParseWrap(const std::string &value) {
  if (value == "word")
    return TextWrap::Word;
  if (value == "character")
    return TextWrap::Character;
  if (value == "none")
    return TextWrap::None;
  throw std::invalid_argument("unknown text wrap: " + value);
}

TextLayoutSizingMode ParseLayoutSizingMode(const std::string &value) {
  if (value == "auto_width")
    return TextLayoutSizingMode::AutoWidth;
  if (value == "auto_height")
    return TextLayoutSizingMode::AutoHeight;
  if (value == "fixed")
    return TextLayoutSizingMode::Fixed;
  if (value == "fit_text")
    return TextLayoutSizingMode::FitText;
  throw std::invalid_argument("unknown text layout sizing mode: " + value);
}

VerticalAlignment ParseVerticalAlignment(const std::string &value) {
  if (value == "top")
    return VerticalAlignment::Top;
  if (value == "center")
    return VerticalAlignment::Center;
  if (value == "bottom")
    return VerticalAlignment::Bottom;
  throw std::invalid_argument("unknown vertical alignment: " + value);
}

TextAnimationPlaybackMode ParseAnimationPlayback(const std::string &value) {
  if (value == "once")
    return TextAnimationPlaybackMode::Once;
  if (value == "loop")
    return TextAnimationPlaybackMode::Loop;
  if (value == "ping_pong")
    return TextAnimationPlaybackMode::PingPong;
  if (value == "hold")
    return TextAnimationPlaybackMode::Hold;
  throw std::invalid_argument("unknown text animation playback: " + value);
}

std::int64_t ParseJsonInt64(const Json &value, const std::string &context) {
  if (value.is_number_unsigned()) {
    const auto result = value.get<std::uint64_t>();
    if (result >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      throw std::invalid_argument(context + " exceeds int64");
    }
    return static_cast<std::int64_t>(result);
  }
  if (!value.is_number_integer())
    throw std::invalid_argument(context + " must be an integer");
  return value.get<std::int64_t>();
}

double ParseJsonFiniteDouble(const Json &value, const std::string &context) {
  if (!value.is_number())
    throw std::invalid_argument(context + " must be numeric");
  const auto result = value.get<double>();
  if (!std::isfinite(result))
    throw std::invalid_argument(context + " must be finite");
  return result;
}

std::uint64_t ParseJsonUint64(const Json &value, const std::string &context) {
  if (value.is_number_unsigned())
    return value.get<std::uint64_t>();
  if (!value.is_number_integer())
    throw std::invalid_argument(context + " must be an unsigned integer");
  const auto result = value.get<std::int64_t>();
  if (result < 0)
    throw std::invalid_argument(context + " must be an unsigned integer");
  return static_cast<std::uint64_t>(result);
}

std::uint32_t ParseJsonUint32(const Json &value, const std::string &context) {
  const auto result = ParseJsonUint64(value, context);
  if (result > std::numeric_limits<std::uint32_t>::max())
    throw std::invalid_argument(context + " exceeds uint32");
  return static_cast<std::uint32_t>(result);
}

const Json &RequiredMember(const Json &parent, const char *key,
                           const std::string &context) {
  const auto found = parent.find(key);
  if (found == parent.end())
    throw std::invalid_argument(context + "." + key + " is required");
  return *found;
}

TextEffectInput ParseTextEffectInputJson(const Json &value,
                                         const std::string &context) {
  if (!value.is_string())
    throw std::invalid_argument(context + " must be a string");
  const auto parsed =
      ParseTextEffectInput(value.get_ref<const std::string &>());
  if (!parsed)
    throw std::invalid_argument(context + " is unsupported");
  return *parsed;
}

TextEffectOpcode ParseTextEffectOpcodeJson(const Json &value,
                                           const std::string &context) {
  if (!value.is_string())
    throw std::invalid_argument(context + " must be a string");
  const auto parsed =
      ParseTextEffectOpcode(value.get_ref<const std::string &>());
  if (!parsed)
    throw std::invalid_argument(context + " is unsupported");
  return *parsed;
}

TextEffectStageKind ParseTextEffectStageKindJson(const Json &value,
                                                 const std::string &context) {
  if (!value.is_string())
    throw std::invalid_argument(context + " must be a string");
  const auto parsed =
      ParseTextEffectStageKind(value.get_ref<const std::string &>());
  if (!parsed)
    throw std::invalid_argument(context + " is unsupported");
  return *parsed;
}

TextEffectOutput ParseTextEffectOutputJson(const Json &value,
                                           const std::string &context) {
  if (!value.is_string())
    throw std::invalid_argument(context + " must be a string");
  const auto parsed =
      ParseTextEffectOutput(value.get_ref<const std::string &>());
  if (!parsed)
    throw std::invalid_argument(context + " is unsupported");
  return *parsed;
}

TextEffectExecutionParameterKind
ParseTextEffectExecutionParameterKindJson(const Json &value,
                                          const std::string &context) {
  if (!value.is_string())
    throw std::invalid_argument(context + " must be a string");
  const auto parsed = ParseTextEffectExecutionParameterKind(
      value.get_ref<const std::string &>());
  if (!parsed)
    throw std::invalid_argument(context + " is unsupported");
  return *parsed;
}

TextEffectExecutionParameterDomain
ParseTextEffectExecutionParameterDomainJson(const Json &value,
                                            const std::string &context) {
  if (!value.is_string())
    throw std::invalid_argument(context + " must be a string");
  const auto parsed = ParseTextEffectExecutionParameterDomain(
      value.get_ref<const std::string &>());
  if (!parsed)
    throw std::invalid_argument(context + " is unsupported");
  return *parsed;
}

TextEffectExecutionParameterSpace
ParseTextEffectExecutionParameterSpaceJson(const Json &value,
                                           const std::string &context) {
  if (!value.is_string())
    throw std::invalid_argument(context + " must be a string");
  const auto parsed = ParseTextEffectExecutionParameterSpace(
      value.get_ref<const std::string &>());
  if (!parsed)
    throw std::invalid_argument(context + " is unsupported");
  return *parsed;
}

TextEffectProgramLibrary ParseTextEffectPrograms(const Json &animation) {
  const auto programs = animation.find("effect_programs");
  if (programs == animation.end())
    return {};
  if (!programs->is_array())
    throw std::invalid_argument("animation.effect_programs must be an array");

  TextEffectProgramLibrary library;
  library.reserve(programs->size());
  for (std::size_t programIndex = 0U; programIndex < programs->size();
       ++programIndex) {
    const auto &source = (*programs)[programIndex];
    const auto context =
        "animation.effect_programs[" + std::to_string(programIndex) + "]";
    RequireOnlyKeys(source, {"program_id", "revision", "random_seed", "stages"},
                    context);
    TextEffectProgramIR program;
    program.programId = RequiredString(source, "program_id", context);
    static_cast<void>(ParseJsonUint32(RequiredMember(source, "revision", context),
                                      context + ".revision"));
    program.randomSeed =
        ParseJsonUint64(RequiredMember(source, "random_seed", context),
                        context + ".random_seed");

    const auto &stages = RequiredArray(source, "stages", context);
    program.stages.reserve(stages.size());
    for (std::size_t stageIndex = 0U; stageIndex < stages.size();
         ++stageIndex) {
      const auto &stageSource = stages[stageIndex];
      const auto stageContext =
          context + ".stages[" + std::to_string(stageIndex) + "]";
      RequireOnlyKeys(
          stageSource,
          {"stage_id", "kind", "register_count", "instructions", "outputs",
           "execution_parameter_bindings"},
          stageContext);
      TextEffectProgramStageIR stage;
      stage.stageId = RequiredString(stageSource, "stage_id", stageContext);
      stage.kind = ParseTextEffectStageKindJson(
          RequiredMember(stageSource, "kind", stageContext),
          stageContext + ".kind");
      stage.registerCount = ParseJsonUint32(
          RequiredMember(stageSource, "register_count", stageContext),
          stageContext + ".register_count");

      const auto &instructions =
          RequiredArray(stageSource, "instructions", stageContext);
      stage.instructions.reserve(instructions.size());
      for (std::size_t instructionIndex = 0U;
           instructionIndex < instructions.size(); ++instructionIndex) {
        const auto &instructionSource = instructions[instructionIndex];
        const auto instructionContext = stageContext + ".instructions[" +
                                        std::to_string(instructionIndex) + "]";
        RequireOnlyKeys(instructionSource,
                        {"opcode", "output_register", "input_registers",
                         "immediates", "input"},
                        instructionContext);
        TextEffectInstruction instruction;
        instruction.opcode = ParseTextEffectOpcodeJson(
            RequiredMember(instructionSource, "opcode", instructionContext),
            instructionContext + ".opcode");
        instruction.outputRegister =
            ParseJsonUint32(RequiredMember(instructionSource, "output_register",
                                           instructionContext),
                            instructionContext + ".output_register");

        const auto &inputRegisters = RequiredArray(
            instructionSource, "input_registers", instructionContext);
        instruction.inputRegisters.reserve(inputRegisters.size());
        for (std::size_t inputIndex = 0U; inputIndex < inputRegisters.size();
             ++inputIndex) {
          instruction.inputRegisters.push_back(
              ParseJsonUint32(inputRegisters[inputIndex],
                              instructionContext + ".input_registers[" +
                                  std::to_string(inputIndex) + "]"));
        }

        const auto &immediates =
            RequiredArray(instructionSource, "immediates", instructionContext);
        instruction.immediates.reserve(immediates.size());
        for (std::size_t immediateIndex = 0U;
             immediateIndex < immediates.size(); ++immediateIndex) {
          instruction.immediates.push_back(
              ParseJsonFiniteDouble(immediates[immediateIndex],
                                    instructionContext + ".immediates[" +
                                        std::to_string(immediateIndex) + "]"));
        }
        const auto &input =
            RequiredMember(instructionSource, "input", instructionContext);
        if (!input.is_null()) {
          instruction.input =
              ParseTextEffectInputJson(input, instructionContext + ".input");
        }
        stage.instructions.push_back(std::move(instruction));
      }

      const auto &outputs = RequiredArray(stageSource, "outputs", stageContext);
      stage.outputs.reserve(outputs.size());
      for (std::size_t outputIndex = 0U; outputIndex < outputs.size();
           ++outputIndex) {
        const auto &outputSource = outputs[outputIndex];
        const auto outputContext =
            stageContext + ".outputs[" + std::to_string(outputIndex) + "]";
        RequireOnlyKeys(outputSource,
                        {"output", "register_index", "transform_index"},
                        outputContext);
        TextEffectOutputBinding binding;
        binding.output = ParseTextEffectOutputJson(
            RequiredMember(outputSource, "output", outputContext),
            outputContext + ".output");
        binding.registerIndex = ParseJsonUint32(
            RequiredMember(outputSource, "register_index", outputContext),
            outputContext + ".register_index");
        binding.transformIndex = ParseJsonUint32(
            RequiredMember(outputSource, "transform_index", outputContext),
            outputContext + ".transform_index");
        stage.outputs.push_back(binding);
      }

      if (const auto bindings =
              stageSource.find("execution_parameter_bindings");
          bindings != stageSource.end()) {
        if (!bindings->is_array()) {
          throw std::invalid_argument(
              stageContext + ".execution_parameter_bindings must be an array");
        }
        stage.executionParameterBindings.reserve(bindings->size());
        for (std::size_t bindingIndex = 0U; bindingIndex < bindings->size();
             ++bindingIndex) {
          const auto &bindingSource = (*bindings)[bindingIndex];
          const auto bindingContext =
              stageContext + ".execution_parameter_bindings[" +
              std::to_string(bindingIndex) + "]";
          RequireOnlyKeys(bindingSource,
                          {"node_id", "parameter", "domain", "value_space",
                           "slot", "register_indices"},
                          bindingContext);
          TextEffectExecutionParameterBinding binding;
          binding.nodeId =
              RequiredString(bindingSource, "node_id", bindingContext);
          binding.parameter = ParseTextEffectExecutionParameterKindJson(
              RequiredMember(bindingSource, "parameter", bindingContext),
              bindingContext + ".parameter");
          binding.domain = ParseTextEffectExecutionParameterDomainJson(
              RequiredMember(bindingSource, "domain", bindingContext),
              bindingContext + ".domain");
          binding.valueSpace = ParseTextEffectExecutionParameterSpaceJson(
              RequiredMember(bindingSource, "value_space", bindingContext),
              bindingContext + ".value_space");
          binding.slot = ParseJsonUint32(
              RequiredMember(bindingSource, "slot", bindingContext),
              bindingContext + ".slot");
          const auto &registerIndices = RequiredArray(
              bindingSource, "register_indices", bindingContext);
          binding.registerIndices.reserve(registerIndices.size());
          for (std::size_t registerIndex = 0U;
               registerIndex < registerIndices.size(); ++registerIndex) {
            binding.registerIndices.push_back(ParseJsonUint32(
                registerIndices[registerIndex],
                bindingContext + ".register_indices[" +
                    std::to_string(registerIndex) + "]"));
          }
          stage.executionParameterBindings.push_back(std::move(binding));
        }
      }
      program.stages.push_back(std::move(stage));
    }
    std::string validationError;
    if (!ValidateTextEffectProgramIR(program, &validationError)) {
      throw std::invalid_argument(context + " is invalid: " + validationError);
    }
    library.push_back(std::move(program));
  }
  return library;
}

TextRenderGroupMode ParseTextRenderGroupMode(const Json &value,
                                             const std::string &context) {
  if (value.is_string()) {
    const auto mode = value.get<std::string>();
    if (mode == "page")
      return TextRenderGroupMode::Page;
    if (mode == "per_letter")
      return TextRenderGroupMode::PerLetter;
    if (mode == "per_line")
      return TextRenderGroupMode::PerLine;
    if (mode == "per_word")
      return TextRenderGroupMode::PerWord;
    if (mode == "custom")
      return TextRenderGroupMode::Custom;
  } else if (value.is_number_integer() || value.is_number_unsigned()) {
    switch (ParseJsonInt64(value, context)) {
    case 0:
      return TextRenderGroupMode::Page;
    case 1:
      return TextRenderGroupMode::PerLetter;
    case 2:
      return TextRenderGroupMode::PerLine;
    case 3:
      return TextRenderGroupMode::PerWord;
    case 99:
      return TextRenderGroupMode::Custom;
    default:
      break;
    }
  }
  throw std::invalid_argument(context + " is unsupported");
}

TextSelectorShape ParseTextRenderGroupShape(const Json &value,
                                            const std::string &context) {
  if (value.is_string()) {
    const auto shape = value.get<std::string>();
    if (shape == "linear")
      return TextSelectorShape::Linear;
    if (shape == "ramp_up")
      return TextSelectorShape::RampUp;
    if (shape == "ramp_down")
      return TextSelectorShape::RampDown;
    if (shape == "triangle")
      return TextSelectorShape::Triangle;
    if (shape == "round")
      return TextSelectorShape::Round;
    if (shape == "smooth")
      return TextSelectorShape::Smooth;
    if (shape == "custom_cubic")
      return TextSelectorShape::CustomCubic;
    if (shape == "square")
      return TextSelectorShape::Square;
  } else if (value.is_number_integer() || value.is_number_unsigned()) {
    // Authored render-group Shape: Square=0, UpSlope=1, DownSlope=2,
    // Triangle=3, Custom=4. It is normalized into the existing semantic enum.
    switch (ParseJsonInt64(value, context)) {
    case 0:
      return TextSelectorShape::Square;
    case 1:
      return TextSelectorShape::RampUp;
    case 2:
      return TextSelectorShape::RampDown;
    case 3:
      return TextSelectorShape::Triangle;
    case 4:
      return TextSelectorShape::CustomCubic;
    default:
      break;
    }
  }
  throw std::invalid_argument(context + " is unsupported");
}

TextRenderGroupTimeRange
ParseTextRenderGroupTimeRange(const Json &value, const std::string &context) {
  if (!value.is_object())
    throw std::invalid_argument(context + " must be an object");
  const auto start = value.find("start_time_us");
  const auto end = value.find("end_time_us");
  if (start == value.end() || end == value.end()) {
    throw std::invalid_argument(context +
                                " requires start_time_us and end_time_us");
  }
  return {ParseJsonInt64(*start, context + ".start_time_us"),
          ParseJsonInt64(*end, context + ".end_time_us")};
}

TextShowpieceAnchor ParseShowpieceAnchor(const std::string &value) {
  if (value == "bbox_center" || value == "text_bounds_center")
    return TextShowpieceAnchor::TextBoundsCenter;
  if (value == "canvas_center")
    return TextShowpieceAnchor::CanvasCenter;
  throw std::invalid_argument("unknown text showpiece anchor: " + value);
}

TextShowpieceFit ParseShowpieceFit(const std::string &value) {
  if (value == "inherit")
    return TextShowpieceFit::Inherit;
  if (value == "contain")
    return TextShowpieceFit::Contain;
  if (value == "cover")
    return TextShowpieceFit::Cover;
  if (value == "stretch")
    return TextShowpieceFit::Stretch;
  if (value == "native")
    return TextShowpieceFit::Native;
  if (value == "fit_width")
    return TextShowpieceFit::FitWidth;
  if (value == "fit_height")
    return TextShowpieceFit::FitHeight;
  if (value == "fit_long_side")
    return TextShowpieceFit::FitLongSide;
  if (value == "fit_short_side")
    return TextShowpieceFit::FitShortSide;
  throw std::invalid_argument("unknown text showpiece fit: " + value);
}

template <typename Keyframe>
Keyframe ParseScalarTextKeyframe(const Json &source) {
  Keyframe keyframe;
  keyframe.offset = OptionalDouble(source, "offset", 0.0);
  keyframe.value = OptionalDouble(source, "value", 0.0);
  keyframe.tangentIn = OptionalDouble(source, "tangent_in", 0.0);
  keyframe.tangentOut = OptionalDouble(source, "tangent_out", 0.0);
  keyframe.cubicBezier = OptionalBool(source, "bezier", false);
  keyframe.bezierTimeIn = OptionalDouble(source, "bezier_time_in", 0.0);
  keyframe.bezierTimeOut = OptionalDouble(source, "bezier_time_out", 0.0);
  return keyframe;
}

void ParseTextKeyframeArray(const Json &source, const char *field,
                            const std::string &context,
                            std::vector<TextKeyframe> &output) {
  const auto found = source.find(field);
  if (found == source.end())
    return;
  if (!found->is_array())
    throw std::invalid_argument(context + "." + field + " must be an array");
  for (const auto &keyframeSource : *found) {
    if (!keyframeSource.is_object())
      throw std::invalid_argument(context + "." + field +
                                  " entry must be an object");
    output.push_back(ParseScalarTextKeyframe<TextKeyframe>(keyframeSource));
  }
}

TextAnimatedProperty ParseAnimatedProperty(const std::string &value) {
  if (value == "opacity")
    return TextAnimatedProperty::Opacity;
  if (value == "position_x")
    return TextAnimatedProperty::PositionX;
  if (value == "position_y")
    return TextAnimatedProperty::PositionY;
  if (value == "scale_x")
    return TextAnimatedProperty::ScaleX;
  if (value == "scale_y")
    return TextAnimatedProperty::ScaleY;
  if (value == "position_z")
    return TextAnimatedProperty::PositionZ;
  if (value == "rotation_x")
    return TextAnimatedProperty::RotationX;
  if (value == "rotation_y")
    return TextAnimatedProperty::RotationY;
  if (value == "rotation_z")
    return TextAnimatedProperty::RotationZ;
  if (value == "shear_x")
    return TextAnimatedProperty::ShearX;
  if (value == "shear_y")
    return TextAnimatedProperty::ShearY;
  if (value == "tracking")
    return TextAnimatedProperty::Tracking;
  if (value == "blur_radius")
    return TextAnimatedProperty::BlurRadius;
  if (value == "distance_from_center")
    return TextAnimatedProperty::DistanceFromCenter;
  throw std::invalid_argument("unknown text animator property: " + value);
}

TextLayerAnimationPhase ParseLayerAnimationPhase(const std::string &value,
                                                 const std::string &context) {
  if (value == "enter")
    return TextLayerAnimationPhase::Enter;
  if (value == "loop")
    return TextLayerAnimationPhase::Loop;
  if (value == "exit")
    return TextLayerAnimationPhase::Exit;
  throw std::invalid_argument(context + " is unsupported");
}

TextLayerAnimationProperty
ParseLayerAnimationProperty(const std::string &value,
                            const std::string &context) {
  if (value == "opacity")
    return TextLayerAnimationProperty::Opacity;
  if (value == "position_x")
    return TextLayerAnimationProperty::PositionX;
  if (value == "position_y")
    return TextLayerAnimationProperty::PositionY;
  if (value == "scale_x")
    return TextLayerAnimationProperty::ScaleX;
  if (value == "scale_y")
    return TextLayerAnimationProperty::ScaleY;
  if (value == "rotation_degrees")
    return TextLayerAnimationProperty::RotationDegrees;
  throw std::invalid_argument(context + " is unsupported");
}

TextAnimationEasing ParseLayerAnimationEasing(const std::string &value,
                                              const std::string &context) {
  if (value == "linear")
    return TextAnimationEasing::Linear;
  if (value == "ease_in")
    return TextAnimationEasing::EaseIn;
  if (value == "ease_out")
    return TextAnimationEasing::EaseOut;
  if (value == "ease_in_out")
    return TextAnimationEasing::EaseInOut;
  throw std::invalid_argument(context + " is unsupported");
}

TextLayerAnimationClip ParseLayerAnimationClip(const Json &source,
                                               const std::string &context) {
  RequireOnlyKeys(source, {"id", "phase", "duration_us", "tracks"}, context);
  TextLayerAnimationClip clip;
  clip.clipId = RequiredString(source, "id", context);
  clip.phase = ParseLayerAnimationPhase(
      RequiredString(source, "phase", context), context + ".phase");
  clip.durationUs = RequiredInteger(source, "duration_us", context);
  if (clip.durationUs <= 0)
    throw std::invalid_argument(context + ".duration_us must be positive");

  const auto &tracks = RequiredArray(source, "tracks", context);
  if (tracks.empty() || tracks.size() > 128U)
    throw std::invalid_argument(context + ".tracks exceeds the native budget");
  std::vector<TextLayerAnimationProperty> properties;
  properties.reserve(tracks.size());
  for (std::size_t trackIndex = 0U; trackIndex < tracks.size(); ++trackIndex) {
    const auto &trackSource = tracks[trackIndex];
    const auto trackContext =
        context + ".tracks[" + std::to_string(trackIndex) + "]";
    RequireOnlyKeys(trackSource, {"property", "keyframes"}, trackContext);
    TextLayerAnimationTrack track;
    track.property = ParseLayerAnimationProperty(
        RequiredString(trackSource, "property", trackContext),
        trackContext + ".property");
    if (std::find(properties.begin(), properties.end(), track.property) !=
        properties.end()) {
      throw std::invalid_argument(trackContext +
                                  ".property must be unique in a clip");
    }
    properties.push_back(track.property);

    const auto &keyframes =
        RequiredArray(trackSource, "keyframes", trackContext);
    if (keyframes.empty() || keyframes.size() > 64U) {
      throw std::invalid_argument(trackContext +
                                  ".keyframes exceeds the native budget");
    }
    float previousOffset = -1.0F;
    for (std::size_t keyframeIndex = 0U; keyframeIndex < keyframes.size();
         ++keyframeIndex) {
      const auto &keyframeSource = keyframes[keyframeIndex];
      const auto keyframeContext =
          trackContext + ".keyframes[" + std::to_string(keyframeIndex) + "]";
      RequireOnlyKeys(keyframeSource, {"offset", "value", "easing"},
                      keyframeContext);
      const float offset = OptionalFloat(
          keyframeSource, "offset", std::numeric_limits<float>::quiet_NaN());
      const float value = OptionalFloat(
          keyframeSource, "value", std::numeric_limits<float>::quiet_NaN());
      if (!std::isfinite(offset) || offset < 0.0F || offset > 1.0F ||
          offset < previousOffset) {
        throw std::invalid_argument(
            keyframeContext +
            ".offset must be finite, normalized, and nondecreasing");
      }
      if (!std::isfinite(value) || std::fabs(value) > 65'536.0F)
        throw std::invalid_argument(keyframeContext + ".value is invalid");
      TextLayerAnimationKeyframe keyframe;
      keyframe.offset = offset;
      keyframe.value = value;
      keyframe.easing = ParseLayerAnimationEasing(
          RequiredString(keyframeSource, "easing", keyframeContext),
          keyframeContext + ".easing");
      track.keyframes.push_back(keyframe);
      previousOffset = offset;
    }
    clip.tracks.push_back(std::move(track));
  }
  return clip;
}

void ParseTimedSpans(const Json &animation, RichTextDocument &document) {
  const auto found = animation.find("timed_spans");
  if (found == animation.end())
    return;
  if (!found->is_array())
    throw std::invalid_argument("animation.timed_spans must be an array");
  for (std::size_t index = 0U; index < found->size(); ++index) {
    const auto &source = (*found)[index];
    const auto context = "animation.timed_spans[" + std::to_string(index) + "]";
    if (!source.is_object())
      throw std::invalid_argument(context + " must be an object");
    RequireOnlyKeys(source,
                    {"id", "paragraph_id", "run_id", "utf8_begin", "utf8_end",
                     "start_offset_us", "end_offset_us",
                     "transition_end_offset_us", "semantic", "progress"},
                    context);
    TimedTextSpan span;
    span.spanId = RequiredString(source, "id", context);
    span.paragraphId = RequiredString(source, "paragraph_id", context);
    span.runId = RequiredString(source, "run_id", context);
    span.utf8Begin = static_cast<std::size_t>(
        RequiredInteger(source, "utf8_begin", context));
    const auto end = source.find("utf8_end");
    if (end == source.end())
      throw std::invalid_argument(context + ".utf8_end is required");
    if (end->is_string() && end->get<std::string>() == "run_end") {
      const auto paragraph =
          std::find_if(document.paragraphs.begin(), document.paragraphs.end(),
                       [&](const RichTextParagraph &candidate) {
                         return candidate.paragraphId == span.paragraphId;
                       });
      if (paragraph == document.paragraphs.end())
        throw std::invalid_argument(context + " paragraph is missing");
      const auto run =
          std::find_if(paragraph->runs.begin(), paragraph->runs.end(),
                       [&](const RichTextRun &candidate) {
                         return candidate.runId == span.runId;
                       });
      if (run == paragraph->runs.end())
        throw std::invalid_argument(context + " run is missing");
      span.utf8End = run->utf8Text.size();
    } else if (end->is_number_integer()) {
      span.utf8End = end->get<std::size_t>();
    } else {
      throw std::invalid_argument(context + ".utf8_end is invalid");
    }
    span.startOffsetUs = RequiredInteger(source, "start_offset_us", context);
    span.endOffsetUs = RequiredInteger(source, "end_offset_us", context);
    span.transitionEndOffsetUs =
        source.value("transition_end_offset_us", std::int64_t{0});
    const auto semantic = OptionalString(source, "semantic", "karaoke_word");
    if (semantic == "keyword")
      span.semantic = TimedTextSpanSemantic::Keyword;
    else if (semantic == "karaoke_word")
      span.semantic = TimedTextSpanSemantic::KaraokeWord;
    else if (semantic == "emphasis")
      span.semantic = TimedTextSpanSemantic::Emphasis;
    else if (semantic == "mention")
      span.semantic = TimedTextSpanSemantic::Mention;
    else
      throw std::invalid_argument(context + ".semantic is unsupported");
    const auto progress = OptionalString(source, "progress", "step");
    if (progress == "step")
      span.progressMode = TimedTextProgressMode::Step;
    else if (progress == "grapheme_sweep")
      span.progressMode = TimedTextProgressMode::GraphemeSweep;
    else
      throw std::invalid_argument(context + ".progress is unsupported");
    document.timedSpans.push_back(std::move(span));
  }
}

void ParseAnimationLayers(const Json &animation, TextAnimationStack &stack) {
  RequireOnlyKeys(animation,
                  {"revision", "effect_programs", "layers", "timed_spans"},
                  "animation");
  stack.effectPrograms = ParseTextEffectPrograms(animation);
  const auto found = animation.find("layers");
  if (found == animation.end())
    return;
  if (!found->is_array())
    throw std::invalid_argument("animation.layers must be an array");
  stack.revision = animation.value("revision", std::uint32_t{1});
  for (std::size_t layerIndex = 0U; layerIndex < found->size(); ++layerIndex) {
    const auto &source = (*found)[layerIndex];
    const auto context = "animation.layers[" + std::to_string(layerIndex) + "]";
    RequireOnlyKeys(source,
                    {"id", "preset_id", "revision", "enabled",
                     "requires_timed_text", "render_group", "time_driver",
                     "layer_track", "animators", "post_effects", "showpieces"},
                    context);
    TextAnimationLayerSpec layer;
    layer.layerId = RequiredString(source, "id", context);
    layer.presetId = OptionalString(source, "preset_id");
    layer.revision = source.value("revision", std::uint32_t{1});
    layer.enabled = OptionalBool(source, "enabled", true);
    layer.requiresTimedText =
        OptionalBool(source, "requires_timed_text", false);
    if (const auto renderGroup = source.find("render_group");
        renderGroup != source.end()) {
      if (!renderGroup->is_object())
        throw std::invalid_argument(context +
                                    ".render_group must be an object");
      TextRenderGroupSpec spec;
      spec.expandRatioX = OptionalFloat(*renderGroup, "expand_ratio_x", 1.0F);
      spec.expandRatioY = OptionalFloat(*renderGroup, "expand_ratio_y", 1.0F);
      if (const auto mode = renderGroup->find("mode");
          mode != renderGroup->end()) {
        spec.mode =
            ParseTextRenderGroupMode(*mode, context + ".render_group.mode");
      }
      spec.offset = OptionalFloat(*renderGroup, "offset", 0.0F);
      if (const auto duration = renderGroup->find("duration");
          duration != renderGroup->end()) {
        spec.duration = ParseTextRenderGroupTimeRange(
            *duration, context + ".render_group.duration");
      }
      if (const auto priority = renderGroup->find("priority");
          priority != renderGroup->end()) {
        const auto value =
            ParseJsonInt64(*priority, context + ".render_group.priority");
        if (value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::int32_t>::max()) {
          throw std::invalid_argument(context +
                                      ".render_group.priority exceeds int32");
        }
        spec.priority = static_cast<std::int32_t>(value);
      }
      if (const auto randomSeed = renderGroup->find("random_seed");
          randomSeed != renderGroup->end()) {
        const auto value =
            ParseJsonInt64(*randomSeed, context + ".render_group.random_seed");
        if (value < 0 || static_cast<std::uint64_t>(value) >
                             std::numeric_limits<std::uint32_t>::max()) {
          throw std::invalid_argument(
              context + ".render_group.random_seed exceeds uint32");
        }
        spec.randomSeed = static_cast<std::uint32_t>(value);
      }
      spec.randomSort = OptionalBool(*renderGroup, "random_sort", false);
      if (const auto shape = renderGroup->find("shape");
          shape != renderGroup->end()) {
        spec.shape =
            ParseTextRenderGroupShape(*shape, context + ".render_group.shape");
      }
      if (const auto customRanges = renderGroup->find("custom_ranges");
          customRanges != renderGroup->end()) {
        if (!customRanges->is_array()) {
          throw std::invalid_argument(
              context + ".render_group.custom_ranges must be an array");
        }
        spec.customRanges.reserve(customRanges->size());
        for (std::size_t rangeIndex = 0U; rangeIndex < customRanges->size();
             ++rangeIndex) {
          const auto &rangeSource = (*customRanges)[rangeIndex];
          const auto rangeContext = context + ".render_group.custom_ranges[" +
                                    std::to_string(rangeIndex) + "]";
          if (!rangeSource.is_object())
            throw std::invalid_argument(rangeContext + " must be an object");
          const auto start = rangeSource.find("start_index");
          const auto end = rangeSource.find("end_index");
          if (start == rangeSource.end() || end == rangeSource.end()) {
            throw std::invalid_argument(rangeContext +
                                        " requires start_index and end_index");
          }
          TextRenderGroupCustomRange range;
          range.startIndex =
              ParseJsonInt64(*start, rangeContext + ".start_index");
          range.endIndex = ParseJsonInt64(*end, rangeContext + ".end_index");
          range.intensity = OptionalFloat(rangeSource, "intensity", 1.0F);
          if (const auto localTime = rangeSource.find("local_time");
              localTime != rangeSource.end()) {
            range.localTime = ParseTextRenderGroupTimeRange(
                *localTime, rangeContext + ".local_time");
          }
          spec.customRanges.push_back(std::move(range));
        }
      }
      layer.renderGroup = std::move(spec);
    }
    if (const auto driver = source.find("time_driver");
        driver != source.end()) {
      if (!driver->is_object())
        throw std::invalid_argument(context + ".time_driver must be an object");
      const auto kind = OptionalString(*driver, "kind", "clip_local");
      if (kind == "enter_phase")
        layer.timeDriver.kind = TextAnimationTimeDriverKind::EnterPhase;
      else if (kind == "exit_phase")
        layer.timeDriver.kind = TextAnimationTimeDriverKind::ExitPhase;
      else if (kind == "loop_phase")
        layer.timeDriver.kind = TextAnimationTimeDriverKind::LoopPhase;
      else if (kind == "clip_local")
        layer.timeDriver.kind = TextAnimationTimeDriverKind::ClipLocal;
      else if (kind == "timed_ranges")
        layer.timeDriver.kind = TextAnimationTimeDriverKind::TimedRanges;
      else
        throw std::invalid_argument(context +
                                    ".time_driver.kind is unsupported");
      layer.timeDriver.startOffsetUs =
          driver->value("start_offset_us", std::int64_t{0});
      layer.timeDriver.durationUs =
          driver->value("duration_us", std::int64_t{1'000'000});
      layer.timeDriver.playback =
          ParseAnimationPlayback(OptionalString(*driver, "playback", "once"));
      if (layer.timeDriver.kind == TextAnimationTimeDriverKind::TimedRanges) {
        TextTimedRangeDriverSpec timed;
        const auto mode =
            OptionalString(*driver, "timed_mode", "span_progress");
        if (mode == "span_step")
          timed.mode = TextTimedDriverMode::SpanStep;
        else if (mode == "span_progress")
          timed.mode = TextTimedDriverMode::SpanProgress;
        else if (mode == "grapheme_sweep")
          timed.mode = TextTimedDriverMode::GraphemeSweep;
        else if (mode == "active_hold")
          timed.mode = TextTimedDriverMode::ActiveHold;
        else
          throw std::invalid_argument(context +
                                      ".time_driver.timed_mode is unsupported");
        timed.useActiveAppearance =
            OptionalBool(*driver, "use_active_appearance", true);
        if (const auto targets = driver->find("span_ids");
            targets != driver->end()) {
          if (!targets->is_array())
            throw std::invalid_argument(
                context + ".time_driver.span_ids must be an array");
          for (const auto &target : *targets) {
            if (!target.is_string())
              throw std::invalid_argument(
                  context + ".time_driver.span_ids entry must be a string");
            timed.spanIds.push_back(target.get<std::string>());
          }
        }
        if (const auto authored = driver->find("active_appearance");
            authored != driver->end()) {
          if (!authored->is_object())
            throw std::invalid_argument(
                context + ".time_driver.active_appearance must be an object");
          TextTimedAppearanceSpec appearance;
          if (const auto color = authored->find("fill_rgba");
              color != authored->end())
            appearance.fill = ParseColor(
                *color, context + ".time_driver.active_appearance.fill_rgba");
          if (const auto color = authored->find("stroke_rgba");
              color != authored->end())
            appearance.stroke = ParseColor(
                *color, context + ".time_driver.active_appearance.stroke_rgba");
          if (const auto color = authored->find("shadow_rgba");
              color != authored->end())
            appearance.shadow = ParseColor(
                *color, context + ".time_driver.active_appearance.shadow_rgba");
          if (const auto background = authored->find("background");
              background != authored->end())
            appearance.background = ParseBoxBackground(
                *background,
                context + ".time_driver.active_appearance.background");
          timed.activeAppearance = std::move(appearance);
          timed.useActiveAppearance = true;
        }
        layer.timeDriver.timedRanges = timed;
      }
    }
    if (const auto layerTrack = source.find("layer_track");
        layerTrack != source.end()) {
      layer.layerTrack =
          ParseLayerAnimationClip(*layerTrack, context + ".layer_track");
      const auto expectedDriver =
          layer.layerTrack->phase == TextLayerAnimationPhase::Enter
              ? TextAnimationTimeDriverKind::EnterPhase
          : layer.layerTrack->phase == TextLayerAnimationPhase::Loop
              ? TextAnimationTimeDriverKind::LoopPhase
              : TextAnimationTimeDriverKind::ExitPhase;
      if (layer.timeDriver.kind != expectedDriver)
        throw std::invalid_argument(
            context + ".layer_track.phase does not match time_driver.kind");
      if (layer.layerTrack->durationUs != layer.timeDriver.durationUs)
        throw std::invalid_argument(
            context + ".layer_track.duration_us does not match time_driver");
    }
    if (const auto animators = source.find("animators");
        animators != source.end()) {
      if (!animators->is_array())
        throw std::invalid_argument(context + ".animators must be an array");
      for (std::size_t animatorIndex = 0U; animatorIndex < animators->size();
           ++animatorIndex) {
        const auto &authored = (*animators)[animatorIndex];
        const auto animatorContext =
            context + ".animators[" + std::to_string(animatorIndex) + "]";
        RequireOnlyKeys(authored,
                        {"id", "position_mode", "anchor", "anchor_basis",
                         "anchor_mode", "anchor_offset_x", "anchor_offset_y",
                         "presentation", "fade_fraction", "effect_program_id",
                         "active_rgba", "projection", "selectors", "tracks",
                         "fill_color_keyframes"},
                        animatorContext);
        TextAnimatorSpec animator;
        animator.animatorId = RequiredString(authored, "id", animatorContext);
        const auto positionMode =
            OptionalString(authored, "position_mode", "absolute_pixels");
        if (positionMode == "absolute_pixels")
          animator.positionMode = TextPositionMode::AbsolutePixels;
        else if (positionMode == "text_bounds_offset")
          animator.positionMode = TextPositionMode::TextBoundsOffset;
        else if (positionMode == "distance_from_center")
          animator.positionMode = TextPositionMode::DistanceFromCenter;
        else if (positionMode == "space_x")
          animator.positionMode = TextPositionMode::SpaceX;
        else if (positionMode == "space_y")
          animator.positionMode = TextPositionMode::SpaceY;
        else
          throw std::invalid_argument(animatorContext +
                                      ".position_mode is unsupported");
        const auto anchor = OptionalString(authored, "anchor", "glyph_center");
        if (anchor == "glyph_center")
          animator.anchor = TextUnitAnchor::GlyphCenter;
        else if (anchor == "baseline")
          animator.anchor = TextUnitAnchor::Baseline;
        else if (anchor == "line_box")
          animator.anchor = TextUnitAnchor::LineBox;
        else
          throw std::invalid_argument(animatorContext +
                                      ".anchor is unsupported");
        const auto anchorBasis =
            OptionalString(authored, "anchor_basis", "letter");
        if (anchorBasis == "letter")
          animator.anchorBasis = TextAnimatorAnchorBasis::Letter;
        else if (anchorBasis == "line")
          animator.anchorBasis = TextAnimatorAnchorBasis::Line;
        else if (anchorBasis == "word")
          animator.anchorBasis = TextAnimatorAnchorBasis::Word;
        else if (anchorBasis == "page")
          animator.anchorBasis = TextAnimatorAnchorBasis::Page;
        else
          throw std::invalid_argument(animatorContext +
                                      ".anchor_basis is unsupported");
        const auto anchorMode =
            OptionalString(authored, "anchor_mode", "fixed");
        if (anchorMode == "fixed")
          animator.anchorMode = TextAnimatorAnchorMode::Fixed;
        else if (anchorMode == "selector_influenced")
          animator.anchorMode = TextAnimatorAnchorMode::SelectorInfluenced;
        else
          throw std::invalid_argument(animatorContext +
                                      ".anchor_mode is unsupported");
        animator.anchorOffsetX =
            OptionalFloat(authored, "anchor_offset_x", 0.0F);
        animator.anchorOffsetY =
            OptionalFloat(authored, "anchor_offset_y", 0.0F);
        const auto presentation =
            OptionalString(authored, "presentation", "transform");
        if (presentation == "transform")
          animator.presentation = TextUnitPresentation::Transform;
        else if (presentation == "reveal")
          animator.presentation = TextUnitPresentation::Reveal;
        else if (presentation == "active_fill")
          animator.presentation = TextUnitPresentation::ActiveFill;
        else
          throw std::invalid_argument(animatorContext +
                                      ".presentation is unsupported");
        animator.fadeFraction = OptionalFloat(authored, "fade_fraction", 0.0F);
        animator.effectProgramId =
            OptionalString(authored, "effect_program_id");
        if (const auto color = authored.find("active_rgba");
            color != authored.end()) {
          animator.activeColor =
              ParseColor(*color, animatorContext + ".active_rgba");
        }
        if (const auto projection = authored.find("projection");
            projection != authored.end()) {
          if (!projection->is_object())
            throw std::invalid_argument(animatorContext +
                                        ".projection must be an object");
          const auto kind = OptionalString(*projection, "kind", "planar_2d");
          if (kind == "planar_2d")
            animator.projection.kind = TextProjectionKind::Planar2D;
          else if (kind == "perspective_3d")
            animator.projection.kind = TextProjectionKind::Perspective3D;
          else
            throw std::invalid_argument(animatorContext +
                                        ".projection.kind is unsupported");
          animator.projection.fieldOfViewDegrees =
              OptionalFloat(*projection, "field_of_view_degrees", 45.0F);
          animator.projection.vanishingPointX =
              OptionalFloat(*projection, "vanishing_point_x", 0.5F);
          animator.projection.vanishingPointY =
              OptionalFloat(*projection, "vanishing_point_y", 0.5F);
        }
        const auto &selectors =
            RequiredArray(authored, "selectors", animatorContext);
        for (std::size_t selectorIndex = 0U; selectorIndex < selectors.size();
             ++selectorIndex) {
          const auto &selectorSource = selectors[selectorIndex];
          if (!selectorSource.is_object())
            throw std::invalid_argument(animatorContext +
                                        ".selector must be an object");
          TextUnitSelector selector;
          const auto selectorKind =
              OptionalString(selectorSource, "kind", "range");
          if (selectorKind == "range")
            selector.kind = TextSelectorKind::Range;
          else if (selectorKind == "time")
            selector.kind = TextSelectorKind::Time;
          else
            throw std::invalid_argument(animatorContext +
                                        ".selector.kind is unsupported");
          const auto basedOn =
              OptionalString(selectorSource, "based_on", "grapheme");
          if (basedOn == "grapheme")
            selector.basedOn = TextUnitBasis::Grapheme;
          else if (basedOn == "word")
            selector.basedOn = TextUnitBasis::Word;
          else if (basedOn == "line")
            selector.basedOn = TextUnitBasis::Line;
          else if (basedOn == "all")
            selector.basedOn = TextUnitBasis::All;
          else
            throw std::invalid_argument(animatorContext +
                                        ".selector.based_on is unsupported");
          selector.rangeStart =
              OptionalDouble(selectorSource, "range_start", 0.0);
          selector.rangeEnd = OptionalDouble(selectorSource, "range_end", 1.0);
          selector.offset = OptionalDouble(selectorSource, "offset", 0.0);
          selector.stagger = OptionalDouble(selectorSource, "stagger", 0.0);
          selector.edgeSmooth =
              OptionalDouble(selectorSource, "edge_smooth", 0.0);
          selector.constrained =
              OptionalBool(selectorSource, "constrained", true);
          const auto shape = OptionalString(selectorSource, "shape", "linear");
          if (shape == "linear")
            selector.shape = TextSelectorShape::Linear;
          else if (shape == "ramp_up")
            selector.shape = TextSelectorShape::RampUp;
          else if (shape == "ramp_down")
            selector.shape = TextSelectorShape::RampDown;
          else if (shape == "triangle")
            selector.shape = TextSelectorShape::Triangle;
          else if (shape == "round")
            selector.shape = TextSelectorShape::Round;
          else if (shape == "smooth")
            selector.shape = TextSelectorShape::Smooth;
          else if (shape == "custom_cubic")
            selector.shape = TextSelectorShape::CustomCubic;
          else if (shape == "square")
            selector.shape = TextSelectorShape::Square;
          else
            throw std::invalid_argument(animatorContext +
                                        ".selector.shape is unsupported");
          const auto order = OptionalString(selectorSource, "order", "forward");
          if (order == "forward")
            selector.order = TextUnitOrder::Forward;
          else if (order == "backward")
            selector.order = TextUnitOrder::Backward;
          else if (order == "center_out")
            selector.order = TextUnitOrder::CenterOut;
          else if (order == "random")
            selector.order = TextUnitOrder::Random;
          else
            throw std::invalid_argument(animatorContext +
                                        ".selector.order is unsupported");
          if (const auto randomSeed = selectorSource.find("random_seed");
              randomSeed != selectorSource.end()) {
            const auto seedContext = animatorContext + ".random_seed";
            selector.randomSeed =
                ParseJsonFiniteDouble(*randomSeed, seedContext);
          }
          selector.intensity = OptionalDouble(selectorSource, "intensity", 1.0);
          selector.intensityStart =
              OptionalDouble(selectorSource, "intensity_start", 0.0);
          selector.intensityEnd =
              OptionalDouble(selectorSource, "intensity_end", 1.0);
          selector.timeStart1 =
              OptionalDouble(selectorSource, "time_start_1", 0.0);
          selector.timeStart2 =
              OptionalDouble(selectorSource, "time_start_2", 0.0);
          selector.timeEnd1 = OptionalDouble(selectorSource, "time_end_1", 1.0);
          selector.timeEnd2 = OptionalDouble(selectorSource, "time_end_2", 1.0);
          selector.timeCycle =
              OptionalBool(selectorSource, "time_cycle", false);
          const auto parseSelectorKeyframes = [&](const char *field,
                                                  auto &output) {
            const auto found = selectorSource.find(field);
            if (found == selectorSource.end())
              return;
            if (!found->is_array())
              throw std::invalid_argument(animatorContext + "." + field +
                                          " must be an array");
            for (const auto &keyframeSource : *found) {
              if (!keyframeSource.is_object())
                throw std::invalid_argument(animatorContext + "." + field +
                                            " entry must be an object");
              output.push_back(ParseScalarTextKeyframe<TextSelectorKeyframe>(
                  keyframeSource));
            }
          };
          parseSelectorKeyframes("range_start_keyframes",
                                 selector.rangeStartKeyframes);
          parseSelectorKeyframes("range_end_keyframes",
                                 selector.rangeEndKeyframes);
          parseSelectorKeyframes("offset_keyframes", selector.offsetKeyframes);
          parseSelectorKeyframes("intensity_keyframes",
                                 selector.intensityKeyframes);
          animator.selectors.push_back(std::move(selector));
        }
        const auto &tracks = RequiredArray(authored, "tracks", animatorContext);
        for (std::size_t trackIndex = 0U; trackIndex < tracks.size();
             ++trackIndex) {
          const auto &trackSource = tracks[trackIndex];
          if (!trackSource.is_object())
            throw std::invalid_argument(animatorContext +
                                        ".track must be an object");
          TextAnimatorTrack track;
          track.property = ParseAnimatedProperty(
              RequiredString(trackSource, "property", animatorContext));
          const auto &keyframes =
              RequiredArray(trackSource, "keyframes", animatorContext);
          for (const auto &keyframeSource : keyframes) {
            if (!keyframeSource.is_object())
              throw std::invalid_argument(animatorContext +
                                          ".keyframe must be an object");
            track.keyframes.push_back(
                ParseScalarTextKeyframe<TextKeyframe>(keyframeSource));
          }
          animator.tracks.push_back(std::move(track));
        }
        if (const auto colorKeyframes = authored.find("fill_color_keyframes");
            colorKeyframes != authored.end()) {
          if (!colorKeyframes->is_array())
            throw std::invalid_argument(
                animatorContext + ".fill_color_keyframes must be an array");
          for (const auto &keyframeSource : *colorKeyframes) {
            if (!keyframeSource.is_object())
              throw std::invalid_argument(
                  animatorContext +
                  ".fill_color_keyframes entry must be an object");
            TextColorKeyframe keyframe;
            keyframe.offset = OptionalFloat(keyframeSource, "offset", 0.0F);
            keyframe.value =
                ParseColor(keyframeSource.at("rgba"),
                           animatorContext + ".fill_color_keyframes.rgba");
            if (const auto tangent = keyframeSource.find("tangent_in");
                tangent != keyframeSource.end())
              keyframe.tangentIn = ParseColorTangent(
                  *tangent,
                  animatorContext + ".fill_color_keyframes.tangent_in");
            if (const auto tangent = keyframeSource.find("tangent_out");
                tangent != keyframeSource.end())
              keyframe.tangentOut = ParseColorTangent(
                  *tangent,
                  animatorContext + ".fill_color_keyframes.tangent_out");
            keyframe.cubicBezier =
                OptionalBool(keyframeSource, "bezier", false);
            keyframe.bezierTimeIn =
                OptionalFloat(keyframeSource, "bezier_time_in", 0.0F);
            keyframe.bezierTimeOut =
                OptionalFloat(keyframeSource, "bezier_time_out", 0.0F);
            animator.fillColorKeyframes.push_back(std::move(keyframe));
          }
        }
        layer.animators.push_back(std::move(animator));
      }
    }
    if (const auto effects = source.find("post_effects");
        effects != source.end()) {
      if (!effects->is_array())
        throw std::invalid_argument(context + ".post_effects must be an array");
      for (const auto &effectSource : *effects) {
        RequireOnlyKeys(effectSource,
                        {"id", "node_kind", "amount", "padding_px",
                         "amount_keyframes", "parameters"},
                        context + ".post_effect");
        TextPostEffectSpec effect;
        effect.effectId = RequiredString(effectSource, "id", context);
        effect.nodeKind = RequiredString(effectSource, "node_kind", context);
        effect.amount = OptionalFloat(effectSource, "amount", 0.0F);
        effect.paddingPx = OptionalFloat(effectSource, "padding_px", 0.0F);
        if (const auto keyframes = effectSource.find("amount_keyframes");
            keyframes != effectSource.end()) {
          if (!keyframes->is_array())
            throw std::invalid_argument(context +
                                        ".amount_keyframes must be an array");
          for (const auto &keyframeSource : *keyframes) {
            if (!keyframeSource.is_object())
              throw std::invalid_argument(context +
                                          ".amount_keyframe must be an object");
            effect.amountKeyframes.push_back(
                ParseScalarTextKeyframe<TextKeyframe>(keyframeSource));
          }
        }
        if (const auto parameters = effectSource.find("parameters");
            parameters != effectSource.end()) {
          if (!parameters->is_array())
            throw std::invalid_argument(context +
                                        ".parameters must be an array");
          for (const auto &parameterSource : *parameters) {
            if (!parameterSource.is_object())
              throw std::invalid_argument(context +
                                          ".parameter must be an object");
            TextPostEffectParameter parameter;
            parameter.name = RequiredString(parameterSource, "name", context);
            const auto &values =
                RequiredArray(parameterSource, "values", context);
            for (const auto &value : values) {
              if (!value.is_number())
                throw std::invalid_argument(context +
                                            ".parameter value must be numeric");
              parameter.values.push_back(value.get<float>());
            }
            if (const auto keyframes = parameterSource.find("keyframes");
                keyframes != parameterSource.end()) {
              if (!keyframes->is_array())
                throw std::invalid_argument(
                    context + ".parameter keyframes must be an array");
              for (const auto &keyframeSource : *keyframes) {
                if (!keyframeSource.is_object())
                  throw std::invalid_argument(
                      context + ".parameter keyframe must be an object");
                parameter.keyframes.push_back(
                    ParseScalarTextKeyframe<TextKeyframe>(keyframeSource));
              }
            }
            effect.parameters.push_back(std::move(parameter));
          }
        }
        layer.postEffects.push_back(std::move(effect));
      }
    }
    if (const auto showpieces = source.find("showpieces");
        showpieces != source.end()) {
      if (!showpieces->is_array())
        throw std::invalid_argument(context + ".showpieces must be an array");
      for (const auto &showpieceSource : *showpieces) {
        if (!showpieceSource.is_object())
          throw std::invalid_argument(context + ".showpiece must be an object");
        TextShowpieceSpec showpiece;
        showpiece.showpieceId = RequiredString(showpieceSource, "id", context);
        showpiece.assetId =
            RequiredString(showpieceSource, "asset_id", context);
        showpiece.anchor = ParseShowpieceAnchor(
            OptionalString(showpieceSource, "anchor", "bbox_center"));
        showpiece.fit = ParseShowpieceFit(
            OptionalString(showpieceSource, "fit", "inherit"));
        const auto extent =
            OptionalString(showpieceSource, "extent_space", "text_local");
        if (extent == "text_local")
          showpiece.extentSpace = TextShowpieceExtentSpace::TextLocal;
        else if (extent == "canvas_width")
          showpiece.extentSpace = TextShowpieceExtentSpace::CanvasWidth;
        else if (extent == "canvas_height")
          showpiece.extentSpace = TextShowpieceExtentSpace::CanvasHeight;
        else if (extent == "canvas_full")
          showpiece.extentSpace = TextShowpieceExtentSpace::CanvasFull;
        else
          throw std::invalid_argument(context +
                                      ".showpiece.extent_space is unsupported");
        const auto inherit = OptionalString(showpieceSource, "inherit", "full");
        if (inherit == "full")
          showpiece.inherit = TextShowpieceTransformInherit::Full;
        else if (inherit == "translate_only")
          showpiece.inherit = TextShowpieceTransformInherit::TranslateOnly;
        else
          throw std::invalid_argument(context +
                                      ".showpiece.inherit is unsupported");
        showpiece.playback = ParseAnimationPlayback(
            OptionalString(showpieceSource, "playback", "once"));
        showpiece.expandRatioX =
            OptionalFloat(showpieceSource, "expand_x", 1.0F);
        showpiece.expandRatioY =
            OptionalFloat(showpieceSource, "expand_y", 1.0F);
        constexpr float neutralPivot = 0.0F;
        showpiece.pivotX =
            OptionalFloat(showpieceSource, "pivot_x", neutralPivot);
        showpiece.pivotY =
            OptionalFloat(showpieceSource, "pivot_y", neutralPivot);
        showpiece.offsetX = OptionalFloat(showpieceSource, "offset_x", 0.0F);
        showpiece.offsetY = OptionalFloat(showpieceSource, "offset_y", 0.0F);
        showpiece.relativeOffsetX =
            OptionalFloat(showpieceSource, "relative_offset_x", 0.0F);
        showpiece.relativeOffsetY =
            OptionalFloat(showpieceSource, "relative_offset_y", 0.0F);
        showpiece.scaleX = OptionalFloat(showpieceSource, "scale_x", 1.0F);
        showpiece.scaleY = OptionalFloat(showpieceSource, "scale_y", 1.0F);
        showpiece.rotationXDegrees =
            OptionalFloat(showpieceSource, "rotation_x_degrees", 0.0F);
        showpiece.rotationYDegrees =
            OptionalFloat(showpieceSource, "rotation_y_degrees", 0.0F);
        showpiece.rotationDegrees =
            OptionalFloat(showpieceSource, "rotation_degrees", 0.0F);
        showpiece.opacity = OptionalFloat(showpieceSource, "opacity", 1.0F);
        if (const auto keyframes = showpieceSource.find("anchor_keyframes");
            keyframes != showpieceSource.end()) {
          if (!keyframes->is_array())
            throw std::invalid_argument(
                context + ".showpiece.anchor_keyframes must be an array");
          for (const auto &keyframe : *keyframes) {
            if (!keyframe.is_object())
              throw std::invalid_argument(
                  context + ".showpiece.anchor_keyframe must be an object");
            showpiece.anchorKeyframes.push_back(
                {OptionalFloat(keyframe, "offset", 0.0F),
                 ParseShowpieceAnchor(
                     OptionalString(keyframe, "value", "bbox_center"))});
          }
        }
        if (const auto keyframes = showpieceSource.find("fit_keyframes");
            keyframes != showpieceSource.end()) {
          if (!keyframes->is_array())
            throw std::invalid_argument(
                context + ".showpiece.fit_keyframes must be an array");
          for (const auto &keyframe : *keyframes) {
            if (!keyframe.is_object())
              throw std::invalid_argument(
                  context + ".showpiece.fit_keyframe must be an object");
            showpiece.fitKeyframes.push_back(
                {OptionalFloat(keyframe, "offset", 0.0F),
                 ParseShowpieceFit(
                     OptionalString(keyframe, "value", "inherit"))});
          }
        }
        ParseTextKeyframeArray(showpieceSource, "pivot_x_keyframes", context,
                               showpiece.pivotXKeyframes);
        ParseTextKeyframeArray(showpieceSource, "pivot_y_keyframes", context,
                               showpiece.pivotYKeyframes);
        ParseTextKeyframeArray(showpieceSource, "offset_x_keyframes", context,
                               showpiece.offsetXKeyframes);
        ParseTextKeyframeArray(showpieceSource, "offset_y_keyframes", context,
                               showpiece.offsetYKeyframes);
        ParseTextKeyframeArray(showpieceSource, "relative_offset_x_keyframes",
                               context, showpiece.relativeOffsetXKeyframes);
        ParseTextKeyframeArray(showpieceSource, "relative_offset_y_keyframes",
                               context, showpiece.relativeOffsetYKeyframes);
        ParseTextKeyframeArray(showpieceSource, "scale_x_keyframes", context,
                               showpiece.scaleXKeyframes);
        ParseTextKeyframeArray(showpieceSource, "scale_y_keyframes", context,
                               showpiece.scaleYKeyframes);
        ParseTextKeyframeArray(showpieceSource, "rotation_x_keyframes", context,
                               showpiece.rotationXKeyframes);
        ParseTextKeyframeArray(showpieceSource, "rotation_y_keyframes", context,
                               showpiece.rotationYKeyframes);
        ParseTextKeyframeArray(showpieceSource, "rotation_keyframes", context,
                               showpiece.rotationKeyframes);
        ParseTextKeyframeArray(showpieceSource, "opacity_keyframes", context,
                               showpiece.opacityKeyframes);
        ParseTextKeyframeArray(showpieceSource, "asset_progress_keyframes",
                               context, showpiece.assetProgressKeyframes);
        layer.showpieces.push_back(std::move(showpiece));
      }
    }
    stack.layers.push_back(std::move(layer));
  }
}

void ParseAnimations(const Json &animation, RichTextDocument &document,
                     TextAnimationStack &stack) {
  ParseTimedSpans(animation, document);
  ParseAnimationLayers(animation, stack);
  std::string error;
  if (!ValidateTextAnimationStack(stack, !document.timedSpans.empty(), &error))
    throw std::invalid_argument(error);
}

} // namespace

TextTemplateParseResult ParseTextTemplateJson(const std::string &json,
                                              const RichTextLimits &limits) {
  TextTemplateParseResult result;
  if (json.size() > 8U * 1024U * 1024U) {
    Add(result.diagnostics, "text.template.byte_limit", "parse", {},
        "text template exceeds the 8 MiB parser limit");
    return result;
  }
  try {
    const Json root = Json::parse(json);
    if (!root.is_object())
      throw std::invalid_argument("template root must be an object");
    RequireOnlyKeys(root,
                    {"format", "id", "revision", "renderer_profile", "display",
                     "placement", "bindings", "document", "appearance",
                     "animation"},
                    "template");
    if (RequiredString(root, "format", "template") != "videocut.text-template")
      throw std::invalid_argument("template format is unsupported");
    result.value.id = RequiredString(root, "id", "template");
    const auto revision = RequiredInteger(root, "revision", "template");
    if (revision <= 0 || revision > std::numeric_limits<std::uint32_t>::max())
      throw std::invalid_argument("template revision is invalid");
    result.value.revision = static_cast<std::uint32_t>(revision);
    result.value.rendererProfile =
        RequiredString(root, "renderer_profile", "template");
    result.value.appearance = ParseAppearance(root);

    const auto &display = RequiredObject(root, "display", "template");
    result.value.fallbackName =
        RequiredString(display, "fallback_name", "template.display");
    result.value.category =
        RequiredString(display, "category", "template.display");
    if (const auto tags = display.find("tags"); tags != display.end()) {
      if (!tags->is_array())
        throw std::invalid_argument("template.display.tags must be an array");
      for (const auto &tag : *tags) {
        if (!tag.is_string() || tag.get<std::string>().empty())
          throw std::invalid_argument(
              "template tag must be a non-empty string");
        result.value.tags.push_back(tag.get<std::string>());
      }
    }

    const auto &placement = RequiredObject(root, "placement", "template");
    result.value.defaultDurationTicks = RequiredInteger(
        placement, "default_duration_ticks", "template.placement");
    if (result.value.defaultDurationTicks <= 0 ||
        result.value.defaultDurationTicks > 10'368'000'000)
      throw std::invalid_argument("template default duration is invalid");
    result.value.document.referenceCanvas.width =
        OptionalFloat(placement, "reference_width", 1920.0F);
    result.value.document.referenceCanvas.height =
        OptionalFloat(placement, "reference_height", 1080.0F);

    const auto &bindings = RequiredArray(root, "bindings", "template");
    std::unordered_set<std::string> bindingIds;
    for (const auto &binding : bindings) {
      if (!binding.is_object())
        throw std::invalid_argument("template binding must be an object");
      TextTemplateBinding decoded;
      decoded.id = RequiredString(binding, "id", "template.binding");
      if (!bindingIds.insert(decoded.id).second)
        throw std::invalid_argument("template binding IDs must be unique");
      if (RequiredString(binding, "kind", "template.binding") != "plain_text")
        throw std::invalid_argument(
            "only plain_text template bindings are supported");
      decoded.defaultText =
          RequiredString(binding, "default", "template.binding");
      const auto &target =
          RequiredObject(binding, "target", "template.binding");
      decoded.target.paragraphId =
          RequiredString(target, "paragraph_id", "template.binding.target");
      decoded.target.runId =
          RequiredString(target, "run_id", "template.binding.target");
      result.value.bindings.push_back(std::move(decoded));
    }
    const auto content =
        std::find_if(result.value.bindings.begin(), result.value.bindings.end(),
                     [](const TextTemplateBinding &binding) {
                       return binding.id == "content";
                     });
    if (content == result.value.bindings.end() ||
        content->defaultText != "Text")
      throw std::invalid_argument(
          "template content binding default must be exactly Text");

    const auto &document = RequiredObject(root, "document", "template");
    result.value.document.version = 1;
    result.value.document.documentId = result.value.id + ".prototype";
    result.value.document.rendererProfile = result.value.rendererProfile;
    result.value.document.originTemplateId = result.value.id;
    result.value.document.originTemplateRevision = result.value.revision;
    const auto &box =
        RequiredObject(document, "layout_box", "template.document");
    result.value.document.layoutBox.x = OptionalFloat(box, "x", 0.0F);
    result.value.document.layoutBox.y = OptionalFloat(box, "y", 0.0F);
    result.value.document.layoutBox.width = OptionalFloat(
        box, "width", result.value.document.referenceCanvas.width);
    result.value.document.layoutBox.height = OptionalFloat(
        box, "height", result.value.document.referenceCanvas.height);
    result.value.document.layoutBox.sizingMode = ParseLayoutSizingMode(
        OptionalString(box, "sizing_mode", "auto_height"));
    result.value.document.layoutBox.verticalAlignment = ParseVerticalAlignment(
        OptionalString(box, "vertical_alignment", "center"));
    result.value.document.layoutBox.clipOverflow =
        OptionalBool(box, "clip_overflow", true);
    if (const auto padding = box.find("padding"); padding != box.end()) {
      if (!padding->is_object())
        throw std::invalid_argument("layout_box.padding must be an object");
      result.value.document.layoutBox.padding = {
          OptionalFloat(*padding, "left", 0.0F),
          OptionalFloat(*padding, "top", 0.0F),
          OptionalFloat(*padding, "right", 0.0F),
          OptionalFloat(*padding, "bottom", 0.0F),
      };
    }

    const auto &paragraphs =
        RequiredArray(document, "paragraphs", "template.document");
    std::unordered_map<std::string, std::string> runBinding;
    for (const auto &paragraph : paragraphs) {
      if (!paragraph.is_object())
        throw std::invalid_argument("template paragraph must be an object");
      RichTextParagraph decoded;
      decoded.paragraphId =
          RequiredString(paragraph, "id", "template.paragraph");
      const auto &paragraphStyle =
          RequiredObject(paragraph, "style", "template.paragraph");
      decoded.style.alignment =
          ParseAlignment(OptionalString(paragraphStyle, "alignment", "center"));
      decoded.style.direction =
          ParseDirection(OptionalString(paragraphStyle, "direction", "auto"));
      decoded.style.locale = OptionalString(paragraphStyle, "locale", "und");
      decoded.style.lineHeight =
          OptionalFloat(paragraphStyle, "line_height", 1.0F);
      decoded.style.overflow =
          ParseOverflow(OptionalString(paragraphStyle, "overflow", "clip"));
      decoded.style.wrap =
          ParseWrap(OptionalString(paragraphStyle, "wrap", "word"));
      if (const auto background = paragraphStyle.find("background");
          background != paragraphStyle.end()) {
        decoded.style.background = ParseBoxBackground(
            *background, "template.paragraph.style.background");
      }
      if (const auto maximumLines = paragraphStyle.find("max_lines");
          maximumLines != paragraphStyle.end()) {
        if (!maximumLines->is_number_unsigned() &&
            !maximumLines->is_number_integer())
          throw std::invalid_argument("paragraph max_lines must be an integer");
        const auto value = maximumLines->get<std::int64_t>();
        if (value <= 0 || value > std::numeric_limits<std::uint32_t>::max())
          throw std::invalid_argument("paragraph max_lines is invalid");
        decoded.style.maximumLines = static_cast<std::uint32_t>(value);
      }
      const auto &runs = RequiredArray(paragraph, "runs", "template.paragraph");
      for (const auto &run : runs) {
        if (!run.is_object())
          throw std::invalid_argument("template run must be an object");
        RichTextRun decodedRun;
        decodedRun.runId = RequiredString(run, "id", "template.run");
        decodedRun.locale = OptionalString(run, "locale", decoded.style.locale);
        const auto bindingId = OptionalString(run, "text_binding");
        const auto literal = OptionalString(run, "text");
        if (bindingId.empty() == literal.empty())
          throw std::invalid_argument(
              "template run requires exactly one of text or text_binding");
        if (!bindingId.empty()) {
          if (bindingIds.find(bindingId) == bindingIds.end())
            throw std::invalid_argument(
                "template run references an unknown binding");
          runBinding.emplace(decodedRun.runId, bindingId);
        } else
          decodedRun.utf8Text = literal;
        decodedRun.style = ParseTextStyle(
            RequiredObject(run, "style", "template.run"), "template.run.style");
        decoded.runs.push_back(std::move(decodedRun));
      }
      result.value.document.paragraphs.push_back(std::move(decoded));
    }

    for (const auto &binding : result.value.bindings) {
      const auto paragraph =
          std::find_if(result.value.document.paragraphs.begin(),
                       result.value.document.paragraphs.end(),
                       [&](const RichTextParagraph &value) {
                         return value.paragraphId == binding.target.paragraphId;
                       });
      if (paragraph == result.value.document.paragraphs.end())
        throw std::invalid_argument(
            "template binding targets a missing paragraph");
      const auto run =
          std::find_if(paragraph->runs.begin(), paragraph->runs.end(),
                       [&](const RichTextRun &value) {
                         return value.runId == binding.target.runId;
                       });
      if (run == paragraph->runs.end())
        throw std::invalid_argument("template binding targets a missing run");
      const auto linked = runBinding.find(run->runId);
      if (linked == runBinding.end() || linked->second != binding.id)
        throw std::invalid_argument(
            "template binding target and run binding disagree");
    }
    if (const auto animation = root.find("animation");
        animation != root.end()) {
      if (!animation->is_object())
        throw std::invalid_argument("template.animation must be an object");
      ParseAnimations(*animation, result.value.document,
                      result.value.animationStack);
    }

    const auto instantiated = InstantiateTextTemplate(result.value);
    result.diagnostics.insert(result.diagnostics.end(),
                              instantiated.diagnostics.begin(),
                              instantiated.diagnostics.end());
    if (!instantiated.valid)
      return result;
    const auto validation =
        ValidateRichTextDocument(instantiated.document, limits);
    result.diagnostics.insert(result.diagnostics.end(),
                              validation.diagnostics.begin(),
                              validation.diagnostics.end());
    result.valid = validation.valid;
  } catch (const Json::exception &exception) {
    Add(result.diagnostics, "text.template.json_invalid", "parse", {},
        exception.what());
  } catch (const std::exception &exception) {
    Add(result.diagnostics, "text.template.schema_invalid", "parse", {},
        exception.what());
  }
  return result;
}

TextTemplateInstantiateResult
InstantiateTextTemplate(const TextTemplate &textTemplate,
                        const std::map<std::string, std::string> &overrides) {
  TextTemplateInstantiateResult result;
  result.document = textTemplate.document;
  result.animationStack = textTemplate.animationStack;
  result.document.documentId = textTemplate.id + ".instance";
  result.document.originTemplateId = textTemplate.id;
  result.document.originTemplateRevision = textTemplate.revision;
  std::unordered_set<std::string> consumedOverrides;
  for (const auto &binding : textTemplate.bindings) {
    const auto replacement = overrides.find(binding.id);
    const std::string &value = replacement == overrides.end()
                                   ? binding.defaultText
                                   : replacement->second;
    if (!IsValidUtf8(value)) {
      Add(result.diagnostics, "text.template.binding_utf8_invalid",
          "instantiate", binding.id,
          "text binding override is not valid UTF-8");
      continue;
    }
    if (replacement != overrides.end())
      consumedOverrides.insert(replacement->first);
    auto paragraph = std::find_if(
        result.document.paragraphs.begin(), result.document.paragraphs.end(),
        [&](const RichTextParagraph &candidate) {
          return candidate.paragraphId == binding.target.paragraphId;
        });
    if (paragraph == result.document.paragraphs.end()) {
      Add(result.diagnostics, "text.template.target_missing", "instantiate",
          binding.id, "binding paragraph target is missing");
      continue;
    }
    auto run = std::find_if(paragraph->runs.begin(), paragraph->runs.end(),
                            [&](const RichTextRun &candidate) {
                              return candidate.runId == binding.target.runId;
                            });
    if (run == paragraph->runs.end()) {
      Add(result.diagnostics, "text.template.target_missing", "instantiate",
          binding.id, "binding run target is missing");
      continue;
    }
    const auto previousSize = run->utf8Text.size();
    run->utf8Text = value;
    for (auto &span : result.document.timedSpans) {
      if (span.paragraphId == paragraph->paragraphId &&
          span.runId == run->runId && span.utf8Begin == 0U &&
          span.utf8End == previousSize) {
        span.utf8End = run->utf8Text.size();
      }
    }
  }
  for (const auto &overrideValue : overrides) {
    if (consumedOverrides.find(overrideValue.first) ==
        consumedOverrides.end()) {
      Add(result.diagnostics, "text.template.binding_unknown", "instantiate",
          overrideValue.first, "unknown text binding override");
    }
  }
  const auto validation = ValidateRichTextDocument(result.document);
  result.diagnostics.insert(result.diagnostics.end(),
                            validation.diagnostics.begin(),
                            validation.diagnostics.end());
  std::string animationError;
  const bool animationValid = ValidateTextAnimationStack(
      result.animationStack, !result.document.timedSpans.empty(),
      &animationError);
  if (!animationValid) {
    Add(result.diagnostics, "text.template.animation_invalid", "instantiate",
        textTemplate.id, animationError);
  }
  result.valid =
      validation.valid && animationValid &&
      std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                   [](const Diagnostic &diagnostic) {
                     return diagnostic.severity == DiagnosticSeverity::Error;
                   });
  return result;
}

} // namespace videocut::text
