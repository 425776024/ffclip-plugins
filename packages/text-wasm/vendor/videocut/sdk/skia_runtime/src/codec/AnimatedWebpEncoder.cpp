#include "videocut/skia_runtime/AnimatedWebpEncoder.h"

#include "codec/WebpAnimationCodec.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>

namespace videocut::skia_runtime {
namespace {

struct EncoderDeleter final {
  void operator()(WebPAnimEncoder *value) const noexcept {
    if (value)
      WebPAnimEncoderDelete(value);
  }
};

struct PictureOwner final {
  WebPPicture value{};
  bool initialized{false};

  ~PictureOwner() {
    if (initialized)
      WebPPictureFree(&value);
  }
};

std::string EncoderError(WebPAnimEncoder *encoder,
                         const char *fallback) {
  const char *message = encoder ? WebPAnimEncoderGetError(encoder) : nullptr;
  return message && *message ? std::string(message) : std::string(fallback);
}

void AppendBigEndian32(std::vector<std::uint8_t> &output,
                       const std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>(value >> 24U));
  output.push_back(static_cast<std::uint8_t>(value >> 16U));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value));
}

const std::array<std::uint32_t, 256U> &PngCrcTable() {
  static const auto table = [] {
    std::array<std::uint32_t, 256U> value{};
    for (std::uint32_t index = 0U; index < value.size(); ++index) {
      std::uint32_t remainder = index;
      for (std::uint32_t bit = 0U; bit < 8U; ++bit) {
        remainder = (remainder >> 1U) ^
                    (0xEDB88320U &
                     (0U - static_cast<std::uint32_t>(remainder & 1U)));
      }
      value[index] = remainder;
    }
    return value;
  }();
  return table;
}

std::uint32_t PngCrc32(const std::uint8_t *bytes, const std::size_t size) {
  std::uint32_t crc = 0xFFFFFFFFU;
  const auto &table = PngCrcTable();
  for (std::size_t index = 0U; index < size; ++index)
    crc = table[(crc ^ bytes[index]) & 0xFFU] ^ (crc >> 8U);
  return crc ^ 0xFFFFFFFFU;
}

std::uint32_t PngAdler32(const std::uint8_t *bytes,
                         const std::size_t size) {
  constexpr std::uint32_t kModulus = 65'521U;
  std::uint32_t a = 1U;
  std::uint32_t b = 0U;
  std::size_t offset = 0U;
  while (offset < size) {
    const std::size_t end = std::min(size, offset + 5'552U);
    for (; offset < end; ++offset) {
      a += bytes[offset];
      b += a;
    }
    a %= kModulus;
    b %= kModulus;
  }
  return (b << 16U) | a;
}

void AppendPngChunk(std::vector<std::uint8_t> &output, const char type[4],
                    const std::vector<std::uint8_t> &payload) {
  AppendBigEndian32(output, static_cast<std::uint32_t>(payload.size()));
  const std::size_t crcBegin = output.size();
  output.insert(output.end(), type, type + 4U);
  output.insert(output.end(), payload.begin(), payload.end());
  AppendBigEndian32(output,
                    PngCrc32(output.data() + crcBegin, 4U + payload.size()));
}

} // namespace

bool EncodeRgbaPng(const std::uint32_t width, const std::uint32_t height,
                   const std::vector<std::uint8_t> &rgba,
                   std::vector<std::uint8_t> &output, std::string &error) {
  output.clear();
  if (width == 0U || height == 0U || width > 4096U || height > 4096U ||
      width > std::numeric_limits<std::uint32_t>::max() / 4U) {
    error = "PNG request is outside the bounded encoder contract";
    return false;
  }
  const std::size_t rowBytes = static_cast<std::size_t>(width) * 4U;
  if (height > std::numeric_limits<std::size_t>::max() / rowBytes ||
      rgba.size() != rowBytes * static_cast<std::size_t>(height)) {
    error = "PNG RGBA payload does not match its extent";
    return false;
  }
  try {
    // The shipping Skia profile deliberately carries decode-only PNG support.
    // Evidence export therefore writes a deterministic standards-compliant
    // zlib stream with stored DEFLATE blocks. It preserves the renderer's
    // straight RGBA bytes exactly and adds no image-library conversion.
    std::vector<std::uint8_t> scanlines;
    scanlines.reserve((rowBytes + 1U) * static_cast<std::size_t>(height));
    for (std::uint32_t y = 0U; y < height; ++y) {
      scanlines.push_back(0U);
      const auto begin = rgba.begin() + static_cast<std::size_t>(y) * rowBytes;
      scanlines.insert(scanlines.end(), begin, begin + rowBytes);
    }

    std::vector<std::uint8_t> compressed;
    compressed.reserve(scanlines.size() + scanlines.size() / 65'535U * 5U +
                       16U);
    compressed.push_back(0x78U);
    compressed.push_back(0x01U);
    std::size_t offset = 0U;
    while (offset < scanlines.size()) {
      const auto blockSize = static_cast<std::uint16_t>(
          std::min<std::size_t>(65'535U, scanlines.size() - offset));
      const bool finalBlock = offset + blockSize == scanlines.size();
      compressed.push_back(finalBlock ? 0x01U : 0x00U);
      compressed.push_back(static_cast<std::uint8_t>(blockSize));
      compressed.push_back(static_cast<std::uint8_t>(blockSize >> 8U));
      const std::uint16_t inverse = static_cast<std::uint16_t>(~blockSize);
      compressed.push_back(static_cast<std::uint8_t>(inverse));
      compressed.push_back(static_cast<std::uint8_t>(inverse >> 8U));
      compressed.insert(compressed.end(), scanlines.begin() + offset,
                        scanlines.begin() + offset + blockSize);
      offset += blockSize;
    }
    AppendBigEndian32(compressed,
                      PngAdler32(scanlines.data(), scanlines.size()));

    output.reserve(compressed.size() + 64U);
    constexpr std::uint8_t kSignature[] = {137U, 80U, 78U, 71U,
                                            13U,  10U, 26U, 10U};
    output.insert(output.end(), kSignature, kSignature + 8U);
    std::vector<std::uint8_t> header;
    header.reserve(13U);
    AppendBigEndian32(header, width);
    AppendBigEndian32(header, height);
    header.insert(header.end(), {8U, 6U, 0U, 0U, 0U});
    AppendPngChunk(output, "IHDR", header);
    AppendPngChunk(output, "IDAT", compressed);
    AppendPngChunk(output, "IEND", {});
  } catch (...) {
    error = "PNG output allocation failed";
    return false;
  }
  error.clear();
  return true;
}

bool EncodeAnimatedWebp(const std::uint32_t width,
                        const std::uint32_t height,
                        const std::vector<RgbaAnimationFrame> &frames,
                        std::vector<std::uint8_t> &output,
                        std::string &error) {
  output.clear();
  if (width == 0U || height == 0U || width > 4096U || height > 4096U ||
      frames.empty() || frames.size() > 120U ||
      width > std::numeric_limits<std::uint32_t>::max() / 4U) {
    error = "animated WebP request is outside the bounded encoder contract";
    return false;
  }
  const std::size_t rowBytes = static_cast<std::size_t>(width) * 4U;
  if (height > std::numeric_limits<std::size_t>::max() / rowBytes) {
    error = "animated WebP RGBA extent overflows addressable memory";
    return false;
  }
  const std::size_t frameBytes = rowBytes * static_cast<std::size_t>(height);

  WebPAnimEncoderOptions options{};
  if (!WebPAnimEncoderOptionsInitInternal(&options, kWebpMuxAbiVersion)) {
    error = "pinned WebP mux ABI is incompatible";
    return false;
  }
  options.anim_params.bgcolor = 0U;
  options.anim_params.loop_count = 0;
  // Flutter fixes the decode target's alpha type from the first frame. An
  // opaque first frame followed by libwebp's transparent delta plus background
  // disposal can then make SkCodec reject a later frame's alpha conversion.
  // Encode independent key frames: retain the supplied RGBA and timestamps
  // without introducing alpha/disposal dependencies between opaque frames.
  // minimize_size must be off because it disables key-frame insertion.
  options.minimize_size = 0;
  options.kmax = 1;
  // Candidate frames are also visual-fidelity evidence. `allow_mixed` lets
  // libwebp silently replace a lossless WebPConfig with a smaller lossy VP8
  // frame; that softens text edges and changes flat template colors before
  // comparison. Keep every frame on the configured VP8L path.
  options.allow_mixed = 0;
  std::unique_ptr<WebPAnimEncoder, EncoderDeleter> encoder(
      WebPAnimEncoderNewInternal(static_cast<int>(width),
                                 static_cast<int>(height), &options,
                                 kWebpMuxAbiVersion));
  if (!encoder) {
    error = "pinned WebP animation encoder allocation failed";
    return false;
  }

  WebPConfig config{};
  if (!WebPConfigInitInternal(&config, WEBP_PRESET_DEFAULT, 90.0F,
                              kWebpEncoderAbiVersion)) {
    error = "pinned WebP encoder ABI is incompatible";
    return false;
  }
  config.lossless = 1;
  // For lossless WebP, quality controls compression effort, not pixel quality.
  // These rebuildable preview loops must favor encode latency over a small
  // file-size saving; exact RGBA, alpha and frame timing remain unchanged.
  config.quality = 50.0F;
  config.method = 0;
  config.alpha_quality = 100;
  config.thread_level = 1;
  config.exact = 1;

  std::uint64_t timestampMs = 0U;
  for (const auto &frame : frames) {
    if (frame.rgba.size() != frameBytes || frame.durationMs == 0U ||
        timestampMs > static_cast<std::uint64_t>(
                          std::numeric_limits<int>::max()) -
                          frame.durationMs) {
      error = "animated WebP frame payload or duration is invalid";
      return false;
    }
    PictureOwner picture;
    picture.initialized =
        WebPPictureInitInternal(&picture.value, kWebpEncoderAbiVersion) != 0;
    if (!picture.initialized) {
      error = "pinned WebP picture ABI is incompatible";
      return false;
    }
    picture.value.use_argb = 1;
    picture.value.width = static_cast<int>(width);
    picture.value.height = static_cast<int>(height);
    if (!WebPPictureImportRGBA(&picture.value, frame.rgba.data(),
                               static_cast<int>(rowBytes)) ||
        !WebPAnimEncoderAdd(encoder.get(), &picture.value,
                            static_cast<int>(timestampMs), &config)) {
      error = EncoderError(encoder.get(), "animated WebP frame encode failed");
      return false;
    }
    timestampMs += frame.durationMs;
  }
  if (!WebPAnimEncoderAdd(encoder.get(), nullptr,
                          static_cast<int>(timestampMs), nullptr)) {
    error = EncoderError(encoder.get(), "animated WebP finalization failed");
    return false;
  }
  WebPData encoded{};
  if (!WebPAnimEncoderAssemble(encoder.get(), &encoded) || !encoded.bytes ||
      encoded.size == 0U) {
    error = EncoderError(encoder.get(), "animated WebP assembly failed");
    return false;
  }
  try {
    output.assign(encoded.bytes, encoded.bytes + encoded.size);
  } catch (...) {
    WebPFree(const_cast<std::uint8_t *>(encoded.bytes));
    error = "animated WebP output allocation failed";
    return false;
  }
  WebPFree(const_cast<std::uint8_t *>(encoded.bytes));
  error.clear();
  return true;
}

} // namespace videocut::skia_runtime
