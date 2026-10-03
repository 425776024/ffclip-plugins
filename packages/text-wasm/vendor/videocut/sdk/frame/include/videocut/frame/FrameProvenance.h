#pragma once

#include "videocut/frame/FrameDesc.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace videocut::frame {

inline constexpr std::int64_t kUnknownFrameTimestamp =
    std::numeric_limits<std::int64_t>::min();

/// Physical decoder/cache route that produced a source picture.
///
/// This deliberately lives in the frame leaf module instead of referring to
/// media-layer types. A published frame can therefore retain diagnostic truth
/// after it has crossed playback, effects, transforms and composition.
enum class FrameSourceRoute : std::uint8_t {
  Unknown = 0,
  CacheHit,
  SequentialDecode,
  ForwardCursorDecode,
  DecoderReseek,
  ExactReseek,
  FastForwardDecode,
  FastForwardReseek,
  FastReverseReseek,
  ReverseWindowReseek,
  ReversePrefetch,
  SourceFrameIndex,
};

/// Why a delivered picture is not an exact covering sample for this request.
enum class FrameProvisionalReason : std::uint8_t {
  None = 0,
  NearestDecodedCache,
  ProgressiveDecode,
  InteractiveSafeSideFlash,
  PlaybackLayerFallback,
  TimestampGap,
};

/// Immutable truth for one decoded source contributing to a rendered frame.
///
/// Source timestamps remain in source/stream domains and are never rewritten
/// when the same pixels are retimed onto a clip, track or output timeline.
struct FrameSourceProvenance {
  std::int64_t requested_source_time_us{kUnknownFrameTimestamp};
  std::int64_t decoded_source_start_us{kUnknownFrameTimestamp};
  std::int64_t decoded_source_end_us{kUnknownFrameTimestamp};
  std::int64_t decoded_stream_pts{kUnknownFrameTimestamp};
  Rational decoded_stream_time_base{};
  std::uint64_t request_serial{0};
  std::uint64_t session_generation{0};
  std::uint64_t lane_generation{0};
  std::int64_t clip_id{-1};
  std::int64_t track_id{-1};
  FrameSourceRoute route{FrameSourceRoute::Unknown};
  FrameProvisionalReason provisional_reason{
      FrameProvisionalReason::None};
  std::int64_t provisional_tolerance_us{0};
  double pointer_velocity_pixels_per_second{0.0};
  double source_time_us_per_pixel{0.0};
  bool pointer_motion_valid{false};
  bool from_decoded_cache{false};
  bool from_published_cache{false};
  bool provisional{false};
  bool approximate{false};
  bool zero_copy{false};
};

/// Opaque proof of the exact Effect FramePlan state that contributed pixels
/// to this delivered frame. The frame layer deliberately carries only stable
/// identities and a canonical digest; Effect topology remains owned by the
/// Effect runtime and is never reconstructed by Preview or Export.
struct FrameEffectExecutionProvenance {
  std::string scope;
  std::int64_t owner_id{-1};
  std::int64_t secondary_owner_id{-1};
  std::string plan_fingerprint;
  std::string execution_fingerprint;
};

/// Validates the opaque Effect execution boundary without interpreting the
/// underlying FramePlan. Preview and Export use this same contract so neither
/// consumer can accidentally admit a malformed digest, an unbounded receipt,
/// or two conflicting executions for one physical application.
[[nodiscard]] bool ValidateFrameEffectExecutionProvenance(
    const std::vector<FrameEffectExecutionProvenance> &executions,
    std::string &error);

/// Provenance of the current presentation plus every physical source that
/// contributes pixels to it.
struct FrameProvenance {
  FrameTiming presentation_timing{};
  std::vector<FrameSourceProvenance> sources;
  std::vector<FrameEffectExecutionProvenance> effect_executions;
};

/// Appends the first record for each source/effect identity. The destination
/// retains its presentation timing; a null input contributes no records.
void AppendUniqueFrameProvenance(const FrameProvenance *input,
                                 FrameProvenance &output);

} // namespace videocut::frame
