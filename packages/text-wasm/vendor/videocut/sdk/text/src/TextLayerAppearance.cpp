#include "videocut/text/TextLayerAppearance.h"

#include "videocut/base/Sha256.h"
#include "videocut/text/TextAnimation.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace videocut::text {
namespace {

bool Finite(const float value) noexcept { return std::isfinite(value); }

bool ValidColor(const Color &color) noexcept {
  const float values[] = {color.red, color.green, color.blue, color.alpha};
  return std::all_of(std::begin(values), std::end(values), [](const float value) {
    return Finite(value) && value >= 0.0F && value <= 1.0F;
  });
}

bool ValidInsets(const Insets &insets) noexcept {
  const float values[] = {insets.left, insets.top, insets.right, insets.bottom};
  return std::all_of(std::begin(values), std::end(values), [](const float value) {
    return Finite(value) && value >= 0.0F && value <= 65'536.0F;
  });
}

bool ValidTexture(const TextureReference &texture) noexcept {
  const bool projectManaged =
      texture.sourceKind == TextureSourceKind::ProjectManaged;
  const bool supportedMedia = texture.mediaType == "image/png" ||
                              texture.mediaType == "image/jpeg" ||
                              texture.mediaType == "image/webp" ||
                              texture.mediaType == "image/avif" ||
                              texture.mediaType == "image/svg+xml";
  return texture.sourceKind >= TextureSourceKind::Builtin &&
         texture.sourceKind <= TextureSourceKind::ProjectManaged &&
         !texture.assetId.empty() && texture.assetId.size() <= 512U &&
         IsValidUtf8(texture.assetId) &&
         (texture.digest.empty()
              ? !projectManaged
              : base::Sha256::IsCanonicalDigest(texture.digest)) &&
         supportedMedia && texture.colorSpace == "srgb" &&
         texture.orientation >= TextureOrientation::Up &&
         texture.orientation <= TextureOrientation::Left;
}

bool ValidStops(const std::vector<GradientStop> &stops) noexcept {
  if (stops.size() < 2U || stops.size() > 16U)
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

bool ValidCoordinates(const TextMaterialCoordinates &coordinates) noexcept {
  return coordinates.coordinateSpace >= PaintCoordinateSpace::LayoutBox &&
         coordinates.coordinateSpace <= PaintCoordinateSpace::Grapheme &&
         Finite(coordinates.coordinateOutset) &&
         coordinates.coordinateOutset >= 0.0F &&
         coordinates.coordinateOutset <= 4096.0F &&
         Finite(coordinates.coordinateScale) &&
         coordinates.coordinateScale >= 0.01F &&
         coordinates.coordinateScale <= 100.0F;
}

bool ValidMaterial(const TextMaterial &material) noexcept {
  return std::visit(
      [](const auto &source) {
        using Source = std::decay_t<decltype(source)>;
        if constexpr (std::is_same_v<Source, SolidTextMaterial>) {
          return ValidColor(source.color);
        } else if constexpr (std::is_same_v<Source,
                                            LinearGradientTextMaterial>) {
          return ValidStops(source.stops) &&
                 source.spread >= PaintSpread::Clamp &&
                 source.spread <= PaintSpread::Mirror &&
                 source.sampling >= GradientSampling::Continuous &&
                 source.sampling <= GradientSampling::Rgba8Lut256 &&
                 ValidCoordinates(source.coordinates) && Finite(source.startX) &&
                 Finite(source.startY) && Finite(source.endX) &&
                 Finite(source.endY) &&
                 (source.startX != source.endX ||
                  source.startY != source.endY);
        } else if constexpr (std::is_same_v<Source,
                                            RadialGradientTextMaterial>) {
          return ValidStops(source.stops) &&
                 source.spread >= PaintSpread::Clamp &&
                 source.spread <= PaintSpread::Mirror &&
                 source.sampling >= GradientSampling::Continuous &&
                 source.sampling <= GradientSampling::Rgba8Lut256 &&
                 ValidCoordinates(source.coordinates) &&
                 Finite(source.centerX) && Finite(source.centerY) &&
                 Finite(source.radius) && source.radius > 0.0F &&
                 source.radius <= 8.0F;
        } else {
          const auto &projection = source.underlayGradientProjection;
          return ValidTexture(source.texture) &&
                 source.fit >= TextureFit::Cover &&
                 source.fit <= TextureFit::Tile &&
                 source.mapping >= TextureMapping::ScopeBounds &&
                 source.mapping <= TextureMapping::GlyphDistanceField &&
                 ValidCoordinates(source.coordinates) && Finite(source.scale) &&
                 source.scale >= 0.01F && source.scale <= 100.0F &&
                 Finite(source.rotationDegrees) &&
                 std::fabs(source.rotationDegrees) <= 3'600.0F &&
                 Finite(source.offsetX) && Finite(source.offsetY) &&
                 source.atlasColumns >= 1U && source.atlasColumns <= 64U &&
                 source.atlasRows >= 1U && source.atlasRows <= 64U &&
                 Finite(source.textureOpacity) &&
                 source.textureOpacity >= 0.0F &&
                 source.textureOpacity <= 1.0F &&
                 Finite(source.opacity) && source.opacity >= 0.0F &&
                 source.opacity <= 1.0F &&
                 (!source.underlayColor || ValidColor(*source.underlayColor)) &&
                 (source.underlayGradient.empty() ||
                  ValidStops(source.underlayGradient)) &&
                 projection.spread >= PaintSpread::Clamp &&
                 projection.spread <= PaintSpread::Mirror &&
                 projection.sampling >= GradientSampling::Continuous &&
                 projection.sampling <= GradientSampling::Rgba8Lut256 &&
                 Finite(projection.startX) && Finite(projection.startY) &&
                 Finite(projection.endX) && Finite(projection.endY) &&
                 (projection.startX != projection.endX ||
                  projection.startY != projection.endY) &&
                 (!source.sourceAlpha || source.atlasRows == 1U);
        }
      },
      material);
}

bool ValidMaterialBinding(const TextMaterialBinding &binding) noexcept {
  return std::visit(
      [](const auto &source) {
        using Source = std::decay_t<decltype(source)>;
        if constexpr (std::is_same_v<Source, LiteralTextMaterial>) {
          return ValidMaterial(source.material);
        } else {
          return !source.semanticRole.empty() &&
                 source.semanticRole.size() <= 256U &&
                 IsValidUtf8(source.semanticRole) &&
                 ValidMaterial(source.fallback) &&
                 (!source.replacementMask ||
                  ValidTexture(*source.replacementMask));
        }
      },
      binding);
}

bool ValidStroke(const TextStrokeLayer &stroke) noexcept {
  return !stroke.layerId.empty() && stroke.layerId.size() <= 512U &&
         IsValidUtf8(stroke.layerId) &&
         stroke.blend >= TextBlendMode::SourceOver &&
         stroke.blend <= TextBlendMode::Lighten &&
         ValidMaterialBinding(stroke.material) && Finite(stroke.width) &&
         stroke.width >= 0.0F && stroke.width <= 4096.0F &&
         Finite(stroke.innerRingWidth) && stroke.innerRingWidth >= 0.0F &&
         stroke.innerRingWidth <= stroke.width &&
         (!stroke.signedStartWidth ||
          (Finite(*stroke.signedStartWidth) &&
           std::fabs(*stroke.signedStartWidth) <= 4096.0F)) &&
         Finite(stroke.offsetX) &&
         Finite(stroke.offsetY) &&
         (!stroke.normalizedPolarOffset ||
          (Finite(stroke.normalizedPolarOffset->radius) &&
           stroke.normalizedPolarOffset->radius >= 0.0F &&
           Finite(stroke.normalizedPolarOffset->angleRadians))) &&
         Finite(stroke.blurRadius) &&
         stroke.blurRadius >= 0.0F && stroke.blurRadius <= 4096.0F &&
         Finite(stroke.spread) && stroke.spread >= 0.0F &&
         stroke.spread <= 4096.0F;
}

bool Fail(std::string *error, const char *message) {
  if (error)
    *error = message;
  return false;
}

TextPathCommand Move(const float x, const float y) {
  TextPathCommand command;
  command.kind = TextPathCommandKind::MoveTo;
  command.end = {x, y};
  return command;
}

TextPathCommand Line(const float x, const float y) {
  TextPathCommand command;
  command.kind = TextPathCommandKind::LineTo;
  command.end = {x, y};
  return command;
}

TextPathCommand Cubic(const float control1X, const float control1Y,
                      const float control2X, const float control2Y,
                      const float endX, const float endY) {
  TextPathCommand command;
  command.kind = TextPathCommandKind::CubicTo;
  command.control1 = {control1X, control1Y};
  command.control2 = {control2X, control2Y};
  command.end = {endX, endY};
  return command;
}

TextPathCommand Close() {
  TextPathCommand command;
  command.kind = TextPathCommandKind::Close;
  return command;
}

bool ValidPathPoint(const TextPathPoint &point) noexcept {
  return Finite(point.x) && Finite(point.y) && std::fabs(point.x) <= 8.0F &&
         std::fabs(point.y) <= 8.0F;
}

bool ValidBackdropSource(const TextBackdropSource &source) noexcept {
  return std::visit(
      [](const auto &value) {
        using Source = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Source, RoundedRectBackdrop>) {
          if (value.family < BubbleFamily::RoundRect ||
              value.family > BubbleFamily::CaptionBar ||
              value.tail.edge < BubbleTailEdge::None ||
              value.tail.edge > BubbleTailEdge::Right ||
              !ValidMaterialBinding(value.fill) || value.strokes.size() > 8U ||
              !Finite(value.cornerRadius) || value.cornerRadius < 0.0F ||
              value.cornerRadius > 8192.0F ||
              !Finite(value.tail.position) || value.tail.position < 0.0F ||
              value.tail.position > 1.0F || !Finite(value.tail.width) ||
              value.tail.width < 0.0F || value.tail.width > 8192.0F ||
              !Finite(value.tail.length) || value.tail.length < 0.0F ||
              value.tail.length > 8192.0F ||
              (value.authoredWidth &&
               (!Finite(*value.authoredWidth) || *value.authoredWidth <= 0.0F ||
                *value.authoredWidth > 65'536.0F)) ||
              (value.authoredHeight &&
               (!Finite(*value.authoredHeight) ||
                *value.authoredHeight <= 0.0F ||
                *value.authoredHeight > 65'536.0F))) {
            return false;
          }
          std::unordered_set<std::string> strokeIds;
          return std::all_of(
              value.strokes.begin(), value.strokes.end(),
              [&](const TextStrokeLayer &stroke) {
                return ValidStroke(stroke) &&
                       strokeIds.insert(stroke.layerId).second;
              });
        } else if constexpr (std::is_same_v<Source, NineSliceBackdrop>) {
          return ValidTexture(value.asset) && ValidInsets(value.capInsets) &&
                 ValidInsets(value.contentInsets) &&
                 Finite(value.minimumContentWidth) &&
                 value.minimumContentWidth >= 0.0F &&
                 value.minimumContentWidth <= 65'536.0F &&
                 Finite(value.minimumContentHeight) &&
                 value.minimumContentHeight >= 0.0F &&
                 value.minimumContentHeight <= 65'536.0F &&
                 value.stretchMode >= TextBackdropStretchMode::NineSlice &&
                 value.stretchMode <= TextBackdropStretchMode::TileCenter &&
                 ValidColor(value.fallbackColor);
        } else if constexpr (std::is_same_v<Source, VectorBackdrop>) {
          return ValidTexture(value.asset) &&
                 value.fit >= TextBackdropFitPolicy::InkBounds &&
                 value.fit <= TextBackdropFitPolicy::LayoutBounds &&
                 ValidColor(value.fallbackColor);
        } else {
          return ValidTexture(value.asset) && ValidInsets(value.contentInsets) &&
                 value.timeSource >= TextBackdropTimeSource::Composition &&
                 value.timeSource <= TextBackdropTimeSource::Source &&
                 value.playback >= TextBackdropPlaybackMode::Once &&
                 value.playback <= TextBackdropPlaybackMode::Hold &&
                 value.sourceInUs >= 0 && value.sourceOutUs > value.sourceInUs &&
                 value.phaseUs >= 0 && ValidColor(value.fallbackColor);
        }
      },
      source);
}

} // namespace

bool ConfigureTextPathPreset(TextPath &path, const std::string_view presetId,
                             std::string *error) {
  TextPath configured;
  if (presetId.empty() || presetId == "none") {
    path = std::move(configured);
    if (error)
      error->clear();
    return true;
  }

  configured.enabled = true;
  configured.geometryId = "builtin.text-path." + std::string(presetId);
  configured.presetId = std::string(presetId);
  configured.rotateToTangent = true;
  configured.keepUpright = true;
  configured.overflow = TextPathOverflow::ScaleToFit;
  if (presetId == "line") {
    configured.commands = {Move(0.0F, 0.5F), Line(1.0F, 0.5F)};
  } else if (presetId == "arc_up") {
    configured.commands = {
        Move(0.0F, 0.68F),
        Cubic(0.22F, 0.06F, 0.78F, 0.06F, 1.0F, 0.68F),
    };
  } else if (presetId == "arc_down") {
    configured.commands = {
        Move(0.0F, 0.32F),
        Cubic(0.22F, 0.94F, 0.78F, 0.94F, 1.0F, 0.32F),
    };
  } else if (presetId == "wave") {
    configured.commands = {
        Move(0.0F, 0.5F),
        Cubic(0.08F, 0.08F, 0.17F, 0.08F, 0.25F, 0.5F),
        Cubic(0.33F, 0.92F, 0.42F, 0.92F, 0.5F, 0.5F),
        Cubic(0.58F, 0.08F, 0.67F, 0.08F, 0.75F, 0.5F),
        Cubic(0.83F, 0.92F, 0.92F, 0.92F, 1.0F, 0.5F),
    };
  } else if (presetId == "circle") {
    constexpr float k = 0.55228475F;
    configured.commands = {
        Move(0.5F, 0.02F),
        Cubic(0.5F + 0.48F * k, 0.02F, 0.98F, 0.5F - 0.48F * k,
              0.98F, 0.5F),
        Cubic(0.98F, 0.5F + 0.48F * k, 0.5F + 0.48F * k, 0.98F,
              0.5F, 0.98F),
        Cubic(0.5F - 0.48F * k, 0.98F, 0.02F, 0.5F + 0.48F * k,
              0.02F, 0.5F),
        Cubic(0.02F, 0.5F - 0.48F * k, 0.5F - 0.48F * k, 0.02F,
              0.5F, 0.02F),
        Close(),
    };
    configured.loop = true;
  } else {
    return Fail(error, "text path preset is unsupported");
  }
  path = std::move(configured);
  if (error)
    error->clear();
  return true;
}

bool ValidateTextLayerAppearance(const TextLayerAppearance &appearance,
                                 std::string *error) {
  if (!Finite(appearance.globalAlpha) || appearance.globalAlpha < 0.0F ||
      appearance.globalAlpha > 1.0F) {
    return Fail(error, "text appearance global alpha is invalid");
  }
  if (appearance.backdrops.layers.size() > 64U)
    return Fail(error, "text backdrop layer budget is invalid");

  std::unordered_set<std::string> layerIds;
  bool hasFrame = false;
  bool hasBubble = false;
  for (const auto &layer : appearance.backdrops.layers) {
    if (layer.layerId.empty() || layer.layerId.size() > 512U ||
        !IsValidUtf8(layer.layerId) ||
        !layerIds.insert(layer.layerId).second ||
        layer.channel < TextBackdropChannel::Frame ||
        layer.channel > TextBackdropChannel::Custom ||
        layer.fit < TextBackdropFitPolicy::InkBounds ||
        layer.fit > TextBackdropFitPolicy::LayoutBounds ||
        layer.fitMode < TextBackdropFitMode::Width ||
        layer.fitMode > TextBackdropFitMode::Stretch ||
        layer.sourceIntrinsicWidth.has_value() !=
            layer.sourceIntrinsicHeight.has_value() ||
        (layer.sourceIntrinsicWidth &&
         (!Finite(*layer.sourceIntrinsicWidth) ||
          !Finite(*layer.sourceIntrinsicHeight) ||
          *layer.sourceIntrinsicWidth <= 0.0F ||
          *layer.sourceIntrinsicHeight <= 0.0F ||
          *layer.sourceIntrinsicWidth > 65'536.0F ||
          *layer.sourceIntrinsicHeight > 65'536.0F)) ||
        !Finite(layer.sourcePivotX) || !Finite(layer.sourcePivotY) ||
        layer.sourcePivotX < 0.0F || layer.sourcePivotX > 1.0F ||
        layer.sourcePivotY < 0.0F || layer.sourcePivotY > 1.0F ||
        !ValidInsets(layer.padding) || !ValidInsets(layer.expand) ||
        !ValidInsets(layer.sourceOutsets) ||
        !Finite(layer.transform.offsetX) ||
        !Finite(layer.transform.offsetY) || !Finite(layer.transform.scaleX) ||
        !Finite(layer.transform.scaleY) || layer.transform.scaleX <= 0.0F ||
        layer.transform.scaleY <= 0.0F ||
        layer.transform.scaleX > 64.0F || layer.transform.scaleY > 64.0F ||
        !Finite(layer.transform.rotationDegrees) ||
        std::fabs(layer.transform.rotationDegrees) > 3'600.0F ||
        !Finite(layer.transform.opacity) || layer.transform.opacity < 0.0F ||
        layer.transform.opacity > 1.0F || !ValidBackdropSource(layer.source) ||
        (layer.materialOverride &&
         !ValidMaterialBinding(*layer.materialOverride)) ||
        layer.animationClips.size() > 16U) {
      return Fail(error, "text backdrop layer is invalid");
    }
    if ((layer.channel == TextBackdropChannel::Frame && hasFrame) ||
        (layer.channel == TextBackdropChannel::Bubble && hasBubble)) {
      return Fail(error, "frame and bubble channels must be uniquely owned");
    }
    hasFrame |= layer.channel == TextBackdropChannel::Frame;
    hasBubble |= layer.channel == TextBackdropChannel::Bubble;
    std::unordered_set<std::string> clipIds;
    std::unordered_set<unsigned> phases;
    for (const auto &clip : layer.animationClips) {
      std::string clipError;
      if (!ValidateTextLayerAnimationClip(clip, &clipError))
        return Fail(error, clipError.c_str());
      if (!clipIds.insert(clip.clipId).second ||
          !phases.insert(static_cast<unsigned>(clip.phase)).second) {
        return Fail(error,
                    "backdrop animation identities and phases must be unique");
      }
    }
  }

  const bool validSourceDesign =
      appearance.sdfMaterial.sourceDesignWidth > 0.0F &&
      appearance.sdfMaterial.sourceDesignWidth <= 65'536.0F &&
      appearance.sdfMaterial.sourceDesignHeight > 0.0F &&
      appearance.sdfMaterial.sourceDesignHeight <= 65'536.0F;
  const bool emptySourceDesign =
      appearance.sdfMaterial.sourceDesignWidth == 0.0F &&
      appearance.sdfMaterial.sourceDesignHeight == 0.0F;
  if (!Finite(appearance.sdfMaterial.distanceRange) ||
      appearance.sdfMaterial.sourceCreationComponent >
          TextSourceCreationComponent::SdfText ||
      appearance.sdfMaterial.distanceRange < 1.0F ||
      appearance.sdfMaterial.distanceRange > 4096.0F ||
      !Finite(appearance.sdfMaterial.rasterDistanceRange) ||
      appearance.sdfMaterial.rasterDistanceRange < 1.0F ||
      appearance.sdfMaterial.rasterDistanceRange > 4096.0F ||
      !Finite(appearance.sdfMaterial.smoothingScale) ||
      appearance.sdfMaterial.smoothingScale <= 0.0F ||
      appearance.sdfMaterial.smoothingScale > 4.0F ||
      !Finite(appearance.sdfMaterial.sourceDesignWidth) ||
      !Finite(appearance.sdfMaterial.sourceDesignHeight) ||
      (appearance.sdfMaterial.enabled
           ? !validSourceDesign
           : (!validSourceDesign && !emptySourceDesign))) {
    return Fail(error, "text SDF material settings are invalid");
  }
  if ((appearance.visualExtent.maximumExtentWidth &&
       (!Finite(*appearance.visualExtent.maximumExtentWidth) ||
        *appearance.visualExtent.maximumExtentWidth <= 0.0F)) ||
      (appearance.visualExtent.maximumExtentHeight &&
       (!Finite(*appearance.visualExtent.maximumExtentHeight) ||
        *appearance.visualExtent.maximumExtentHeight <= 0.0F))) {
    return Fail(error, "text visual extent policy is invalid");
  }
  if (!Finite(appearance.bend.amount) || appearance.bend.amount < -1.0F ||
      appearance.bend.amount > 1.0F) {
    return Fail(error, "text bend appearance is invalid");
  }

  const auto &path = appearance.path;
  if (appearance.bend.enabled && path.enabled)
    return Fail(error, "text bend and text path cannot be enabled together");
  if (path.geometryId.size() > 512U || path.presetId.size() > 512U ||
      !IsValidUtf8(path.geometryId) || !IsValidUtf8(path.presetId) ||
      path.commands.size() > 256U || !Finite(path.startOffset) ||
      std::fabs(path.startOffset) > 16.0F || !Finite(path.baselineOffset) ||
      std::fabs(path.baselineOffset) > 65'536.0F ||
      path.overflow < TextPathOverflow::Clip ||
      path.overflow > TextPathOverflow::ScaleToFit) {
    return Fail(error, "text path authoring is invalid");
  }
  if (path.enabled) {
    if (path.geometryId.empty() || path.commands.size() < 2U ||
        path.commands.front().kind != TextPathCommandKind::MoveTo) {
      return Fail(error, "enabled text path geometry is incomplete");
    }
    bool hasDrawable = false;
    bool contourOpen = false;
    for (const auto &command : path.commands) {
      switch (command.kind) {
      case TextPathCommandKind::MoveTo:
        if (!ValidPathPoint(command.end))
          return Fail(error, "text path move point is invalid");
        contourOpen = true;
        break;
      case TextPathCommandKind::LineTo:
        if (!contourOpen || !ValidPathPoint(command.end))
          return Fail(error, "text path line is invalid");
        hasDrawable = true;
        break;
      case TextPathCommandKind::QuadraticTo:
        if (!contourOpen || !ValidPathPoint(command.control1) ||
            !ValidPathPoint(command.end)) {
          return Fail(error, "text path quadratic segment is invalid");
        }
        hasDrawable = true;
        break;
      case TextPathCommandKind::CubicTo:
        if (!contourOpen || !ValidPathPoint(command.control1) ||
            !ValidPathPoint(command.control2) ||
            !ValidPathPoint(command.end)) {
          return Fail(error, "text path cubic segment is invalid");
        }
        hasDrawable = true;
        break;
      case TextPathCommandKind::Close:
        if (!contourOpen)
          return Fail(error, "text path close has no open contour");
        contourOpen = false;
        break;
      }
    }
    if (!hasDrawable)
      return Fail(error, "text path has no drawable segment");
  }
  if (error)
    error->clear();
  return true;
}

} // namespace videocut::text
