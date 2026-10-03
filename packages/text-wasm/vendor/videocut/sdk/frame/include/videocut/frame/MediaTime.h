#pragma once

#include "videocut/frame/FrameDesc.h"

#include <cstdint>

namespace videocut::frame {

/// Canonical authored/editor timeline clock.
///
/// 120,000 ticks per second represents the frame duration of common integer
/// and NTSC-derived rates exactly (for example 24, 25, 30, 60, 24000/1001,
/// 30000/1001, and 60000/1001). Source PTS values keep their native rational
/// time base and are rescaled only at media/editor boundaries.
inline constexpr std::int64_t kMediaTicksPerSecond = 120'000;
inline constexpr Rational kMediaTickTimeBase{
    1, static_cast<std::int32_t>(kMediaTicksPerSecond)};

using MediaTick = std::int64_t;

[[nodiscard]] inline Result<MediaTick>
TimestampToMediaTicks(std::int64_t value, Rational source,
                      TimestampRounding rounding =
                          TimestampRounding::Nearest) {
  return RescaleTimestamp(value, source, kMediaTickTimeBase, rounding);
}

[[nodiscard]] inline Result<std::int64_t>
MediaTicksToTimestamp(MediaTick ticks, Rational destination,
                      TimestampRounding rounding =
                          TimestampRounding::Nearest) {
  return RescaleTimestamp(ticks, kMediaTickTimeBase, destination, rounding);
}

[[nodiscard]] inline Result<MediaTick>
FrameIndexToMediaTicks(std::int64_t frame_index, Rational frame_rate,
                       TimestampRounding rounding =
                           TimestampRounding::Nearest) {
  if (frame_rate.num <= 0 || frame_rate.den <= 0) {
    return Result<MediaTick>::Failure(MakeFrameError(
        FrameErrorCode::InvalidArgument, "frame rate must be positive"));
  }
  return TimestampToMediaTicks(
      frame_index, Rational{frame_rate.den, frame_rate.num}, rounding);
}

[[nodiscard]] inline Result<std::int64_t>
MediaTicksToFrameIndex(MediaTick ticks, Rational frame_rate,
                       TimestampRounding rounding =
                           TimestampRounding::Nearest) {
  if (frame_rate.num <= 0 || frame_rate.den <= 0) {
    return Result<std::int64_t>::Failure(MakeFrameError(
        FrameErrorCode::InvalidArgument, "frame rate must be positive"));
  }
  return MediaTicksToTimestamp(
      ticks, Rational{frame_rate.den, frame_rate.num}, rounding);
}

} // namespace videocut::frame
