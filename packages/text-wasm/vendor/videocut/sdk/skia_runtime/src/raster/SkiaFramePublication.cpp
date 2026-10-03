#include "raster/SkiaFramePublication.h"

#include "internal/skia/SkiaHeaders.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace videocut::skia_runtime::internal {
namespace {

bool IsCanceled(const std::function<bool()>& cancel)
{
    return cancel && cancel();
}

std::uint8_t Unpremultiply(
    std::uint8_t channel,
    std::uint8_t alpha) noexcept
{
    if (alpha == 0)
        return 0;
    const std::uint32_t value =
        (static_cast<std::uint32_t>(channel) * 255U
            + static_cast<std::uint32_t>(alpha) / 2U)
        / static_cast<std::uint32_t>(alpha);
    return static_cast<std::uint8_t>(std::min(value, 255U));
}

} // namespace

PublishedFrame PublishRgba(
    SkSurface& surface,
    std::uint32_t width,
    std::uint32_t height,
    std::int64_t timestampUs,
    std::uint64_t generation,
    const std::function<bool()>& cancel,
    const bool premultiplied)
{
    PublishedFrame result;
    if (width == 0 || height == 0)
    {
        result.error = "cannot publish an empty Skia surface";
        return result;
    }
    if (IsCanceled(cancel))
    {
        result.error = "canceled";
        return result;
    }

    const auto alphaType = surface.imageInfo().alphaType();
    const bool highPrecision =
        surface.imageInfo().colorType() == kRGBA_F16_SkColorType;
    // GPU readback writes directly into the publication allocation. Raster
    // surfaces still copy their borrowed pixels into this owned frame.
    frame::ColorInfo color;
    color.primaries = frame::ColorPrimaries::Unknown;
    color.transfer = frame::TransferFunction::Srgb;
    color.matrix = frame::MatrixCoefficients::Identity;
    color.range = frame::ColorRange::Full;
    color.alpha = premultiplied ? frame::AlphaMode::Premultiplied
                                : frame::AlphaMode::Straight;
    color.model = frame::ColorModel::Rgb;
    auto descriptor = frame::MakePackedFrameDesc(
        frame::FrameKind::Image,
        highPrecision ? frame::PixelFormat::RgbaF32
                      : frame::PixelFormat::Rgba8,
        width,
        height,
        color,
        {timestampUs, 1, {1, 1'000'000}});
    if (!descriptor)
    {
        result.error = descriptor.error().message();
        return result;
    }
    auto allocation = frame::VideoFrame::AllocateCpu(
        descriptor.value(), 64, generation);
    if (!allocation)
    {
        result.error = allocation.error().message();
        return result;
    }
    auto writable = std::move(allocation).value();
    auto plane = writable.plane(0);
    if (!plane)
    {
        result.error = plane.error().message();
        return result;
    }
    const auto view = plane.value();
    SkPixmap source;
    bool sourceIsPublication = false;
    if (highPrecision || !surface.peekPixels(&source))
    {
        const auto publicationAlpha = highPrecision
            ? (premultiplied ? kPremul_SkAlphaType
                             : kUnpremul_SkAlphaType)
            : alphaType;
        const auto readbackInfo = SkImageInfo::Make(
            static_cast<int>(width), static_cast<int>(height),
            highPrecision ? kRGBA_F32_SkColorType
                          : kRGBA_8888_SkColorType,
            publicationAlpha,
            surface.imageInfo().refColorSpace());
        const auto rowBytes = static_cast<std::size_t>(view.row_stride);
        if (!surface.readPixels(
                readbackInfo, view.data, rowBytes, 0, 0))
        {
            result.error = "Skia GPU surface readback failed";
            return result;
        }
        source.reset(readbackInfo, view.data, rowBytes);
        sourceIsPublication = true;
    }
    const auto expectedColorType = highPrecision
        ? kRGBA_F32_SkColorType : kRGBA_8888_SkColorType;
    const auto expectedAlphaType = highPrecision
        ? (premultiplied ? kPremul_SkAlphaType : kUnpremul_SkAlphaType)
        : alphaType;
    const std::size_t pixelBytes = highPrecision ? 16U : 4U;
    if (source.colorType() != expectedColorType
        || (source.alphaType() != kPremul_SkAlphaType
            && source.alphaType() != kUnpremul_SkAlphaType)
        || source.alphaType() != expectedAlphaType
        || source.width() != static_cast<int>(width)
        || source.height() != static_cast<int>(height)
        || source.rowBytes() < static_cast<std::size_t>(width) * pixelBytes)
    {
        result.error =
            highPrecision
                ? "Skia surface does not match the RGBAF32 publication contract"
                : "Skia surface does not match the RGBA8888 publication contract";
        return result;
    }

    const auto* sourceBase =
        static_cast<const std::uint8_t*>(source.addr());
    for (std::uint32_t y = 0; y < height; ++y)
    {
        if (IsCanceled(cancel))
        {
            result.error = "canceled";
            return result;
        }
        const auto* sourceRow =
            sourceBase + static_cast<std::size_t>(y) * source.rowBytes();
        auto* destinationRow = reinterpret_cast<std::uint8_t*>(
            view.data + static_cast<std::size_t>(y) * view.row_stride);
        if (highPrecision)
        {
            // Skia performs the premultiplied-to-straight conversion while
            // the 16-bit working samples are still available.  Publishing
            // float pixels keeps low-coverage RGB alive through later GPU
            // composition; the final RGBA8 export is the only quantization.
            if (!sourceIsPublication)
                std::memcpy(destinationRow, sourceRow,
                        static_cast<std::size_t>(width) * pixelBytes);
            continue;
        }
        if ((source.alphaType() == kUnpremul_SkAlphaType && !premultiplied) ||
            (source.alphaType() == kPremul_SkAlphaType && premultiplied))
        {
            // Lottie renders directly into straight RGBA8. Copying it keeps
            // low-coverage RGB that would already be lost if Skia first
            // quantized a premultiplied 8-bit working surface.
            if (!sourceIsPublication)
                std::memcpy(destinationRow, sourceRow,
                        static_cast<std::size_t>(width) * 4U);
            continue;
        }
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const std::uint8_t alpha = sourceRow[x * 4U + 3U];
            for (std::uint32_t channel = 0U; channel < 3U; ++channel) {
                destinationRow[x * 4U + channel] = premultiplied
                    ? static_cast<std::uint8_t>(
                          (static_cast<std::uint32_t>(
                               sourceRow[x * 4U + channel]) * alpha + 127U) /
                          255U)
                    : Unpremultiply(sourceRow[x * 4U + channel], alpha);
            }
            destinationRow[x * 4U + 3U] = alpha;
        }
    }
    auto finished = writable.finish(result.frame);
    if (!finished)
    {
        result.error = finished.error().message();
        result.frame = {};
    }
    return result;
}

PublishedFrame PublishStraightRgba(
    SkSurface& surface, const std::uint32_t width,
    const std::uint32_t height, const std::int64_t timestampUs,
    const std::uint64_t generation,
    const std::function<bool()>& cancel)
{
    return PublishRgba(surface, width, height, timestampUs, generation,
                       cancel, false);
}

PublishedFrame PublishPremultipliedRgba(
    SkSurface& surface, const std::uint32_t width,
    const std::uint32_t height, const std::int64_t timestampUs,
    const std::uint64_t generation,
    const std::function<bool()>& cancel)
{
    return PublishRgba(surface, width, height, timestampUs, generation,
                       cancel, true);
}

} // namespace videocut::skia_runtime::internal
