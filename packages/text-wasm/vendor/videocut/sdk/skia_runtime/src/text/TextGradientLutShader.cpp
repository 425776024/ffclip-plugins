#include "text/TextRuntimeShader.h"
#include "text/TextGradientLutShader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace videocut::skia_runtime::internal {
namespace {

constexpr float kLutLastTexel =
    static_cast<float>(kTextGradientLutSampleCount - 1U);
constexpr float kDegenerateGeometryEpsilon = 1.0e-12F;

struct QuantizedStop final {
  float offset{0.0F};
  std::array<std::uint8_t, kTextGradientLutChannelCount> color{};
};

std::uint8_t TruncatedByte(const float value) noexcept {
  return static_cast<std::uint8_t>(
      static_cast<int>(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

std::uint8_t PremultipliedByte(const std::uint8_t component,
                               const std::uint8_t alpha) noexcept {
  return static_cast<std::uint8_t>(
      (static_cast<std::uint16_t>(component) *
       static_cast<std::uint16_t>(alpha)) /
      255U);
}

bool IsFinite(const SkPoint point) noexcept {
  return std::isfinite(point.x()) && std::isfinite(point.y());
}

sk_sp<SkShader> MakeLutImageShader(const TextGradientLut256 &lut,
                                   std::string &error) {
  if (!lut.image ||
      lut.image->width() != static_cast<int>(kTextGradientLutSampleCount) ||
      lut.image->height() != 1 ||
      lut.image->colorType() != kRGBA_8888_SkColorType ||
      lut.image->alphaType() != kPremul_SkAlphaType) {
    error = "text gradient LUT is not a 256x1 premultiplied RGBA8 image";
    return {};
  }
  auto shader =
      lut.image->makeRawShader(SkTileMode::kClamp, SkTileMode::kClamp,
                               SkSamplingOptions(SkFilterMode::kLinear));
  if (!shader)
    error = "text gradient LUT raw image shader creation failed";
  return shader;
}

} // namespace

SkPoint
TextGradientPaintFrame::Resolve(const SkPoint normalizedPoint) const noexcept {
  const SkPoint scaled{
      0.5F + (normalizedPoint.x() - 0.5F) * coordinateScale,
      0.5F + (normalizedPoint.y() - 0.5F) * coordinateScale,
  };
  return {bounds.left() + scaled.x() * bounds.width(),
          bounds.top() + scaled.y() * bounds.height()};
}

std::optional<TextGradientLut256> BuildTextGradientLut256(
    const std::span<const TextGradientLutStop> authoredStops,
    std::string &error) {
  error.clear();
  if (authoredStops.empty()) {
    error = "text gradient LUT requires at least one stop";
    return std::nullopt;
  }

  std::vector<QuantizedStop> stops;
  stops.reserve(authoredStops.size());
  for (const auto &stop : authoredStops) {
    if (!std::isfinite(stop.offset) || !std::isfinite(stop.color.fR) ||
        !std::isfinite(stop.color.fG) || !std::isfinite(stop.color.fB) ||
        !std::isfinite(stop.color.fA)) {
      error = "text gradient LUT stop contains a non-finite value";
      return std::nullopt;
    }
    const std::uint8_t alpha = TruncatedByte(stop.color.fA);
    const std::uint8_t red = TruncatedByte(stop.color.fR);
    const std::uint8_t green = TruncatedByte(stop.color.fG);
    const std::uint8_t blue = TruncatedByte(stop.color.fB);
    // Qt's captured RGBA8 LUT premultiplies every quantized stop before its
    // adjacent-stop float32 fold. Moving this multiply to the fragment is not
    // equivalent: transparent colored endpoints then contribute a different
    // curve and cross different byte truncation boundaries.
    stops.push_back({std::clamp(stop.offset, 0.0F, 1.0F),
                     {PremultipliedByte(red, alpha),
                      PremultipliedByte(green, alpha),
                      PremultipliedByte(blue, alpha), alpha}});
  }
  std::stable_sort(stops.begin(), stops.end(),
                   [](const auto &left, const auto &right) {
                     return left.offset < right.offset;
                   });

  TextGradientLut256 lut;
  for (std::size_t index = 0U; index < kTextGradientLutSampleCount; ++index) {
    const float position = static_cast<float>(index) / kLutLastTexel;
    std::array<float, kTextGradientLutChannelCount> sampled{};
    for (std::size_t channel = 0U; channel < kTextGradientLutChannelCount;
         ++channel) {
      sampled[channel] = static_cast<float>(stops.front().color[channel]);
    }

    // TextPro folds each adjacent stop into the previous integer result.  It
    // deliberately performs two multiplies and one add before every signed
    // truncation; this is observably different from selecting one segment and
    // evaluating left + t * (right - left).  In particular, an otherwise
    // constant 255 channel becomes 254 at a few float32 sample positions.
    for (std::size_t stopIndex = 1U; stopIndex < stops.size(); ++stopIndex) {
      const float span = stops[stopIndex].offset - stops[stopIndex - 1U].offset;
      const float amount =
          span >= 1.0e-6F
              ? std::clamp((position - stops[stopIndex - 1U].offset) / span,
                           0.0F, 1.0F)
              : 0.0F;
      const float inverseAmount = 1.0F - amount;
      for (std::size_t channel = 0U; channel < kTextGradientLutChannelCount;
           ++channel) {
#pragma clang fp contract(off)
        const float leftContribution = sampled[channel] * inverseAmount;
        const float rightContribution =
            static_cast<float>(stops[stopIndex].color[channel]) * amount;
        const float interpolated = leftContribution + rightContribution;
        sampled[channel] = static_cast<float>(
            static_cast<int>(std::clamp(interpolated, 0.0F, 255.0F)));
      }
    }
    for (std::size_t channel = 0U; channel < kTextGradientLutChannelCount;
         ++channel) {
      lut.rgba[index * kTextGradientLutChannelCount + channel] =
          static_cast<std::uint8_t>(sampled[channel]);
    }
  }

  const auto data = SkData::MakeWithCopy(lut.rgba.data(), lut.rgba.size());
  if (!data) {
    error = "text gradient LUT pixel allocation failed";
    return std::nullopt;
  }
  const auto info =
      SkImageInfo::Make(static_cast<int>(kTextGradientLutSampleCount), 1,
                        kRGBA_8888_SkColorType, kPremul_SkAlphaType, nullptr);
  lut.image = SkImages::RasterFromData(
      info, data, kTextGradientLutSampleCount * kTextGradientLutChannelCount);
  if (!lut.image) {
    error = "text gradient LUT image creation failed";
    return std::nullopt;
  }
  return lut;
}

sk_sp<SkShader> MakeTextLinearGradientLutShader(const TextGradientLut256 &lut,
                                                const SkPoint start,
                                                const SkPoint end,
                                                std::string &error) {
  error.clear();
  if (!IsFinite(start) || !IsFinite(end)) {
    error = "text linear gradient coordinates must be finite";
    return {};
  }
  const auto &program =
      GetTextRuntimeProgram(TextRuntimeShader::LinearGradient);
  if (!program.effect) {
    error = "text linear gradient runtime shader compilation failed: " +
            program.error;
    return {};
  }
  auto imageShader = MakeLutImageShader(lut, error);
  if (!imageShader)
    return {};

  const float deltaX = end.x() - start.x();
  const float deltaY = end.y() - start.y();
  const float lengthSquared = deltaX * deltaX + deltaY * deltaY;
  const float inverseLengthSquared =
      lengthSquared > kDegenerateGeometryEpsilon ? 1.0F / lengthSquared : 0.0F;
  const std::array<float, 2> startUniform{start.x(), start.y()};
  const std::array<float, 2> projectionUniform{deltaX * inverseLengthSquared,
                                               deltaY * inverseLengthSquared};
  SkRuntimeEffectBuilder builder(program.effect);
  builder.uniform("gradientStart") = startUniform;
  builder.uniform("gradientProjection") = projectionUniform;
  builder.child("gradientLut") = std::move(imageShader);
  auto shader = builder.makeShader();
  if (!shader)
    error = "text linear gradient runtime shader creation failed";
  return shader;
}

sk_sp<SkShader> MakeTextLinearGradientLutShader(
    const TextGradientLut256 &lut, const TextGradientPaintFrame &frame,
    const SkPoint normalizedStart, const SkPoint normalizedEnd,
    std::string &error) {
  if (!IsFinite(normalizedStart) || !IsFinite(normalizedEnd) ||
      !std::isfinite(frame.bounds.left()) ||
      !std::isfinite(frame.bounds.top()) ||
      !std::isfinite(frame.bounds.right()) ||
      !std::isfinite(frame.bounds.bottom()) ||
      !std::isfinite(frame.coordinateScale) ||
      frame.coordinateScale <= 0.0F) {
    error = "text linear gradient paint frame must be finite";
    return {};
  }
  return MakeTextLinearGradientLutShader(lut, frame.Resolve(normalizedStart),
                                         frame.Resolve(normalizedEnd), error);
}

sk_sp<SkShader> MakeTextRadialGradientLutShader(const TextGradientLut256 &lut,
                                                const SkPoint center,
                                                const float radius,
                                                std::string &error) {
  error.clear();
  if (!IsFinite(center) || !std::isfinite(radius) || radius < 0.0F) {
    error = "text radial gradient geometry must be finite and non-negative";
    return {};
  }
  const auto &program =
      GetTextRuntimeProgram(TextRuntimeShader::RadialGradient);
  if (!program.effect) {
    error = "text radial gradient runtime shader compilation failed: " +
            program.error;
    return {};
  }
  auto imageShader = MakeLutImageShader(lut, error);
  if (!imageShader)
    return {};

  const std::array<float, 2> centerUniform{center.x(), center.y()};
  const float inverseRadius =
      radius > kDegenerateGeometryEpsilon ? 1.0F / radius : 0.0F;
  SkRuntimeEffectBuilder builder(program.effect);
  builder.uniform("gradientCenter") = centerUniform;
  builder.uniform("inverseRadius") = inverseRadius;
  builder.child("gradientLut") = std::move(imageShader);
  auto shader = builder.makeShader();
  if (!shader)
    error = "text radial gradient runtime shader creation failed";
  return shader;
}

sk_sp<SkShader> MakeTextRadialGradientLutShader(
    const TextGradientLut256 &lut, const TextGradientPaintFrame &frame,
    const SkPoint normalizedCenter, const float normalizedRadius,
    std::string &error) {
  if (!IsFinite(normalizedCenter) || !std::isfinite(normalizedRadius) ||
      !std::isfinite(frame.bounds.left()) ||
      !std::isfinite(frame.bounds.top()) ||
      !std::isfinite(frame.bounds.right()) ||
      !std::isfinite(frame.bounds.bottom()) ||
      !std::isfinite(frame.coordinateScale) ||
      frame.coordinateScale <= 0.0F) {
    error = "text radial gradient paint frame must be finite";
    return {};
  }
  const float radius = normalizedRadius * frame.coordinateScale *
                       std::max(frame.bounds.width(), frame.bounds.height());
  return MakeTextRadialGradientLutShader(lut, frame.Resolve(normalizedCenter),
                                         radius, error);
}

} // namespace videocut::skia_runtime::internal
