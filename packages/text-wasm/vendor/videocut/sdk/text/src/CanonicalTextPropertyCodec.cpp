#include "videocut/text/internal/CanonicalByteCodec.h"
#include "videocut/text/internal/CanonicalTextPropertyCodec.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace videocut::text {
namespace {

constexpr std::string_view kCanonicalPropertyDomain{
    "videocut.text-property.current.v1"};
constexpr std::size_t kMaximumCanonicalPropertyBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumCanonicalStringBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumCanonicalCollectionElements = 65'536U;
constexpr std::size_t kMaximumMaterialLayers = 128U;
constexpr std::size_t kMaximumBackdropLayers = 128U;
constexpr std::size_t kMaximumPathCommands = 65'536U;

struct CodecContext final {
  const RichTextLimits &limits;

  [[nodiscard]] std::size_t maximumStringBytes() const noexcept {
    return std::min(kMaximumCanonicalStringBytes,
                    std::max<std::size_t>(limits.maximumUtf8Bytes, 512U));
  }
};

using CanonicalWriter = videocut::text::internal::CanonicalByteWriter;
using CanonicalReader = videocut::text::internal::CanonicalByteReader;

using internal::TransferEnum;
using internal::TransferOptional;

template <typename Archive, typename Value, typename Transfer>
bool TransferVector(Archive &archive, std::vector<Value> &values,
                    const std::size_t maximum, Transfer &&transfer) {
  return internal::TransferBoundedVector(
      archive, values, std::min(maximum, kMaximumCanonicalCollectionElements),
      std::forward<Transfer>(transfer));
}

template <typename Archive>
bool TransferColor(Archive &archive, Color &value) {
  if (!archive.Float(value.red) || !archive.Float(value.green) ||
      !archive.Float(value.blue) || !archive.Float(value.alpha))
    return false;
  const auto inRange = [](const float component) {
    return component >= 0.0F && component <= 1.0F;
  };
  return inRange(value.red) && inRange(value.green) && inRange(value.blue) &&
         inRange(value.alpha);
}

template <typename Archive>
bool TransferInsets(Archive &archive, Insets &value) {
  return archive.Float(value.left) && archive.Float(value.top) &&
         archive.Float(value.right) && archive.Float(value.bottom);
}

template <typename Archive>
bool TransferFontAxis(Archive &archive, FontAxis &value) {
  return archive.String(value.tag) && !value.tag.empty() &&
         value.tag.size() <= 16U && archive.Float(value.value);
}

template <typename Archive>
bool TransferFontFeature(Archive &archive, FontFeature &value) {
  std::uint64_t featureValue = value.value;
  if (!archive.String(value.tag) || value.tag.empty() || value.tag.size() > 16U ||
      !archive.Unsigned(featureValue) ||
      featureValue > std::numeric_limits<std::uint32_t>::max())
    return false;
  if constexpr (Archive::kReading)
    value.value = static_cast<std::uint32_t>(featureValue);
  return true;
}

template <typename Archive>
bool TransferFontReference(Archive &archive, FontReference &value,
                           const CodecContext &context) {
  std::int64_t weight = value.weight;
  std::int64_t width = value.width;
  std::uint64_t faceIndex = value.faceIndex;
  if (!TransferEnum(archive, value.kind, FontSourceKind::System) ||
      !archive.String(value.family) || !archive.String(value.postscriptName) ||
      !archive.Signed(weight) || !archive.Signed(width) ||
      !TransferEnum(archive, value.slant, FontSlant::Oblique) ||
      !archive.Unsigned(faceIndex) || weight < 1 || weight > 1000 || width < 1 ||
      width > 9 || faceIndex > std::numeric_limits<std::uint32_t>::max() ||
      !TransferVector(archive, value.variationAxes,
                      context.limits.maximumFontFeaturesPerRun,
                      [&](auto &axis) { return TransferFontAxis(archive, axis); }) ||
      !archive.String(value.assetId) || !archive.String(value.platform) ||
      !archive.String(value.digest) ||
      !archive.String(value.faceFingerprint) ||
      !archive.Boolean(value.allowSystemGlyphFallback))
    return false;
  if constexpr (Archive::kReading) {
    value.weight = static_cast<std::int32_t>(weight);
    value.width = static_cast<std::int32_t>(width);
    value.faceIndex = static_cast<std::uint32_t>(faceIndex);
  }
  return true;
}

template <typename Archive>
bool TransferFontSpec(Archive &archive, FontSpec &value,
                      const CodecContext &context) {
  std::int64_t weight = value.weight;
  std::int64_t width = value.width;
  std::uint64_t faceIndex = value.faceIndex;
  if (!TransferFontReference(archive, value.primary, context) ||
      !TransferVector(archive, value.fallbacks,
                      context.limits.maximumFallbackFontsPerRun,
                      [&](auto &font) {
                        return TransferFontReference(archive, font, context);
                      }) ||
      !archive.String(value.family) || !archive.String(value.postscriptName) ||
      !archive.Signed(weight) || !archive.Signed(width) ||
      !TransferEnum(archive, value.slant, FontSlant::Oblique) ||
      !archive.Unsigned(faceIndex) || weight < 1 || weight > 1000 || width < 1 ||
      width > 9 || faceIndex > std::numeric_limits<std::uint32_t>::max() ||
      !TransferVector(archive, value.variationAxes,
                      context.limits.maximumFontFeaturesPerRun,
                      [&](auto &axis) { return TransferFontAxis(archive, axis); }) ||
      !TransferVector(archive, value.features,
                      context.limits.maximumFontFeaturesPerRun,
                      [&](auto &feature) {
                        return TransferFontFeature(archive, feature);
                      }) ||
      !archive.Boolean(value.allowSystemGlyphFallback))
    return false;
  if constexpr (Archive::kReading) {
    value.weight = static_cast<std::int32_t>(weight);
    value.width = static_cast<std::int32_t>(width);
    value.faceIndex = static_cast<std::uint32_t>(faceIndex);
  }
  return true;
}

template <typename Archive>
bool TransferGradientStop(Archive &archive, GradientStop &value) {
  return archive.Float(value.offset) && value.offset >= 0.0F &&
         value.offset <= 1.0F && TransferColor(archive, value.color);
}

template <typename Archive>
bool TransferTextureReference(Archive &archive, TextureReference &value) {
  return TransferEnum(archive, value.sourceKind,
                      TextureSourceKind::ProjectManaged) &&
         archive.String(value.assetId) && archive.String(value.digest) &&
         archive.String(value.mediaType) && archive.String(value.colorSpace) &&
         TransferEnum(archive, value.orientation, TextureOrientation::Left);
}

template <typename Archive>
bool TransferMaterialCoordinates(Archive &archive,
                                 TextMaterialCoordinates &value) {
  return TransferEnum(archive, value.coordinateSpace,
                      PaintCoordinateSpace::Grapheme) &&
         archive.Float(value.coordinateOutset) &&
         archive.Float(value.coordinateScale) && value.coordinateScale > 0.0F;
}

template <typename Archive>
bool TransferGradientStops(Archive &archive, std::vector<GradientStop> &stops,
                           const CodecContext &context) {
  if (!TransferVector(archive, stops, context.limits.maximumGradientStops,
                      [&](auto &stop) {
                        return TransferGradientStop(archive, stop);
                      }))
    return false;
  return std::is_sorted(stops.begin(), stops.end(),
                        [](const auto &left, const auto &right) {
                          return left.offset < right.offset;
                        });
}

template <typename Archive>
bool TransferMaterial(Archive &archive, TextMaterial &value,
                      const CodecContext &context) {
  std::uint64_t tag = value.index();
  if (!archive.Unsigned(tag) || tag > 3U)
    return false;
  if constexpr (Archive::kReading) {
    switch (tag) {
    case 0U: value = SolidTextMaterial{}; break;
    case 1U: value = LinearGradientTextMaterial{}; break;
    case 2U: value = RadialGradientTextMaterial{}; break;
    case 3U: value = TextureTextMaterial{}; break;
    default: return false;
    }
  }
  return std::visit(
      [&](auto &material) {
        using Material = std::decay_t<decltype(material)>;
        if constexpr (std::is_same_v<Material, SolidTextMaterial>) {
          return TransferColor(archive, material.color);
        } else if constexpr (std::is_same_v<Material,
                                             LinearGradientTextMaterial>) {
          return TransferGradientStops(archive, material.stops, context) &&
                 archive.Float(material.startX) &&
                 archive.Float(material.startY) &&
                 archive.Float(material.endX) && archive.Float(material.endY) &&
                 TransferEnum(archive, material.spread, PaintSpread::Mirror) &&
                 TransferEnum(archive, material.sampling,
                              GradientSampling::Rgba8Lut256) &&
                 TransferMaterialCoordinates(archive, material.coordinates);
        } else if constexpr (std::is_same_v<Material,
                                             RadialGradientTextMaterial>) {
          return TransferGradientStops(archive, material.stops, context) &&
                 archive.Float(material.centerX) &&
                 archive.Float(material.centerY) &&
                 archive.Float(material.radius) && material.radius > 0.0F &&
                 TransferEnum(archive, material.spread, PaintSpread::Mirror) &&
                 TransferEnum(archive, material.sampling,
                              GradientSampling::Rgba8Lut256) &&
                 TransferMaterialCoordinates(archive, material.coordinates);
        } else {
          std::uint64_t columns = material.atlasColumns;
          std::uint64_t rows = material.atlasRows;
          if (!TransferTextureReference(archive, material.texture) ||
              !TransferEnum(archive, material.fit, TextureFit::Tile) ||
              !TransferEnum(archive, material.mapping,
                            TextureMapping::GlyphDistanceField) ||
              !TransferMaterialCoordinates(archive, material.coordinates) ||
              !archive.Float(material.scale) || material.scale <= 0.0F ||
              !archive.Float(material.rotationDegrees) ||
              !archive.Float(material.offsetX) || !archive.Float(material.offsetY) ||
              !archive.Boolean(material.flipX) ||
              !archive.Boolean(material.flipY) || !archive.Unsigned(columns) ||
              !archive.Unsigned(rows) || columns == 0U || rows == 0U ||
              columns > std::numeric_limits<std::uint16_t>::max() ||
              rows > std::numeric_limits<std::uint16_t>::max() ||
              !archive.Float(material.textureOpacity) ||
              material.textureOpacity < 0.0F ||
              material.textureOpacity > 1.0F ||
              !archive.Float(material.opacity) || material.opacity < 0.0F ||
              material.opacity > 1.0F ||
              !archive.Boolean(material.sourceAlpha) ||
              !TransferOptional(archive, material.underlayColor,
                                [&](auto &color) {
                                  return TransferColor(archive, color);
                                }) ||
              !TransferGradientStops(archive, material.underlayGradient,
                                     context) ||
              !archive.Float(material.underlayGradientProjection.startX) ||
              !archive.Float(material.underlayGradientProjection.startY) ||
              !archive.Float(material.underlayGradientProjection.endX) ||
              !archive.Float(material.underlayGradientProjection.endY) ||
              !TransferEnum(archive,
                            material.underlayGradientProjection.spread,
                            PaintSpread::Mirror) ||
              !TransferEnum(archive,
                            material.underlayGradientProjection.sampling,
                            GradientSampling::Rgba8Lut256))
            return false;
          if constexpr (Archive::kReading) {
            material.atlasColumns = static_cast<std::uint16_t>(columns);
            material.atlasRows = static_cast<std::uint16_t>(rows);
          }
          return true;
        }
      },
      value);
}

template <typename Archive>
bool TransferMaterialBinding(Archive &archive, TextMaterialBinding &value,
                             const CodecContext &context) {
  std::uint64_t tag = value.index();
  if (!archive.Unsigned(tag) || tag > 1U)
    return false;
  if constexpr (Archive::kReading)
    value = tag == 0U ? TextMaterialBinding{LiteralTextMaterial{}}
                      : TextMaterialBinding{EditableTextStyleSlot{}};
  return std::visit(
      [&](auto &binding) {
        using Binding = std::decay_t<decltype(binding)>;
        if constexpr (std::is_same_v<Binding, LiteralTextMaterial>) {
          return TransferMaterial(archive, binding.material, context);
        } else {
          return archive.String(binding.semanticRole) &&
                 !binding.semanticRole.empty() &&
                 TransferMaterial(archive, binding.fallback, context) &&
                 TransferOptional(archive, binding.replacementMask,
                                  [&](auto &mask) {
                                    return TransferTextureReference(archive,
                                                                    mask);
                                  });
        }
      },
      value);
}

template <typename Archive>
bool TransferNormalizedPolarOffset(
    Archive &archive, std::optional<TextNormalizedPolarOffset> &value) {
  bool present = value.has_value();
  if (!archive.Boolean(present))
    return false;
  if constexpr (Archive::kReading) {
    if (!present) {
      value.reset();
      return true;
    }
    value = TextNormalizedPolarOffset{};
  } else if (!present) {
    return true;
  }
  return archive.Float(value->radius) && value->radius >= 0.0F &&
         archive.Float(value->angleRadians);
}

template <typename Archive>
bool TransferFillLayer(Archive &archive, TextFillLayer &value,
                       const CodecContext &context) {
  std::int64_t zOrder = value.zOrder;
  if (!archive.String(value.layerId) || !archive.Signed(zOrder) ||
      zOrder < std::numeric_limits<std::int32_t>::min() ||
      zOrder > std::numeric_limits<std::int32_t>::max() ||
      !TransferEnum(archive, value.blend, TextBlendMode::Lighten) ||
      !TransferMaterialBinding(archive, value.material, context) ||
      !archive.Float(value.offsetX) || !archive.Float(value.offsetY) ||
      !TransferNormalizedPolarOffset(archive, value.normalizedPolarOffset))
    return false;
  if constexpr (Archive::kReading) {
    value.zOrder = static_cast<std::int32_t>(zOrder);
  }
  return true;
}

template <typename Archive>
bool TransferNormalizedUvOffset(
    Archive &archive, std::optional<TextNormalizedUvOffset> &value) {
  bool present = value.has_value();
  if (!archive.Boolean(present))
    return false;
  if constexpr (Archive::kReading) {
    if (!present) {
      value.reset();
      return true;
    }
    value = TextNormalizedUvOffset{};
  } else if (!present) {
    return true;
  }
  return archive.Float(value->x) && archive.Float(value->y);
}

template <typename Archive>
bool TransferStrokeLayer(Archive &archive, TextStrokeLayer &value,
                         const CodecContext &context) {
  std::int64_t zOrder = value.zOrder;
  if (!archive.String(value.layerId) || !archive.Signed(zOrder) ||
      zOrder < std::numeric_limits<std::int32_t>::min() ||
      zOrder > std::numeric_limits<std::int32_t>::max() ||
      !TransferEnum(archive, value.blend, TextBlendMode::Lighten) ||
      !TransferMaterialBinding(archive, value.material, context) ||
      !archive.Float(value.width) || value.width < 0.0F ||
      !archive.Float(value.innerRingWidth) || value.innerRingWidth < 0.0F ||
      !TransferOptional(archive, value.signedStartWidth, [&](auto &start) {
         return archive.Float(start) && std::fabs(start) <= 4096.0F;
       }) ||
      !archive.Float(value.offsetX) || !archive.Float(value.offsetY) ||
      !archive.Float(value.blurRadius) || value.blurRadius < 0.0F ||
      !archive.Float(value.spread) ||
      !TransferNormalizedPolarOffset(archive, value.normalizedPolarOffset))
    return false;
  if constexpr (Archive::kReading)
    value.zOrder = static_cast<std::int32_t>(zOrder);
  return true;
}

template <typename Archive>
bool TransferShadowLayer(Archive &archive, TextShadowLayer &value,
                         const CodecContext &context) {
  std::int64_t zOrder = value.zOrder;
  if (!archive.String(value.layerId) || !archive.Signed(zOrder) ||
      zOrder < std::numeric_limits<std::int32_t>::min() ||
      zOrder > std::numeric_limits<std::int32_t>::max() ||
      !TransferEnum(archive, value.blend, TextBlendMode::Lighten) ||
      !TransferMaterialBinding(archive, value.material, context) ||
      !TransferEnum(archive, value.kind, TextShadowKind::Inner) ||
      !archive.Float(value.offsetX) || !archive.Float(value.offsetY) ||
      !archive.Float(value.blurRadius) || value.blurRadius < 0.0F ||
      !archive.Float(value.spread) ||
      !archive.Float(value.thicknessAngleDegrees) ||
      !archive.Float(value.thicknessDistance) || value.thicknessDistance < 0.0F ||
      !TransferEnum(archive, value.smoothing,
                    TextOuterShadowSmoothingMode::DiffuseRoundMask) ||
      !archive.Float(value.roundMaskIntensity) ||
      value.roundMaskIntensity < 0.0F ||
      !archive.Float(value.sdfBlurScale) || value.sdfBlurScale < 0.0F ||
      value.sdfBlurScale > 16.0F ||
      !TransferNormalizedPolarOffset(archive, value.normalizedPolarOffset) ||
      !TransferNormalizedUvOffset(archive, value.normalizedUvOffset) ||
      !TransferVector(archive, value.strokes,
                       context.limits.maximumStrokesPerRun,
                       [&](TextStrokeLayer &stroke) {
                         return TransferStrokeLayer(archive, stroke, context);
                       }))
    return false;
  if constexpr (Archive::kReading)
    value.zOrder = static_cast<std::int32_t>(zOrder);
  return true;
}

template <typename Archive>
bool TransferGlowLayer(Archive &archive, TextGlowLayer &value,
                       const CodecContext &context) {
  std::int64_t zOrder = value.zOrder;
  if (!archive.String(value.layerId) || !archive.Signed(zOrder) ||
      zOrder < std::numeric_limits<std::int32_t>::min() ||
      zOrder > std::numeric_limits<std::int32_t>::max() ||
      !TransferEnum(archive, value.blend, TextBlendMode::Lighten) ||
      !TransferMaterialBinding(archive, value.material, context) ||
      !archive.Float(value.radius) || value.radius < 0.0F ||
      !archive.Float(value.spread) || !archive.Float(value.directionX) ||
      !archive.Float(value.directionY) ||
      !TransferNormalizedPolarOffset(archive, value.normalizedPolarOffset))
    return false;
  if constexpr (Archive::kReading)
    value.zOrder = static_cast<std::int32_t>(zOrder);
  return true;
}

template <typename Archive>
bool TransferGlyphLayer(Archive &archive, TextGlyphMaterialLayer &value,
                        const CodecContext &context) {
  std::uint64_t tag = value.index();
  if (!archive.Unsigned(tag) || tag > 3U)
    return false;
  if constexpr (Archive::kReading) {
    switch (tag) {
    case 0U: value = TextFillLayer{}; break;
    case 1U: value = TextStrokeLayer{}; break;
    case 2U: value = TextShadowLayer{}; break;
    case 3U: value = TextGlowLayer{}; break;
    default: return false;
    }
  }
  return std::visit(
      [&](auto &layer) {
        using Layer = std::decay_t<decltype(layer)>;
        if constexpr (std::is_same_v<Layer, TextFillLayer>)
          return TransferFillLayer(archive, layer, context);
        else if constexpr (std::is_same_v<Layer, TextStrokeLayer>)
          return TransferStrokeLayer(archive, layer, context);
        else if constexpr (std::is_same_v<Layer, TextShadowLayer>)
          return TransferShadowLayer(archive, layer, context);
        else
          return TransferGlowLayer(archive, layer, context);
      },
      value);
}

template <typename Archive>
bool TransferGlyphStack(Archive &archive, TextGlyphMaterialStack &value,
                        const CodecContext &context) {
  return TransferVector(archive, value.layers, kMaximumMaterialLayers,
                        [&](auto &layer) {
                          return TransferGlyphLayer(archive, layer, context);
                        });
}

template <typename Archive>
bool TransferDecorationLine(Archive &archive, TextDecorationLine &value,
                            const CodecContext &context) {
  return archive.Boolean(value.enabled) &&
         TransferMaterialBinding(archive, value.material, context) &&
         archive.Float(value.thickness) && value.thickness >= 0.0F &&
         archive.Float(value.offset) &&
         TransferEnum(archive, value.style, TextDecorationLineStyle::Wavy) &&
         archive.Boolean(value.skipInk);
}

template <typename Archive>
bool TransferInlineDecoration(Archive &archive, InlineTextDecoration &value,
                              const CodecContext &context) {
  return TransferDecorationLine(archive, value.underline, context) &&
         TransferDecorationLine(archive, value.strikeThrough, context);
}

template <typename Archive>
bool TransferBoxBackground(Archive &archive, TextBoxBackground &value) {
  return archive.Boolean(value.enabled) && TransferColor(archive, value.color) &&
         TransferInsets(archive, value.padding) &&
         archive.Float(value.cornerRadius) && value.cornerRadius >= 0.0F;
}

template <typename Archive>
bool TransferTabStop(Archive &archive, TextTabStop &value) {
  std::uint64_t character = static_cast<std::uint32_t>(value.decimalCharacter);
  if (!archive.Float(value.position) || value.position < 0.0F ||
      !TransferEnum(archive, value.alignment, TextAlignment::Justify) ||
      !archive.Unsigned(character) || character > 0x10ffffU ||
      (character >= 0xd800U && character <= 0xdfffU))
    return false;
  if constexpr (Archive::kReading)
    value.decimalCharacter = static_cast<char32_t>(character);
  return true;
}

template <typename Archive>
bool TransferParagraphStyle(Archive &archive, ParagraphStyle &value,
                            const CodecContext &context) {
  if (!TransferEnum(archive, value.alignment, TextAlignment::Justify) ||
      !TransferEnum(archive, value.direction, TextDirection::RightToLeft) ||
      !archive.String(value.locale) || value.locale.empty() ||
      !TransferOptional(archive, value.maximumLines,
                        [&](std::uint32_t &lines) {
                          std::uint64_t encoded = lines;
                          if (!archive.Unsigned(encoded) || encoded == 0U ||
                              encoded > std::numeric_limits<std::uint32_t>::max())
                            return false;
                          if constexpr (Archive::kReading)
                            lines = static_cast<std::uint32_t>(encoded);
                          return true;
                        }) ||
      !TransferEnum(archive, value.overflow, TextOverflow::Visible) ||
      !archive.Float(value.lineHeight) || value.lineHeight <= 0.0F ||
      !TransferEnum(archive, value.wrap, TextWrap::None) ||
      !TransferEnum(archive, value.lineBreakPolicy,
                    TextLineBreakPolicy::Anywhere) ||
      !TransferEnum(archive, value.hyphenation,
                    TextHyphenation::Automatic) ||
      !archive.Float(value.firstLineIndent) ||
      !archive.Float(value.startIndent) || !archive.Float(value.endIndent) ||
      !archive.Float(value.spacingBefore) ||
      !archive.Float(value.spacingAfter) ||
      !archive.Boolean(value.hangingPunctuation) ||
      !TransferVector(archive, value.tabStops,
                      context.limits.maximumTabStopsPerParagraph,
                      [&](auto &tab) { return TransferTabStop(archive, tab); }) ||
      !TransferBoxBackground(archive, value.background))
    return false;
  return true;
}

template <typename Archive>
bool TransferLayoutBox(Archive &archive, LayoutBox &value) {
  if (!archive.Float(value.x) || !archive.Float(value.y) ||
      !archive.Float(value.width) || value.width <= 0.0F ||
      !archive.Float(value.height) || value.height <= 0.0F ||
      !TransferEnum(archive, value.sizingMode, TextLayoutSizingMode::FitText) ||
      !archive.Float(value.minimumWidth) || value.minimumWidth < 0.0F ||
      !archive.Float(value.minimumHeight) || value.minimumHeight < 0.0F ||
      !TransferOptional(archive, value.maximumWidth,
                        [&](float &maximum) {
                          return archive.Float(maximum) &&
                                 maximum >= value.minimumWidth;
                        }) ||
      !TransferOptional(archive, value.maximumHeight,
                        [&](float &maximum) {
                          return archive.Float(maximum) &&
                                 maximum >= value.minimumHeight;
                        }) ||
      !archive.Float(value.minimumFitFontSize) ||
      value.minimumFitFontSize <= 0.0F ||
      !archive.Float(value.maximumFitFontSize) ||
      value.maximumFitFontSize < value.minimumFitFontSize ||
      !TransferInsets(archive, value.fitMeasurementOutsets) ||
      !TransferOptional(archive, value.fitOriginX,
                        [&](float &origin) {
                          return archive.Float(origin) &&
                                 std::isfinite(origin);
                        }) ||
      !TransferInsets(archive, value.padding) ||
      !TransferEnum(archive, value.verticalAlignment,
                    VerticalAlignment::Bottom) ||
      !archive.Boolean(value.clipOverflow) ||
      !archive.Boolean(value.pixelSnap))
    return false;
  return true;
}

template <typename Archive>
bool TransferLayerAnimationKeyframe(Archive &archive,
                                    TextLayerAnimationKeyframe &value) {
  return archive.Float(value.offset) && value.offset >= 0.0F &&
         value.offset <= 1.0F && archive.Float(value.value) &&
         TransferEnum(archive, value.easing, TextAnimationEasing::EaseInOut);
}

template <typename Archive>
bool TransferLayerAnimationTrack(Archive &archive,
                                 TextLayerAnimationTrack &value,
                                 const CodecContext &context) {
  return TransferEnum(archive, value.property,
                      TextLayerAnimationProperty::RotationDegrees) &&
         TransferVector(archive, value.keyframes,
                        context.limits.maximumAnimationKeyframesPerTrack,
                        [&](auto &keyframe) {
                          return TransferLayerAnimationKeyframe(archive,
                                                                keyframe);
                        });
}

template <typename Archive>
bool TransferLayerAnimationClip(Archive &archive,
                                TextLayerAnimationClip &value,
                                const CodecContext &context) {
  return archive.String(value.clipId) && archive.String(value.presetId) &&
         TransferEnum(archive, value.phase, TextLayerAnimationPhase::Caption) &&
         archive.Signed(value.durationUs) && value.durationUs > 0 &&
         TransferVector(archive, value.tracks,
                        context.limits.maximumAnimationTracks,
                        [&](auto &track) {
                          return TransferLayerAnimationTrack(archive, track,
                                                             context);
                        });
}

template <typename Archive>
bool TransferBackdropTransform(Archive &archive,
                               TextBackdropTransform &value) {
  return archive.Float(value.offsetX) && archive.Float(value.offsetY) &&
         archive.Float(value.scaleX) && value.scaleX > 0.0F &&
         archive.Float(value.scaleY) && value.scaleY > 0.0F &&
         archive.Float(value.rotationDegrees) && archive.Float(value.opacity) &&
         value.opacity >= 0.0F && value.opacity <= 1.0F;
}

template <typename Archive>
bool TransferBackdropSource(Archive &archive, TextBackdropSource &value,
                            const CodecContext &context) {
  std::uint64_t tag = value.index();
  if (!archive.Unsigned(tag) || tag > 3U)
    return false;
  if constexpr (Archive::kReading) {
    switch (tag) {
    case 0U: value = RoundedRectBackdrop{}; break;
    case 1U: value = NineSliceBackdrop{}; break;
    case 2U: value = VectorBackdrop{}; break;
    case 3U: value = AnimatedBackdrop{}; break;
    default: return false;
    }
  }
  return std::visit(
      [&](auto &source) {
        using Source = std::decay_t<decltype(source)>;
        if constexpr (std::is_same_v<Source, RoundedRectBackdrop>) {
          return TransferEnum(archive, source.family,
                              BubbleFamily::CaptionBar) &&
                 TransferMaterialBinding(archive, source.fill, context) &&
                 TransferVector(archive, source.strokes,
                                context.limits.maximumStrokesPerRun,
                                [&](auto &stroke) {
                                  return TransferStrokeLayer(archive, stroke,
                                                             context);
                                }) &&
                 archive.Float(source.cornerRadius) &&
                 source.cornerRadius >= 0.0F &&
                 TransferEnum(archive, source.tail.edge,
                              BubbleTailEdge::Right) &&
                 archive.Float(source.tail.position) &&
                 source.tail.position >= 0.0F && source.tail.position <= 1.0F &&
                 archive.Float(source.tail.width) && source.tail.width >= 0.0F &&
                 archive.Float(source.tail.length) &&
                 source.tail.length >= 0.0F &&
                 TransferOptional(archive, source.authoredWidth,
                                  [&](float &width) {
                                    return archive.Float(width) && width > 0.0F;
                                  }) &&
                 TransferOptional(archive, source.authoredHeight,
                                  [&](float &height) {
                                    return archive.Float(height) && height > 0.0F;
                                  });
        } else if constexpr (std::is_same_v<Source, NineSliceBackdrop>) {
          return TransferTextureReference(archive, source.asset) &&
                 TransferInsets(archive, source.capInsets) &&
                 TransferInsets(archive, source.contentInsets) &&
                 archive.Float(source.minimumContentWidth) &&
                 source.minimumContentWidth >= 0.0F &&
                 archive.Float(source.minimumContentHeight) &&
                 source.minimumContentHeight >= 0.0F &&
                 TransferEnum(archive, source.stretchMode,
                              TextBackdropStretchMode::TileCenter) &&
                 TransferColor(archive, source.fallbackColor);
        } else if constexpr (std::is_same_v<Source, VectorBackdrop>) {
          return TransferTextureReference(archive, source.asset) &&
                 TransferEnum(archive, source.fit,
                              TextBackdropFitPolicy::LayoutBounds) &&
                 TransferColor(archive, source.fallbackColor);
        } else {
          return TransferTextureReference(archive, source.asset) &&
                 TransferInsets(archive, source.contentInsets) &&
                 TransferEnum(archive, source.timeSource,
                              TextBackdropTimeSource::Source) &&
                 TransferEnum(archive, source.playback,
                              TextBackdropPlaybackMode::Hold) &&
                 archive.Signed(source.sourceInUs) &&
                 archive.Signed(source.sourceOutUs) &&
                 source.sourceOutUs > source.sourceInUs &&
                 archive.Signed(source.phaseUs) &&
                 TransferColor(archive, source.fallbackColor);
        }
      },
      value);
}

template <typename Archive>
bool TransferBackdropLayer(Archive &archive, TextBackdropLayer &value,
                           const CodecContext &context) {
  std::int64_t zOrder = value.zOrder;
  if (!archive.String(value.layerId) ||
      !TransferEnum(archive, value.channel, TextBackdropChannel::Custom) ||
      !archive.Boolean(value.enabled) ||
      !TransferBackdropSource(archive, value.source, context) ||
      !TransferOptional(archive, value.materialOverride,
                        [&](auto &material) {
                          return TransferMaterialBinding(archive, material,
                                                         context);
                        }) ||
      !TransferInsets(archive, value.padding) ||
      !TransferEnum(archive, value.fit, TextBackdropFitPolicy::LayoutBounds) ||
      !TransferBackdropTransform(archive, value.transform) ||
      !archive.Signed(zOrder) ||
      zOrder < std::numeric_limits<std::int32_t>::min() ||
      zOrder > std::numeric_limits<std::int32_t>::max() ||
      !TransferVector(archive, value.animationClips,
                      context.limits.maximumAnimationTracks,
                      [&](auto &clip) {
                        return TransferLayerAnimationClip(archive, clip,
                                                          context);
                      }))
    return false;
  if constexpr (Archive::kReading)
    value.zOrder = static_cast<std::int32_t>(zOrder);
  return true;
}

template <typename Archive>
bool TransferBackdropStack(Archive &archive, TextBackdropStack &value,
                           const CodecContext &context) {
  return TransferVector(archive, value.layers, kMaximumBackdropLayers,
                        [&](auto &layer) {
                          return TransferBackdropLayer(archive, layer, context);
                        });
}

template <typename Archive>
bool TransferBend(Archive &archive, TextBend &value) {
  return archive.Boolean(value.enabled) && archive.Float(value.amount) &&
         value.amount >= -1.0F && value.amount <= 1.0F;
}

template <typename Archive>
bool TransferPathPoint(Archive &archive, TextPathPoint &value) {
  return archive.Float(value.x) && archive.Float(value.y);
}

template <typename Archive>
bool TransferPathCommand(Archive &archive, TextPathCommand &value) {
  return TransferEnum(archive, value.kind, TextPathCommandKind::Close) &&
         TransferPathPoint(archive, value.control1) &&
         TransferPathPoint(archive, value.control2) &&
         TransferPathPoint(archive, value.end);
}

template <typename Archive>
bool TransferPath(Archive &archive, TextPath &value,
                  const CodecContext &) {
  return archive.Boolean(value.enabled) && archive.String(value.geometryId) &&
         archive.String(value.presetId) &&
         TransferVector(archive, value.commands, kMaximumPathCommands,
                        [&](auto &command) {
                          return TransferPathCommand(archive, command);
                        }) &&
         archive.Float(value.startOffset) &&
         archive.Float(value.baselineOffset) &&
         TransferEnum(archive, value.overflow, TextPathOverflow::ScaleToFit) &&
         archive.Boolean(value.loop) && archive.Boolean(value.rotateToTangent) &&
         archive.Boolean(value.keepUpright);
}

template <typename Archive>
bool TransferSdfMaterial(Archive &archive, TextSdfMaterial &value,
                         const CodecContext &) {
  if (!archive.Boolean(value.enabled) ||
      !TransferEnum(archive, value.sourceCreationComponent,
                    TextSourceCreationComponent::SdfText) ||
      !archive.Float(value.distanceRange) ||
      !(value.distanceRange > 0.0F) ||
      !archive.Float(value.rasterDistanceRange) ||
      !(value.rasterDistanceRange > 0.0F) ||
      !archive.Float(value.smoothingScale) ||
      !(value.smoothingScale > 0.0F) ||
      !archive.Float(value.sourceDesignWidth) ||
      !(value.sourceDesignWidth >= 0.0F) ||
      !archive.Float(value.sourceDesignHeight) ||
      !(value.sourceDesignHeight >= 0.0F)) {
    return false;
  }
  return true;
}

template <typename Archive>
bool TransferPropertyValue(Archive &archive, TextPropertyValue &value,
                           const CodecContext &context) {
  std::uint64_t tag = value.index();
  if (!archive.Unsigned(tag) ||
      tag >= std::variant_size_v<TextPropertyValue>)
    return false;
  if constexpr (Archive::kReading) {
    switch (tag) {
    case 0U: value = std::monostate{}; break;
    case 1U: value = false; break;
    case 2U: value = std::int64_t{0}; break;
    case 3U: value = 0.0; break;
    case 4U: value = std::string{}; break;
    case 5U: value = Color{}; break;
    case 6U: value = FontReference{}; break;
    case 7U: value = FontSpec{}; break;
    case 8U: value = std::vector<FontAxis>{}; break;
    case 9U: value = std::vector<FontFeature>{}; break;
    case 10U: value = TextMaterial{}; break;
    case 11U: value = TextGlyphMaterialStack{}; break;
    case 12U: value = std::vector<TextStrokeLayer>{}; break;
    case 13U: value = std::vector<TextShadowLayer>{}; break;
    case 14U: value = std::vector<TextGlowLayer>{}; break;
    case 15U: value = InlineTextDecoration{}; break;
    case 16U: value = TextBoxBackground{}; break;
    case 17U: value = ParagraphStyle{}; break;
    case 18U: value = std::vector<TextTabStop>{}; break;
    case 19U: value = LayoutBox{}; break;
    case 20U: value = Insets{}; break;
    case 21U: value = TextWritingMode::Horizontal; break;
    case 22U: value = TextBackdropLayer{}; break;
    case 23U: value = TextBackdropStack{}; break;
    case 24U: value = TextBend{}; break;
    case 25U: value = TextPath{}; break;
    case 26U: value = TextSdfMaterial{}; break;
    default: return false;
    }
  }
  return std::visit(
      [&](auto &typed) {
        using Value = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Value, std::monostate>) {
          return true;
        } else if constexpr (std::is_same_v<Value, bool>) {
          return archive.Boolean(typed);
        } else if constexpr (std::is_same_v<Value, std::int64_t>) {
          return archive.Signed(typed);
        } else if constexpr (std::is_same_v<Value, double>) {
          return archive.Double(typed);
        } else if constexpr (std::is_same_v<Value, std::string>) {
          return archive.String(typed);
        } else if constexpr (std::is_same_v<Value, Color>) {
          return TransferColor(archive, typed);
        } else if constexpr (std::is_same_v<Value, FontReference>) {
          return TransferFontReference(archive, typed, context);
        } else if constexpr (std::is_same_v<Value, FontSpec>) {
          return TransferFontSpec(archive, typed, context);
        } else if constexpr (std::is_same_v<Value, std::vector<FontAxis>>) {
          return TransferVector(archive, typed,
                                context.limits.maximumFontFeaturesPerRun,
                                [&](auto &axis) {
                                  return TransferFontAxis(archive, axis);
                                });
        } else if constexpr (std::is_same_v<Value,
                                             std::vector<FontFeature>>) {
          return TransferVector(archive, typed,
                                context.limits.maximumFontFeaturesPerRun,
                                [&](auto &feature) {
                                  return TransferFontFeature(archive, feature);
                                });
        } else if constexpr (std::is_same_v<Value, TextMaterial>) {
          return TransferMaterial(archive, typed, context);
        } else if constexpr (std::is_same_v<Value,
                                             TextGlyphMaterialStack>) {
          return TransferGlyphStack(archive, typed, context);
        } else if constexpr (std::is_same_v<Value,
                                             std::vector<TextStrokeLayer>>) {
          return TransferVector(archive, typed,
                                context.limits.maximumStrokesPerRun,
                                [&](auto &layer) {
                                  return TransferStrokeLayer(archive, layer,
                                                             context);
                                });
        } else if constexpr (std::is_same_v<Value,
                                             std::vector<TextShadowLayer>>) {
          return TransferVector(archive, typed,
                                context.limits.maximumShadowsPerRun,
                                [&](auto &layer) {
                                  return TransferShadowLayer(archive, layer,
                                                             context);
                                });
        } else if constexpr (std::is_same_v<Value,
                                             std::vector<TextGlowLayer>>) {
          return TransferVector(archive, typed,
                                context.limits.maximumShadowsPerRun,
                                [&](auto &layer) {
                                  return TransferGlowLayer(archive, layer,
                                                           context);
                                });
        } else if constexpr (std::is_same_v<Value, InlineTextDecoration>) {
          return TransferInlineDecoration(archive, typed, context);
        } else if constexpr (std::is_same_v<Value, TextBoxBackground>) {
          return TransferBoxBackground(archive, typed);
        } else if constexpr (std::is_same_v<Value, ParagraphStyle>) {
          return TransferParagraphStyle(archive, typed, context);
        } else if constexpr (std::is_same_v<Value,
                                             std::vector<TextTabStop>>) {
          return TransferVector(archive, typed,
                                context.limits.maximumTabStopsPerParagraph,
                                [&](auto &tab) {
                                  return TransferTabStop(archive, tab);
                                });
        } else if constexpr (std::is_same_v<Value, LayoutBox>) {
          return TransferLayoutBox(archive, typed);
        } else if constexpr (std::is_same_v<Value, Insets>) {
          return TransferInsets(archive, typed);
        } else if constexpr (std::is_same_v<Value, TextWritingMode>) {
          return TransferEnum(archive, typed,
                              TextWritingMode::VerticalLeftToRight);
        } else if constexpr (std::is_same_v<Value, TextBackdropLayer>) {
          return TransferBackdropLayer(archive, typed, context);
        } else if constexpr (std::is_same_v<Value, TextBackdropStack>) {
          return TransferBackdropStack(archive, typed, context);
        } else if constexpr (std::is_same_v<Value, TextBend>) {
          return TransferBend(archive, typed);
        } else if constexpr (std::is_same_v<Value, TextPath>) {
          return TransferPath(archive, typed, context);
        } else {
          return TransferSdfMaterial(archive, typed, context);
        }
      },
      value);
}

template <typename Archive>
bool TransferCanonicalProperty(Archive &archive, TextPropertyValue &value,
                               const CodecContext &context) {
  std::string domain{kCanonicalPropertyDomain};
  if (!archive.String(domain) || domain != kCanonicalPropertyDomain)
    return false;
  return TransferPropertyValue(archive, value, context);
}

} // namespace

bool internal::EncodeCanonicalTextPropertyValueInPlace(
    TextPropertyValue &value, std::vector<std::uint8_t> &output,
    const RichTextLimits &limits) {
  const CodecContext context{limits};
  CanonicalWriter writer(kMaximumCanonicalPropertyBytes,
                         context.maximumStringBytes());
  if (!TransferCanonicalProperty(writer, value, context) || !writer.ok())
    return false;
  auto encoded = writer.Take();
  output.swap(encoded);
  return true;
}

bool EncodeCanonicalTextPropertyValue(const TextPropertyValue &value,
                                      std::vector<std::uint8_t> &output,
                                      const RichTextLimits &limits) {
  TextPropertyValue canonical = value;
  return internal::EncodeCanonicalTextPropertyValueInPlace(canonical, output,
                                                          limits);
}

bool DecodeCanonicalTextPropertyValue(const std::vector<std::uint8_t> &bytes,
                                      TextPropertyValue &output,
                                      const RichTextLimits &limits) {
  const CodecContext context{limits};
  CanonicalReader reader(bytes, kMaximumCanonicalPropertyBytes,
                         context.maximumStringBytes());
  TextPropertyValue decoded;
  if (!TransferCanonicalProperty(reader, decoded, context) || !reader.Complete())
    return false;
  std::vector<std::uint8_t> canonical;
  if (!internal::EncodeCanonicalTextPropertyValueInPlace(decoded, canonical,
                                                        limits) ||
      canonical != bytes)
    return false;
  output = std::move(decoded);
  return true;
}

} // namespace videocut::text
