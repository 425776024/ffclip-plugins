#include "videocut/text/RichTextDocument.h"

#include "videocut/base/Sha256.h"
#include "videocut/text/TextAnimation.h"
#include "videocut/text/internal/RichTextValidation.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace videocut::text {
namespace {

constexpr std::size_t kMaximumMaterialLayersPerRun = 64U;

void Add(std::vector<Diagnostic> &diagnostics, std::string code,
         std::string subject, std::string message) {
  diagnostics.push_back({std::move(code), DiagnosticSeverity::Error,
                         "text.current-schema.validation",
                         std::move(subject), std::move(message)});
}

bool Finite(const float value) noexcept { return std::isfinite(value); }

bool ValidColor(const Color &color) noexcept {
  const float values[] = {color.red, color.green, color.blue, color.alpha};
  return std::all_of(std::begin(values), std::end(values), [](const float value) {
    return Finite(value) && value >= 0.0F && value <= 1.0F;
  });
}

bool ValidInsets(const Insets &insets, const float maximum) noexcept {
  const float values[] = {insets.left, insets.top, insets.right,
                          insets.bottom};
  return std::all_of(std::begin(values), std::end(values),
                     [maximum](const float value) {
                       return Finite(value) && value >= 0.0F &&
                              value <= maximum;
                     });
}

bool ValidBackground(const TextBoxBackground &background,
                     const float maximum) noexcept {
  return ValidColor(background.color) &&
         ValidInsets(background.padding, maximum) &&
         Finite(background.cornerRadius) && background.cornerRadius >= 0.0F &&
         background.cornerRadius <= maximum;
}

bool ValidPortableAssetIdentity(const std::string &assetId,
                                const std::string &digest,
                                const bool digestRequired) noexcept {
  return !assetId.empty() && assetId.size() <= 512U && IsValidUtf8(assetId) &&
         (digest.empty() ? !digestRequired
                         : base::Sha256::IsCanonicalDigest(digest));
}

bool ValidTextureReference(const TextureReference &reference) noexcept {
  const bool digestRequired =
      reference.sourceKind == TextureSourceKind::ProjectManaged;
  const bool supportedMedia = reference.mediaType == "image/png" ||
                              reference.mediaType == "image/jpeg" ||
                              reference.mediaType == "image/webp" ||
                              reference.mediaType == "image/avif" ||
                              reference.mediaType == "image/svg+xml";
  return reference.sourceKind >= TextureSourceKind::Builtin &&
         reference.sourceKind <= TextureSourceKind::ProjectManaged &&
         ValidPortableAssetIdentity(reference.assetId, reference.digest,
                                    digestRequired) &&
         supportedMedia && reference.colorSpace == "srgb" &&
         reference.orientation >= TextureOrientation::Up &&
         reference.orientation <= TextureOrientation::Left;
}

bool ValidGradientStops(const std::vector<GradientStop> &stops,
                        const RichTextLimits &limits) noexcept {
  if (stops.size() < 2U || stops.size() > limits.maximumGradientStops)
    return false;
  float previous = -1.0F;
  for (const auto &stop : stops) {
    if (!Finite(stop.offset) || stop.offset < 0.0F || stop.offset > 1.0F ||
        stop.offset <= previous || !ValidColor(stop.color)) {
      return false;
    }
    previous = stop.offset;
  }
  return true;
}

bool ValidMaterialCoordinates(const TextMaterialCoordinates &coordinates,
                              const RichTextLimits &limits) noexcept {
  return coordinates.coordinateSpace >= PaintCoordinateSpace::LayoutBox &&
         coordinates.coordinateSpace <= PaintCoordinateSpace::Grapheme &&
         Finite(coordinates.coordinateOutset) &&
         coordinates.coordinateOutset >= 0.0F &&
         coordinates.coordinateOutset <= limits.maximumFontSize &&
         Finite(coordinates.coordinateScale) &&
         coordinates.coordinateScale >= 0.01F &&
         coordinates.coordinateScale <= 100.0F;
}

bool ValidMaterial(const TextMaterial &material, const RichTextLimits &limits,
                   const std::string &subject,
                   std::vector<Diagnostic> &diagnostics) {
  const bool valid = std::visit(
      [&](const auto &source) {
        using Source = std::decay_t<decltype(source)>;
        if constexpr (std::is_same_v<Source, SolidTextMaterial>) {
          return ValidColor(source.color);
        } else if constexpr (std::is_same_v<Source,
                                            LinearGradientTextMaterial>) {
          return ValidGradientStops(source.stops, limits) &&
                 source.spread >= PaintSpread::Clamp &&
                 source.spread <= PaintSpread::Mirror &&
                 source.sampling >= GradientSampling::Continuous &&
                 source.sampling <= GradientSampling::Rgba8Lut256 &&
                 ValidMaterialCoordinates(source.coordinates, limits) &&
                 Finite(source.startX) && Finite(source.startY) &&
                 Finite(source.endX) && Finite(source.endY) &&
                 std::fabs(source.startX) <= 8.0F &&
                 std::fabs(source.startY) <= 8.0F &&
                 std::fabs(source.endX) <= 8.0F &&
                 std::fabs(source.endY) <= 8.0F &&
                 (source.startX != source.endX ||
                  source.startY != source.endY);
        } else if constexpr (std::is_same_v<Source,
                                            RadialGradientTextMaterial>) {
          return ValidGradientStops(source.stops, limits) &&
                 source.spread >= PaintSpread::Clamp &&
                 source.spread <= PaintSpread::Mirror &&
                 source.sampling >= GradientSampling::Continuous &&
                 source.sampling <= GradientSampling::Rgba8Lut256 &&
                 ValidMaterialCoordinates(source.coordinates, limits) &&
                 Finite(source.centerX) && Finite(source.centerY) &&
                 std::fabs(source.centerX) <= 8.0F &&
                 std::fabs(source.centerY) <= 8.0F && Finite(source.radius) &&
                 source.radius > 0.0F && source.radius <= 8.0F;
        } else {
          const bool underlayValid =
              source.underlayGradient.empty() ||
              ValidGradientStops(source.underlayGradient, limits);
          const auto &projection = source.underlayGradientProjection;
          const bool underlayProjectionValid =
              projection.spread >= PaintSpread::Clamp &&
              projection.spread <= PaintSpread::Mirror &&
              projection.sampling >= GradientSampling::Continuous &&
              projection.sampling <= GradientSampling::Rgba8Lut256 &&
              Finite(projection.startX) && Finite(projection.startY) &&
              Finite(projection.endX) && Finite(projection.endY) &&
              std::fabs(projection.startX) <= 8.0F &&
              std::fabs(projection.startY) <= 8.0F &&
              std::fabs(projection.endX) <= 8.0F &&
              std::fabs(projection.endY) <= 8.0F &&
              (projection.startX != projection.endX ||
               projection.startY != projection.endY);
          return ValidTextureReference(source.texture) &&
                 source.fit >= TextureFit::Cover &&
                 source.fit <= TextureFit::Tile &&
                 source.mapping >= TextureMapping::ScopeBounds &&
                 source.mapping <= TextureMapping::GlyphDistanceField &&
                 ValidMaterialCoordinates(source.coordinates, limits) &&
                 Finite(source.scale) && source.scale >= 0.01F &&
                 source.scale <= 100.0F && Finite(source.rotationDegrees) &&
                 std::fabs(source.rotationDegrees) <= 3'600.0F &&
                 Finite(source.offsetX) && Finite(source.offsetY) &&
                 std::fabs(source.offsetX) <= 8.0F &&
                 std::fabs(source.offsetY) <= 8.0F &&
                 source.atlasColumns >= 1U && source.atlasColumns <= 64U &&
                 source.atlasRows >= 1U && source.atlasRows <= 64U &&
                 Finite(source.textureOpacity) &&
                 source.textureOpacity >= 0.0F &&
                 source.textureOpacity <= 1.0F &&
                 Finite(source.opacity) && source.opacity >= 0.0F &&
                 source.opacity <= 1.0F &&
                 (!source.underlayColor || ValidColor(*source.underlayColor)) &&
                 underlayValid && underlayProjectionValid &&
                 (!source.sourceAlpha || source.atlasRows == 1U);
        }
      },
      material);
  if (!valid) {
    Add(diagnostics, "text.material.invalid", subject,
        "text material contains invalid geometry, color, or portable resource "
        "identity");
  }
  return valid;
}

bool ValidMaterialBinding(const TextMaterialBinding &binding,
                          const RichTextLimits &limits,
                          const std::string &subject,
                          std::vector<Diagnostic> &diagnostics) {
  return std::visit(
      [&](const auto &source) {
        using Source = std::decay_t<decltype(source)>;
        if constexpr (std::is_same_v<Source, LiteralTextMaterial>) {
          return ValidMaterial(source.material, limits, subject, diagnostics);
        } else {
          if (source.semanticRole.empty() || source.semanticRole.size() > 256U ||
              !IsValidUtf8(source.semanticRole)) {
            Add(diagnostics, "text.material.role_invalid", subject,
                "editable material roles must be non-empty UTF-8 identities");
            return false;
          }
          bool valid =
              ValidMaterial(source.fallback, limits, subject, diagnostics);
          if (source.replacementMask &&
              !ValidTextureReference(*source.replacementMask)) {
            Add(diagnostics, "text.material.mask_invalid", subject,
                "editable material masks require a portable texture identity");
            valid = false;
          }
          return valid;
        }
      },
      binding);
}

bool ValidFontReference(const FontReference &reference,
                        const std::string &subject,
                        std::vector<Diagnostic> &diagnostics) {
  if (reference.family.empty() || !IsValidUtf8(reference.family) ||
      reference.weight < 1 || reference.weight > 1000 || reference.width < 1 ||
      reference.width > 9 || reference.slant < FontSlant::Upright ||
      reference.slant > FontSlant::Oblique) {
    Add(diagnostics, "text.font.identity_invalid", subject,
        "font family, weight, width, or slant is invalid");
    return false;
  }
  if (reference.kind == FontSourceKind::System) {
    const bool portablePlatform =
        !reference.platform.empty() && reference.platform.size() <= 64U &&
        IsValidUtf8(reference.platform) &&
        std::all_of(reference.platform.begin(), reference.platform.end(),
                    [](const unsigned char value) {
                      return (value >= 'a' && value <= 'z') ||
                             (value >= '0' && value <= '9') || value == '.' ||
                             value == '_' || value == '-';
                    });
    if (!portablePlatform || !reference.assetId.empty() ||
        !reference.digest.empty()) {
      Add(diagnostics, "text.font.system_identity_invalid", subject,
          "system fonts require a portable platform identity and no asset "
          "locator");
      return false;
    }
  } else if (reference.kind != FontSourceKind::Builtin &&
             reference.kind != FontSourceKind::ProjectManaged) {
    Add(diagnostics, "text.font.not_admitted", subject,
        "font source kind is not admitted");
    return false;
  } else {
  const bool digestRequired = reference.kind == FontSourceKind::ProjectManaged;
  if (!ValidPortableAssetIdentity(reference.assetId, reference.digest,
                                  digestRequired) ||
      !reference.platform.empty()) {
    Add(diagnostics, "text.font.asset_identity_invalid", subject,
        "font identity contains a path or lacks an admitted asset identity");
    return false;
  }
  }
  std::unordered_set<std::string> axes;
  for (const auto &axis : reference.variationAxes) {
    if (axis.tag.size() != 4U || !IsValidUtf8(axis.tag) || !Finite(axis.value) ||
        !axes.insert(axis.tag).second) {
      Add(diagnostics, "text.font.axis_invalid", subject,
          "font axes require unique four-byte tags and finite values");
      return false;
    }
  }
  return true;
}

bool ValidFontSpec(const FontSpec &font, const RichTextLimits &limits,
                   const std::string &subject,
                   std::vector<Diagnostic> &diagnostics) {
  bool valid = ValidFontReference(font.primary, subject, diagnostics);
  if (font.fallbacks.size() > limits.maximumFallbackFontsPerRun) {
    Add(diagnostics, "text.font.fallback_limit", subject,
        "font fallback count exceeds the current-schema limit");
    valid = false;
  }
  for (const auto &fallback : font.fallbacks)
    valid = ValidFontReference(fallback, subject, diagnostics) && valid;
  if ((!font.family.empty() && !IsValidUtf8(font.family)) ||
      (!font.postscriptName.empty() && !IsValidUtf8(font.postscriptName)) ||
      font.weight < 1 || font.weight > 1000 || font.width < 1 ||
      font.width > 9 || font.slant < FontSlant::Upright ||
      font.slant > FontSlant::Oblique) {
    Add(diagnostics, "text.font.style_invalid", subject,
        "font style selection is invalid");
    valid = false;
  }
  std::unordered_set<std::string> axisTags;
  for (const auto &axis : font.variationAxes) {
    if (axis.tag.size() != 4U || !Finite(axis.value) ||
        !axisTags.insert(axis.tag).second) {
      Add(diagnostics, "text.font.axis_invalid", subject,
          "font style axes require unique four-byte tags and finite values");
      valid = false;
      break;
    }
  }
  if (font.features.size() > limits.maximumFontFeaturesPerRun) {
    Add(diagnostics, "text.font.feature_limit", subject,
        "OpenType feature count exceeds the current-schema limit");
    valid = false;
  }
  std::unordered_set<std::string> featureTags;
  for (const auto &feature : font.features) {
    if (feature.tag.size() != 4U || !IsValidUtf8(feature.tag) ||
        !featureTags.insert(feature.tag).second) {
      Add(diagnostics, "text.font.feature_invalid", subject,
          "OpenType features require unique four-byte tags");
      valid = false;
      break;
    }
  }
  return valid;
}

bool ValidMaterialStack(const TextGlyphMaterialStack &stack,
                        const RichTextLimits &limits,
                        const std::string &subject,
                        std::vector<Diagnostic> &diagnostics) {
  if (stack.layers.empty() || stack.layers.size() > kMaximumMaterialLayersPerRun) {
    Add(diagnostics, "text.material_stack.layer_limit", subject,
        "glyph material stack must contain a bounded set of layers");
    return false;
  }
  std::unordered_set<std::string> layerIds;
  std::size_t strokes = 0U;
  std::size_t shadows = 0U;
  bool valid = true;
  for (const auto &layer : stack.layers) {
    const bool layerValid = std::visit(
        [&](const auto &value) {
          using Layer = std::decay_t<decltype(value)>;
          if (value.layerId.empty() || value.layerId.size() > 512U ||
              !IsValidUtf8(value.layerId) ||
              !layerIds.insert(value.layerId).second ||
              value.blend < TextBlendMode::SourceOver ||
              value.blend > TextBlendMode::Lighten ||
              !ValidMaterialBinding(value.material, limits, value.layerId,
                                    diagnostics)) {
            return false;
          }
          if constexpr (std::is_same_v<Layer, TextFillLayer>) {
            return Finite(value.offsetX) && Finite(value.offsetY) &&
                   (!value.normalizedPolarOffset ||
                    (Finite(value.normalizedPolarOffset->radius) &&
                     value.normalizedPolarOffset->radius >= 0.0F &&
                     Finite(value.normalizedPolarOffset->angleRadians)));
          } else if constexpr (std::is_same_v<Layer, TextStrokeLayer>) {
            ++strokes;
            return Finite(value.width) && value.width >= 0.0F &&
                   value.width <= limits.maximumFontSize &&
                   Finite(value.innerRingWidth) && value.innerRingWidth >= 0.0F &&
                   value.innerRingWidth <= value.width &&
                   (!value.signedStartWidth ||
                    (Finite(*value.signedStartWidth) &&
                     std::fabs(*value.signedStartWidth) <=
                         limits.maximumFontSize)) &&
                   Finite(value.offsetX) && Finite(value.offsetY) &&
                   (!value.normalizedPolarOffset ||
                    (Finite(value.normalizedPolarOffset->radius) &&
                     value.normalizedPolarOffset->radius >= 0.0F &&
                     Finite(value.normalizedPolarOffset->angleRadians))) &&
                   Finite(value.blurRadius) && value.blurRadius >= 0.0F &&
                   value.blurRadius <= limits.maximumFontSize &&
                   Finite(value.spread) && value.spread >= 0.0F &&
                   value.spread <= limits.maximumFontSize;
          } else if constexpr (std::is_same_v<Layer, TextShadowLayer>) {
            ++shadows;
            if (value.strokes.size() > limits.maximumStrokesPerRun)
              return false;
            for (const auto &stroke : value.strokes) {
              ++strokes;
              if (stroke.layerId.empty() || stroke.layerId.size() > 512U ||
                  !IsValidUtf8(stroke.layerId) ||
                  !layerIds.insert(stroke.layerId).second ||
                  stroke.blend < TextBlendMode::SourceOver ||
                  stroke.blend > TextBlendMode::Lighten ||
                  !ValidMaterialBinding(stroke.material, limits,
                                        stroke.layerId, diagnostics) ||
                  !Finite(stroke.width) || stroke.width < 0.0F ||
                  stroke.width > limits.maximumFontSize ||
                  !Finite(stroke.innerRingWidth) ||
                  stroke.innerRingWidth < 0.0F ||
                  stroke.innerRingWidth > stroke.width ||
                  (stroke.signedStartWidth &&
                   (!Finite(*stroke.signedStartWidth) ||
                    std::fabs(*stroke.signedStartWidth) >
                        limits.maximumFontSize)) ||
                  !Finite(stroke.offsetX) || !Finite(stroke.offsetY) ||
                  !Finite(stroke.blurRadius) || stroke.blurRadius < 0.0F ||
                  stroke.blurRadius > limits.maximumFontSize ||
                  !Finite(stroke.spread) || stroke.spread < 0.0F ||
                  stroke.spread > limits.maximumFontSize ||
                  (stroke.normalizedPolarOffset &&
                   (!Finite(stroke.normalizedPolarOffset->radius) ||
                    stroke.normalizedPolarOffset->radius < 0.0F ||
                    !Finite(stroke.normalizedPolarOffset->angleRadians)))) {
                return false;
              }
            }
            return value.kind >= TextShadowKind::Outer &&
                   value.kind <= TextShadowKind::Inner &&
                   Finite(value.offsetX) && Finite(value.offsetY) &&
                   (!value.normalizedPolarOffset ||
                    (Finite(value.normalizedPolarOffset->radius) &&
                     value.normalizedPolarOffset->radius >= 0.0F &&
                     Finite(value.normalizedPolarOffset->angleRadians))) &&
                   (!value.normalizedUvOffset ||
                    (value.kind == TextShadowKind::Inner &&
                     Finite(value.normalizedUvOffset->x) &&
                     Finite(value.normalizedUvOffset->y))) &&
                   Finite(value.blurRadius) && value.blurRadius >= 0.0F &&
                   value.blurRadius <= limits.maximumFontSize &&
                   Finite(value.spread) &&
                   (value.spread >= 0.0F ||
                    (value.kind == TextShadowKind::Outer &&
                     value.smoothing ==
                         TextOuterShadowSmoothingMode::Feather)) &&
                   std::fabs(value.spread) <= limits.maximumFontSize &&
                   Finite(value.thicknessAngleDegrees) &&
                   std::fabs(value.thicknessAngleDegrees) <= 3'600.0F &&
                   Finite(value.thicknessDistance) &&
                   value.thicknessDistance >= 0.0F &&
                   value.thicknessDistance <= limits.maximumFontSize &&
                   value.smoothing >= TextOuterShadowSmoothingMode::Auto &&
                   value.smoothing <=
                       TextOuterShadowSmoothingMode::DiffuseRoundMask &&
                   Finite(value.roundMaskIntensity) &&
                   value.roundMaskIntensity >= 0.0F &&
                   value.roundMaskIntensity <= limits.maximumFontSize &&
                   Finite(value.sdfBlurScale) &&
                   value.sdfBlurScale >= 0.0F &&
                   value.sdfBlurScale <= 16.0F;
          } else if constexpr (std::is_same_v<Layer, TextGlowLayer>) {
            return Finite(value.radius) && value.radius >= 0.0F &&
                   value.radius <= limits.maximumFontSize &&
                   Finite(value.spread) && value.spread >= 0.0F &&
                   value.spread <= limits.maximumFontSize &&
                   Finite(value.directionX) && Finite(value.directionY) &&
                   std::fabs(value.directionX) <= 1.0F &&
                   std::fabs(value.directionY) <= 1.0F &&
                   (!value.normalizedPolarOffset ||
                    (Finite(value.normalizedPolarOffset->radius) &&
                     value.normalizedPolarOffset->radius >= 0.0F &&
                     Finite(value.normalizedPolarOffset->angleRadians)));
          } else {
            return true;
          }
        },
        layer);
    if (!layerValid) {
      Add(diagnostics, "text.material_stack.layer_invalid", subject,
          "glyph material layer is invalid or has a duplicate identity");
      valid = false;
    }
  }
  if (strokes > limits.maximumStrokesPerRun ||
      shadows > limits.maximumShadowsPerRun) {
    Add(diagnostics, "text.material_stack.kind_limit", subject,
        "stroke or shadow count exceeds the current-schema limit");
    valid = false;
  }
  return valid;
}

bool ValidLayout(const LayoutBox &layout, const RichTextLimits &limits) noexcept {
  const bool maximaValid =
      (!layout.maximumWidth ||
       (Finite(*layout.maximumWidth) && *layout.maximumWidth > 0.0F &&
        *layout.maximumWidth >= layout.minimumWidth)) &&
      (!layout.maximumHeight ||
       (Finite(*layout.maximumHeight) && *layout.maximumHeight > 0.0F &&
        *layout.maximumHeight >= layout.minimumHeight));
  return layout.sizingMode >= TextLayoutSizingMode::AutoWidth &&
         layout.sizingMode <= TextLayoutSizingMode::FitText &&
         layout.verticalAlignment >= VerticalAlignment::Top &&
         layout.verticalAlignment <= VerticalAlignment::Bottom &&
         Finite(layout.x) && Finite(layout.y) && Finite(layout.width) &&
         Finite(layout.height) && layout.width > 0.0F && layout.height > 0.0F &&
         std::fabs(layout.x) <= limits.maximumCanvasDimension &&
         std::fabs(layout.y) <= limits.maximumCanvasDimension &&
         layout.width <= limits.maximumCanvasDimension &&
         layout.height <= limits.maximumCanvasDimension &&
         Finite(layout.minimumWidth) && layout.minimumWidth >= 0.0F &&
         Finite(layout.minimumHeight) && layout.minimumHeight >= 0.0F &&
         maximaValid && Finite(layout.minimumFitFontSize) &&
         layout.minimumFitFontSize > 0.0F &&
         layout.minimumFitFontSize <= limits.maximumFontSize &&
         Finite(layout.maximumFitFontSize) &&
         layout.maximumFitFontSize >= layout.minimumFitFontSize &&
         layout.maximumFitFontSize <= limits.maximumFontSize &&
         ValidInsets(layout.fitMeasurementOutsets,
                     limits.maximumCanvasDimension) &&
         (!layout.fitOriginX ||
          (Finite(*layout.fitOriginX) &&
           std::fabs(*layout.fitOriginX) <= limits.maximumCanvasDimension)) &&
         ValidInsets(layout.padding, limits.maximumCanvasDimension) &&
         layout.padding.left + layout.padding.right < layout.width &&
         layout.padding.top + layout.padding.bottom < layout.height;
}

bool ValidParagraphStyle(const ParagraphStyle &style,
                         const RichTextLimits &limits) noexcept {
  if (style.alignment < TextAlignment::Start ||
      style.alignment > TextAlignment::Justify ||
      style.direction < TextDirection::Auto ||
      style.direction > TextDirection::RightToLeft ||
      style.overflow < TextOverflow::Clip ||
      style.overflow > TextOverflow::Visible || style.wrap < TextWrap::Word ||
      style.wrap > TextWrap::None ||
      style.lineBreakPolicy < TextLineBreakPolicy::Unicode ||
      style.lineBreakPolicy > TextLineBreakPolicy::Anywhere ||
      style.hyphenation < TextHyphenation::None ||
      style.hyphenation > TextHyphenation::Automatic || style.locale.empty() ||
      !IsValidUtf8(style.locale) || !Finite(style.lineHeight) ||
      style.lineHeight <= 0.0F || style.lineHeight > 16.0F ||
      (style.maximumLines && *style.maximumLines == 0U) ||
      !Finite(style.firstLineIndent) || !Finite(style.startIndent) ||
      !Finite(style.endIndent) || !Finite(style.spacingBefore) ||
      !Finite(style.spacingAfter) ||
      std::fabs(style.firstLineIndent) > limits.maximumCanvasDimension ||
      std::fabs(style.startIndent) > limits.maximumCanvasDimension ||
      std::fabs(style.endIndent) > limits.maximumCanvasDimension ||
      style.spacingBefore < 0.0F || style.spacingAfter < 0.0F ||
      style.spacingBefore > limits.maximumCanvasDimension ||
      style.spacingAfter > limits.maximumCanvasDimension ||
      style.tabStops.size() > limits.maximumTabStopsPerParagraph ||
      !ValidBackground(style.background, limits.maximumCanvasDimension)) {
    return false;
  }
  float previous = -std::numeric_limits<float>::infinity();
  for (const auto &tab : style.tabStops) {
    if (!Finite(tab.position) || tab.position < 0.0F ||
        tab.position <= previous || tab.position > limits.maximumCanvasDimension ||
        tab.alignment < TextAlignment::Start ||
        tab.alignment > TextAlignment::Justify ||
        tab.decimalCharacter == U'\0' || tab.decimalCharacter > 0x10FFFFU ||
        (tab.decimalCharacter >= 0xD800U && tab.decimalCharacter <= 0xDFFFU)) {
      return false;
    }
    previous = tab.position;
  }
  return true;
}

bool ValidTextStyle(const TextStyle &style, const RichTextLimits &limits,
                    const std::string &subject,
                    std::vector<Diagnostic> &diagnostics) {
  bool valid = ValidFontSpec(style.font, limits, subject, diagnostics);
  const auto validDecorationLine = [&](const TextDecorationLine &line,
                                       const std::string &lineSubject) {
    return ValidMaterialBinding(line.material, limits, lineSubject,
                                diagnostics) &&
           Finite(line.thickness) && line.thickness >= 0.0F &&
           line.thickness <= limits.maximumFontSize && Finite(line.offset) &&
           std::fabs(line.offset) <= limits.maximumFontSize &&
           line.style >= TextDecorationLineStyle::Solid &&
           line.style <= TextDecorationLineStyle::Wavy;
  };
  if (!Finite(style.fontSize) || style.fontSize <= 0.0F ||
      style.fontSize > limits.maximumFontSize || !Finite(style.letterSpacing) ||
      !Finite(style.wordSpacing) || !Finite(style.baselineShift) ||
      std::fabs(style.letterSpacing) > limits.maximumFontSize ||
      std::fabs(style.wordSpacing) > limits.maximumFontSize ||
      std::fabs(style.baselineShift) > limits.maximumFontSize ||
      !ValidBackground(style.background, limits.maximumCanvasDimension) ||
      !validDecorationLine(style.decoration.underline,
                           subject + ".underline") ||
      !validDecorationLine(style.decoration.strikeThrough,
                           subject + ".strikeThrough")) {
    Add(diagnostics, "text.style.metric_invalid", subject,
        "text metrics, background, or inline decoration is invalid");
    valid = false;
  }
  return ValidMaterialStack(style.materials, limits, subject, diagnostics) &&
         valid;
}

} // namespace

RichTextValidationResult
internal::ValidateRichText(const RichTextValidationView &view,
                           const RichTextLimits &limits) {
  RichTextValidationResult result;
  if (!Finite(view.referenceCanvas.width) ||
      !Finite(view.referenceCanvas.height) ||
      view.referenceCanvas.width <= 0.0F ||
      view.referenceCanvas.height <= 0.0F ||
      view.referenceCanvas.width > limits.maximumCanvasDimension ||
      view.referenceCanvas.height > limits.maximumCanvasDimension ||
      view.referenceCanvas.scalePolicy < CanvasScalePolicy::Fit ||
      view.referenceCanvas.scalePolicy > CanvasScalePolicy::None) {
    Add(result.diagnostics, "text.canvas.invalid", {},
        "reference canvas is outside the current-schema bounds");
  }
  if (!ValidLayout(view.layoutBox, limits)) {
    Add(result.diagnostics, "text.layout.invalid", {},
        "layout sizing, constraints, alignment, or padding is invalid");
  }
  if (view.writingMode < TextWritingMode::Horizontal ||
      view.writingMode > TextWritingMode::VerticalLeftToRight) {
    Add(result.diagnostics, "text.writing_mode.invalid", {},
        "writing mode is invalid");
  }
  if (view.paragraphs.empty() ||
      view.paragraphs.size() > limits.maximumParagraphs) {
    Add(result.diagnostics, "text.paragraph.limit", {},
        "paragraph count is outside the current-schema limit");
  }
  if (view.slots.empty() || view.slots.size() > limits.maximumSlots) {
    Add(result.diagnostics, "text.slot.limit", {},
        "content-slot count is outside the current-schema limit");
  }

  std::unordered_set<std::string> paragraphIds;
  std::unordered_set<std::string> runIds;
  std::unordered_map<std::string, std::string> runParagraphIds;
  std::size_t runCount = 0U;
  std::size_t utf8Bytes = 0U;
  for (const auto &paragraph : view.paragraphs) {
    if (paragraph.paragraphId.empty() ||
        !IsValidUtf8(paragraph.paragraphId) ||
        !paragraphIds.insert(paragraph.paragraphId).second) {
      Add(result.diagnostics, "text.paragraph.id_invalid",
          paragraph.paragraphId,
          "paragraph identities must be non-empty, valid, and unique");
    }
    if (!ValidParagraphStyle(paragraph.style, limits)) {
      Add(result.diagnostics, "text.paragraph.style_invalid",
          paragraph.paragraphId,
          "paragraph layout, language, tab, or background state is invalid");
    }
    if (paragraph.runs.empty()) {
      Add(result.diagnostics, "text.paragraph.runs_missing",
          paragraph.paragraphId, "each paragraph must own at least one run");
    }
    for (const auto &run : paragraph.runs) {
      ++runCount;
      if (run.runId.empty() || !IsValidUtf8(run.runId) ||
          !runIds.insert(run.runId).second) {
        Add(result.diagnostics, "text.run.id_invalid", run.runId,
            "run identities must be non-empty, valid, and globally unique");
      } else {
        runParagraphIds.emplace(run.runId, paragraph.paragraphId);
      }
      if (!IsValidUtf8(run.utf8Text) || run.locale.empty() ||
          !IsValidUtf8(run.locale)) {
        Add(result.diagnostics, "text.run.content_invalid", run.runId,
            "run text and locale must be valid UTF-8");
      }
      if (utf8Bytes > limits.maximumUtf8Bytes -
                          std::min(limits.maximumUtf8Bytes,
                                   run.utf8Text.size())) {
        utf8Bytes = limits.maximumUtf8Bytes + 1U;
      } else {
        utf8Bytes += run.utf8Text.size();
      }
      (void)ValidTextStyle(run.style, limits, run.runId, result.diagnostics);
    }
  }
  if (runCount > limits.maximumRuns) {
    Add(result.diagnostics, "text.run.limit", {},
        "run count exceeds the current-schema limit");
  }
  if (utf8Bytes > limits.maximumUtf8Bytes) {
    Add(result.diagnostics, "text.content.byte_limit", {},
        "UTF-8 content exceeds the current-schema byte limit");
  }

  std::unordered_set<std::string> slotIds;
  std::unordered_set<std::string> ownedParagraphIds;
  std::unordered_set<std::string> ownedRunIds;
  for (const auto &slot : view.slots) {
    if (slot.slotId.empty() || !IsValidUtf8(slot.slotId) ||
        !slotIds.insert(slot.slotId).second || slot.semanticRole.empty() ||
        !IsValidUtf8(slot.semanticRole)) {
      Add(result.diagnostics, "text.slot.identity_invalid", slot.slotId,
          "content slots require unique identities and semantic roles");
    }
    std::unordered_set<std::string> localParagraphIds;
    for (const auto &paragraphId : slot.paragraphIds) {
      if (paragraphIds.find(paragraphId) == paragraphIds.end() ||
          !localParagraphIds.insert(paragraphId).second ||
          !ownedParagraphIds.insert(paragraphId).second) {
        Add(result.diagnostics, "text.slot.paragraph_invalid", slot.slotId,
            "slot paragraph references must exist and have one owner");
      }
    }
    std::unordered_set<std::string> localRunIds;
    for (const auto &runId : slot.runIds) {
      const auto runParagraph = runParagraphIds.find(runId);
      if (runParagraph == runParagraphIds.end() ||
          localParagraphIds.find(runParagraph->second) ==
              localParagraphIds.end() ||
          !localRunIds.insert(runId).second ||
          !ownedRunIds.insert(runId).second) {
        Add(result.diagnostics, "text.slot.run_invalid", slot.slotId,
            "slot run references must belong to its paragraphs and one slot");
      }
    }
  }
  if (ownedParagraphIds.size() != paragraphIds.size() ||
      ownedRunIds.size() != runIds.size()) {
    Add(result.diagnostics, "text.slot.coverage_incomplete", {},
        "every paragraph and run must belong to exactly one content slot");
  }

  result.valid =
      std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                   [](const Diagnostic &diagnostic) {
                     return diagnostic.severity == DiagnosticSeverity::Error;
                   });
  return result;
}

RichTextValidationResult
ValidateResolvedRichTextView(const ResolvedRichTextView &view,
                             const RichTextLimits &limits) {
  return internal::ValidateRichText(
      {view.referenceCanvas, view.layoutBox, view.writingMode, view.slots,
       view.paragraphs},
      limits);
}

} // namespace videocut::text
