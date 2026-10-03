#include "text/TextRenderPipeline.h"

namespace videocut::skia_runtime::internal::text_lane {


std::uint64_t NextTextPostEffectGraphInstanceId() noexcept {
  static std::atomic<std::uint64_t> next{1U};
  auto value = next.fetch_add(1U, std::memory_order_relaxed);
  if (value == 0U)
    value = next.fetch_add(1U, std::memory_order_relaxed);
  return value;
}


Diagnostic MakeDiagnostic(std::string code, DiagnosticSeverity severity,
                          std::string stage, std::string subject,
                          std::string message) {
  return {std::move(code), severity, std::move(stage), std::move(subject),
          std::move(message)};
}


bool IsCanceled(const text::CancelCheck &cancel) { return cancel && cancel(); }


std::uint8_t ColorByte(const float value) {
  return static_cast<std::uint8_t>(
      std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}


SkColor ToSkColor(const text::Color &color, const float opacity) {
  return SkColorSetARGB(
      ColorByte(color.alpha * std::clamp(opacity, 0.0F, 1.0F)),
      ColorByte(color.red), ColorByte(color.green), ColorByte(color.blue));
}


SkBlendMode ToSkBlendMode(const text::TextBlendMode mode) noexcept {
  switch (mode) {
  case text::TextBlendMode::SourceOver:
    return SkBlendMode::kSrcOver;
  case text::TextBlendMode::Multiply:
    return SkBlendMode::kMultiply;
  case text::TextBlendMode::Screen:
    return SkBlendMode::kScreen;
  case text::TextBlendMode::Overlay:
    return SkBlendMode::kOverlay;
  case text::TextBlendMode::Add:
    return SkBlendMode::kPlus;
  case text::TextBlendMode::Darken:
    return SkBlendMode::kDarken;
  case text::TextBlendMode::Lighten:
    return SkBlendMode::kLighten;
  }
  return SkBlendMode::kSrcOver;
}



SkFontStyle ToSkFontStyle(const text::FontSpec &font) {
  return SkFontStyle(std::clamp(font.weight, 1, 1000),
                     std::clamp(font.width, 1, 9), ToSkSlant(font.slant));
}



std::string TextureKey(const text::TextureReference &reference) {
  IdentityBuilder identity;
  identity.Add(static_cast<unsigned>(reference.sourceKind));
  identity.AddString(reference.assetId);
  identity.AddString(reference.digest);
  identity.AddString(reference.mediaType);
  identity.AddString(reference.colorSpace);
  identity.Add(static_cast<unsigned>(reference.orientation));
  return identity.Finish();
}


bool IsSvgMediaType(const std::string_view mediaType) noexcept {
  return mediaType == "image/svg+xml" || mediaType == "application/svg+xml";
}


bool IsLottieMediaType(const std::string_view mediaType) noexcept {
  return mediaType == "application/json" ||
         mediaType == "application/lottie+json" ||
         mediaType == "application/vnd.lottie+json";
}


bool IsStreamingMediaType(const std::string_view mediaType) noexcept {
  return mediaType == "image/gif" || mediaType == "image/apng" ||
         mediaType.starts_with("video/");
}


RuntimeAssetKind RuntimeKindForMediaType(const std::string_view mediaType) {
  if (IsSvgMediaType(mediaType) || IsLottieMediaType(mediaType))
    return RuntimeAssetKind::Vector;
  if (IsStreamingMediaType(mediaType))
    return RuntimeAssetKind::NestedAnimation;
  return RuntimeAssetKind::Image;
}


RuntimeAssetKind RuntimeKindForFrameResource(
    const text::TextEffectResourceKind kind) noexcept {
  using Kind = text::TextEffectResourceKind;
  switch (kind) {
  case Kind::Font:
    return RuntimeAssetKind::Font;
  case Kind::Vector:
    return RuntimeAssetKind::Vector;
  case Kind::Animated:
    return RuntimeAssetKind::Image;
  case Kind::Mesh:
    return RuntimeAssetKind::Mesh;
  case Kind::FloatTexture:
    return RuntimeAssetKind::FloatTexture;
  case Kind::Unspecified:
  case Kind::Texture:
    return RuntimeAssetKind::Image;
  }
  return RuntimeAssetKind::Image;
}


text::TextAssetKind ProjectKindForFrameResource(
    const text::TextEffectResourceKind kind) noexcept {
  using Kind = text::TextEffectResourceKind;
  switch (kind) {
  case Kind::Font:
    return text::TextAssetKind::Font;
  case Kind::Mesh:
    return text::TextAssetKind::Mesh;
  case Kind::FloatTexture:
    return text::TextAssetKind::FloatTexture;
  case Kind::Unspecified:
  case Kind::Texture:
  case Kind::Vector:
  case Kind::Animated:
    return text::TextAssetKind::Image;
  }
  return text::TextAssetKind::Image;
}


SkTileMode ToSkTileMode(const text::PaintSpread spread) noexcept {
  switch (spread) {
  case text::PaintSpread::Clamp:
    return SkTileMode::kClamp;
  case text::PaintSpread::Repeat:
    return SkTileMode::kRepeat;
  case text::PaintSpread::Mirror:
    return SkTileMode::kMirror;
  }
  return SkTileMode::kClamp;
}


SkRect MaterialCoordinateBounds(const text::TextMaterialCoordinates &coordinates,
                                const SkRect &layoutBounds,
                                const SkRect &textBounds,
                                const SkRect &graphemeBounds) {
  SkRect result = coordinates.coordinateSpace == text::PaintCoordinateSpace::LayoutBox
                      ? layoutBounds
                  : coordinates.coordinateSpace ==
                            text::PaintCoordinateSpace::TextBounds
                      ? textBounds
                      : graphemeBounds;
  const float outset = std::max(0.0F, coordinates.coordinateOutset);
  result.outset(outset, outset);
  const float coordinateScale =
      std::isfinite(coordinates.coordinateScale) &&
              coordinates.coordinateScale > 0.0F
          ? coordinates.coordinateScale
          : 1.0F;
  if (coordinateScale != 1.0F) {
    result = SkRect::MakeXYWH(
        result.centerX() - result.width() / coordinateScale * 0.5F,
        result.centerY() - result.height() / coordinateScale * 0.5F,
        result.width() / coordinateScale,
        result.height() / coordinateScale);
  }
  if (result.isEmpty())
    result = SkRect::MakeWH(1.0F, 1.0F);
  return result;
}


sk_sp<SkShader> MakeGradientShader(
    const std::vector<text::GradientStop> &stops, const bool radial,
    const float startX, const float startY, const float endX,
    const float endY, const float centerX, const float centerY,
    const float radius, const text::PaintSpread spread,
    const text::GradientSampling sampling, const SkRect &bounds) {
  if (stops.size() < 2U)
    return {};
  std::vector<SkColor4f> colors;
  std::vector<float> positions;
  if (sampling == text::GradientSampling::Rgba8Lut256) {
    colors.reserve(256U);
    positions.reserve(256U);
    for (std::size_t index = 0U; index < 256U; ++index) {
      const float position = static_cast<float>(index) / 255.0F;
      auto upper = std::upper_bound(
          stops.begin(), stops.end(), position,
          [](const float value, const text::GradientStop &stop) {
            return value < stop.offset;
          });
      const auto *right = upper == stops.end() ? &stops.back() : &*upper;
      const auto *left = upper == stops.begin() ? right : &*std::prev(upper);
      const float span = std::max(1.0e-6F, right->offset - left->offset);
      const float mix = std::clamp((position - left->offset) / span, 0.0F,
                                   1.0F);
      const auto channel = [&](const float begin, const float end) {
        return static_cast<float>(ColorByte(begin + (end - begin) * mix)) /
               255.0F;
      };
      colors.push_back({channel(left->color.red, right->color.red),
                        channel(left->color.green, right->color.green),
                        channel(left->color.blue, right->color.blue),
                        channel(left->color.alpha, right->color.alpha)});
      positions.push_back(position);
    }
  } else {
    colors.reserve(stops.size());
    positions.reserve(stops.size());
    for (const auto &stop : stops) {
      colors.push_back({stop.color.red, stop.color.green, stop.color.blue,
                        stop.color.alpha});
      positions.push_back(stop.offset);
    }
  }
  SkGradient::Interpolation interpolation;
  interpolation.fInPremul = SkGradient::Interpolation::InPremul::kYes;
  interpolation.fColorSpace = SkGradient::Interpolation::ColorSpace::kSRGB;
  const SkGradient gradient(
      SkGradient::Colors(
          SkSpan<const SkColor4f>(colors.data(), colors.size()),
          SkSpan<const float>(positions.data(), positions.size()),
          ToSkTileMode(spread), SkColorSpace::MakeSRGB()),
      interpolation);
  if (radial) {
    return SkShaders::RadialGradient(
        SkPoint::Make(bounds.left() + centerX * bounds.width(),
                      bounds.top() + centerY * bounds.height()),
        std::max(0.001F, radius * std::max(bounds.width(), bounds.height())),
        gradient);
  }
  const SkPoint points[]{
      SkPoint::Make(bounds.left() + startX * bounds.width(),
                    bounds.top() + startY * bounds.height()),
      SkPoint::Make(bounds.left() + endX * bounds.width(),
                    bounds.top() + endY * bounds.height())};
  return SkShaders::LinearGradient(points, gradient);
}


sk_sp<SkImage> ResolveVisualTextureFrame(
    const text::TextureReference &reference, const std::int64_t localTimeUs,
    TextVisualAssetStore &assets, std::string &sampleIdentity,
    std::string &error) {
  auto *asset = assets.Resolve(reference, error);
  return asset ? assets.SampleRaster(*asset, localTimeUs, sampleIdentity, error)
               : sk_sp<SkImage>{};
}


sk_sp<SkImage> ResolveTextureMaterialFrame(
    const text::TextureTextMaterial &material,
    const std::size_t materialUnitIndex, const std::int64_t localTimeUs,
    TextVisualAssetStore &assets, std::string &sampleIdentity,
    std::string &error) {
  auto image = ResolveVisualTextureFrame(material.texture, localTimeUs, assets,
                                         sampleIdentity, error);
  if (!image)
    return {};
  // TEXT_LETTER_MAT receives the complete authored texture atlas.  Its
  // vertex program selects the cell from grid + materialIndex; pre-subsetting
  // here destroys both the native grid phase and the per-Letter index.
  static_cast<void>(materialUnitIndex);
  return image;
}


bool ConfigureTextMaterialPaint(
    const text::TextMaterialBinding &binding, const SkRect &layoutBounds,
    const SkRect &textBounds, const SkRect &graphemeBounds,
    const std::int64_t localTimeUs, TextVisualAssetStore &assets,
    SkPaint &paint, std::string &sampleIdentity, std::string &error,
    const std::size_t materialUnitIndex) {
  paint.setAntiAlias(true);
  const auto &material = ResolveTextMaterial(binding);
  bool valid = true;
  std::visit(
      [&](const auto &value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, text::SolidTextMaterial>) {
          paint.setColor(ToSkColor(value.color));
        } else if constexpr (std::is_same_v<
                                 Value, text::LinearGradientTextMaterial>) {
          const auto bounds = MaterialCoordinateBounds(
              value.coordinates, layoutBounds, textBounds, graphemeBounds);
          paint.setShader(MakeGradientShader(
              value.stops, false, value.startX, value.startY, value.endX,
              value.endY, 0.0F, 0.0F, 0.0F, value.spread, value.sampling,
              bounds));
        } else if constexpr (std::is_same_v<
                                 Value, text::RadialGradientTextMaterial>) {
          const auto bounds = MaterialCoordinateBounds(
              value.coordinates, layoutBounds, textBounds, graphemeBounds);
          paint.setShader(MakeGradientShader(
              value.stops, true, 0.0F, 0.0F, 0.0F, 0.0F, value.centerX,
              value.centerY, value.radius, value.spread, value.sampling,
              bounds));
        } else {
          std::string frameIdentity;
          auto image = ResolveTextureMaterialFrame(
              value, materialUnitIndex, localTimeUs, assets, frameIdentity,
              error);
          if (!image) {
            valid = false;
            return;
          }
          sampleIdentity = frameIdentity;
          const auto bounds = MaterialCoordinateBounds(
              value.coordinates, layoutBounds, textBounds, graphemeBounds);
          const float imageWidth = static_cast<float>(image->width());
          const float imageHeight = static_cast<float>(image->height());
          float scaleX = bounds.width() / imageWidth;
          float scaleY = bounds.height() / imageHeight;
          if (value.fit == text::TextureFit::Tile) {
            // Repeat in source-pixel units; cover fitting would leave only
            // one atlas-sized image inside the text coordinate bounds.
            scaleX = 1.0F;
            scaleY = 1.0F;
          } else if (value.fit != text::TextureFit::Stretch) {
            const float fit = value.fit == text::TextureFit::Contain
                                  ? std::min(scaleX, scaleY)
                                  : std::max(scaleX, scaleY);
            scaleX = fit;
            scaleY = fit;
          }
          scaleX *= value.scale * (value.flipX ? -1.0F : 1.0F);
          scaleY *= value.scale * (value.flipY ? -1.0F : 1.0F);
          SkMatrix matrix;
          matrix.setScale(scaleX, scaleY);
          matrix.postTranslate(
              bounds.centerX() - imageWidth * scaleX * 0.5F +
                  value.offsetX * bounds.width(),
              bounds.centerY() - imageHeight * scaleY * 0.5F +
                  value.offsetY * bounds.height());
          if (value.rotationDegrees != 0.0F)
            matrix.postRotate(value.rotationDegrees, bounds.centerX(),
                              bounds.centerY());
          const float orientationDegrees =
              value.texture.orientation == text::TextureOrientation::Right
                  ? 90.0F
              : value.texture.orientation == text::TextureOrientation::Down
                  ? 180.0F
              : value.texture.orientation == text::TextureOrientation::Left
                  ? -90.0F
                  : 0.0F;
          if (orientationDegrees != 0.0F)
            matrix.postRotate(orientationDegrees, bounds.centerX(),
                              bounds.centerY());
          const auto tile = value.fit == text::TextureFit::Tile
                                ? SkTileMode::kRepeat
                            : value.fit == text::TextureFit::Contain
                                ? SkTileMode::kDecal
                                : SkTileMode::kClamp;
          auto shader = image->makeShader(
              tile, tile,
              SkSamplingOptions(SkFilterMode::kLinear,
                                SkMipmapMode::kLinear),
              &matrix);
          if (value.textureOpacity != 1.0F) {
            shader = SkShaders::Blend(
                SkBlendMode::kDstIn, std::move(shader),
                SkShaders::Color(SkColor4f{1.0F, 1.0F, 1.0F,
                                          value.textureOpacity}, nullptr));
          }
          sk_sp<SkShader> underlay;
          if (value.underlayColor) {
            underlay = SkShaders::Color(ToSkColor(*value.underlayColor));
          } else if (value.underlayGradient.size() >= 2U) {
            const auto &projection = value.underlayGradientProjection;
            underlay = MakeGradientShader(
                value.underlayGradient, false, projection.startX,
                projection.startY, projection.endX, projection.endY,
                0.0F, 0.0F, 0.0F, projection.spread,
                projection.sampling, bounds);
          }
          if (underlay) {
            shader = value.sourceAlpha
                         ? SkShaders::Blend(SkBlendMode::kSrcIn,
                                            std::move(shader),
                                            std::move(underlay))
                         : SkShaders::Blend(SkBlendMode::kSrcOver,
                                            std::move(underlay),
                                            std::move(shader));
          }
          paint.setShader(std::move(shader));
          paint.setAlphaf(std::clamp(value.opacity, 0.0F, 1.0F));
        }
      },
      material);
  if (!valid)
    return false;

  if (const auto *slot = std::get_if<text::EditableTextStyleSlot>(&binding);
      slot && slot->replacementMask) {
    std::string maskIdentity;
    auto mask = ResolveVisualTextureFrame(*slot->replacementMask, localTimeUs,
                                          assets, maskIdentity, error);
    if (!mask)
      return false;
    const SkRect bounds = textBounds.isEmpty() ? layoutBounds : textBounds;
    SkMatrix matrix;
    matrix.setRectToRect(SkRect::MakeIWH(mask->width(), mask->height()), bounds,
                         SkMatrix::kFill_ScaleToFit);
    auto maskShader = mask->makeShader(
        SkTileMode::kClamp, SkTileMode::kClamp,
        SkSamplingOptions(SkFilterMode::kLinear), &matrix);
    const auto materialShader = paint.refShader();
    if (materialShader) {
      // Shader-backed materials can be masked directly.  A solid material,
      // however, keeps its color in SkPaint and has no refShader; silently
      // skipping the mask makes an editable replacement behave differently
      // from gradient/texture fallbacks.  Lift the solid color into an opaque
      // shader and preserve its alpha on the paint so DstIn has the same
      // premultiplied coverage contract for every material variant.
      paint.setShader(SkShaders::Blend(SkBlendMode::kDstIn,
                                       std::move(materialShader),
                                       std::move(maskShader)));
    } else {
      const auto color = paint.getColor();
      auto solidShader = SkShaders::Color(SkColorSetA(color, 0xff));
      paint.setShader(SkShaders::Blend(SkBlendMode::kDstIn,
                                       std::move(solidShader),
                                       std::move(maskShader)));
      paint.setAlphaf(static_cast<float>(SkColorGetA(color)) / 255.0F);
    }
    sampleIdentity += "|mask:" + maskIdentity;
  }
  return true;
}

TextVisualAssetStore::TextVisualAssetStore(const SkiaRuntimeConfig &config,
                       text::TextAssetResolver::Holder projectAssets)
      : config_(config), projectAssets_(std::move(projectAssets)) {}

void TextVisualAssetStore::Reset() {
    assets_.clear();
    residentBytes_ = 0U;
  }

std::size_t TextVisualAssetStore::residentBytes() const noexcept {
    return residentBytes_;
  }

ResolvedVisualAsset *TextVisualAssetStore::Resolve(const text::TextureReference &reference,
                               std::string &error,
                               const RasterDecodeUsage usage) {
    const auto key = TextureKey(reference) + "|decode:" +
                     std::to_string(static_cast<unsigned>(usage));
    const auto existing = assets_.find(key);
    if (existing != assets_.end())
      return existing->second.get();

    auto asset = std::make_unique<ResolvedVisualAsset>();
    asset->reference = reference;
    if (!ResolveBytes(reference, asset->bytes, asset->mediaType, error))
      return nullptr;
    if (!Parse(*asset, usage, error))
      return nullptr;
    residentBytes_ += asset->bytes ? asset->bytes->size() : 0U;
    if (asset->raster) {
      residentBytes_ += asset->raster->imageInfo().computeMinByteSize();
    }
    auto *result = asset.get();
    assets_.emplace(key, std::move(asset));
    return result;
  }

ResolvedVisualAsset *TextVisualAssetStore::ResolveUnqualified(const std::string &assetId,
                                          const std::string &digest,
                                          std::string &error) {
    text::TextureReference builtin;
    builtin.sourceKind = text::TextureSourceKind::Builtin;
    builtin.assetId = assetId;
    builtin.digest = digest;
    builtin.mediaType = "application/octet-stream";
    if (auto *resolved = Resolve(builtin, error))
      return resolved;

    text::TextureReference project = builtin;
    project.sourceKind = text::TextureSourceKind::ProjectManaged;
    error.clear();
    return Resolve(project, error);
  }

sk_sp<SkImage> TextVisualAssetStore::SampleRaster(ResolvedVisualAsset &asset,
                              const std::int64_t localTimeUs,
                              std::string &sampleIdentity,
                              std::string &error) {
    if (asset.raster) {
      sampleIdentity = TextureKey(asset.reference);
      return asset.raster;
    }
    if (asset.mediaType == kQtPackedAlphaMp4ResourceMediaType &&
        asset.immutableLottieResources &&
        !asset.qtFollowerResourceId.empty()) {
      if (asset.sampledFrame && asset.sampledTimeUs == localTimeUs) {
        sampleIdentity = asset.sampledFrameIdentity;
        return asset.sampledFrame;
      }
      QtFollowerDirectFrame frame;
      if (!asset.immutableLottieResources->ResolveQtFollowerDirectFrame(
              asset.qtFollowerResourceId, localTimeUs, std::nullopt, frame,
              error)) {
        return {};
      }
      if (frame.contractVersion != kQtFollowerDirectFrameContractVersion ||
          frame.width == 0U || frame.height == 0U ||
          frame.rowBytes != static_cast<std::size_t>(frame.width) * 4U ||
          !frame.pixels ||
          frame.pixels->size() != frame.rowBytes * frame.height) {
        error = "text packed-alpha media frame identity is invalid";
        return {};
      }
      auto data =
          RetainImmutableSkData(frame.pixels);
      const auto info = SkImageInfo::Make(
          static_cast<int>(frame.width), static_cast<int>(frame.height),
          kRGBA_8888_SkColorType, kPremul_SkAlphaType, nullptr);
      auto image = data ? SkImages::RasterFromData(info, std::move(data),
                                                   frame.rowBytes)
                        : nullptr;
      if (!image) {
        error = "text packed-alpha media frame publication failed";
        return {};
      }
      asset.intrinsicWidth = static_cast<float>(frame.width);
      asset.intrinsicHeight = static_cast<float>(frame.height);
      asset.sampledTimeUs = localTimeUs;
      asset.sampledFrame = image;
      asset.sampledFrameIdentity =
          frame.frameIdentity + "|media-video-associated";
      sampleIdentity = asset.sampledFrameIdentity;
      error.clear();
      return image;
    }
    if (!asset.stream) {
      error = "text asset is not a raster or streaming image: " +
              asset.reference.assetId;
      return {};
    }
    if (asset.sampledFrame && asset.sampledTimeUs == localTimeUs) {
      sampleIdentity = asset.sampledFrameIdentity;
      return asset.sampledFrame;
    }
    RuntimeAnimatedImageFrame frame;
    if (!asset.stream->GetFrame(localTimeUs, frame, error) || !frame.bytes ||
        frame.width == 0U || frame.height == 0U ||
        frame.rowBytes < frame.width * 4U ||
        frame.bytes->size() !=
            static_cast<std::size_t>(frame.rowBytes) * frame.height) {
      if (error.empty())
        error = "animated text asset did not return a valid frame";
      return {};
    }
    const auto info = SkImageInfo::Make(
        static_cast<int>(frame.width), static_cast<int>(frame.height),
        kRGBA_8888_SkColorType, kUnpremul_SkAlphaType,
        SkColorSpace::MakeSRGB());
    auto data = RetainImmutableSkData(frame.bytes);
    auto image = data ? SkImages::RasterFromData(info, std::move(data),
                                                 frame.rowBytes)
                      : nullptr;
    if (!image) {
      error = "animated text frame could not be admitted by Skia";
      return {};
    }
    asset.sampledTimeUs = localTimeUs;
    asset.sampledFrame = image;
    asset.sampledFrameIdentity = frame.frameIdentity.empty()
                                     ? TextureKey(asset.reference) + "|" +
                                           std::to_string(localTimeUs)
                                     : frame.frameIdentity;
    sampleIdentity = asset.sampledFrameIdentity;
    return image;
  }

sk_sp<SkImage> TextVisualAssetStore::SampleQtFollowerDirectRaster(
      ResolvedVisualAsset &asset, const std::int64_t localTimeUs,
      std::string &sampleIdentity, std::string &error) {
    if (!asset.immutableLottieResources ||
        asset.qtFollowerResourceId.empty() ||
        asset.qtFollowerFrameCount == 0U ||
        asset.qtFollowerRenderSize == 0U || asset.durationUs <= 0) {
      error = "text follower direct-frame metadata is unavailable";
      return {};
    }
    if (asset.sampledFrame && asset.sampledTimeUs == localTimeUs) {
      sampleIdentity = asset.sampledFrameIdentity;
      return asset.sampledFrame;
    }

    const double progress = std::clamp(
        static_cast<double>(localTimeUs) /
            static_cast<double>(asset.durationUs),
        0.0, 1.0);
    const double coordinate =
        progress * (static_cast<double>(asset.qtFollowerFrameCount) - 0.01);
    if (!std::isfinite(coordinate) || coordinate < 0.0) {
      error = "text follower direct-frame progress is invalid";
      return {};
    }
    const auto logicalFrame =
        static_cast<std::uint64_t>(std::floor(coordinate));
    if (logicalFrame >= asset.qtFollowerFrameCount) {
      error = "text follower direct-frame progress exceeded the source";
      return {};
    }
    // Qt VideoAnimSeq v3 publishes source zero for logical zero and the
    // preceding display frame for every positive fixed seek.
    const std::uint64_t sourceFrameIndex =
        logicalFrame > 0U ? logicalFrame - 1U : logicalFrame;
    QtFollowerDirectFrame frame;
    if (!asset.immutableLottieResources->ResolveQtFollowerDirectFrame(
            asset.qtFollowerResourceId, localTimeUs, sourceFrameIndex, frame,
            error)) {
      return {};
    }
    if (frame.contractVersion != kQtFollowerDirectFrameContractVersion ||
        frame.width != asset.qtFollowerRenderSize ||
        frame.height != asset.qtFollowerRenderSize ||
        frame.exactSourceFrameCount != asset.qtFollowerFrameCount ||
        frame.rowBytes != static_cast<std::size_t>(frame.width) * 4U ||
        !frame.pixels ||
        frame.pixels->size() != frame.rowBytes * frame.height) {
      error = "text follower direct-frame identity is invalid";
      return {};
    }
    // Qt's captured VideoAnimSeq merge publishes RGBA8 whose RGB is already
    // associated with alpha, and the following Sprite/RenderGroup path blends
    // those bytes directly.  Preserve that association through Skia.  A
    // round-trip through straight RGBA8 divides and requantizes low-alpha RGB,
    // then multiplies it again while drawing; that changes the colored fringe
    // of every animated follower even when geometry and source-frame selection
    // are exact.
    auto data =
        RetainImmutableSkData(frame.pixels);
    const auto info = SkImageInfo::Make(
        static_cast<int>(frame.width), static_cast<int>(frame.height),
        kRGBA_8888_SkColorType, kPremul_SkAlphaType,
        nullptr);
    auto image = data ? SkImages::RasterFromData(info, std::move(data),
                                                 frame.rowBytes)
                      : nullptr;
    if (!image) {
      error = "text follower direct-frame image publication failed";
      return {};
    }
    asset.sampledTimeUs = localTimeUs;
    asset.sampledFrame = image;
    asset.sampledFrameIdentity =
        frame.frameIdentity + "|associated:" +
        std::to_string(sourceFrameIndex);
    sampleIdentity = asset.sampledFrameIdentity;
    error.clear();
    return image;
  }

bool TextVisualAssetStore::AdmitOpaque(const std::string &assetId, const std::string &digest,
                   const RuntimeAssetKind builtinKind,
                   const text::TextAssetKind projectKind,
                   std::string &identity, std::string &error,
                   std::shared_ptr<const std::vector<std::uint8_t>>
                       *resolvedBytes,
                   std::string *resolvedMediaType,
                   const bool allowKindFallback) const {
    identity.clear();
    if (assetId.empty() || digest.empty()) {
      error = "opaque text execution resource has no closed identity";
      return false;
    }
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    std::string admittedDigest;
    std::string admittedMediaType;
    if (config_.assets) {
      RuntimeAsset resolved;
      bool didResolve = false;
      constexpr std::array<RuntimeAssetKind, 6> kinds{
          RuntimeAssetKind::Font, RuntimeAssetKind::Image,
          RuntimeAssetKind::Vector, RuntimeAssetKind::NestedAnimation,
          RuntimeAssetKind::Mesh, RuntimeAssetKind::FloatTexture};
      for (const auto kind : kinds) {
        if (kind != builtinKind)
          continue;
        RuntimeAsset candidate;
        if (config_.assets->Resolve(kind, assetId, candidate, error)) {
          resolved = std::move(candidate);
          didResolve = true;
          break;
        }
      }
      if (!didResolve && allowKindFallback) {
        for (const auto kind : kinds) {
          if (kind == builtinKind)
            continue;
          error.clear();
          RuntimeAsset candidate;
          if (config_.assets->Resolve(kind, assetId, candidate, error)) {
            resolved = std::move(candidate);
            didResolve = true;
            break;
          }
        }
      }
      if (didResolve && resolved.logicalId == assetId && resolved.bytes &&
          !resolved.bytes->empty() &&
          resolved.bytes->size() <= kMaximumDecodedAssetBytes) {
        admittedDigest = resolved.contentDigest;
        admittedMediaType = resolved.mediaType;
        bytes = std::move(resolved.bytes);
      }
    }
    if (!bytes) {
      error.clear();
      text::TextAssetBytes resolved;
      bool didResolve = false;
      if (projectAssets_) {
        constexpr std::array<text::TextAssetKind, 4> kinds{
            text::TextAssetKind::Font, text::TextAssetKind::Image,
            text::TextAssetKind::Mesh, text::TextAssetKind::FloatTexture};
        for (const auto kind : kinds) {
          if (kind != projectKind)
            continue;
          text::TextAssetBytes candidate;
          if (projectAssets_->Resolve(kind, assetId, digest, candidate,
                                      error)) {
            resolved = std::move(candidate);
            didResolve = true;
            break;
          }
        }
        if (!didResolve && allowKindFallback) {
          for (const auto kind : kinds) {
            if (kind == projectKind)
              continue;
            error.clear();
            text::TextAssetBytes candidate;
            if (projectAssets_->Resolve(kind, assetId, digest, candidate,
                                        error)) {
              resolved = std::move(candidate);
              didResolve = true;
              break;
            }
          }
        }
      }
      const bool admittedProjectKind =
          allowKindFallback
              ? (resolved.kind == text::TextAssetKind::Font ||
                 resolved.kind == text::TextAssetKind::Image ||
                 resolved.kind == text::TextAssetKind::Mesh ||
                 resolved.kind == text::TextAssetKind::FloatTexture)
              : resolved.kind == projectKind;
      if (!didResolve || !admittedProjectKind ||
          resolved.logicalId != assetId || resolved.contentDigest != digest ||
          !resolved.bytes || resolved.bytes->empty() ||
          resolved.byteLength != resolved.bytes->size() ||
          resolved.byteLength > kMaximumDecodedAssetBytes) {
        if (error.empty())
          error = "opaque text execution resource did not resolve";
        return false;
      }
      admittedDigest = resolved.contentDigest;
      admittedMediaType = resolved.mediaType;
      bytes = std::move(resolved.bytes);
    }
    const auto byteDigest =
        videocut::vector::Sha256Digest(bytes->data(), bytes->size());
    if (byteDigest != digest && admittedDigest != digest) {
      error = "opaque text execution resource digest does not match";
      identity.clear();
      return false;
    }
    identity = digest;
    if (resolvedBytes)
      *resolvedBytes = bytes;
    if (resolvedMediaType)
      *resolvedMediaType = std::move(admittedMediaType);
    error.clear();
    return true;
  }

bool TextVisualAssetStore::ResolveBytes(
      const text::TextureReference &reference,
      std::shared_ptr<const std::vector<std::uint8_t>> &bytes,
      std::string &mediaType, std::string &error) const {
    if (reference.assetId.empty()) {
      error = "text visual resource has no asset identity";
      return false;
    }
    if (reference.sourceKind == text::TextureSourceKind::Builtin) {
      if (!config_.assets) {
        error = "built-in text asset resolver is unavailable";
        return false;
      }
      RuntimeAsset resolved;
      const auto preferredKind = RuntimeKindForMediaType(reference.mediaType);
      if (!config_.assets->Resolve(preferredKind, reference.assetId, resolved,
                                   error)) {
        // Unqualified frame-plan samples are allowed to probe the closed
        // built-in kinds. The resolver still owns the allowlist and bytes.
        constexpr std::array<RuntimeAssetKind, 3> kinds{
            RuntimeAssetKind::Image, RuntimeAssetKind::Vector,
            RuntimeAssetKind::NestedAnimation};
        for (const auto kind : kinds) {
          error.clear();
          if (kind != preferredKind &&
              config_.assets->Resolve(kind, reference.assetId, resolved,
                                      error)) {
            break;
          }
        }
      }
      if (resolved.logicalId != reference.assetId || !resolved.bytes ||
          resolved.bytes->empty() ||
          resolved.bytes->size() > kMaximumDecodedAssetBytes) {
        if (error.empty())
          error = "built-in text asset identity did not resolve";
        return false;
      }
      const auto byteDigest = videocut::vector::Sha256Digest(
          resolved.bytes->data(), resolved.bytes->size());
      if (!reference.digest.empty() && reference.digest != byteDigest &&
          reference.digest != resolved.contentDigest) {
        error = "built-in text asset digest does not match";
        return false;
      }
      bytes = std::move(resolved.bytes);
      mediaType = resolved.mediaType.empty() ? reference.mediaType
                                             : resolved.mediaType;
      return true;
    }

    text::TextAssetBytes resolved;
    if (!projectAssets_ ||
        !projectAssets_->Resolve(text::TextAssetKind::Image,
                                 reference.assetId, reference.digest,
                                 resolved, error) ||
        resolved.kind != text::TextAssetKind::Image ||
        resolved.logicalId != reference.assetId ||
        resolved.contentDigest != reference.digest || !resolved.bytes ||
        resolved.bytes->empty() ||
        resolved.byteLength != resolved.bytes->size() ||
        resolved.byteLength > kMaximumDecodedAssetBytes) {
      if (error.empty())
        error = "project-managed text asset identity did not resolve";
      return false;
    }
    bytes = std::move(resolved.bytes);
    mediaType = resolved.mediaType.empty() ? reference.mediaType
                                           : resolved.mediaType;
    return true;
  }

bool TextVisualAssetStore::Parse(ResolvedVisualAsset &asset, const RasterDecodeUsage usage,
             std::string &error) const {
    if (IsSvgMediaType(asset.mediaType)) {
      SkMemoryStream stream(asset.bytes->data(), asset.bytes->size(), false);
      SkSVGDOM::Builder builder;
      builder.setTextShapingFactory(SkShapers::BestAvailable());
      asset.svg = builder.make(stream);
      if (!asset.svg) {
        error = "text vector backdrop SVG parsing failed";
        return false;
      }
      const auto size = asset.svg->containerSize();
      asset.intrinsicWidth = size.width();
      asset.intrinsicHeight = size.height();
      return asset.intrinsicWidth > 0.0F && asset.intrinsicHeight > 0.0F;
    }
    if (IsLottieMediaType(asset.mediaType)) {
      const auto root = nlohmann::json::parse(
          asset.bytes->begin(), asset.bytes->end(), nullptr, false);
      if (!root.is_discarded() && root.is_object()) {
        const auto metadata = root.find("meta");
        if (metadata != root.end() && metadata->is_object()) {
          const auto packedAlpha =
              metadata->find("videocut_packed_alpha");
          if (packedAlpha != metadata->end() && packedAlpha->is_object()) {
            const auto duration = packedAlpha->find("duration_us");
            if (duration != packedAlpha->end() &&
                duration->is_number_integer()) {
              const auto value = duration->get<std::int64_t>();
              if (value > 0 && value <= 3'600'000'000LL)
                asset.durationUs = value;
            }
            if (packedAlpha->value("runtime", std::string{}) ==
                "packed-alpha-associated-direct-frame-lottie") {
              const auto frameCount =
                  packedAlpha->value("frame_count", std::uint64_t{0U});
              const auto renderSize =
                  packedAlpha->value("render_size", std::uint32_t{0U});
              if (frameCount > 0U &&
                  frameCount <= 9'007'199'254'740'991ULL &&
                  renderSize > 0U && renderSize <= 16'384U) {
                asset.qtFollowerFrameCount = frameCount;
                asset.qtFollowerRenderSize = renderSize;
              }
            }
          }
        }
        const auto closure =
            metadata != root.end() && metadata->is_object()
                ? metadata->find("videocut_resource_closure")
                : nlohmann::json::const_iterator{};
        if (metadata != root.end() && metadata->is_object() &&
            closure != metadata->end()) {
          if (!closure->is_object() ||
              closure->value("format", std::string{}) !=
                  "videocut.vector-resource-closure") {
            error = "text Lottie resource closure is invalid";
            return false;
          }
          const auto entries = closure->find("resources");
          if (entries == closure->end() || !entries->is_array() ||
              !config_.assets || !config_.animatedImages) {
            error = "text Lottie resource closure is unavailable";
            return false;
          }
          std::vector<vector::VectorResource> resources;
          resources.reserve(entries->size());
          for (const auto &entry : *entries) {
            if (!entry.is_object()) {
              error = "text Lottie resource closure entry is invalid";
              return false;
            }
            const auto logicalName =
                entry.value("logical_name", std::string{});
            const auto assetId = entry.value("asset_id", std::string{});
            const auto mediaType = entry.value("media_type", std::string{});
            const auto byteLength = entry.value("byte_length", 0U);
            if (logicalName.empty() || assetId.empty() || mediaType.empty() ||
                byteLength == 0U) {
              error = "text Lottie resource closure entry is incomplete";
              return false;
            }
            RuntimeAsset resolved;
            const auto preferredKind = RuntimeKindForMediaType(mediaType);
            if (!config_.assets->Resolve(preferredKind, assetId, resolved,
                                         error)) {
              constexpr std::array<RuntimeAssetKind, 3> kinds{
                  RuntimeAssetKind::Image, RuntimeAssetKind::Vector,
                  RuntimeAssetKind::NestedAnimation};
              for (const auto kind : kinds) {
                error.clear();
                if (kind != preferredKind &&
                    config_.assets->Resolve(kind, assetId, resolved, error)) {
                  break;
                }
              }
            }
            if (resolved.logicalId != assetId || !resolved.bytes ||
                resolved.bytes->size() != byteLength ||
                resolved.mediaType != mediaType) {
              if (error.empty())
                error = "text Lottie resource closure did not resolve";
              return false;
            }
            if (asset.qtFollowerFrameCount > 0U &&
                mediaType == kQtPackedAlphaMp4ResourceMediaType) {
              if (!asset.qtFollowerResourceId.empty() &&
                  asset.qtFollowerResourceId != logicalName) {
                error = "text follower direct-frame closure is ambiguous";
                return false;
              }
              asset.qtFollowerResourceId = logicalName;
            }
            resources.push_back(
                {logicalName, mediaType, std::move(resolved.bytes)});
          }
          auto immutableResources = sk_make_sp<ImmutableResourceProvider>(
              resources, false, config_.animatedImages);
          asset.immutableLottieResources = immutableResources;
          asset.lottieResources =
              skresources::CachingResourceProvider::Make(
                  std::move(immutableResources));
          if (!asset.lottieResources) {
            error = "text Lottie resource provider creation failed";
            return false;
          }
          if (asset.qtFollowerFrameCount > 0U &&
              (asset.qtFollowerResourceId.empty() ||
               !asset.immutableLottieResources)) {
            error = "text follower direct-frame metadata is incomplete";
            return false;
          }
        }
      }
      skottie::Animation::Builder builder(
          skottie::Animation::Builder::kDeferImageLoading |
          skottie::Animation::Builder::kPreferEmbeddedFonts);
      builder.setTextShapingFactory(SkShapers::BestAvailable());
      if (asset.lottieResources)
        builder.setResourceProvider(asset.lottieResources);
      asset.lottie = builder.make(
          reinterpret_cast<const char *>(asset.bytes->data()),
          asset.bytes->size());
      if (!asset.lottie) {
        error = "text animated backdrop Lottie parsing failed";
        return false;
      }
      const auto size = asset.lottie->size();
      asset.intrinsicWidth = size.width();
      asset.intrinsicHeight = size.height();
      if (asset.durationUs <= 0) {
        const double seconds = asset.lottie->duration();
        if (std::isfinite(seconds) && seconds > 0.0 &&
            seconds <= 3600.0) {
          asset.durationUs = static_cast<std::int64_t>(
              std::llround(seconds * 1'000'000.0));
        }
      }
      if (asset.qtFollowerFrameCount > 0U &&
          (!asset.immutableLottieResources || !asset.lottieResources)) {
        // A packed-alpha follower is only valid with its admitted immutable
        // closure. Letting Skottie proceed with an implicit/empty provider
        // yields a transparent component and loses the direct YUV/alpha
        // merge contract; fail this asset so the caller can use its explicit
        // source fallback instead.
        error = "text packed-alpha follower has no admitted resource provider";
        return false;
      }
      return asset.intrinsicWidth > 0.0F && asset.intrinsicHeight > 0.0F;
    }
    if (IsStreamingMediaType(asset.mediaType)) {
      if (!config_.animatedImages) {
        error = "animated text asset decoder is unavailable";
        return false;
      }
      if (asset.mediaType == kQtPackedAlphaMp4ResourceMediaType) {
        std::vector<vector::VectorResource> resources;
        resources.push_back({asset.reference.assetId, asset.mediaType,
                             asset.bytes});
        asset.immutableLottieResources =
            sk_make_sp<ImmutableResourceProvider>(resources, false,
                                                  config_.animatedImages);
        asset.qtFollowerResourceId = asset.reference.assetId;
        if (!asset.immutableLottieResources) {
          error = "packed-alpha text media provider is unavailable";
          return false;
        }
        return true;
      }
      RuntimeAnimatedImageOpenRequest request;
      request.logicalId = asset.reference.assetId;
      request.mediaType = asset.mediaType;
      if (asset.mediaType == kQtPackedAlphaMp4ResourceMediaType)
        request.capability = std::string(kQtPackedAlphaMp4Capability);
      request.bytes = asset.bytes;
      asset.stream = config_.animatedImages->Create(request, error);
      return asset.stream != nullptr;
    }

    auto data = RetainImmutableSkData(asset.bytes);
    if (usage == RasterDecodeUsage::QtStretchableBubble && data) {
      auto decoded = SkImages::DeferredFromEncodedData(std::move(data));
      asset.raster = decoded
                         ? decoded->reinterpretColorSpace(
                               SkColorSpace::MakeSRGB())
                         : nullptr;
    } else {
      asset.raster =
          data ? SkImages::DeferredFromEncodedData(std::move(data)) : nullptr;
    }
    if (!asset.raster || asset.raster->width() <= 0 ||
        asset.raster->height() <= 0 ||
        static_cast<std::size_t>(asset.raster->width()) *
                static_cast<std::size_t>(asset.raster->height()) >
            kMaximumDecodedAssetPixels) {
      error = "text raster asset decode failed or exceeded its budget";
      return false;
    }
    if (const auto colorSpace = asset.raster->colorSpace();
        colorSpace && !colorSpace->isSRGB()) {
      auto converted = asset.raster->makeColorSpace(
          nullptr, SkColorSpace::MakeSRGB(), SkImage::RequiredProperties{});
      if (!converted || !converted->colorSpace() ||
          !converted->colorSpace()->isSRGB()) {
        error = "text raster asset could not be converted to sRGB";
        return false;
      }
      asset.raster = std::move(converted);
    }
    // Keep decoded immutable pixels in the lane's budgeted asset store.
    // A deferred PNG image can be decoded again by every native material
    // upload, multiplying decompression cost by letters, layers and frames.
    asset.raster = asset.raster->makeRasterImage(nullptr);
    if (!asset.raster) {
      error = "text raster asset pixel decoding failed";
      return false;
    }
    asset.intrinsicWidth = static_cast<float>(asset.raster->width());
    asset.intrinsicHeight = static_cast<float>(asset.raster->height());
    return true;
  }

} // namespace videocut::skia_runtime::internal::text_lane
