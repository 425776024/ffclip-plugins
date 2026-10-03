#include "text/FontInstanceResolver.h"

#include "include/core/SkFontArguments.h"
#include "include/core/SkFontParameters.h"
#include "include/core/SkTypeface.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace videocut::skia_runtime::internal {
namespace {

using Coordinate = SkFontArguments::VariationPosition::Coordinate;
using AxisParameter = SkFontParameters::Variation::Axis;
constexpr std::size_t kMaximumFontAxes = 256U;

struct CanonicalAxis final {
  std::string text;
  SkFourByteTag tag{0U};
  float value{0.0F};
};

SkFourByteTag AxisTag(const std::string_view value) noexcept {
  return static_cast<SkFourByteTag>(
      (static_cast<std::uint32_t>(static_cast<unsigned char>(value[0U]))
       << 24U) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(value[1U]))
       << 16U) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(value[2U]))
       << 8U) |
      static_cast<std::uint32_t>(static_cast<unsigned char>(value[3U])));
}

std::string AxisText(const SkFourByteTag tag) {
  std::string result(4U, '\0');
  result[0U] = static_cast<char>((tag >> 24U) & 0xffU);
  result[1U] = static_cast<char>((tag >> 16U) & 0xffU);
  result[2U] = static_cast<char>((tag >> 8U) & 0xffU);
  result[3U] = static_cast<char>(tag & 0xffU);
  return result;
}

ResolvedFontInstance Failure(const FontInstanceFailureCode code,
                             std::string axisTag = {},
                             const float requestedValue = 0.0F,
                             const float minimumValue = 0.0F,
                             const float maximumValue = 0.0F) {
  ResolvedFontInstance result;
  result.failure = FontInstanceFailure{code, std::move(axisTag), requestedValue,
                                       minimumValue, maximumValue};
  return result;
}

bool CanonicalizeAxes(const text::FontReference &reference,
                      std::vector<CanonicalAxis> &axes,
                      FontInstanceFailure &failure) {
  axes.clear();
  if (reference.variationAxes.size() > kMaximumFontAxes) {
    failure = {FontInstanceFailureCode::AxisCapacityExceeded};
    return false;
  }
  axes.reserve(reference.variationAxes.size());
  for (const auto &authored : reference.variationAxes) {
    if (authored.tag.size() != 4U) {
      failure = {FontInstanceFailureCode::InvalidAxisTag, authored.tag};
      return false;
    }
    if (!std::isfinite(authored.value)) {
      failure = {FontInstanceFailureCode::NonFiniteAxisValue, authored.tag,
                 authored.value};
      return false;
    }
    const float canonicalValue = authored.value == 0.0F ? 0.0F : authored.value;
    axes.push_back({authored.tag, AxisTag(authored.tag), canonicalValue});
  }
  std::sort(axes.begin(), axes.end(), [](const auto &left, const auto &right) {
    return left.tag < right.tag;
  });
  const auto duplicate = std::adjacent_find(
      axes.begin(), axes.end(), [](const auto &left, const auto &right) {
        return left.tag == right.tag;
      });
  if (duplicate != axes.end()) {
    failure = {FontInstanceFailureCode::DuplicateAxis, duplicate->text,
               duplicate->value};
    return false;
  }
  return true;
}

std::string InstanceIdentity(const std::string_view contentIdentity,
                             const std::uint32_t faceIndex,
                             const std::vector<CanonicalAxis> &axes) {
  std::ostringstream stream;
  stream << contentIdentity.size() << ':' << contentIdentity
         << "|face=" << faceIndex;
  for (const auto &axis : axes) {
    stream << "|axis=" << axis.text << ':' << std::hexfloat << axis.value;
  }
  return stream.str();
}

bool NearlyEqual(const float left, const float right) noexcept {
  const float scale = std::max({1.0F, std::fabs(left), std::fabs(right)});
  return std::fabs(left - right) <=
         8.0F * std::numeric_limits<float>::epsilon() * scale;
}

ResolvedFontInstance Resolve(const text::FontReference &reference,
                             sk_sp<SkTypeface> baseTypeface,
                             std::string contentIdentity) {
  if (contentIdentity.empty())
    return Failure(FontInstanceFailureCode::InvalidContentIdentity);
  if (reference.faceIndex >
      static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
    return Failure(FontInstanceFailureCode::InvalidFaceIndex);
  }
  if (!baseTypeface)
    return Failure(FontInstanceFailureCode::BaseTypefaceUnavailable);

  std::vector<CanonicalAxis> axes;
  FontInstanceFailure canonicalFailure;
  if (!CanonicalizeAxes(reference, axes, canonicalFailure)) {
    ResolvedFontInstance result;
    result.failure = std::move(canonicalFailure);
    return result;
  }

  const std::string identity =
      InstanceIdentity(contentIdentity, reference.faceIndex, axes);
  const int parameterCount = baseTypeface->getVariationDesignParameters({});
  if (parameterCount <= 0) {
    // Preserve the established static-font path exactly. In particular, do
    // not clone static fonts or reinterpret their matched family/style.
    if (axes.empty())
      return {std::move(baseTypeface), identity, std::nullopt, {}, false};
    return Failure(FontInstanceFailureCode::TypefaceAxisMetadataUnavailable,
                   axes.front().text, axes.front().value);
  }
  if (static_cast<std::size_t>(parameterCount) > kMaximumFontAxes) {
    if (axes.empty())
      return {std::move(baseTypeface), identity, std::nullopt, {}, false};
    return Failure(FontInstanceFailureCode::AxisCapacityExceeded);
  }
  std::vector<AxisParameter> parameters(
      static_cast<std::size_t>(parameterCount));
  if (baseTypeface->getVariationDesignParameters(parameters) !=
      parameterCount) {
    if (axes.empty())
      return {std::move(baseTypeface), identity, std::nullopt, {}, false};
    return Failure(FontInstanceFailureCode::TypefaceAxisMetadataUnavailable,
                   axes.front().text, axes.front().value);
  }
  std::sort(
      parameters.begin(), parameters.end(),
      [](const auto &left, const auto &right) { return left.tag < right.tag; });
  for (std::size_t index = 0U; index < parameters.size(); ++index) {
    const auto &parameter = parameters[index];
    if (!std::isfinite(parameter.min) || !std::isfinite(parameter.def) ||
        !std::isfinite(parameter.max) || parameter.min > parameter.def ||
        parameter.def > parameter.max ||
        (index != 0U && parameters[index - 1U].tag == parameter.tag)) {
      if (axes.empty())
        return {std::move(baseTypeface), identity, std::nullopt, {}, false};
      return Failure(FontInstanceFailureCode::TypefaceAxisMetadataInvalid,
                     AxisText(parameter.tag));
    }
  }
  std::vector<text::FontAxisRange> supportedAxes;
  supportedAxes.reserve(parameters.size());
  for (const auto &parameter : parameters) {
    supportedAxes.push_back({AxisText(parameter.tag), parameter.min,
                             parameter.def, parameter.max});
  }
  if (axes.empty()) {
    return {std::move(baseTypeface), identity, std::nullopt,
            std::move(supportedAxes), false};
  }

  std::vector<Coordinate> coordinates;
  coordinates.reserve(axes.size());
  for (const auto &axis : axes) {
    const auto found = std::lower_bound(
        parameters.begin(), parameters.end(), axis.tag,
        [](const AxisParameter &parameter, const SkFourByteTag tag) {
          return parameter.tag < tag;
        });
    if (found == parameters.end() || found->tag != axis.tag) {
      return Failure(FontInstanceFailureCode::UnknownAxis, axis.text,
                     axis.value);
    }
    if (axis.value < found->min || axis.value > found->max) {
      return Failure(FontInstanceFailureCode::AxisValueOutOfRange, axis.text,
                     axis.value, found->min, found->max);
    }
    coordinates.push_back({axis.tag, axis.value});
  }

  SkFontArguments arguments;
  arguments.setCollectionIndex(static_cast<int>(reference.faceIndex));
  arguments.setVariationDesignPosition(
      {coordinates.data(), static_cast<int>(coordinates.size())});
  auto instance = baseTypeface->makeClone(arguments);
  if (!instance) {
    return Failure(FontInstanceFailureCode::CloneFailed, axes.front().text,
                   axes.front().value);
  }

  const int resolvedCount = instance->getVariationDesignPosition({});
  if (resolvedCount <= 0) {
    return Failure(FontInstanceFailureCode::CloneVerificationFailed,
                   axes.front().text, axes.front().value);
  }
  if (static_cast<std::size_t>(resolvedCount) > kMaximumFontAxes)
    return Failure(FontInstanceFailureCode::AxisCapacityExceeded);
  std::vector<Coordinate> resolved(static_cast<std::size_t>(resolvedCount));
  if (instance->getVariationDesignPosition(resolved) != resolvedCount) {
    return Failure(FontInstanceFailureCode::CloneVerificationFailed,
                   axes.front().text, axes.front().value);
  }
  std::sort(resolved.begin(), resolved.end(),
            [](const auto &left, const auto &right) {
              return left.axis < right.axis;
            });
  for (const auto &axis : axes) {
    const auto found = std::lower_bound(
        resolved.begin(), resolved.end(), axis.tag,
        [](const Coordinate &coordinate, const SkFourByteTag tag) {
          return coordinate.axis < tag;
        });
    if (found == resolved.end() || found->axis != axis.tag ||
        !std::isfinite(found->value) ||
        !NearlyEqual(found->value, axis.value)) {
      return Failure(FontInstanceFailureCode::CloneVerificationFailed,
                     axis.text, axis.value);
    }
  }
  return {std::move(instance), identity, std::nullopt,
          std::move(supportedAxes), false};
}

} // namespace

std::string FontInstanceFailure::Describe(const std::string_view family) const {
  std::ostringstream stream;
  stream << "font instance ";
  switch (code) {
  case FontInstanceFailureCode::InvalidContentIdentity:
    stream << "has no immutable content identity";
    break;
  case FontInstanceFailureCode::InvalidFaceIndex:
    stream << "face index exceeds the Skia representation";
    break;
  case FontInstanceFailureCode::BaseTypefaceUnavailable:
    stream << "base typeface is unavailable";
    break;
  case FontInstanceFailureCode::InvalidAxisTag:
    stream << "axis tag must contain exactly four bytes";
    break;
  case FontInstanceFailureCode::NonFiniteAxisValue:
    stream << "axis value is not finite";
    break;
  case FontInstanceFailureCode::DuplicateAxis:
    stream << "contains a duplicate axis";
    break;
  case FontInstanceFailureCode::AxisCapacityExceeded:
    stream << "axis count exceeds the maintained capacity";
    break;
  case FontInstanceFailureCode::TypefaceAxisMetadataUnavailable:
    stream << "requested axes from a typeface without variation metadata";
    break;
  case FontInstanceFailureCode::TypefaceAxisMetadataInvalid:
    stream << "typeface variation metadata is invalid";
    break;
  case FontInstanceFailureCode::UnknownAxis:
    stream << "requested an unknown axis";
    break;
  case FontInstanceFailureCode::AxisValueOutOfRange:
    stream << "axis value is outside the authored typeface range";
    break;
  case FontInstanceFailureCode::CloneFailed:
    stream << "could not create the requested variation clone";
    break;
  case FontInstanceFailureCode::CloneVerificationFailed:
    stream << "variation clone did not retain the requested coordinates";
    break;
  }
  if (!axisTag.empty()) {
    stream << ": axis=" << axisTag;
    if (code == FontInstanceFailureCode::NonFiniteAxisValue ||
        code == FontInstanceFailureCode::DuplicateAxis ||
        code == FontInstanceFailureCode::TypefaceAxisMetadataUnavailable ||
        code == FontInstanceFailureCode::UnknownAxis ||
        code == FontInstanceFailureCode::AxisValueOutOfRange ||
        code == FontInstanceFailureCode::CloneFailed ||
        code == FontInstanceFailureCode::CloneVerificationFailed) {
      stream << " value=" << requestedValue;
    }
    if (code == FontInstanceFailureCode::AxisValueOutOfRange) {
      stream << " range=[" << minimumValue << ',' << maximumValue << ']';
    }
  }
  if (!family.empty())
    stream << "; family=" << family;
  return stream.str();
}

std::string
FontInstanceResolver::ReferenceCacheKey(const text::FontReference &reference) {
  std::vector<text::FontAxis> canonicalAxes = reference.variationAxes;
  std::sort(canonicalAxes.begin(), canonicalAxes.end(),
            [](const auto &left, const auto &right) {
              if (left.tag != right.tag)
                return left.tag < right.tag;
              return left.value < right.value;
            });
  std::ostringstream stream;
  stream << static_cast<unsigned>(reference.kind) << '|' << reference.family
         << '|' << reference.postscriptName << '|' << reference.weight << '|'
         << reference.width << '|' << static_cast<unsigned>(reference.slant)
         << '|' << reference.faceIndex << '|' << reference.assetId << '|'
         << reference.platform << '|' << reference.digest << '|'
         << reference.faceFingerprint << '|'
         << reference.allowSystemGlyphFallback;
  for (const auto &axis : canonicalAxes) {
    const float canonicalValue = axis.value == 0.0F ? 0.0F : axis.value;
    stream << '|' << axis.tag << '=' << std::hexfloat << canonicalValue;
  }
  return stream.str();
}

ResolvedFontInstance FontInstanceResolver::ResolveFromData(
    const text::FontReference &reference, const sk_sp<SkFontMgr> &fontManager,
    const sk_sp<SkData> &fontData, std::string contentIdentity) {
  if (!fontManager || !fontData || fontData->isEmpty())
    return Failure(FontInstanceFailureCode::BaseTypefaceUnavailable);
  if (reference.faceIndex >
      static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
    return Failure(FontInstanceFailureCode::InvalidFaceIndex);
  }
  auto baseTypeface = fontManager->makeFromData(
      fontData, static_cast<int>(reference.faceIndex));
  return Resolve(reference, std::move(baseTypeface),
                 std::move(contentIdentity));
}

ResolvedFontInstance
FontInstanceResolver::ResolveFromTypeface(const text::FontReference &reference,
                                          sk_sp<SkTypeface> baseTypeface,
                                          std::string contentIdentity) {
  return Resolve(reference, std::move(baseTypeface),
                 std::move(contentIdentity));
}

} // namespace videocut::skia_runtime::internal
