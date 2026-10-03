#pragma once

#include "internal/skia/SkiaHeaders.h"

#include "videocut/text/FontReference.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace videocut::skia_runtime::internal {

enum class FontInstanceFailureCode : std::uint8_t {
  InvalidContentIdentity = 0,
  InvalidFaceIndex,
  BaseTypefaceUnavailable,
  InvalidAxisTag,
  NonFiniteAxisValue,
  DuplicateAxis,
  AxisCapacityExceeded,
  TypefaceAxisMetadataUnavailable,
  TypefaceAxisMetadataInvalid,
  UnknownAxis,
  AxisValueOutOfRange,
  CloneFailed,
  CloneVerificationFailed,
};

struct FontInstanceFailure final {
  FontInstanceFailureCode code{
      FontInstanceFailureCode::BaseTypefaceUnavailable};
  std::string axisTag;
  float requestedValue{0.0F};
  float minimumValue{0.0F};
  float maximumValue{0.0F};

  [[nodiscard]] std::string Describe(std::string_view family) const;
};

struct ResolvedFontInstance final {
  sk_sp<SkTypeface> typeface;
  std::string identity;
  std::optional<FontInstanceFailure> failure;
  std::vector<text::FontAxisRange> supportedAxes;
  bool substituted{false};

  [[nodiscard]] explicit operator bool() const noexcept {
    return typeface != nullptr && !failure.has_value();
  }
};

/// Stateless authority for immutable SkTypeface instances. The caller keeps
/// the established lane-local FontKey cache; this resolver only canonicalizes
/// and validates one requested face/axis identity before creating it.
class FontInstanceResolver final {
public:
  [[nodiscard]] static std::string
  ReferenceCacheKey(const text::FontReference &reference);

  [[nodiscard]] static ResolvedFontInstance
  ResolveFromData(const text::FontReference &reference,
                  const sk_sp<SkFontMgr> &fontManager,
                  const sk_sp<SkData> &fontData, std::string contentIdentity);

  [[nodiscard]] static ResolvedFontInstance
  ResolveFromTypeface(const text::FontReference &reference,
                      sk_sp<SkTypeface> baseTypeface,
                      std::string contentIdentity);
};

} // namespace videocut::skia_runtime::internal
