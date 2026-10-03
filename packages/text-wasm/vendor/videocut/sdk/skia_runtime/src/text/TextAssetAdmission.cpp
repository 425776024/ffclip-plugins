#include "videocut/skia_runtime/SkiaRuntimeFactory.h"
#include "internal/skia/SkiaHeaders.h"
#include "include/core/SkFourByteTag.h"
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>

namespace videocut::skia_runtime {

bool ValidateTextFontBytes(const std::vector<std::uint8_t> &bytes,
                           const std::uint32_t faceIndex,
                           std::string &error) noexcept {
  return ValidateTextFontBytes(bytes.data(), bytes.size(), faceIndex, error);
}

bool ValidateTextFontBytes(const std::uint8_t *bytes,
                           const std::size_t byteLength,
                           const std::uint32_t faceIndex,
                           std::string &error) noexcept {
  try {
    if (bytes == nullptr || byteLength == 0U ||
        byteLength > 128U * 1024U * 1024U) {
      error = "Text Font bytes are empty or exceed the admission budget";
      return false;
    }
    auto data = SkData::MakeWithCopy(bytes, byteLength);
    auto manager = SkFontMgr_New_Custom_Data({});
    auto typeface =
        data && manager
            ? manager->makeFromData(data, static_cast<int>(faceIndex))
            : nullptr;
    if (!typeface) {
      error = "Text Font bytes do not contain the selected font face";
      return false;
    }
    error.clear();
    return true;
  } catch (const std::exception &exception) {
    error = exception.what();
  } catch (...) {
    error = "Text Font validation raised an unknown failure";
  }
  return false;
}

bool ResolveTextFontSdfMetricProfile(
    const std::vector<std::uint8_t> &bytes, const std::uint32_t faceIndex,
    TextFontSdfMetricProfile &profile, std::string &error) noexcept {
  profile = {};
  profile.qtSdfScale = 1.0F;
  try {
    if (bytes.empty() || bytes.size() > 128U * 1024U * 1024U) {
      error = "Text Font bytes are empty or exceed the admission budget";
      return false;
    }
    if (faceIndex > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
      error = "Text Font face index exceeds the Skia representation";
      return false;
    }
    auto data = SkData::MakeWithCopy(bytes.data(), bytes.size());
    auto manager = SkFontMgr_New_Custom_Data({});
    auto typeface =
        data && manager
            ? manager->makeFromData(data, static_cast<int>(faceIndex))
            : nullptr;
    if (!typeface) {
      error = "Text Font bytes do not contain the selected font face";
      return false;
    }
    const int unitsPerEm = typeface->getUnitsPerEm();
    if (unitsPerEm <= 0 || unitsPerEm > std::numeric_limits<std::uint16_t>::max()) {
      error = "Text Font face has no bounded units-per-em metric";
      return false;
    }
    profile.unitsPerEm = static_cast<std::uint32_t>(unitsPerEm);

    // OS/2 v0 and later place sTypoAscender/sTypoDescender at byte offsets
    // 68/70. SkTypeface exposes the selected TTC face's decompressed table,
    // so this remains valid for TTC/WOFF/WOFF2 inputs as well as raw sfnt.
    constexpr SkFontTableTag kOs2Tag = SkSetFourByteTag('O', 'S', '/', '2');
    const auto table = typeface->copyTableData(kOs2Tag);
    if (!table || table->size() < 74U) {
      // A valid font can omit OS/2 (or carry a legacy short table). Keep the
      // established neutral scale and let the caller report availability;
      // inventing metrics here would make the text geometry less portable.
      error.clear();
      return true;
    }
    const auto *tableBytes = static_cast<const std::uint8_t *>(table->data());
    const auto readSigned16 = [](const std::uint8_t *value) {
      const auto raw = static_cast<std::uint16_t>(
          (static_cast<std::uint16_t>(value[0U]) << 8U) |
          static_cast<std::uint16_t>(value[1U]));
      return static_cast<std::int32_t>(static_cast<std::int16_t>(raw));
    };
    const auto ascender = readSigned16(tableBytes + 68U);
    const auto descender = readSigned16(tableBytes + 70U);
    const auto lineGap = readSigned16(tableBytes + 72U);
    const auto selectionFlags = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(tableBytes[62U]) << 8U) |
        static_cast<std::uint16_t>(tableBytes[63U]));
    const auto height = ascender - descender;
    if (ascender <= descender || height <= 0 ||
        height > std::numeric_limits<std::uint16_t>::max()) {
      error.clear();
      return true;
    }
    const float scale =
        1.2F * static_cast<float>(profile.unitsPerEm) /
        static_cast<float>(height);
    if (!std::isfinite(scale) || scale <= 0.0F || scale > 64.0F) {
      error.clear();
      return true;
    }
    profile.typographicAscender = ascender;
    profile.typographicDescender = descender;
    profile.typographicLineGap = lineGap;
    profile.selectionFlags = selectionFlags;
    profile.typographicMetricsAvailable = true;
    profile.qtSdfScale = scale;

    // Skia's paragraph baseline is derived from hhea ascent/descent. TextPro
    // follows the OS/2 typographic pair for fonts that opt into the
    // USE_TYPO_METRICS line-box flag; the complete OS/2 line gap completes
    // that typographic box. Keep this as a normalized em correction so the
    // caller can apply it at any authored size and output scale.
    constexpr SkFontTableTag kHheaTag = SkSetFourByteTag('h', 'h', 'e', 'a');
    const auto hhea = typeface->copyTableData(kHheaTag);
    if (hhea && hhea->size() >= 10U) {
      const auto *hheaBytes =
          static_cast<const std::uint8_t *>(hhea->data());
      const auto horizontalAscender = readSigned16(hheaBytes + 4U);
      const auto horizontalDescender = readSigned16(hheaBytes + 6U);
      const auto horizontalLineGap = readSigned16(hheaBytes + 8U);
      const auto horizontalHeight =
          horizontalAscender - horizontalDescender;
      if (horizontalAscender > horizontalDescender &&
          horizontalHeight > 0 &&
          horizontalHeight <= std::numeric_limits<std::uint16_t>::max()) {
        profile.horizontalAscender = horizontalAscender;
        profile.horizontalDescender = horizontalDescender;
        profile.horizontalLineGap = horizontalLineGap;
        profile.baselineMetricsAvailable = true;
        // OS/2 fsSelection bit 7 is USE_TYPO_METRICS. TextPro centers the
        // glyph packet between the typographic pair and the hhea pair after
        // the complete typographic line gap is included. Leaving equal pairs
        // unchanged keeps the correction neutral for fonts whose tables
        // already agree. Keep this as one public, data-driven formula rather
        // than branching on font family or cache identity.
        constexpr float kQtSdfTypographicLineGapCenterWeight = 1.0F;
        const bool hheaIncludesTypographicLeading =
            horizontalAscender != ascender || horizontalDescender != descender;
        const std::int32_t qtBaselineSum =
            ascender + descender +
            ((selectionFlags & 0x0080U) != 0U &&
             hheaIncludesTypographicLeading
                 ? static_cast<std::int32_t>(std::lround(
                       static_cast<float>(lineGap) *
                       kQtSdfTypographicLineGapCenterWeight))
                 : 0);
        const std::int32_t skiaBaselineSum =
            horizontalAscender + horizontalDescender;
        profile.qtSdfBaselineShiftEm =
            static_cast<float>(qtBaselineSum - skiaBaselineSum) /
            (2.0F * static_cast<float>(profile.unitsPerEm));
        if (!std::isfinite(profile.qtSdfBaselineShiftEm))
          profile.qtSdfBaselineShiftEm = 0.0F;
      }
    }
    error.clear();
    return true;
  } catch (const std::exception &exception) {
    error = exception.what();
  } catch (...) {
    error = "Text Font SDF metric resolution raised an unknown failure";
  }
  return false;
}

bool ValidateTextTextureBytes(const std::vector<std::uint8_t> &bytes,
                              const std::string &mediaType,
                              std::string &error) noexcept {
  try {
    if (bytes.empty() || bytes.size() > 64U * 1024U * 1024U) {
      error = "Text texture bytes are empty or exceed the admission budget";
      return false;
    }
    const auto *dataBytes = bytes.data();
    const bool signatureMatches =
        mediaType == "image/png"
            ? bytes.size() >= 8U && dataBytes[0] == 0x89U &&
                  dataBytes[1] == 0x50U && dataBytes[2] == 0x4eU &&
                  dataBytes[3] == 0x47U && dataBytes[4] == 0x0dU &&
                  dataBytes[5] == 0x0aU && dataBytes[6] == 0x1aU &&
                  dataBytes[7] == 0x0aU
        : mediaType == "image/jpeg"
            ? bytes.size() >= 3U && dataBytes[0] == 0xffU &&
                  dataBytes[1] == 0xd8U && dataBytes[2] == 0xffU
        : mediaType == "image/webp"
            ? bytes.size() >= 12U &&
                  std::memcmp(dataBytes, "RIFF", 4U) == 0 &&
                  std::memcmp(dataBytes + 8U, "WEBP", 4U) == 0
            : false;
    auto data = signatureMatches
                    ? SkData::MakeWithCopy(bytes.data(), bytes.size())
                    : nullptr;
    auto image =
        data ? SkImages::DeferredFromEncodedData(std::move(data)) : nullptr;
    if (!image || image->width() <= 0 || image->height() <= 0 ||
        static_cast<std::uint64_t>(image->width()) *
                static_cast<std::uint64_t>(image->height()) >
            64U * 1024U * 1024U ||
        (image->colorSpace() && !image->colorSpace()->isSRGB())) {
      error = "Text texture bytes do not match the declared sRGB image";
      return false;
    }
    error.clear();
    return true;
  } catch (const std::exception &exception) {
    error = exception.what();
  } catch (...) {
    error = "Text texture validation raised an unknown failure";
  }
  return false;
}

} // namespace videocut::skia_runtime
