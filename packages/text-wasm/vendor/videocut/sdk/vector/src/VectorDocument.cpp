#include "videocut/vector/VectorDocument.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace videocut::vector {
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

bool SafeLogicalName(const std::string &value) noexcept {
  if (value.empty() || value.size() > 4096 || value.front() == '/' ||
      value.front() == '\\' || value.find('\\') != std::string::npos ||
      value.find(':') != std::string::npos ||
      value.find('\0') != std::string::npos)
    return false;
  std::size_t begin = 0;
  while (begin <= value.size()) {
    const auto end = value.find('/', begin);
    const auto count =
        end == std::string::npos ? value.size() - begin : end - begin;
    const auto part = value.substr(begin, count);
    if (part.empty() || part == "." || part == "..")
      return false;
    if (end == std::string::npos)
      break;
    begin = end + 1;
  }
  return true;
}

bool HasCapability(const VectorDocument &document,
                   const std::string_view capability) noexcept {
  return std::any_of(document.probe.capabilitiesUsed.begin(),
                     document.probe.capabilitiesUsed.end(),
                     [&](const std::string &value) {
                       return value == capability;
                     });
}

std::uint32_t BigEndian32(const std::uint8_t *bytes) noexcept;

bool SniffIsoBmffFtyp(const std::vector<std::uint8_t> &bytes) noexcept {
  // The full MP4 structure and codec are admitted by the memory-only media
  // decoder. Vector's neutral boundary only accepts a bounded ISO-BMFF byte
  // source carrying an early ftyp box; this deliberately does not make plain
  // video/mp4 a generally supported Vector resource.
  if (bytes.size() < 12U)
    return false;
  const std::size_t searchLimit = std::min<std::size_t>(bytes.size(), 4096U);
  std::size_t offset = 0U;
  while (offset + 8U <= searchLimit) {
    const std::uint32_t size32 = BigEndian32(bytes.data() + offset);
    const bool isFtyp =
        std::memcmp(bytes.data() + offset + 4U, "ftyp", 4U) == 0;
    if (isFtyp)
      return size32 >= 12U && size32 <= bytes.size() - offset;
    if (size32 == 0U)
      return false;
    if (size32 == 1U) {
      if (offset + 16U > searchLimit)
        return false;
      std::uint64_t size64 = 0U;
      for (std::size_t index = 0U; index < 8U; ++index)
        size64 = (size64 << 8U) | bytes[offset + 8U + index];
      if (size64 < 16U || size64 > searchLimit - offset)
        return false;
      offset += static_cast<std::size_t>(size64);
      continue;
    }
    if (size32 < 8U || size32 > searchLimit - offset)
      return false;
    offset += size32;
  }
  return false;
}

std::uint32_t BigEndian32(const std::uint8_t *bytes) noexcept {
  return (static_cast<std::uint32_t>(bytes[0]) << 24) |
         (static_cast<std::uint32_t>(bytes[1]) << 16) |
         (static_cast<std::uint32_t>(bytes[2]) << 8) |
         static_cast<std::uint32_t>(bytes[3]);
}

std::uint32_t LittleEndian24(const std::uint8_t *bytes) noexcept {
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16);
}

std::uint32_t LittleEndian32(const std::uint8_t *bytes) noexcept {
  return LittleEndian24(bytes) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

bool SniffRasterDimensions(const std::string_view mediaType,
                           const std::vector<std::uint8_t> &bytes,
                           std::uint32_t &width,
                           std::uint32_t &height) noexcept {
  width = 0;
  height = 0;
  if (mediaType == "image/png" && bytes.size() >= 24 &&
      std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0 &&
      std::memcmp(bytes.data() + 12, "IHDR", 4) == 0) {
    width = BigEndian32(bytes.data() + 16);
    height = BigEndian32(bytes.data() + 20);
    return width > 0 && height > 0;
  }
  if (mediaType == "image/webp" && bytes.size() >= 30 &&
      std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
      std::memcmp(bytes.data() + 8, "WEBP", 4) == 0) {
    if (std::memcmp(bytes.data() + 12, "VP8X", 4) == 0) {
      width = LittleEndian24(bytes.data() + 24) + 1;
      height = LittleEndian24(bytes.data() + 27) + 1;
      return width > 0 && height > 0;
    }
    if (std::memcmp(bytes.data() + 12, "VP8L", 4) == 0 && bytes.size() >= 25 &&
        bytes[20] == 0x2f) {
      const std::uint32_t bits = static_cast<std::uint32_t>(bytes[21]) |
                                 (static_cast<std::uint32_t>(bytes[22]) << 8) |
                                 (static_cast<std::uint32_t>(bytes[23]) << 16) |
                                 (static_cast<std::uint32_t>(bytes[24]) << 24);
      width = (bits & 0x3fffU) + 1U;
      height = ((bits >> 14) & 0x3fffU) + 1U;
      return true;
    }
    if (std::memcmp(bytes.data() + 12, "VP8 ", 4) == 0 && bytes.size() >= 30 &&
        bytes[23] == 0x9d && bytes[24] == 0x01 && bytes[25] == 0x2a) {
      width = (static_cast<std::uint32_t>(bytes[26]) |
               (static_cast<std::uint32_t>(bytes[27]) << 8)) &
              0x3fffU;
      height = (static_cast<std::uint32_t>(bytes[28]) |
                (static_cast<std::uint32_t>(bytes[29]) << 8)) &
               0x3fffU;
      return width > 0 && height > 0;
    }
    return false;
  }
  if (mediaType == "image/jpeg" && bytes.size() >= 4 && bytes[0] == 0xff &&
      bytes[1] == 0xd8) {
    std::size_t offset = 2;
    while (offset + 8 < bytes.size()) {
      if (bytes[offset++] != 0xff)
        return false;
      while (offset < bytes.size() && bytes[offset] == 0xff)
        ++offset;
      if (offset >= bytes.size())
        return false;
      const std::uint8_t marker = bytes[offset++];
      if (marker == 0xd9 || marker == 0xda)
        break;
      if (offset + 2 > bytes.size())
        return false;
      const std::size_t length =
          (static_cast<std::size_t>(bytes[offset]) << 8) | bytes[offset + 1];
      if (length < 2 || offset + length > bytes.size())
        return false;
      const bool startOfFrame = (marker >= 0xc0 && marker <= 0xc3) ||
                                (marker >= 0xc5 && marker <= 0xc7) ||
                                (marker >= 0xc9 && marker <= 0xcb) ||
                                (marker >= 0xcd && marker <= 0xcf);
      if (startOfFrame && length >= 7) {
        height = (static_cast<std::uint32_t>(bytes[offset + 3]) << 8) |
                 bytes[offset + 4];
        width = (static_cast<std::uint32_t>(bytes[offset + 5]) << 8) |
                bytes[offset + 6];
        return width > 0 && height > 0;
      }
      offset += length;
    }
  }
  return false;
}

bool SniffFont(const std::vector<std::uint8_t> &bytes) noexcept {
  if (bytes.size() < 12)
    return false;
  return (bytes[0] == 0x00 && bytes[1] == 0x01 && bytes[2] == 0x00 &&
          bytes[3] == 0x00) ||
         std::memcmp(bytes.data(), "OTTO", 4) == 0 ||
         std::memcmp(bytes.data(), "true", 4) == 0 ||
         std::memcmp(bytes.data(), "typ1", 4) == 0 ||
         std::memcmp(bytes.data(), "ttcf", 4) == 0;
}

bool DecodeBase64(const std::string_view value, const std::size_t maximumBytes,
                  std::vector<std::uint8_t> &output) {
  output.clear();
  if (value.empty() || value.size() % 4 != 0 ||
      value.size() / 4 > maximumBytes / 3 + 1)
    return false;
  output.reserve(std::min(maximumBytes, value.size() / 4 * 3));
  const auto digit = [](const unsigned char byte) -> int {
    if (byte >= 'A' && byte <= 'Z')
      return byte - 'A';
    if (byte >= 'a' && byte <= 'z')
      return byte - 'a' + 26;
    if (byte >= '0' && byte <= '9')
      return byte - '0' + 52;
    if (byte == '+')
      return 62;
    if (byte == '/')
      return 63;
    return -1;
  };
  for (std::size_t offset = 0; offset < value.size(); offset += 4) {
    const bool last = offset + 4 == value.size();
    const int a = digit(static_cast<unsigned char>(value[offset]));
    const int b = digit(static_cast<unsigned char>(value[offset + 1]));
    const bool padC = value[offset + 2] == '=';
    const bool padD = value[offset + 3] == '=';
    const int c =
        padC ? 0 : digit(static_cast<unsigned char>(value[offset + 2]));
    const int d =
        padD ? 0 : digit(static_cast<unsigned char>(value[offset + 3]));
    if (a < 0 || b < 0 || c < 0 || d < 0 || (padC && !padD) ||
        ((padC || padD) && !last)) {
      output.clear();
      return false;
    }
    const std::uint32_t bits = (static_cast<std::uint32_t>(a) << 18) |
                               (static_cast<std::uint32_t>(b) << 12) |
                               (static_cast<std::uint32_t>(c) << 6) |
                               static_cast<std::uint32_t>(d);
    output.push_back(static_cast<std::uint8_t>(bits >> 16));
    if (!padC)
      output.push_back(static_cast<std::uint8_t>(bits >> 8));
    if (!padD)
      output.push_back(static_cast<std::uint8_t>(bits));
    if (output.size() > maximumBytes) {
      output.clear();
      return false;
    }
  }
  return true;
}

std::size_t RasterFrameCount(const std::string_view mediaType,
                             const std::vector<std::uint8_t> &bytes) noexcept {
  if (mediaType == "image/png") {
    std::size_t offset = 8;
    std::size_t frames = 1;
    while (offset + 12 <= bytes.size()) {
      const auto length = BigEndian32(bytes.data() + offset);
      if (length > bytes.size() - offset - 12)
        return 0;
      const auto *kind = bytes.data() + offset + 4;
      if (std::memcmp(kind, "acTL", 4) == 0) {
        if (length != 8)
          return 0;
        frames = BigEndian32(bytes.data() + offset + 8);
        if (frames == 0)
          return 0;
      }
      offset += 12 + length;
      if (std::memcmp(kind, "IEND", 4) == 0)
        return length == 0 && offset == bytes.size() ? frames : 0;
    }
    return 0;
  }
  if (mediaType == "image/webp") {
    if (bytes.size() < 12 ||
        LittleEndian32(bytes.data() + 4) + 8U != bytes.size())
      return 0;
    std::size_t frames = 0;
    std::size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
      const auto length = LittleEndian32(bytes.data() + offset + 4);
      if (length > bytes.size() - offset - 8)
        return 0;
      if (std::memcmp(bytes.data() + offset, "ANMF", 4) == 0)
        ++frames;
      offset += 8 + length + (length & 1U);
    }
    return offset == bytes.size() ? (frames == 0 ? 1 : frames) : 0;
  }
  return mediaType == "image/jpeg" && bytes.size() >= 4 &&
                 bytes[bytes.size() - 2] == 0xff &&
                 bytes[bytes.size() - 1] == 0xd9
             ? 1
             : 0;
}

struct RasterBudgetState final {
  std::uint64_t decodedPixels{0};
  std::uint64_t decodedPixelFrames{0};
  std::size_t decodedFrames{0};
};

bool AddRasterPayloadToBudget(
    const std::string_view mediaType, const std::vector<std::uint8_t> &bytes,
    const VectorLimits &limits, RasterBudgetState &budget,
    std::uint32_t *admittedWidth = nullptr,
    std::uint32_t *admittedHeight = nullptr) noexcept {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  if (!SniffRasterDimensions(mediaType, bytes, width, height))
    return false;
  const auto pixels = static_cast<std::uint64_t>(width) * height;
  const auto frames = RasterFrameCount(mediaType, bytes);
  if (pixels == 0 || pixels > limits.maximumPixels || frames == 0 ||
      frames > limits.maximumLottieImageFrames ||
      budget.decodedPixels > limits.maximumPixels - pixels ||
      budget.decodedFrames > limits.maximumLottieImageFrames - frames ||
      pixels > limits.maximumDecodedPixelFrames / frames ||
      budget.decodedPixelFrames >
          limits.maximumDecodedPixelFrames - pixels * frames)
    return false;
  budget.decodedPixels += pixels;
  budget.decodedFrames += frames;
  budget.decodedPixelFrames += pixels * frames;
  if (admittedWidth)
    *admittedWidth = width;
  if (admittedHeight)
    *admittedHeight = height;
  return true;
}

bool DecodeRasterDataUri(const std::string_view value,
                         const VectorLimits &limits, RasterBudgetState &budget,
                         std::uint32_t *admittedWidth = nullptr,
                         std::uint32_t *admittedHeight = nullptr) {
  struct Prefix final {
    std::string_view text;
    std::string_view mediaType;
  };
  static constexpr Prefix prefixes[] = {
      {"data:image/png;base64,", "image/png"},
      {"data:image/jpeg;base64,", "image/jpeg"},
      {"data:image/webp;base64,", "image/webp"},
  };
  const auto prefix = std::find_if(
      std::begin(prefixes), std::end(prefixes), [&](const Prefix &candidate) {
        return value.substr(0, candidate.text.size()) == candidate.text;
      });
  if (prefix == std::end(prefixes) ||
      value.size() > limits.maximumEmbeddedDataUriBytes)
    return false;
  std::vector<std::uint8_t> decoded;
  if (!DecodeBase64(value.substr(prefix->text.size()),
                    limits.maximumEmbeddedDataUriBytes, decoded))
    return false;
  return AddRasterPayloadToBudget(prefix->mediaType, decoded, limits, budget,
                                  admittedWidth, admittedHeight);
}

std::string LowerAscii(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const unsigned char byte : value) {
    result.push_back(static_cast<char>(
        byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte));
  }
  return result;
}

bool XmlNameStart(const unsigned char value) noexcept {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         value == '_';
}

bool XmlNameByte(const unsigned char value) noexcept {
  return XmlNameStart(value) || (value >= '0' && value <= '9') ||
         value == '-' || value == '.' || value == ':';
}

bool ValidUtf8(const std::string_view value) noexcept {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto lead = static_cast<unsigned char>(value[offset++]);
    if (lead <= 0x7f)
      continue;
    std::size_t continuations = 0;
    unsigned char secondMinimum = 0x80;
    unsigned char secondMaximum = 0xbf;
    if (lead >= 0xc2 && lead <= 0xdf) {
      continuations = 1;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      continuations = 2;
      if (lead == 0xe0)
        secondMinimum = 0xa0;
      else if (lead == 0xed)
        secondMaximum = 0x9f;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      continuations = 3;
      if (lead == 0xf0)
        secondMinimum = 0x90;
      else if (lead == 0xf4)
        secondMaximum = 0x8f;
    } else {
      return false;
    }
    if (continuations > value.size() - offset)
      return false;
    for (std::size_t index = 0; index < continuations; ++index) {
      const auto byte = static_cast<unsigned char>(value[offset + index]);
      if (byte < (index == 0 ? secondMinimum : 0x80) ||
          byte > (index == 0 ? secondMaximum : 0xbf))
        return false;
    }
    offset += continuations;
  }
  return true;
}

bool ValidXmlCodePoint(const std::uint32_t value) noexcept {
  return value == 0x09 || value == 0x0a || value == 0x0d ||
         (value >= 0x20 && value <= 0xd7ff) ||
         (value >= 0xe000 && value <= 0xfffd) ||
         (value >= 0x10000 && value <= 0x10ffff);
}

bool SafeXmlText(std::string_view value) noexcept {
  if (value.find("]]>") != std::string_view::npos)
    return false;
  for (const auto raw : value) {
    const auto byte = static_cast<unsigned char>(raw);
    if (byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r')
      return false;
  }
  std::size_t offset = 0;
  while ((offset = value.find('&', offset)) != std::string_view::npos) {
    const auto end = value.find(';', offset + 1);
    if (end == std::string_view::npos || end - offset > 12)
      return false;
    const auto entity = value.substr(offset, end - offset + 1);
    const bool predefined = entity == "&amp;" || entity == "&lt;" ||
                            entity == "&gt;" || entity == "&quot;" ||
                            entity == "&apos;";
    bool numeric = entity.size() >= 4 && entity[1] == '#';
    std::uint32_t codePoint = 0;
    if (numeric) {
      std::size_t index = 2;
      unsigned int base = 10;
      if (index + 1 < entity.size() &&
          (entity[index] == 'x' || entity[index] == 'X')) {
        base = 16;
        ++index;
      }
      const auto digitsEnd = entity.size() - 1;
      if (index == digitsEnd)
        numeric = false;
      for (; numeric && index < digitsEnd; ++index) {
        const auto byte = static_cast<unsigned char>(entity[index]);
        unsigned int digit = 0;
        if (byte >= '0' && byte <= '9')
          digit = byte - '0';
        else if (base == 16 && byte >= 'a' && byte <= 'f')
          digit = byte - 'a' + 10U;
        else if (base == 16 && byte >= 'A' && byte <= 'F')
          digit = byte - 'A' + 10U;
        else {
          numeric = false;
          break;
        }
        if (digit >= base ||
            codePoint >
                (std::numeric_limits<std::uint32_t>::max() - digit) / base) {
          numeric = false;
          break;
        }
        codePoint = codePoint * base + digit;
      }
      numeric = numeric && ValidXmlCodePoint(codePoint);
    }
    if (!predefined && !numeric)
      return false;
    offset = end + 1;
  }
  return true;
}

bool DeclaredVectorResource(const VectorDocument &document,
                            const std::string_view value) {
  return std::any_of(document.resources.begin(), document.resources.end(),
                     [&](const VectorResource &resource) {
                       return resource.logicalName == value;
                     });
}

bool SafeLocalFragment(const std::string_view value) noexcept {
  return value.size() > 1 && value.front() == '#' &&
         std::all_of(value.begin() + 1, value.end(), [](unsigned char byte) {
           return (byte >= 'a' && byte <= 'z') ||
                  (byte >= 'A' && byte <= 'Z') ||
                  (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' ||
                  byte == '.' || byte == ':';
         });
}

bool SafeSvgReferenceValue(const VectorDocument &document,
                           const std::string_view value,
                           const VectorLimits &limits,
                           RasterBudgetState &rasterBudget) {
  if (SafeLocalFragment(value))
    return true;
  if (value.substr(0, 5) == "data:")
    return DecodeRasterDataUri(value, limits, rasterBudget);
  return value.size() <= limits.maximumStringBytes &&
         DeclaredVectorResource(document, value);
}

bool SafeSvgPaintValue(const std::string_view value) noexcept {
  const auto lower = LowerAscii(value);
  if (lower.find("javascript:") != std::string::npos ||
      lower.find("file:") != std::string::npos ||
      lower.find("http:") != std::string::npos ||
      lower.find("https:") != std::string::npos ||
      lower.find("@import") != std::string::npos ||
      lower.find("expression(") != std::string::npos)
    return false;
  std::size_t offset = 0;
  while ((offset = lower.find("url(", offset)) != std::string::npos) {
    const auto end = lower.find(')', offset + 4);
    if (end == std::string::npos ||
        !SafeLocalFragment(
            std::string_view(lower).substr(offset + 4, end - offset - 4)))
      return false;
    offset = end + 1;
  }
  return true;
}

bool ParseSvgNumber(std::string_view value, double &parsed) noexcept {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())))
    value.remove_suffix(1);
  if (value.empty() || value.size() > 128U)
    return false;
  if (value.front() == '+') {
    value.remove_prefix(1);
    if (value.empty())
      return false;
  }
  // Floating-point from_chars in current libc++ requires macOS 26; keep the
  // macOS 15 baseline and a locale-independent, fully consumed numeric token.
  if (value.front() == '+')
    return false;
  try {
    std::istringstream input{std::string(value)};
    input.imbue(std::locale::classic());
    input >> std::noskipws >> parsed;
    return !input.fail() && input.eof() && std::isfinite(parsed);
  } catch (...) {
    return false;
  }
}

bool ParseSvgViewBox(const std::string_view value,
                     std::array<double, 4> &parsed) noexcept {
  std::size_t cursor = 0U;
  const auto skipWhitespace = [&] {
    const auto before = cursor;
    while (cursor < value.size() &&
           std::isspace(static_cast<unsigned char>(value[cursor])))
      ++cursor;
    return cursor != before;
  };
  skipWhitespace();
  for (std::size_t index = 0U; index < parsed.size(); ++index) {
    const auto begin = cursor;
    while (cursor < value.size() && value[cursor] != ',' &&
           !std::isspace(static_cast<unsigned char>(value[cursor])))
      ++cursor;
    if (begin == cursor ||
        !ParseSvgNumber(value.substr(begin, cursor - begin), parsed[index]))
      return false;
    if (index + 1U == parsed.size()) {
      skipWhitespace();
      return cursor == value.size();
    }
    const bool whitespace = skipWhitespace();
    if (cursor < value.size() && value[cursor] == ',') {
      ++cursor;
      skipWhitespace();
    } else if (!whitespace) {
      return false;
    }
    if (cursor >= value.size() || value[cursor] == ',')
      return false;
  }
  return false;
}

void ValidateSvg(const VectorDocument &document, const VectorLimits &limits,
                 RasterBudgetState &rasterBudget,
                 std::vector<Diagnostic> &diagnostics) {
  const auto &bytes = *document.source.bytes;
  const std::string source(bytes.begin(), bytes.end());
  const auto reject = [&](std::string code, std::string message) {
    Add(diagnostics, std::move(code), "validate", document.source.assetId,
        std::move(message));
  };
  static const std::unordered_set<std::string> allowedElements{
      "svg",      "g",       "defs",           "title",          "desc",
      "path",     "rect",    "circle",         "ellipse",        "line",
      "polyline", "polygon", "lineargradient", "radialgradient", "stop",
      "pattern",  "use",     "image",          "clippath",       "mask",
  };
  static const std::unordered_set<std::string> allowedAttributes{
      "xmlns",
      "xmlns:xlink",
      "id",
      "class",
      "transform",
      "opacity",
      "fill",
      "fill-opacity",
      "fill-rule",
      "stroke",
      "stroke-width",
      "stroke-opacity",
      "stroke-linecap",
      "stroke-linejoin",
      "stroke-miterlimit",
      "stroke-dasharray",
      "stroke-dashoffset",
      "clip-path",
      "mask",
      "x",
      "y",
      "x1",
      "x2",
      "y1",
      "y2",
      "cx",
      "cy",
      "r",
      "rx",
      "ry",
      "width",
      "height",
      "viewbox",
      "preserveaspectratio",
      "d",
      "points",
      "offset",
      "stop-color",
      "stop-opacity",
      "gradientunits",
      "gradienttransform",
      "spreadmethod",
      "patternunits",
      "patterncontentunits",
      "patterntransform",
      "href",
      "xlink:href",
  };

  std::vector<std::string> stack;
  std::size_t offset = 0;
  std::size_t elements = 0;
  std::size_t attributes = 0;
  std::size_t pathCommands = 0;
  std::size_t containerDepth = 0;
  bool sawRoot = false;
  bool sawDeclaration = false;
  std::string rootWidth;
  std::string rootHeight;
  std::string rootViewBox;
  if (!ValidUtf8(source)) {
    reject("vector.svg.utf8_invalid", "SVG source must be well-formed UTF-8");
    return;
  }
  while (offset < source.size()) {
    const auto open = source.find('<', offset);
    const auto textEnd = open == std::string::npos ? source.size() : open;
    const auto text = std::string_view(source).substr(offset, textEnd - offset);
    const bool outsideRoot = stack.empty();
    const bool nonWhitespace =
        std::any_of(text.begin(), text.end(), [](const unsigned char byte) {
          return !std::isspace(byte);
        });
    if (text.size() > limits.maximumStringBytes || !SafeXmlText(text) ||
        (outsideRoot && nonWhitespace)) {
      reject("vector.svg.text_invalid",
             "SVG text is outside the root, contains an invalid entity, or "
             "exceeds its budget");
      return;
    }
    if (open == std::string::npos)
      break;
    offset = open + 1;
    if (source.compare(offset, 3, "!--") == 0) {
      const auto end = source.find("-->", offset + 3);
      if (end == std::string::npos ||
          std::string_view(source)
                  .substr(offset + 3, end - offset - 3)
                  .find("--") != std::string_view::npos) {
        reject("vector.svg.structure_invalid",
               "SVG comment is unterminated or malformed");
        return;
      }
      offset = end + 3;
      continue;
    }
    if (offset < source.size() && source[offset] == '?') {
      const auto end = source.find("?>", offset + 1);
      const auto declaration = LowerAscii(std::string_view(source).substr(
          offset + 1, end == std::string::npos ? 0 : end - offset - 1));
      const bool xmlDeclaration =
          declaration.size() >= 3 && declaration.rfind("xml", 0) == 0 &&
          (declaration.size() == 3 ||
           std::isspace(static_cast<unsigned char>(declaration[3])));
      if (end == std::string::npos || sawRoot || sawDeclaration ||
          !xmlDeclaration) {
        reject("vector.svg.declaration_invalid",
               "SVG processing instructions are not allowed");
        return;
      }
      sawDeclaration = true;
      offset = end + 2;
      continue;
    }
    if (offset < source.size() && source[offset] == '!') {
      reject("vector.svg.declaration_forbidden",
             "SVG DTD, entity, and CDATA declarations are not allowed");
      return;
    }

    bool closing = false;
    if (offset < source.size() && source[offset] == '/') {
      closing = true;
      ++offset;
    }
    if (offset >= source.size() ||
        !XmlNameStart(static_cast<unsigned char>(source[offset]))) {
      reject("vector.svg.structure_invalid", "SVG element name is invalid");
      return;
    }
    const auto nameBegin = offset++;
    while (offset < source.size() &&
           XmlNameByte(static_cast<unsigned char>(source[offset])))
      ++offset;
    const std::string name = LowerAscii(
        std::string_view(source).substr(nameBegin, offset - nameBegin));
    if (name.find(':') != std::string::npos ||
        allowedElements.count(name) == 0) {
      reject("vector.svg.element_forbidden",
             "SVG element is outside the renderer capability allowlist");
      return;
    }
    const bool container =
        name == "mask" || name == "clippath" || name == "pattern";
    if (closing) {
      while (offset < source.size() &&
             std::isspace(static_cast<unsigned char>(source[offset])))
        ++offset;
      if (offset >= source.size() || source[offset] != '>' || stack.empty() ||
          stack.back() != name) {
        reject("vector.svg.structure_invalid",
               "SVG closing element does not match the open stack");
        return;
      }
      if (container)
        --containerDepth;
      stack.pop_back();
      ++offset;
      continue;
    }

    if (++elements > limits.maximumXmlElements) {
      reject("vector.svg.element_limit",
             "SVG element count exceeds the renderer profile limit");
      return;
    }
    if (sawRoot && stack.empty()) {
      reject("vector.svg.root_invalid",
             "SVG document contains more than one root element");
      return;
    }
    if (!sawRoot) {
      if (name != "svg") {
        reject("vector.svg.root_invalid", "SVG root element must be svg");
        return;
      }
      sawRoot = true;
    }
    const bool rootElement = elements == 1U && stack.empty() && name == "svg";
    std::unordered_set<std::string> names;
    bool selfClosing = false;
    while (offset < source.size()) {
      while (offset < source.size() &&
             std::isspace(static_cast<unsigned char>(source[offset])))
        ++offset;
      if (offset >= source.size())
        break;
      if (source[offset] == '>') {
        ++offset;
        break;
      }
      if (source[offset] == '/' && offset + 1 < source.size() &&
          source[offset + 1] == '>') {
        offset += 2;
        selfClosing = true;
        break;
      }
      if (!XmlNameStart(static_cast<unsigned char>(source[offset]))) {
        reject("vector.svg.attribute_invalid", "SVG attribute name is invalid");
        return;
      }
      const auto attributeBegin = offset++;
      while (offset < source.size() &&
             XmlNameByte(static_cast<unsigned char>(source[offset])))
        ++offset;
      const std::string attribute = LowerAscii(std::string_view(source).substr(
          attributeBegin, offset - attributeBegin));
      while (offset < source.size() &&
             std::isspace(static_cast<unsigned char>(source[offset])))
        ++offset;
      if (++attributes > limits.maximumXmlAttributes ||
          allowedAttributes.count(attribute) == 0 ||
          attribute.rfind("on", 0) == 0 || !names.insert(attribute).second ||
          offset >= source.size() || source[offset] != '=') {
        reject(
            "vector.svg.attribute_forbidden",
            "SVG attribute is duplicated or outside the capability allowlist");
        return;
      }
      ++offset;
      while (offset < source.size() &&
             std::isspace(static_cast<unsigned char>(source[offset])))
        ++offset;
      if (offset >= source.size() ||
          (source[offset] != '\'' && source[offset] != '"')) {
        reject("vector.svg.attribute_invalid",
               "SVG attributes must use quoted values");
        return;
      }
      const char quote = source[offset++];
      const auto valueBegin = offset;
      const auto valueEnd = source.find(quote, valueBegin);
      if (valueEnd == std::string::npos) {
        reject("vector.svg.attribute_invalid",
               "SVG attribute value is unterminated");
        return;
      }
      const auto value =
          std::string_view(source).substr(valueBegin, valueEnd - valueBegin);
      offset = valueEnd + 1;
      if (value.size() > limits.maximumStringBytes ||
          value.find('&') != std::string_view::npos ||
          value.find('<') != std::string_view::npos || !SafeXmlText(value)) {
        reject("vector.svg.attribute_value_invalid",
               "SVG attribute value is malformed, oversized, or "
               "entity-obfuscated");
        return;
      }
      if (attribute == "xmlns" && value != "http://www.w3.org/2000/svg") {
        reject("vector.svg.namespace_invalid", "SVG namespace is invalid");
        return;
      }
      if (attribute == "xmlns:xlink" &&
          value != "http://www.w3.org/1999/xlink") {
        reject("vector.svg.namespace_invalid",
               "SVG xlink namespace is invalid");
        return;
      }
      if ((attribute == "href" || attribute == "xlink:href") &&
          !SafeSvgReferenceValue(document, value, limits, rasterBudget)) {
        reject("vector.svg.reference_forbidden",
               "SVG reference is not local or resource-allowlisted");
        return;
      }
      if ((attribute == "style" || attribute == "fill" ||
           attribute == "stroke" || attribute == "clip-path" ||
           attribute == "mask") &&
          !SafeSvgPaintValue(value)) {
        reject("vector.svg.paint_forbidden",
               "SVG paint/style contains external or executable content");
        return;
      }
      if (attribute == "d") {
        pathCommands += static_cast<std::size_t>(
            std::count_if(value.begin(), value.end(), [](unsigned char byte) {
              return (byte >= 'a' && byte <= 'z') ||
                     (byte >= 'A' && byte <= 'Z');
            }));
        if (pathCommands > limits.maximumSvgPathCommands) {
          reject("vector.svg.path_limit",
                 "SVG path command count exceeds the renderer profile limit");
          return;
        }
      }
      if (rootElement) {
        if (attribute == "width")
          rootWidth.assign(value);
        else if (attribute == "height")
          rootHeight.assign(value);
        else if (attribute == "viewbox")
          rootViewBox.assign(value);
      }
    }
    if (!selfClosing) {
      stack.push_back(name);
      if (stack.size() > limits.maximumXmlDepth ||
          (container && ++containerDepth > limits.maximumSvgContainerDepth)) {
        reject("vector.svg.depth_limit",
               "SVG nesting exceeds the renderer profile limit");
        return;
      }
    }
  }
  if (!sawRoot || !stack.empty()) {
    reject("vector.svg.structure_invalid",
           "SVG root is absent or element nesting is incomplete");
    return;
  }
  double width = 0.0;
  double height = 0.0;
  std::array<double, 4> viewBox{};
  const bool validRootMetrics =
      ParseSvgNumber(rootWidth, width) && ParseSvgNumber(rootHeight, height) &&
      ParseSvgViewBox(rootViewBox, viewBox) && width > 0.0 && height > 0.0 &&
      width <= limits.maximumDimension && height <= limits.maximumDimension &&
      width * height <= static_cast<double>(limits.maximumPixels) &&
      std::abs(viewBox[0]) <= limits.maximumDimension &&
      std::abs(viewBox[1]) <= limits.maximumDimension && viewBox[2] > 0.0 &&
      viewBox[3] > 0.0 && viewBox[2] <= limits.maximumDimension &&
      viewBox[3] <= limits.maximumDimension &&
      viewBox[2] * viewBox[3] <= static_cast<double>(limits.maximumPixels) &&
      std::abs(width - document.probe.intrinsicWidth) <= 0.01 &&
      std::abs(height - document.probe.intrinsicHeight) <= 0.01 &&
      std::abs(viewBox[2] - document.probe.intrinsicWidth) <= 0.01 &&
      std::abs(viewBox[3] - document.probe.intrinsicHeight) <= 0.01;
  if (!validRootMetrics) {
    reject("vector.svg.header_invalid",
           "SVG width, height, and viewBox must be finite, unitless, "
           "budgeted, and exactly match the admitted probe");
  }
}

struct LottieWalkState final {
  std::size_t nodes{0};
  std::size_t layers{0};
  std::size_t shapes{0};
  std::size_t masks{0};
  std::size_t effects{0};
  std::size_t keyframes{0};
  std::size_t embeddedDataUriBytes{0};
  bool expressionFound{false};
  bool stringLimitExceeded{false};
  bool nonFiniteNumber{false};
  bool semanticBudgetExceeded{false};
  std::unordered_set<std::string> referencedFonts;
};

bool AddBudget(std::size_t &aggregate, const std::size_t amount,
               const std::size_t maximum) noexcept {
  if (amount > maximum || aggregate > maximum - amount)
    return false;
  aggregate += amount;
  return true;
}

bool WalkJson(const Json &value, std::size_t depth, const VectorLimits &limits,
              LottieWalkState &state, const bool embeddedImageDataUri = false) {
  if (depth > limits.maximumJsonDepth ||
      ++state.nodes > limits.maximumJsonNodes)
    return false;
  if (value.is_string() &&
      value.get_ref<const std::string &>().size() > limits.maximumStringBytes &&
      !embeddedImageDataUri)
    state.stringLimitExceeded = true;
  if (value.is_number_float() && !std::isfinite(value.get<double>()))
    state.nonFiniteNumber = true;
  if (value.is_object()) {
    for (auto item = value.begin(); item != value.end(); ++item) {
      if ((item.key() == "x" || item.key() == "expression") &&
          item.value().is_string() &&
          !item.value().get_ref<const std::string &>().empty())
        state.expressionFound = true;
      if (item.key() == "f" && item.value().is_string())
        state.referencedFonts.insert(
            item.value().get_ref<const std::string &>());
      if (item.value().is_array()) {
        const auto count = item.value().size();
        if ((item.key() == "layers" &&
             !AddBudget(state.layers, count, limits.maximumLottieLayers)) ||
            (item.key() == "shapes" &&
             !AddBudget(state.shapes, count, limits.maximumLottieShapes)) ||
            (item.key() == "masksProperties" &&
             !AddBudget(state.masks, count, limits.maximumLottieMasks)) ||
            (item.key() == "ef" &&
             !AddBudget(state.effects, count, limits.maximumLottieEffects)))
          state.semanticBudgetExceeded = true;
        if (item.key() == "k" && !item.value().empty() &&
            item.value().front().is_object() &&
            item.value().front().contains("t") &&
            !AddBudget(state.keyframes, count, limits.maximumLottieKeyframes))
          state.semanticBudgetExceeded = true;
      }
      bool childIsEmbeddedImageDataUri = false;
      if (item.key() == "p" && item.value().is_string()) {
        const auto &string = item.value().get_ref<const std::string &>();
        // Skottie image assets may carry an immutable data URI in
        // assets[].p. The complete Vector source is already bounded
        // by maximumSourceBytes, so applying the generic 1 MiB JSON
        // metadata-string limit here would reject ordinary animated
        // PNG/WebP payloads before the resource provider can decode
        // them. Keep SVG and arbitrary data MIME types excluded.
        constexpr const char *prefixes[] = {
            "data:image/png;base64,",
            "data:image/jpeg;base64,",
            "data:image/webp;base64,",
        };
        childIsEmbeddedImageDataUri = std::any_of(
            std::begin(prefixes), std::end(prefixes), [&](const char *prefix) {
              return string.compare(0, std::strlen(prefix), prefix) == 0;
            });
        if (childIsEmbeddedImageDataUri &&
            (!AddBudget(state.embeddedDataUriBytes, string.size(),
                        limits.maximumEmbeddedDataUriBytes) ||
             string.size() > limits.maximumEmbeddedDataUriBytes))
          state.semanticBudgetExceeded = true;
      }
      if (!WalkJson(item.value(), depth + 1, limits, state,
                    childIsEmbeddedImageDataUri))
        return false;
    }
  } else if (value.is_array()) {
    for (const auto &item : value) {
      if (!WalkJson(item, depth + 1, limits, state))
        return false;
    }
  }
  return true;
}

void ValidateLottie(const VectorDocument &document, const VectorLimits &limits,
                    RasterBudgetState &rasterBudget,
                    std::vector<Diagnostic> &diagnostics) {
  try {
    const auto &bytes = *document.source.bytes;
    bool duplicateKey = false;
    std::vector<std::unordered_set<std::string>> objectKeys;
    const auto callback = [&](const int, const Json::parse_event_t event,
                              Json &parsed) {
      if (event == Json::parse_event_t::object_start) {
        objectKeys.emplace_back();
      } else if (event == Json::parse_event_t::key) {
        if (objectKeys.empty() ||
            !objectKeys.back()
                 .insert(parsed.get_ref<const std::string &>())
                 .second)
          duplicateKey = true;
      } else if (event == Json::parse_event_t::object_end) {
        if (!objectKeys.empty())
          objectKeys.pop_back();
      }
      return true;
    };
    const Json source = Json::parse(bytes.begin(), bytes.end(), callback);
    if (!source.is_object()) {
      Add(diagnostics, "vector.lottie.root_invalid", "validate",
          document.source.assetId, "Lottie JSON root must be an object");
      return;
    }
    if (duplicateKey) {
      Add(diagnostics, "vector.lottie.duplicate_key", "validate",
          document.source.assetId, "Lottie JSON object keys must be unique");
    }
    LottieWalkState state;
    if (!WalkJson(source, 0, limits, state)) {
      Add(diagnostics, "vector.lottie.structure_limit", "validate",
          document.source.assetId,
          "Lottie JSON depth or node count exceeds the profile limit");
    }
    if (state.stringLimitExceeded) {
      Add(diagnostics, "vector.lottie.string_limit", "validate",
          document.source.assetId, "Lottie JSON contains an oversized string");
    }
    if (state.expressionFound) {
      Add(diagnostics, "vector.lottie.expression_unsupported", "validate",
          document.source.assetId,
          "Lottie expressions are not enabled in renderer profile v1");
    }
    if (state.nonFiniteNumber) {
      Add(diagnostics, "vector.lottie.number_invalid", "validate",
          document.source.assetId, "Lottie JSON contains a non-finite number");
    }
    if (state.semanticBudgetExceeded) {
      Add(diagnostics, "vector.lottie.semantic_budget", "validate",
          document.source.assetId,
          "Lottie layers, shapes, masks, effects, keyframes, or embedded data "
          "exceed the profile budget");
    }
    const auto width = source.find("w");
    const auto height = source.find("h");
    const auto frameRate = source.find("fr");
    const auto inPoint = source.find("ip");
    const auto outPoint = source.find("op");
    bool validHeader = !(width == source.end() || height == source.end() ||
                         frameRate == source.end() || inPoint == source.end() ||
                         outPoint == source.end() || !width->is_number() ||
                         !height->is_number() || !frameRate->is_number() ||
                         !inPoint->is_number() || !outPoint->is_number());
    if (validHeader) {
      const double widthValue = width->get<double>();
      const double heightValue = height->get<double>();
      const double frameRateValue = frameRate->get<double>();
      const double inPointValue = inPoint->get<double>();
      const double outPointValue = outPoint->get<double>();
      const double durationUs =
          (outPointValue - inPointValue) / frameRateValue * 1'000'000.0;
      validHeader =
          std::isfinite(widthValue) && std::isfinite(heightValue) &&
          std::isfinite(frameRateValue) && std::isfinite(inPointValue) &&
          std::isfinite(outPointValue) && std::isfinite(durationUs) &&
          widthValue > 0.0 && heightValue > 0.0 &&
          widthValue <= limits.maximumDimension &&
          heightValue <= limits.maximumDimension &&
          widthValue * heightValue <=
              static_cast<double>(limits.maximumPixels) &&
          frameRateValue > 0.0 &&
          frameRateValue <= limits.maximumFramesPerSecond &&
          inPointValue >= 0.0 && outPointValue > inPointValue &&
          durationUs <= static_cast<double>(limits.maximumDurationUs) &&
          std::abs(widthValue - document.probe.intrinsicWidth) <= 0.01 &&
          std::abs(heightValue - document.probe.intrinsicHeight) <= 0.01 &&
          std::abs(frameRateValue - document.probe.nativeFramesPerSecond) <=
              1.0e-6 &&
          std::abs(durationUs -
                   static_cast<double>(document.probe.durationUs)) <= 1.1;
    }
    if (!validHeader) {
      Add(diagnostics, "vector.lottie.header_invalid", "validate",
          document.source.assetId,
          "Lottie w/h/fr/ip/op must be finite, positive, budgeted, and exactly "
          "match the admitted probe");
    }

    std::unordered_set<std::string> declaredFonts;
    const auto fonts = source.find("fonts");
    if (fonts != source.end()) {
      const auto list = fonts->is_object() ? fonts->find("list") : fonts->end();
      if (!fonts->is_object() || list == fonts->end() || !list->is_array() ||
          list->empty() || list->size() > limits.maximumLottieFontFaces) {
        Add(diagnostics, "vector.lottie.font_manifest_invalid", "validate",
            document.source.assetId,
            "Lottie font manifest is malformed or over budget");
      } else {
        for (const auto &face : *list) {
          if (!face.is_object() || !face.contains("fName") ||
              !face["fName"].is_string() || !face.contains("fFamily") ||
              !face["fFamily"].is_string() || !face.contains("fStyle") ||
              !face["fStyle"].is_string()) {
            Add(diagnostics, "vector.lottie.font_face_invalid", "validate",
                document.source.assetId, "Lottie font face is incomplete");
            continue;
          }
          const auto &name = face["fName"].get_ref<const std::string &>();
          const auto &family = face["fFamily"].get_ref<const std::string &>();
          const auto &style = face["fStyle"].get_ref<const std::string &>();
          if (name.empty() || family.empty() || style.empty() ||
              name.size() > limits.maximumFontFamilyBytes ||
              family.size() > limits.maximumFontFamilyBytes ||
              style.size() > limits.maximumFontFamilyBytes ||
              !declaredFonts.insert(name).second) {
            Add(diagnostics, "vector.lottie.font_face_invalid", "validate",
                document.source.assetId,
                "Lottie font face identity is invalid or duplicated");
          }
        }
      }
    }
    for (const auto &reference : state.referencedFonts) {
      if (declaredFonts.count(reference) == 0) {
        Add(diagnostics, "vector.lottie.font_reference_missing", "resolve",
            reference, "Lottie text references an undeclared font face");
      }
    }
    if (!declaredFonts.empty()) {
      const bool requiresClosure =
          std::find(document.probe.capabilitiesUsed.begin(),
                    document.probe.capabilitiesUsed.end(),
                    "embedded-font-closure-required") !=
          document.probe.capabilitiesUsed.end();
      for (const auto &name : declaredFonts) {
        const bool resolved =
            std::any_of(document.resources.begin(), document.resources.end(),
                        [&](const VectorResource &resource) {
                          return resource.logicalName == name &&
                                 resource.mediaType.rfind("font/", 0) == 0 &&
                                 resource.bytes && !resource.bytes->empty();
                        });
        if (!requiresClosure || !resolved) {
          Add(diagnostics, "vector.lottie.font_closure_missing", "resolve",
              name,
              "Lottie font bytes are not part of the required immutable "
              "resource closure");
        }
      }
    }
    const auto chars = source.find("chars");
    if (chars != source.end() &&
        (!chars->is_array() || chars->size() > limits.maximumLottieGlyphs)) {
      Add(diagnostics, "vector.lottie.glyph_limit", "validate",
          document.source.assetId,
          "Lottie glyph table is malformed or exceeds the profile limit");
    }
    const auto assets = source.find("assets");
    if (assets != source.end()) {
      if (!assets->is_array()) {
        Add(diagnostics, "vector.lottie.assets_invalid", "validate",
            document.source.assetId, "Lottie assets must be an array");
      } else {
        std::size_t imageAssets = 0;
        for (const auto &asset : *assets) {
          if (!asset.is_object()) {
            Add(diagnostics, "vector.lottie.asset_invalid", "validate",
                document.source.assetId,
                "Lottie asset entries must be objects");
            continue;
          }
          const auto path = asset.find("p");
          if (path == asset.end() || !path->is_string() ||
              path->get_ref<const std::string &>().empty())
            continue;
          if (++imageAssets > limits.maximumLottieImageAssets) {
            Add(diagnostics, "vector.lottie.image_asset_limit", "validate",
                document.source.assetId,
                "Lottie image asset count exceeds the profile limit");
            break;
          }
          const std::string name = path->get<std::string>();
          const auto declaredDimensionsMatch = [&asset](
                                                   const std::uint32_t width,
                                                   const std::uint32_t height) {
            const auto declaredWidth = asset.find("w");
            const auto declaredHeight = asset.find("h");
            if (declaredWidth == asset.end() || declaredHeight == asset.end() ||
                !declaredWidth->is_number() || !declaredHeight->is_number())
              return false;
            const double widthValue = declaredWidth->get<double>();
            const double heightValue = declaredHeight->get<double>();
            return std::isfinite(widthValue) && std::isfinite(heightValue) &&
                   widthValue == static_cast<double>(width) &&
                   heightValue == static_cast<double>(height);
          };
          std::string lowerName = name;
          std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                         [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                         });
          if (lowerName.compare(0, 5, "data:") == 0) {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            const bool admitted = DecodeRasterDataUri(
                name, limits, rasterBudget, &width, &height);
            const bool dimensionsMatch =
                admitted && declaredDimensionsMatch(width, height);
            if (!admitted || !dimensionsMatch) {
              Add(diagnostics, "vector.lottie.data_uri_invalid", "validate",
                  document.source.assetId,
                  "Lottie data URI media, signature, dimensions, frame count, "
                  "or decoded working set is invalid");
            }
            continue;
          }
          std::string logicalName;
          const auto directory = asset.find("u");
          if (directory != asset.end()) {
            if (!directory->is_string()) {
              Add(diagnostics, "vector.lottie.resource_name_invalid",
                  "validate", document.source.assetId,
                  "Lottie image resource directory must be a string");
              continue;
            }
            logicalName = directory->get<std::string>();
          }
          if (!logicalName.empty() && logicalName.back() != '/')
            logicalName.push_back('/');
          logicalName.append(name);
          const auto declared =
              std::find_if(document.resources.begin(), document.resources.end(),
                           [&](const VectorResource &resource) {
                             return resource.logicalName == logicalName ||
                                    resource.logicalName == name;
                           });
          if (!SafeLogicalName(logicalName)) {
            Add(diagnostics, "vector.lottie.resource_name_invalid", "validate",
                document.source.assetId,
                "Lottie image resource path is not a safe logical name");
          } else if (declared == document.resources.end()) {
            Add(diagnostics, "vector.lottie.resource_missing", "resolve",
                logicalName,
                "Lottie image resource is not declared in the immutable "
                "resource set");
          } else if (
              declared->bytes &&
              declared->mediaType == kVideoCutPackedAlphaMp4MediaType) {
            const auto declaredWidth = asset.find("w");
            const auto declaredHeight = asset.find("h");
            const double width = declaredWidth != asset.end() &&
                                         declaredWidth->is_number()
                                     ? declaredWidth->get<double>()
                                     : 0.0;
            const double height = declaredHeight != asset.end() &&
                                          declaredHeight->is_number()
                                      ? declaredHeight->get<double>()
                                      : 0.0;
            if (!HasCapability(document,
                               kVideoCutPackedAlphaMp4Capability) ||
                !SniffIsoBmffFtyp(*declared->bytes) ||
                !std::isfinite(width) || !std::isfinite(height) ||
                width <= 0.0 || height <= 0.0 ||
                width > static_cast<double>(limits.maximumDimension) ||
                height > static_cast<double>(limits.maximumDimension) ||
                width * height > static_cast<double>(limits.maximumPixels)) {
              Add(diagnostics,
                  "vector.lottie.packed_alpha_resource_invalid", "validate",
                  logicalName,
                  "packed-alpha MP4 requires its exact versioned capability, an "
                  "allowlisted ISO-BMFF source, and bounded logical "
                  "dimensions");
            }
          } else if (declared->bytes) {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            if (!SniffRasterDimensions(declared->mediaType, *declared->bytes,
                                       width, height) ||
                !declaredDimensionsMatch(width, height)) {
              Add(diagnostics, "vector.lottie.resource_dimensions_invalid",
                  "validate", logicalName,
                  "Lottie image metadata does not exactly match the immutable "
                  "raster resource");
            }
          }
        }
      }
    }
  } catch (const Json::exception &exception) {
    Add(diagnostics, "vector.lottie.parse_failed", "validate",
        document.source.assetId, exception.what());
  }
}

} // namespace

VectorValidationResult ValidateVectorDocument(const VectorDocument &document,
                                              const VectorLimits &limits) {
  VectorValidationResult result;
  if (document.version != 1)
    Add(result.diagnostics, "vector.document.version_unsupported", "validate",
        document.documentId, "only VectorDocument version 1 is supported");
  if (document.documentId.empty())
    Add(result.diagnostics, "vector.document.id_missing", "validate", {},
        "vector document ID must not be empty");
  if (document.rendererProfile.empty())
    Add(result.diagnostics, "vector.document.profile_missing", "validate",
        document.documentId, "renderer profile must not be empty");
  if (document.source.kind == VectorSourceKind::DotLottie)
    Add(result.diagnostics, "vector.dotlottie.unsupported", "validate",
        document.source.assetId,
        "dotLottie containers are not enabled in profile v1");
  if (document.source.assetId.empty())
    Add(result.diagnostics, "vector.source.identity_invalid", "validate",
        document.documentId, "vector source requires an asset ID");
  const bool sourceMediaMatchesKind =
      (document.source.kind == VectorSourceKind::Svg &&
       document.source.mediaType == "image/svg+xml") ||
      (document.source.kind == VectorSourceKind::LottieJson &&
       document.source.mediaType == "application/json") ||
      (document.source.kind == VectorSourceKind::DotLottie &&
       (document.source.mediaType == "application/zip" ||
        document.source.mediaType == "application/vnd.lottie+zip"));
  if (!sourceMediaMatchesKind)
    Add(result.diagnostics, "vector.source.media_type_mismatch", "validate",
        document.source.assetId,
        "vector source kind and media type are inconsistent");
  if (!document.source.bytes || document.source.bytes->empty())
    Add(result.diagnostics, "vector.source.missing", "resolve",
        document.source.assetId, "vector source bytes are unavailable");
  else if (document.source.bytes->size() > limits.maximumSourceBytes)
    Add(result.diagnostics, "vector.source.byte_limit", "validate",
        document.source.assetId,
        "vector source exceeds the renderer profile byte limit");
  if (!std::isfinite(document.viewport.alignmentX) ||
      !std::isfinite(document.viewport.alignmentY) ||
      !std::isfinite(document.viewport.referenceWidth) ||
      !std::isfinite(document.viewport.referenceHeight) ||
      !std::isfinite(document.viewport.overscan) ||
      document.viewport.alignmentX < 0.0F ||
      document.viewport.alignmentX > 1.0F ||
      document.viewport.alignmentY < 0.0F ||
      document.viewport.alignmentY > 1.0F ||
      document.viewport.referenceWidth < 0.0F ||
      document.viewport.referenceHeight < 0.0F ||
      document.viewport.overscan < 0.0F ||
      document.viewport.overscan > limits.maximumOverscan)
    Add(result.diagnostics, "vector.viewport.invalid", "validate",
        document.documentId, "vector viewport contains an invalid value");

  if (!std::isfinite(document.probe.intrinsicWidth) ||
      !std::isfinite(document.probe.intrinsicHeight) ||
      document.probe.intrinsicWidth < 0.0F ||
      document.probe.intrinsicHeight < 0.0F ||
      document.probe.intrinsicWidth > limits.maximumDimension ||
      document.probe.intrinsicHeight > limits.maximumDimension ||
      static_cast<double>(document.probe.intrinsicWidth) *
              static_cast<double>(document.probe.intrinsicHeight) >
          static_cast<double>(limits.maximumPixels) ||
      document.probe.durationUs < 0 ||
      document.probe.durationUs > limits.maximumDurationUs ||
      !std::isfinite(document.probe.nativeFramesPerSecond) ||
      document.probe.nativeFramesPerSecond < 0.0 ||
      document.probe.nativeFramesPerSecond > limits.maximumFramesPerSecond)
    Add(result.diagnostics, "vector.probe.invalid", "validate",
        document.source.assetId,
        "vector probe metadata is outside the profile limits");
  if (document.source.kind == VectorSourceKind::LottieJson &&
      (document.timeMapping.sourceInUs < 0 ||
       document.timeMapping.sourceOutUs <= document.timeMapping.sourceInUs ||
       document.timeMapping.rateNumerator == 0 ||
       document.timeMapping.rateDenominator == 0))
    Add(result.diagnostics, "vector.time.invalid", "validate",
        document.source.assetId,
        "animated vector source has an invalid time mapping");

  if (document.resources.size() > limits.maximumResources)
    Add(result.diagnostics, "vector.resource.count_limit", "validate",
        document.source.assetId,
        "vector resource count exceeds the profile limit");
  std::unordered_set<std::string> resourceNames;
  std::size_t totalResourceBytes = 0;
  RasterBudgetState rasterBudget;
  for (const auto &resource : document.resources) {
    if (!SafeLogicalName(resource.logicalName) ||
        !resourceNames.insert(resource.logicalName).second)
      Add(result.diagnostics, "vector.resource.name_invalid", "validate",
          resource.logicalName,
          "resource logical names must be safe and unique");
    if (!resource.bytes)
      Add(result.diagnostics, "vector.resource.missing", "resolve",
          resource.logicalName,
          "declared vector resource bytes are unavailable");
    else {
      const auto size = resource.bytes->size();
      if (size > limits.maximumResourceBytes ||
          totalResourceBytes > limits.maximumResourceBytes -
                                   std::min(limits.maximumResourceBytes, size))
        totalResourceBytes = limits.maximumResourceBytes + 1;
      else
        totalResourceBytes += size;
      if (resource.mediaType == "image/png" ||
          resource.mediaType == "image/jpeg" ||
          resource.mediaType == "image/webp") {
        if (!AddRasterPayloadToBudget(resource.mediaType, *resource.bytes,
                                      limits, rasterBudget)) {
          Add(result.diagnostics, "vector.resource.image_budget_invalid",
              "validate", resource.logicalName,
              "vector image media signature or decoded pixel budget is "
              "invalid");
        }
      } else if (resource.mediaType ==
                 kVideoCutPackedAlphaMp4MediaType) {
        if (!HasCapability(document,
                           kVideoCutPackedAlphaMp4Capability) ||
            !SniffIsoBmffFtyp(*resource.bytes)) {
          Add(result.diagnostics,
              "vector.resource.packed_alpha_mp4_invalid", "validate",
              resource.logicalName,
              "packed-alpha MP4 resource is missing its exact "
              "capability or ISO-BMFF signature");
        }
      } else if (resource.mediaType == "font/ttf" ||
                 resource.mediaType == "font/otf" ||
                 resource.mediaType == "font/collection") {
        if (!SniffFont(*resource.bytes))
          Add(result.diagnostics, "vector.resource.font_signature_invalid",
              "validate", resource.logicalName,
              "vector font media type does not match its bytes");
      } else
        Add(result.diagnostics, "vector.resource.media_type_unsupported",
            "validate", resource.logicalName,
            "vector resource media type is outside the profile "
            "allowlist");
    }
  }
  if (totalResourceBytes > limits.maximumResourceBytes)
    Add(result.diagnostics, "vector.resource.byte_limit", "validate",
        document.source.assetId,
        "vector resources exceed the aggregate byte limit");

  if (document.source.bytes &&
      document.source.bytes->size() <= limits.maximumSourceBytes) {
    if (document.source.kind == VectorSourceKind::Svg)
      ValidateSvg(document, limits, rasterBudget, result.diagnostics);
    else if (document.source.kind == VectorSourceKind::LottieJson)
      ValidateLottie(document, limits, rasterBudget, result.diagnostics);
  }
  result.valid =
      std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                   [](const Diagnostic &diagnostic) {
                     return diagnostic.severity == DiagnosticSeverity::Error;
                   });
  return result;
}

} // namespace videocut::vector
