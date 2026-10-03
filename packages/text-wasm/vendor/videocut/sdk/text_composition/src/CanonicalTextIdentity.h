#pragma once

#include "videocut/text_composition/TextCompositionDocument.h"
#include "videocut/vector/VectorDigest.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace videocut::text_composition::detail {

class CanonicalTextIdentityWriter final {
public:
  explicit CanonicalTextIdentityWriter(const std::string_view domain) {
    String(domain);
  }

  CanonicalTextIdentityWriter(const std::string_view domain,
                              std::uint8_t *archive,
                              const std::size_t archiveCapacity) noexcept
      : archive_(archive), archiveCapacity_(archiveCapacity) {
    String(domain);
  }

  void Boolean(const bool value) noexcept { Byte(value ? 1U : 0U); }

  template <typename Enum>
  void Enumeration(const Enum value) noexcept {
    static_assert(std::is_enum_v<Enum>);
    using Underlying = std::underlying_type_t<Enum>;
    if constexpr (std::is_signed_v<Underlying>)
      Signed(static_cast<std::int64_t>(value));
    else
      Unsigned(static_cast<std::uint64_t>(value));
  }

  void Unsigned(const std::uint64_t value) noexcept {
    for (unsigned shift = 0U; shift != 64U; shift += 8U)
      Byte(static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }

  void Signed(const std::int64_t value) noexcept {
    Unsigned(static_cast<std::uint64_t>(value));
  }

  void Float(const float value) noexcept {
    float canonical = value == 0.0F ? 0.0F : value;
    std::uint32_t bits = 0U;
    std::memcpy(&bits, &canonical, sizeof(bits));
    Unsigned(bits);
  }

  void Double(const double value) noexcept {
    double canonical = value == 0.0 ? 0.0 : value;
    std::uint64_t bits = 0U;
    std::memcpy(&bits, &canonical, sizeof(bits));
    Unsigned(bits);
  }

  void String(const std::string_view value) noexcept {
    Unsigned(value.size());
    for (const unsigned char byte : value)
      Byte(byte);
  }

  void Count(const std::size_t count) noexcept { Unsigned(count); }

  [[nodiscard]] std::string Finish() const {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << first_
           << std::setw(16) << second_;
    return stream.str();
  }

  [[nodiscard]] std::size_t EncodedBytes() const noexcept { return bytes_; }

  [[nodiscard]] bool ArchiveComplete() const noexcept {
    return archive_ != nullptr && !archiveOverflow_ &&
           bytes_ == archiveCapacity_;
  }

private:
  void Byte(const std::uint8_t value) noexcept {
    if (archive_) {
      if (bytes_ < archiveCapacity_)
        archive_[bytes_] = value;
      else
        archiveOverflow_ = true;
    }
    if (bytes_ != std::numeric_limits<std::size_t>::max())
      ++bytes_;
    first_ ^= value;
    first_ *= 1099511628211ULL;
    second_ += value + 0x9e3779b97f4a7c15ULL;
    second_ ^= second_ >> 29U;
    second_ *= 0xbf58476d1ce4e5b9ULL;
    second_ ^= first_ >> 31U;
  }

  std::uint64_t first_{1469598103934665603ULL};
  std::uint64_t second_{0x6a09e667f3bcc909ULL};
  std::size_t bytes_{0U};
  std::uint8_t *archive_{nullptr};
  std::size_t archiveCapacity_{0U};
  bool archiveOverflow_{false};
};

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::Color &value) noexcept {
  writer.Float(value.red);
  writer.Float(value.green);
  writer.Float(value.blue);
  writer.Float(value.alpha);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::Insets &value) noexcept {
  writer.Float(value.left);
  writer.Float(value.top);
  writer.Float(value.right);
  writer.Float(value.bottom);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::Rect &value) noexcept {
  writer.Float(value.x);
  writer.Float(value.y);
  writer.Float(value.width);
  writer.Float(value.height);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextUtf8Range &value) noexcept {
  writer.Unsigned(value.begin);
  writer.Unsigned(value.end);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::FontAxis &value) noexcept {
  writer.String(value.tag);
  writer.Float(value.value);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::FontReference &value) noexcept {
  writer.Enumeration(value.kind);
  writer.String(value.family);
  writer.String(value.postscriptName);
  writer.Signed(value.weight);
  writer.Signed(value.width);
  writer.Enumeration(value.slant);
  writer.Unsigned(value.faceIndex);
  writer.Count(value.variationAxes.size());
  for (const auto &axis : value.variationAxes)
    Encode(writer, axis);
  writer.String(value.assetId);
  writer.String(value.platform);
  writer.String(value.digest);
  writer.String(value.faceFingerprint);
  writer.Boolean(value.allowSystemGlyphFallback);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::FontFeature &value) noexcept {
  writer.String(value.tag);
  writer.Unsigned(value.value);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::FontSpec &value) noexcept {
  Encode(writer, value.primary);
  writer.Count(value.fallbacks.size());
  for (const auto &fallback : value.fallbacks)
    Encode(writer, fallback);
  writer.String(value.family);
  writer.String(value.postscriptName);
  writer.Signed(value.weight);
  writer.Signed(value.width);
  writer.Enumeration(value.slant);
  writer.Unsigned(value.faceIndex);
  writer.Count(value.variationAxes.size());
  for (const auto &axis : value.variationAxes)
    Encode(writer, axis);
  writer.Count(value.features.size());
  for (const auto &feature : value.features)
    Encode(writer, feature);
  writer.Boolean(value.allowSystemGlyphFallback);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::GradientStop &value) noexcept {
  writer.Float(value.offset);
  Encode(writer, value.color);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextureReference &value) noexcept {
  writer.Enumeration(value.sourceKind);
  writer.String(value.assetId);
  writer.String(value.digest);
  writer.String(value.mediaType);
  writer.String(value.colorSpace);
  writer.Enumeration(value.orientation);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextMaterialCoordinates &value) noexcept {
  writer.Enumeration(value.coordinateSpace);
  writer.Float(value.coordinateOutset);
  writer.Float(value.coordinateScale);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextMaterial &value) noexcept {
  writer.Unsigned(value.index());
  std::visit(
      [&](const auto &material) {
        using Material = std::decay_t<decltype(material)>;
        if constexpr (std::is_same_v<Material, text::SolidTextMaterial>) {
          Encode(writer, material.color);
        } else if constexpr (std::is_same_v<
                                 Material,
                                 text::LinearGradientTextMaterial>) {
          writer.Count(material.stops.size());
          for (const auto &stop : material.stops)
            Encode(writer, stop);
          writer.Float(material.startX);
          writer.Float(material.startY);
          writer.Float(material.endX);
          writer.Float(material.endY);
          writer.Enumeration(material.spread);
          writer.Enumeration(material.sampling);
          Encode(writer, material.coordinates);
        } else if constexpr (std::is_same_v<
                                 Material,
                                 text::RadialGradientTextMaterial>) {
          writer.Count(material.stops.size());
          for (const auto &stop : material.stops)
            Encode(writer, stop);
          writer.Float(material.centerX);
          writer.Float(material.centerY);
          writer.Float(material.radius);
          writer.Enumeration(material.spread);
          writer.Enumeration(material.sampling);
          Encode(writer, material.coordinates);
        } else {
          Encode(writer, material.texture);
          writer.Enumeration(material.fit);
          writer.Enumeration(material.mapping);
          Encode(writer, material.coordinates);
          writer.Float(material.scale);
          writer.Float(material.rotationDegrees);
          writer.Float(material.offsetX);
          writer.Float(material.offsetY);
          writer.Boolean(material.flipX);
          writer.Boolean(material.flipY);
          writer.Unsigned(material.atlasColumns);
          writer.Unsigned(material.atlasRows);
          writer.Float(material.textureOpacity);
          writer.Float(material.opacity);
          writer.Boolean(material.sourceAlpha);
          writer.Boolean(material.underlayColor.has_value());
          if (material.underlayColor)
            Encode(writer, *material.underlayColor);
          writer.Count(material.underlayGradient.size());
          for (const auto &stop : material.underlayGradient)
            Encode(writer, stop);
          writer.Float(material.underlayGradientProjection.startX);
          writer.Float(material.underlayGradientProjection.startY);
          writer.Float(material.underlayGradientProjection.endX);
          writer.Float(material.underlayGradientProjection.endY);
          writer.Enumeration(material.underlayGradientProjection.spread);
          writer.Enumeration(material.underlayGradientProjection.sampling);
        }
      },
      value);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextMaterialBinding &value) noexcept {
  writer.Unsigned(value.index());
  std::visit(
      [&](const auto &binding) {
        using Binding = std::decay_t<decltype(binding)>;
        if constexpr (std::is_same_v<Binding, text::LiteralTextMaterial>) {
          Encode(writer, binding.material);
        } else {
          writer.String(binding.semanticRole);
          Encode(writer, binding.fallback);
          writer.Boolean(binding.replacementMask.has_value());
          if (binding.replacementMask)
            Encode(writer, *binding.replacementMask);
        }
      },
      value);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextFillLayer &value) noexcept {
  writer.String(value.layerId);
  writer.Signed(value.zOrder);
  writer.Enumeration(value.blend);
  Encode(writer, value.material);
  writer.Float(value.offsetX);
  writer.Float(value.offsetY);
  writer.Boolean(value.normalizedPolarOffset.has_value());
  if (value.normalizedPolarOffset) {
    writer.Float(value.normalizedPolarOffset->radius);
    writer.Float(value.normalizedPolarOffset->angleRadians);
  }
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextNormalizedPolarOffset &value) noexcept {
  writer.Float(value.radius);
  writer.Float(value.angleRadians);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextStrokeLayer &value) noexcept {
  writer.String(value.layerId);
  writer.Signed(value.zOrder);
  writer.Enumeration(value.blend);
  Encode(writer, value.material);
  writer.Float(value.width);
  writer.Float(value.innerRingWidth);
  writer.Boolean(value.signedStartWidth.has_value());
  if (value.signedStartWidth)
    writer.Float(*value.signedStartWidth);
  writer.Float(value.offsetX);
  writer.Float(value.offsetY);
  writer.Boolean(value.normalizedPolarOffset.has_value());
  if (value.normalizedPolarOffset)
    Encode(writer, *value.normalizedPolarOffset);
  writer.Float(value.blurRadius);
  writer.Float(value.spread);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextShadowLayer &value) noexcept {
  writer.String(value.layerId);
  writer.Signed(value.zOrder);
  writer.Enumeration(value.blend);
  Encode(writer, value.material);
  writer.Enumeration(value.kind);
  writer.Float(value.offsetX);
  writer.Float(value.offsetY);
  writer.Boolean(value.normalizedPolarOffset.has_value());
  if (value.normalizedPolarOffset)
    Encode(writer, *value.normalizedPolarOffset);
  writer.Float(value.blurRadius);
  writer.Float(value.spread);
  writer.Float(value.thicknessAngleDegrees);
  writer.Float(value.thicknessDistance);
  writer.Enumeration(value.smoothing);
  writer.Float(value.roundMaskIntensity);
  writer.Float(value.sdfBlurScale);
  writer.Boolean(value.normalizedUvOffset.has_value());
  if (value.normalizedUvOffset) {
    writer.Float(value.normalizedUvOffset->x);
    writer.Float(value.normalizedUvOffset->y);
  }
  writer.Count(value.strokes.size());
  for (const auto &stroke : value.strokes)
    Encode(writer, stroke);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextGlowLayer &value) noexcept {
  writer.String(value.layerId);
  writer.Signed(value.zOrder);
  writer.Enumeration(value.blend);
  Encode(writer, value.material);
  writer.Float(value.radius);
  writer.Float(value.spread);
  writer.Float(value.directionX);
  writer.Float(value.directionY);
  writer.Boolean(value.normalizedPolarOffset.has_value());
  if (value.normalizedPolarOffset)
    Encode(writer, *value.normalizedPolarOffset);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextGlyphMaterialLayer &value) noexcept {
  writer.Unsigned(value.index());
  std::visit([&](const auto &layer) { Encode(writer, layer); }, value);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextGlyphMaterialStack &value) noexcept {
  writer.Count(value.layers.size());
  for (const auto &layer : value.layers)
    Encode(writer, layer);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextDecorationLine &value) noexcept {
  writer.Boolean(value.enabled);
  Encode(writer, value.material);
  writer.Float(value.thickness);
  writer.Float(value.offset);
  writer.Enumeration(value.style);
  writer.Boolean(value.skipInk);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::InlineTextDecoration &value) noexcept {
  Encode(writer, value.underline);
  Encode(writer, value.strikeThrough);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextBoxBackground &value) noexcept {
  writer.Boolean(value.enabled);
  Encode(writer, value.color);
  Encode(writer, value.padding);
  writer.Float(value.cornerRadius);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextStyle &value) noexcept {
  Encode(writer, value.font);
  writer.Float(value.fontSize);
  writer.Float(value.letterSpacing);
  writer.Float(value.wordSpacing);
  writer.Float(value.baselineShift);
  Encode(writer, value.materials);
  Encode(writer, value.background);
  Encode(writer, value.decoration);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextTabStop &value) noexcept {
  writer.Float(value.position);
  writer.Enumeration(value.alignment);
  writer.Unsigned(static_cast<std::uint32_t>(value.decimalCharacter));
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::ParagraphStyle &value) noexcept {
  writer.Enumeration(value.alignment);
  writer.Enumeration(value.direction);
  writer.String(value.locale);
  writer.Boolean(value.maximumLines.has_value());
  if (value.maximumLines)
    writer.Unsigned(*value.maximumLines);
  writer.Enumeration(value.overflow);
  writer.Float(value.lineHeight);
  writer.Enumeration(value.wrap);
  writer.Enumeration(value.lineBreakPolicy);
  writer.Enumeration(value.hyphenation);
  writer.Float(value.firstLineIndent);
  writer.Float(value.startIndent);
  writer.Float(value.endIndent);
  writer.Float(value.spacingBefore);
  writer.Float(value.spacingAfter);
  writer.Boolean(value.hangingPunctuation);
  writer.Count(value.tabStops.size());
  for (const auto &tab : value.tabStops)
    Encode(writer, tab);
  Encode(writer, value.background);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::RichTextRun &value,
                   const bool includeMaterial = true) noexcept {
  writer.String(value.runId);
  writer.String(value.utf8Text);
  writer.String(value.locale);
  if (includeMaterial) {
    Encode(writer, value.style);
  } else {
    Encode(writer, value.style.font);
    writer.Float(value.style.fontSize);
    writer.Float(value.style.letterSpacing);
    writer.Float(value.style.wordSpacing);
    writer.Float(value.style.baselineShift);
    Encode(writer, value.style.background);
    Encode(writer, value.style.decoration);
  }
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::RichTextParagraph &value,
                   const bool includeMaterial = true) noexcept {
  writer.String(value.paragraphId);
  Encode(writer, value.style);
  writer.Count(value.runs.size());
  for (const auto &run : value.runs)
    Encode(writer, run, includeMaterial);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextContentSlot &value) noexcept {
  writer.String(value.slotId);
  writer.String(value.semanticRole);
  writer.Count(value.paragraphIds.size());
  for (const auto &id : value.paragraphIds)
    writer.String(id);
  writer.Count(value.runIds.size());
  for (const auto &id : value.runIds)
    writer.String(id);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::ReferenceCanvas &value) noexcept {
  writer.Float(value.width);
  writer.Float(value.height);
  writer.Enumeration(value.scalePolicy);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::LayoutBox &value) noexcept {
  writer.Float(value.x);
  writer.Float(value.y);
  writer.Float(value.width);
  writer.Float(value.height);
  writer.Enumeration(value.sizingMode);
  writer.Float(value.minimumWidth);
  writer.Float(value.minimumHeight);
  writer.Boolean(value.maximumWidth.has_value());
  if (value.maximumWidth)
    writer.Float(*value.maximumWidth);
  writer.Boolean(value.maximumHeight.has_value());
  if (value.maximumHeight)
    writer.Float(*value.maximumHeight);
  writer.Float(value.minimumFitFontSize);
  writer.Float(value.maximumFitFontSize);
  Encode(writer, value.fitMeasurementOutsets);
  writer.Boolean(value.fitOriginX.has_value());
  if (value.fitOriginX)
    writer.Float(*value.fitOriginX);
  Encode(writer, value.padding);
  writer.Enumeration(value.verticalAlignment);
  writer.Boolean(value.clipOverflow);
  writer.Boolean(value.pixelSnap);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextLayerAnimationKeyframe &value) noexcept {
  writer.Float(value.offset);
  writer.Float(value.value);
  writer.Enumeration(value.easing);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextLayerAnimationTrack &value) noexcept {
  writer.Enumeration(value.property);
  writer.Count(value.keyframes.size());
  for (const auto &keyframe : value.keyframes)
    Encode(writer, keyframe);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextLayerAnimationClip &value) noexcept {
  writer.String(value.clipId);
  writer.String(value.presetId);
  writer.Enumeration(value.phase);
  writer.Signed(value.durationUs);
  writer.Count(value.tracks.size());
  for (const auto &track : value.tracks)
    Encode(writer, track);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextBackdropTransform &value) noexcept {
  writer.Float(value.offsetX);
  writer.Float(value.offsetY);
  writer.Float(value.scaleX);
  writer.Float(value.scaleY);
  writer.Float(value.rotationDegrees);
  writer.Float(value.opacity);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextBackdropSource &value) noexcept {
  writer.Unsigned(value.index());
  std::visit(
      [&](const auto &source) {
        using Source = std::decay_t<decltype(source)>;
        if constexpr (std::is_same_v<Source, text::RoundedRectBackdrop>) {
          writer.Enumeration(source.family);
          Encode(writer, source.fill);
          writer.Count(source.strokes.size());
          for (const auto &stroke : source.strokes)
            Encode(writer, stroke);
          writer.Float(source.cornerRadius);
          writer.Enumeration(source.tail.edge);
          writer.Float(source.tail.position);
          writer.Float(source.tail.width);
          writer.Float(source.tail.length);
          writer.Boolean(source.authoredWidth.has_value());
          if (source.authoredWidth)
            writer.Float(*source.authoredWidth);
          writer.Boolean(source.authoredHeight.has_value());
          if (source.authoredHeight)
            writer.Float(*source.authoredHeight);
        } else if constexpr (std::is_same_v<Source,
                                            text::NineSliceBackdrop>) {
          Encode(writer, source.asset);
          Encode(writer, source.capInsets);
          Encode(writer, source.contentInsets);
          writer.Float(source.minimumContentWidth);
          writer.Float(source.minimumContentHeight);
          writer.Enumeration(source.stretchMode);
          Encode(writer, source.fallbackColor);
        } else if constexpr (std::is_same_v<Source,
                                            text::VectorBackdrop>) {
          Encode(writer, source.asset);
          writer.Enumeration(source.fit);
          Encode(writer, source.fallbackColor);
        } else {
          Encode(writer, source.asset);
          Encode(writer, source.contentInsets);
          writer.Enumeration(source.timeSource);
          writer.Enumeration(source.playback);
          writer.Signed(source.sourceInUs);
          writer.Signed(source.sourceOutUs);
          writer.Signed(source.phaseUs);
          Encode(writer, source.fallbackColor);
        }
      },
      value);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextBackdropLayer &value) noexcept {
  writer.String(value.layerId);
  writer.Enumeration(value.channel);
  writer.Boolean(value.enabled);
  Encode(writer, value.source);
  writer.Boolean(value.materialOverride.has_value());
  if (value.materialOverride)
    Encode(writer, *value.materialOverride);
  Encode(writer, value.padding);
  writer.Enumeration(value.fit);
  writer.Boolean(value.sourceIntrinsicWidth.has_value());
  if (value.sourceIntrinsicWidth)
    writer.Float(*value.sourceIntrinsicWidth);
  writer.Boolean(value.sourceIntrinsicHeight.has_value());
  if (value.sourceIntrinsicHeight)
    writer.Float(*value.sourceIntrinsicHeight);
  writer.Enumeration(value.fitMode);
  writer.Float(value.sourcePivotX);
  writer.Float(value.sourcePivotY);
  Encode(writer, value.expand);
  Encode(writer, value.sourceOutsets);
  Encode(writer, value.transform);
  writer.Signed(value.zOrder);
  writer.Count(value.animationClips.size());
  for (const auto &clip : value.animationClips)
    Encode(writer, clip);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextBackdropStack &value) noexcept {
  writer.Count(value.layers.size());
  for (const auto &layer : value.layers)
    Encode(writer, layer);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextSdfMaterial &value) noexcept {
  writer.Boolean(value.enabled);
  writer.Enumeration(value.sourceCreationComponent);
  writer.Float(value.distanceRange);
  writer.Float(value.rasterDistanceRange);
  writer.Float(value.smoothingScale);
  writer.Float(value.sourceDesignWidth);
  writer.Float(value.sourceDesignHeight);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextBend &value) noexcept {
  writer.Boolean(value.enabled);
  writer.Float(value.amount);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextPath &value) noexcept {
  writer.Boolean(value.enabled);
  writer.String(value.geometryId);
  writer.String(value.presetId);
  writer.Count(value.commands.size());
  for (const auto &command : value.commands) {
    writer.Enumeration(command.kind);
    writer.Float(command.control1.x);
    writer.Float(command.control1.y);
    writer.Float(command.control2.x);
    writer.Float(command.control2.y);
    writer.Float(command.end.x);
    writer.Float(command.end.y);
  }
  writer.Float(value.startOffset);
  writer.Float(value.baselineOffset);
  writer.Enumeration(value.overflow);
  writer.Boolean(value.loop);
  writer.Boolean(value.rotateToTangent);
  writer.Boolean(value.keepUpright);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextLayerAppearance &value) noexcept {
  Encode(writer, value.backdrops);
  Encode(writer, value.sdfMaterial);
  Encode(writer, value.bend);
  Encode(writer, value.path);
  writer.Boolean(value.visualExtent.allowControlOverflow);
  writer.Boolean(value.visualExtent.maximumExtentWidth.has_value());
  if (value.visualExtent.maximumExtentWidth)
    writer.Float(*value.visualExtent.maximumExtentWidth);
  writer.Boolean(value.visualExtent.maximumExtentHeight.has_value());
  if (value.visualExtent.maximumExtentHeight)
    writer.Float(*value.visualExtent.maximumExtentHeight);
  writer.Float(value.globalAlpha);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextPropertyTarget &value) noexcept {
  writer.Enumeration(value.scope);
  writer.String(value.contentSlotId);
  writer.Count(value.paragraphIds.size());
  for (const auto &id : value.paragraphIds)
    writer.String(id);
  writer.Count(value.runIds.size());
  for (const auto &id : value.runIds)
    writer.String(id);
  writer.Boolean(value.range.has_value());
  if (value.range)
    Encode(writer, *value.range);
  writer.String(value.layerId);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextPropertyValue &value) noexcept {
  writer.Unsigned(value.index());
  std::visit(
      [&](const auto &typed) {
        using Value = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Value, std::monostate>) {
          return;
        } else if constexpr (std::is_same_v<Value, bool>) {
          writer.Boolean(typed);
        } else if constexpr (std::is_same_v<Value, std::int64_t>) {
          writer.Signed(typed);
        } else if constexpr (std::is_same_v<Value, double>) {
          writer.Double(typed);
        } else if constexpr (std::is_same_v<Value, std::string>) {
          writer.String(typed);
        } else if constexpr (
            std::is_same_v<Value, std::vector<text::FontAxis>> ||
            std::is_same_v<Value, std::vector<text::FontFeature>> ||
            std::is_same_v<Value, std::vector<text::TextStrokeLayer>> ||
            std::is_same_v<Value, std::vector<text::TextShadowLayer>> ||
            std::is_same_v<Value, std::vector<text::TextGlowLayer>> ||
            std::is_same_v<Value, std::vector<text::TextTabStop>>) {
          writer.Count(typed.size());
          for (const auto &element : typed)
            Encode(writer, element);
        } else if constexpr (std::is_same_v<Value, text::TextWritingMode>) {
          writer.Enumeration(typed);
        } else {
          Encode(writer, typed);
        }
      },
      value);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextPropertyAssignment &value) noexcept {
  writer.Enumeration(value.address.property);
  Encode(writer, value.address.target);
  writer.Enumeration(value.disposition);
  writer.Enumeration(value.combineMode);
  Encode(writer, value.value);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextPropertyPatch &value) noexcept {
  writer.String(value.patchId);
  writer.Unsigned(value.baseRevision);
  writer.Count(value.assignments.size());
  for (const auto &assignment : value.assignments)
    Encode(writer, assignment);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextKeyframe &value) noexcept {
  writer.Double(value.offset);
  writer.Double(value.value);
  writer.Double(value.tangentIn);
  writer.Double(value.tangentOut);
  writer.Boolean(value.cubicBezier);
  writer.Double(value.bezierTimeIn);
  writer.Double(value.bezierTimeOut);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextSelectorKeyframe &value) noexcept {
  writer.Double(value.offset);
  writer.Double(value.value);
  writer.Double(value.tangentIn);
  writer.Double(value.tangentOut);
  writer.Boolean(value.cubicBezier);
  writer.Double(value.bezierTimeIn);
  writer.Double(value.bezierTimeOut);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextUnitSelector &value) noexcept {
  writer.Enumeration(value.kind);
  writer.Enumeration(value.basedOn);
  writer.Double(value.rangeStart);
  writer.Double(value.rangeEnd);
  writer.Double(value.offset);
  writer.Double(value.stagger);
  writer.Double(value.edgeSmooth);
  writer.Enumeration(value.shape);
  writer.Boolean(value.constrained);
  writer.Enumeration(value.order);
  writer.Double(value.randomSeed);
  writer.Double(value.intensity);
  writer.Double(value.intensityStart);
  writer.Double(value.intensityEnd);
  writer.Double(value.timeStart1);
  writer.Double(value.timeStart2);
  writer.Double(value.timeEnd1);
  writer.Double(value.timeEnd2);
  writer.Boolean(value.timeCycle);
  writer.Count(value.rangeStartKeyframes.size());
  for (const auto &keyframe : value.rangeStartKeyframes)
    Encode(writer, keyframe);
  writer.Count(value.rangeEndKeyframes.size());
  for (const auto &keyframe : value.rangeEndKeyframes)
    Encode(writer, keyframe);
  writer.Count(value.offsetKeyframes.size());
  for (const auto &keyframe : value.offsetKeyframes)
    Encode(writer, keyframe);
  writer.Count(value.intensityKeyframes.size());
  for (const auto &keyframe : value.intensityKeyframes)
    Encode(writer, keyframe);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextAnimatorTrack &value) noexcept {
  writer.Enumeration(value.property);
  writer.Count(value.keyframes.size());
  for (const auto &keyframe : value.keyframes)
    Encode(writer, keyframe);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextColorKeyframe &value) noexcept {
  writer.Float(value.offset);
  Encode(writer, value.value);
  Encode(writer, value.tangentIn);
  Encode(writer, value.tangentOut);
  writer.Boolean(value.cubicBezier);
  writer.Float(value.bezierTimeIn);
  writer.Float(value.bezierTimeOut);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::RevealCursor &value) noexcept {
  writer.Boolean(value.enabled);
  Encode(writer, value.color);
  writer.Float(value.width);
  writer.Signed(value.periodUs);
  writer.Float(value.dutyCycle);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextAnimatorSpec &value) noexcept {
  writer.String(value.animatorId);
  writer.Count(value.paragraphIds.size());
  for (const auto &id : value.paragraphIds)
    writer.String(id);
  writer.Count(value.runIds.size());
  for (const auto &id : value.runIds)
    writer.String(id);
  writer.Count(value.selectors.size());
  for (const auto &selector : value.selectors)
    Encode(writer, selector);
  writer.Count(value.tracks.size());
  for (const auto &track : value.tracks)
    Encode(writer, track);
  writer.Count(value.fillColorKeyframes.size());
  for (const auto &keyframe : value.fillColorKeyframes)
    Encode(writer, keyframe);
  writer.String(value.effectProgramId);
  writer.Enumeration(value.positionMode);
  writer.Enumeration(value.anchor);
  writer.Enumeration(value.anchorBasis);
  writer.Enumeration(value.anchorMode);
  writer.Float(value.anchorOffsetX);
  writer.Float(value.anchorOffsetY);
  writer.Enumeration(value.projection.kind);
  writer.Float(value.projection.fieldOfViewDegrees);
  writer.Float(value.projection.vanishingPointX);
  writer.Float(value.projection.vanishingPointY);
  writer.Enumeration(value.presentation);
  writer.Float(value.fadeFraction);
  Encode(writer, value.activeColor);
  Encode(writer, value.cursor);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextAnimationTimeDriver &value) noexcept {
  writer.Enumeration(value.kind);
  writer.Signed(value.startOffsetUs);
  writer.Signed(value.durationUs);
  writer.Enumeration(value.playback);
  writer.Boolean(value.timedRanges.has_value());
  if (value.timedRanges) {
    writer.Enumeration(value.timedRanges->mode);
    writer.Count(value.timedRanges->spanIds.size());
    for (const auto &id : value.timedRanges->spanIds)
      writer.String(id);
    writer.Boolean(value.timedRanges->activePatch.has_value());
    if (value.timedRanges->activePatch)
      Encode(writer, *value.timedRanges->activePatch);
  }
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextPostEffectParameter &value) noexcept {
  writer.String(value.name);
  writer.Count(value.values.size());
  for (const float component : value.values)
    writer.Float(component);
  writer.Count(value.keyframes.size());
  for (const auto &keyframe : value.keyframes)
    Encode(writer, keyframe);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextPostEffectSpec &value) noexcept {
  writer.String(value.effectId);
  writer.Enumeration(value.kind);
  writer.Count(value.inputIds.size());
  for (const auto &inputId : value.inputIds)
    writer.String(inputId);
  writer.Float(value.amount);
  writer.Float(value.paddingPx);
  writer.Count(value.amountKeyframes.size());
  for (const auto &keyframe : value.amountKeyframes)
    Encode(writer, keyframe);
  writer.Count(value.parameters.size());
  for (const auto &parameter : value.parameters)
    Encode(writer, parameter);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextDecorationAnchorKeyframe &value) noexcept {
  writer.Float(value.offset);
  writer.Enumeration(value.value);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextDecorationFitKeyframe &value) noexcept {
  writer.Float(value.offset);
  writer.Enumeration(value.value);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextDecorationAnimationSpec &value) noexcept {
  writer.String(value.decorationId);
  writer.String(value.assetId);
  writer.Enumeration(value.anchor);
  writer.Count(value.anchorKeyframes.size());
  for (const auto &keyframe : value.anchorKeyframes)
    Encode(writer, keyframe);
  writer.Enumeration(value.fit);
  writer.Count(value.fitKeyframes.size());
  for (const auto &keyframe : value.fitKeyframes)
    Encode(writer, keyframe);
  writer.Enumeration(value.extentSpace);
  writer.Enumeration(value.inherit);
  writer.Enumeration(value.playback);
  writer.Float(value.expandRatioX);
  writer.Float(value.expandRatioY);
  writer.Float(value.sourceOutsets.left);
  writer.Float(value.sourceOutsets.top);
  writer.Float(value.sourceOutsets.right);
  writer.Float(value.sourceOutsets.bottom);
  writer.Float(value.pivotX);
  writer.Float(value.pivotY);
  writer.Float(value.offsetX);
  writer.Float(value.offsetY);
  writer.Float(value.relativeOffsetX);
  writer.Float(value.relativeOffsetY);
  writer.Float(value.scaleX);
  writer.Float(value.scaleY);
  writer.Float(value.rotationXDegrees);
  writer.Float(value.rotationYDegrees);
  writer.Float(value.rotationDegrees);
  writer.Float(value.opacity);
  const auto encodeKeyframes = [&](const std::vector<text::TextKeyframe> &keyframes) {
    writer.Count(keyframes.size());
    for (const auto &keyframe : keyframes)
      Encode(writer, keyframe);
  };
  encodeKeyframes(value.pivotXKeyframes);
  encodeKeyframes(value.pivotYKeyframes);
  encodeKeyframes(value.offsetXKeyframes);
  encodeKeyframes(value.offsetYKeyframes);
  encodeKeyframes(value.relativeOffsetXKeyframes);
  encodeKeyframes(value.relativeOffsetYKeyframes);
  encodeKeyframes(value.scaleXKeyframes);
  encodeKeyframes(value.scaleYKeyframes);
  encodeKeyframes(value.rotationXKeyframes);
  encodeKeyframes(value.rotationYKeyframes);
  encodeKeyframes(value.rotationKeyframes);
  encodeKeyframes(value.opacityKeyframes);
  encodeKeyframes(value.assetProgressKeyframes);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextRenderGroupSpec &value) noexcept {
  writer.Float(value.expandRatioX);
  writer.Float(value.expandRatioY);
  writer.Enumeration(value.mode);
  writer.Float(value.offset);
  writer.Boolean(value.duration.has_value());
  if (value.duration) {
    writer.Signed(value.duration->startTimeUs);
    writer.Signed(value.duration->endTimeUs);
  }
  writer.Signed(value.priority);
  writer.Unsigned(value.randomSeed);
  writer.Boolean(value.randomSort);
  writer.Enumeration(value.shape);
  writer.Count(value.customRanges.size());
  for (const auto &range : value.customRanges) {
    writer.Signed(range.startIndex);
    writer.Signed(range.endIndex);
    writer.Boolean(range.localTime.has_value());
    if (range.localTime) {
      writer.Signed(range.localTime->startTimeUs);
      writer.Signed(range.localTime->endTimeUs);
    }
    writer.Float(range.intensity);
  }
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextEffectInstruction &value) noexcept {
  writer.Enumeration(value.opcode);
  writer.Unsigned(value.outputRegister);
  writer.Count(value.inputRegisters.size());
  for (const auto input : value.inputRegisters)
    writer.Unsigned(input);
  writer.Count(value.immediates.size());
  for (const auto immediate : value.immediates)
    writer.Double(immediate);
  writer.Boolean(value.input.has_value());
  if (value.input)
    writer.Enumeration(*value.input);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextEffectProgramStageIR &value) noexcept {
  writer.String(value.stageId);
  writer.Enumeration(value.kind);
  writer.Unsigned(value.registerCount);
  writer.Count(value.instructions.size());
  for (const auto &instruction : value.instructions)
    Encode(writer, instruction);
  writer.Count(value.outputs.size());
  for (const auto &output : value.outputs) {
    writer.Enumeration(output.output);
    writer.Unsigned(output.registerIndex);
    writer.Unsigned(output.transformIndex);
  }
  writer.Count(value.executionParameterBindings.size());
  for (const auto &binding : value.executionParameterBindings) {
    writer.String(binding.nodeId);
    writer.Enumeration(binding.parameter);
    writer.Enumeration(binding.domain);
    writer.Enumeration(binding.valueSpace);
    writer.Unsigned(binding.slot);
    writer.Count(binding.registerIndices.size());
    for (const auto registerIndex : binding.registerIndices)
      writer.Unsigned(registerIndex);
  }
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextEffectProgramIR &value) noexcept {
  writer.String(value.programId);
  writer.Unsigned(value.randomSeed);
  writer.Count(value.stages.size());
  for (const auto &stage : value.stages)
    Encode(writer, stage);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextAnimationLayerSpec &value) noexcept {
  writer.String(value.layerId);
  writer.Boolean(value.enabled);
  Encode(writer, value.target);
  writer.Enumeration(value.combineMode);
  Encode(writer, value.timeDriver);
  writer.Boolean(value.layerTrack.has_value());
  if (value.layerTrack)
    Encode(writer, *value.layerTrack);
  writer.Count(value.animators.size());
  for (const auto &animator : value.animators)
    Encode(writer, animator);
  writer.Boolean(value.renderGroup.has_value());
  if (value.renderGroup)
    Encode(writer, *value.renderGroup);
  writer.Count(value.decorations.size());
  for (const auto &decoration : value.decorations)
    Encode(writer, decoration);
  writer.Count(value.postEffects.size());
  for (const auto &effect : value.postEffects)
    Encode(writer, effect);
  writer.Boolean(value.requiresTimedText);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextEffectPhysicsSpec &value) noexcept {
  writer.Unsigned(value.fixedStepHz);
  writer.Float(value.gravityX);
  writer.Float(value.gravityY);
  writer.Float(value.angularDamping);
  writer.Signed(value.explosionDelayUs);
  writer.Signed(value.explosionForceDurationUs);
  writer.Signed(value.explosionRampUpUs);
  writer.Float(value.explosionAccelerationMin);
  writer.Float(value.explosionAccelerationMax);
  writer.Float(value.explosionOriginYShiftFactor);
  writer.Float(value.explosionAngleRangeDegrees);
  writer.Float(value.initialAngularVelocityScale);
  writer.Unsigned(value.randomSeed);
  writer.Float(value.randomPhase);
  writer.Float(value.randomScale);
  writer.Unsigned(value.angleSeedStride);
  writer.Unsigned(value.speedSeedStride);
  writer.Unsigned(value.rotationSeedStride);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextEffectCollisionSpec &value) noexcept {
  writer.Float(value.fixedBoxWidth);
  writer.Float(value.fixedBoxHeight);
  writer.Float(value.boxCenterXScale);
  writer.Float(value.wallDamping);
  writer.Float(value.sideWallVelocityScale);
  writer.Float(value.topWallVelocityScale);
  writer.Float(value.bottomWallDamping);
  writer.Float(value.wallTorqueScale);
  writer.Float(value.collisionBoundaryScale);
  writer.Float(value.collisionRestitution);
  writer.Float(value.collisionTorqueScale);
  writer.Float(value.minimumCellSize);
  writer.Float(value.cellSizeScale);
  writer.Boolean(value.expandHorizontalToContent);
  writer.Boolean(value.expandTopToContent);
  writer.Boolean(value.collideLeft);
  writer.Boolean(value.collideRight);
  writer.Boolean(value.collideTop);
  writer.Boolean(value.collideBottom);
}

inline void Encode(
    CanonicalTextIdentityWriter &writer,
    const text::TextEffectExecutionStaticAffine &value) noexcept {
  writer.Float(value.translationX);
  writer.Float(value.translationY);
  writer.Float(value.scaleX);
  writer.Float(value.scaleY);
  writer.Float(value.rotationDegrees);
  writer.Float(value.pivotX);
  writer.Float(value.pivotY);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextEffectExecutionCamera &value) noexcept {
  for (const auto component : value.worldToClip)
    writer.Float(component);
  for (const auto component : value.viewport)
    writer.Float(component);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextEffectExecutionNode &value) noexcept {
  writer.String(value.nodeId);
  writer.String(value.ownerLayerId);
  writer.Enumeration(value.kind);
  writer.Enumeration(value.capability);
  writer.Count(value.inputIds.size());
  for (const auto &inputId : value.inputIds)
    writer.String(inputId);
  writer.Count(value.resourceIds.size());
  for (const auto &resourceId : value.resourceIds)
    writer.String(resourceId);
  writer.Boolean(value.postEffectKind.has_value());
  if (value.postEffectKind)
    writer.Enumeration(*value.postEffectKind);
  writer.String(value.stateId);
  writer.Unsigned(value.randomSeed);
  writer.String(value.historyId);
  writer.Boolean(value.timeDriver.has_value());
  if (value.timeDriver)
    Encode(writer, *value.timeDriver);
  writer.Boolean(value.physicsSpec.has_value());
  if (value.physicsSpec)
    Encode(writer, *value.physicsSpec);
  writer.Boolean(value.collisionSpec.has_value());
  if (value.collisionSpec)
    Encode(writer, *value.collisionSpec);
  writer.Boolean(value.staticAffine.has_value());
  if (value.staticAffine)
    Encode(writer, *value.staticAffine);
  writer.Boolean(value.camera.has_value());
  if (value.camera)
    Encode(writer, *value.camera);
  writer.Count(value.children.size());
  for (const auto &child : value.children)
    Encode(writer, child);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextEffectExecutionGraph &value) noexcept {
  writer.Count(value.nodes.size());
  for (const auto &node : value.nodes)
    Encode(writer, node);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const text::TextAnimationStack &value) noexcept {
  writer.Count(value.effectPrograms.size());
  for (const auto &program : value.effectPrograms)
    Encode(writer, program);
  writer.Count(value.layers.size());
  for (const auto &layer : value.layers)
    Encode(writer, layer);
  Encode(writer, value.executionGraph);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const vector::VectorFieldValue &value) noexcept {
  writer.Enumeration(value.kind);
  switch (value.kind) {
  case vector::VectorFieldKind::Color:
    writer.Float(value.color.red);
    writer.Float(value.color.green);
    writer.Float(value.color.blue);
    writer.Float(value.color.alpha);
    break;
  case vector::VectorFieldKind::Text:
    writer.String(value.text);
    break;
  case vector::VectorFieldKind::Number:
    writer.Double(value.number);
    break;
  }
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const vector::VectorFieldOverride &value) noexcept {
  writer.String(value.fieldId);
  Encode(writer, value.value);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const DecorationAssetReference &value) noexcept {
  writer.String(value.assetId);
  writer.String(value.digest);
  writer.String(value.mediaType);
  writer.Float(value.intrinsicWidth);
  writer.Float(value.intrinsicHeight);
  writer.Unsigned(value.estimatedResourceBytes);
  writer.Count(value.staticFieldOverrides.size());
  for (const auto &field : value.staticFieldOverrides)
    Encode(writer, field);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const DecorationTarget &value) noexcept {
  writer.Enumeration(value.scope);
  writer.String(value.paragraphId);
  writer.String(value.runId);
  Encode(writer, value.range);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const VectorDecorationBinding &value) noexcept {
  writer.String(value.decorationId);
  writer.Boolean(value.enabled);
  Encode(writer, value.asset);
  writer.Boolean(value.instancePattern.has_value());
  if (value.instancePattern) {
    writer.Count(value.instancePattern->assetVariants.size());
    for (const auto &variant : value.instancePattern->assetVariants) {
      writer.String(variant.variantId);
      Encode(writer, variant.asset);
    }
    writer.Enumeration(value.instancePattern->selectionPolicy);
    writer.Count(value.instancePattern->explicitPattern.size());
    for (const auto index : value.instancePattern->explicitPattern)
      writer.Unsigned(index);
    writer.Unsigned(value.instancePattern->randomSeed);
    writer.Float(value.instancePattern->offsetXStep);
    writer.Float(value.instancePattern->offsetYStep);
    writer.Float(value.instancePattern->scaleStep);
    writer.Float(value.instancePattern->rotationStepDegrees);
    writer.Enumeration(value.instancePattern->phasePolicy);
    writer.Signed(value.instancePattern->phaseStepUs);
  }
  writer.Enumeration(value.mode);
  Encode(writer, value.target);
  writer.Enumeration(value.placementDriver);
  writer.Enumeration(value.anchor);
  writer.Enumeration(value.fit);
  Encode(writer, value.padding);
  writer.Float(value.localTransform.offsetX);
  writer.Float(value.localTransform.offsetY);
  writer.Float(value.localTransform.scaleX);
  writer.Float(value.localTransform.scaleY);
  writer.Float(value.localTransform.rotationDegrees);
  writer.Float(value.localTransform.opacity);
  writer.Enumeration(value.assetPlayback.clock);
  writer.Enumeration(value.assetPlayback.mode);
  writer.Signed(value.assetPlayback.sourceInUs);
  writer.Signed(value.assetPlayback.sourceOutUs);
  writer.Double(value.assetPlayback.speed);
  writer.Signed(value.assetPlayback.phaseUs);
  writer.Signed(value.zOrder);
  writer.Enumeration(value.transformInherit);
  writer.Enumeration(value.sampling);
  writer.Enumeration(value.effectScope);
  writer.Enumeration(value.fallback);
  writer.String(value.fallbackAssetId);
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const TimedTextTrack &value) noexcept {
  writer.Enumeration(value.clock);
  writer.Count(value.spans.size());
  for (const auto &span : value.spans) {
    writer.String(span.spanId);
    writer.String(span.paragraphId);
    writer.String(span.runId);
    Encode(writer, span.range);
    writer.Signed(span.startOffsetUs);
    writer.Signed(span.endOffsetUs);
    writer.String(span.semantic);
    writer.Enumeration(span.progressMode);
    writer.Signed(span.transitionEndOffsetUs);
  }
}

inline void Encode(CanonicalTextIdentityWriter &writer,
                   const TextResourceReference &value) noexcept {
  writer.String(value.resourceId);
  writer.Enumeration(value.kind);
  writer.Enumeration(value.ownership);
  writer.String(value.assetId);
  writer.String(value.digest);
  writer.String(value.mediaType);
}

inline void EncodeContentIdentity(CanonicalTextIdentityWriter &writer,
                                  const TextCompositionDocument &document)
    noexcept {
  writer.Count(document.contentSlots.size());
  for (const auto &slot : document.contentSlots)
    Encode(writer, slot);
  writer.Count(document.content.size());
  for (const auto &paragraph : document.content) {
    writer.String(paragraph.paragraphId);
    writer.Count(paragraph.runs.size());
    for (const auto &run : paragraph.runs) {
      writer.String(run.runId);
      writer.String(run.utf8Text);
      writer.String(run.locale);
    }
  }
}

inline void EncodeLayoutIdentity(CanonicalTextIdentityWriter &writer,
                                 const TextCompositionDocument &document)
    noexcept {
  Encode(writer, document.presentation.referenceCanvas);
  Encode(writer, document.presentation.authoredLayoutFrame);
  writer.Enumeration(document.presentation.writingMode);
  writer.Count(document.content.size());
  for (const auto &paragraph : document.content)
    Encode(writer, paragraph, false);
  Encode(writer, document.presentation.appearance.bend);
  writer.Boolean(document.presentation.appearance.path.enabled);
  writer.String(document.presentation.appearance.path.geometryId);
  writer.Count(document.presentation.appearance.path.commands.size());
  for (const auto &command : document.presentation.appearance.path.commands) {
    writer.Enumeration(command.kind);
    writer.Float(command.control1.x);
    writer.Float(command.control1.y);
    writer.Float(command.control2.x);
    writer.Float(command.control2.y);
    writer.Float(command.end.x);
    writer.Float(command.end.y);
  }
  writer.Float(document.presentation.appearance.path.startOffset);
  writer.Float(document.presentation.appearance.path.baselineOffset);
  writer.Enumeration(document.presentation.appearance.path.overflow);
  writer.Boolean(document.presentation.appearance.path.loop);
  writer.Boolean(document.presentation.appearance.path.rotateToTangent);
  writer.Boolean(document.presentation.appearance.path.keepUpright);
}

inline void EncodeGlyphIdentity(CanonicalTextIdentityWriter &writer,
                                const TextCompositionDocument &document)
    noexcept {
  writer.Count(document.content.size());
  for (const auto &paragraph : document.content) {
    writer.String(paragraph.paragraphId);
    writer.Count(paragraph.runs.size());
    for (const auto &run : paragraph.runs) {
      writer.String(run.runId);
      Encode(writer, run.style.materials);
      Encode(writer, run.style.background);
      Encode(writer, run.style.decoration);
    }
  }
  Encode(writer, document.presentation.appearance.sdfMaterial);
  writer.Float(document.presentation.appearance.globalAlpha);
}

inline void EncodeBackdropIdentity(CanonicalTextIdentityWriter &writer,
                                   const TextCompositionDocument &document)
    noexcept {
  Encode(writer, document.presentation.appearance.backdrops);
}

inline void EncodeResourceIdentity(CanonicalTextIdentityWriter &writer,
                                   const TextCompositionDocument &document)
    noexcept {
  writer.Count(document.resources.size());
  for (const auto &resource : document.resources)
    Encode(writer, resource);
  writer.Count(document.decorations.size());
  for (const auto &binding : document.decorations) {
    Encode(writer, binding.asset);
    writer.Boolean(binding.instancePattern.has_value());
    if (binding.instancePattern) {
      writer.Count(binding.instancePattern->assetVariants.size());
      for (const auto &variant : binding.instancePattern->assetVariants)
        Encode(writer, variant.asset);
    }
  }
}

inline void EncodeDecorationIdentity(CanonicalTextIdentityWriter &writer,
                                     const TextCompositionDocument &document)
    noexcept {
  writer.Count(document.decorations.size());
  for (const auto &binding : document.decorations)
    Encode(writer, binding);
  writer.Enumeration(document.effectPolicy);
  writer.Unsigned(document.decorationBudget.maximumDecorations);
  writer.Unsigned(document.decorationBudget.maximumInstancesPerBinding);
  writer.Unsigned(document.decorationBudget.maximumInstances);
  writer.Unsigned(document.decorationBudget.maximumPixelsPerBinding);
  writer.Unsigned(document.decorationBudget.maximumPixels);
  writer.Unsigned(document.decorationBudget.maximumWorkingSetBytes);
  writer.Unsigned(document.decorationBudget.maximumInstanceDimension);
  writer.Unsigned(document.decorationBudget.maximumResourceBytesPerDecoration);
  writer.Unsigned(document.decorationBudget.maximumResourceBytes);
}

inline void EncodeDocumentIdentity(CanonicalTextIdentityWriter &writer,
                                   const TextCompositionDocument &document)
    noexcept {
  writer.Unsigned(document.schemaRevision);
  writer.Signed(document.durationTicks);
  writer.Enumeration(document.role);
  writer.Count(document.contentSlots.size());
  for (const auto &slot : document.contentSlots)
    Encode(writer, slot);
  writer.Count(document.content.size());
  for (const auto &paragraph : document.content)
    Encode(writer, paragraph);
  writer.Boolean(document.timedText.has_value());
  if (document.timedText)
    Encode(writer, *document.timedText);
  Encode(writer, document.presentation.referenceCanvas);
  Encode(writer, document.presentation.authoredLayoutFrame);
  writer.Enumeration(document.presentation.writingMode);
  Encode(writer, document.presentation.appearance);
  Encode(writer, document.animations);
  writer.Count(document.decorations.size());
  for (const auto &binding : document.decorations)
    Encode(writer, binding);
  writer.Count(document.resources.size());
  for (const auto &resource : document.resources)
    Encode(writer, resource);
  writer.Enumeration(document.effectPolicy);
  writer.Unsigned(document.decorationBudget.maximumDecorations);
  writer.Unsigned(document.decorationBudget.maximumInstancesPerBinding);
  writer.Unsigned(document.decorationBudget.maximumInstances);
  writer.Unsigned(document.decorationBudget.maximumPixelsPerBinding);
  writer.Unsigned(document.decorationBudget.maximumPixels);
  writer.Unsigned(document.decorationBudget.maximumWorkingSetBytes);
  writer.Unsigned(document.decorationBudget.maximumInstanceDimension);
  writer.Unsigned(document.decorationBudget.maximumResourceBytesPerDecoration);
  writer.Unsigned(document.decorationBudget.maximumResourceBytes);
  writer.Boolean(document.templateOrigin.has_value());
  if (document.templateOrigin) {
    writer.String(document.templateOrigin->templateId);
    writer.String(document.templateOrigin->packageDigest);
  }
  const auto encodeOrigin = [&](const std::optional<TextTemplateOrigin> &origin) {
    writer.Boolean(origin.has_value());
    if (origin) {
      writer.String(origin->templateId);
      writer.String(origin->packageDigest);
    }
  };
  encodeOrigin(document.bubbleTemplateOrigin);
  encodeOrigin(document.flowerTemplateOrigin);
  encodeOrigin(document.animationTemplateOrigin);
}

template <typename Encoder>
inline std::string MakeIdentity(const std::string_view domain,
                                Encoder &&encoder) {
  CanonicalTextIdentityWriter writer(domain);
  encoder(writer);
  return writer.Finish();
}

inline std::string TextContentDigest(
    const TextCompositionDocument &document) {
  std::string flattened;
  std::size_t byteCount = document.content.empty()
                              ? 0U
                              : document.content.size() - 1U;
  for (const auto &paragraph : document.content)
    for (const auto &run : paragraph.runs)
      byteCount += run.utf8Text.size();
  flattened.reserve(byteCount);
  for (std::size_t paragraphIndex = 0U;
       paragraphIndex < document.content.size(); ++paragraphIndex) {
    if (paragraphIndex != 0U)
      flattened.push_back('\n');
    for (const auto &run : document.content[paragraphIndex].runs)
      flattened.append(run.utf8Text);
  }
  return vector::Sha256Digest(
      reinterpret_cast<const std::uint8_t *>(flattened.data()),
      flattened.size());
}

inline std::string TextStyleDigest(const text::TextStyle &style) {
  return MakeIdentity("videocut.text.style", [&](auto &writer) {
    Encode(writer, style);
  });
}

} // namespace videocut::text_composition::detail
