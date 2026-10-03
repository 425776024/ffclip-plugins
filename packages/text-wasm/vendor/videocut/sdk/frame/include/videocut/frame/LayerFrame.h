#pragma once

#include "videocut/frame/VideoFrame.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace videocut::frame {

struct LayerPresentationPoint final {
  float x{0.0F};
  float y{0.0F};
};

/// Renderer-owned visible control region for one composited Clip. Coordinates
/// are normalized against the exact output canvas that received the layer.
/// The point order follows one non-self-intersecting perimeter; callers must
/// not reinterpret it as authored source geometry.
struct LayerPresentationHitRegion final {
  bool valid{false};
  std::int64_t clip_id{-1};
  std::int64_t track_id{-1};
  std::array<LayerPresentationPoint, 4U> quad{};
  float opacity{1.0F};
  /// Exact renderer sampling map: normalized output canvas to normalized
  /// input raster coordinates, row-major projective matrix. Unlike quad,
  /// this retains the full source domain when the visible region is cropped.
  std::optional<std::array<double, 9U>> canvas_to_source;
  std::uint32_t source_raster_width{0U};
  std::uint32_t source_raster_height{0U};
};

/// Immutable pre-effect decoded picture from one renderer frame. Preview
/// retains these only within a bounded displayed-surface sampling window.
struct DecodedSourceSample final {
  std::int64_t clip_id{-1};
  std::int64_t track_id{-1};
  /// Empty for the decoded source; nonempty for the exact Chroma input.
  std::string effect_instance_id;
  std::string media_fingerprint;
  VideoFrame frame;
};

struct ChromaInputFrame final {
  std::string effect_instance_id;
  VideoFrame frame;
};

/// Exact text-mask control geometry sampled while producing this raster.
/// It travels with the LayerFrame so a preview cannot pair a control box
/// with a different filter fork or a frame that was never delivered.
struct LayerMaskTextGeometry final {
  std::int64_t clip_id{-1};
  std::string stack_identity;
  std::string layer_identity;
  std::string composition_identity;
  float x{0.0F};
  float y{0.0F};
  float width{0.0F};
  float height{0.0F};
  float rotation_degrees{0.0F};
  std::uint32_t output_width{0U};
  std::uint32_t output_height{0U};
  float authored_to_output_scale{1.0F};
  std::string text_digest;
};

struct LayerFrame {
  VideoFrame frame;
  float opacity{1.0F};
  std::string blend_mode{"normal"};
  std::int64_t clip_id{-1};
  std::int64_t track_id{-1};
  /// Back-to-front within this physical layer. Transition implementations
  /// preserve both input regions when both rasters contribute.
  std::vector<LayerPresentationHitRegion> presentation_hit_regions;
  std::vector<LayerMaskTextGeometry> mask_text_geometry;
  std::vector<ChromaInputFrame> chroma_input_frames;
  bool chroma_inputs_unavailable{false};
};

} // namespace videocut::frame
