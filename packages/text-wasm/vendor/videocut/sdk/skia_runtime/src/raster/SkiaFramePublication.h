#pragma once

#include "videocut/frame/VideoFrame.h"

#include <cstdint>
#include <functional>
#include <string>

class SkSurface;

namespace videocut::skia_runtime::internal {

struct PublishedFrame final {
    frame::VideoFrame frame;
    std::string error;
};

PublishedFrame PublishStraightRgba(
    SkSurface& surface,
    std::uint32_t width,
    std::uint32_t height,
    std::int64_t timestampUs,
    std::uint64_t generation,
    const std::function<bool()>& cancel);

PublishedFrame PublishPremultipliedRgba(
    SkSurface& surface,
    std::uint32_t width,
    std::uint32_t height,
    std::int64_t timestampUs,
    std::uint64_t generation,
    const std::function<bool()>& cancel);

} // namespace videocut::skia_runtime::internal
