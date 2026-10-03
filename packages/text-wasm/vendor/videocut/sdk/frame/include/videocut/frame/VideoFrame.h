#pragma once

#include "videocut/frame/FrameDesc.h"
#include "videocut/frame/FrameExport.h"
#include "videocut/frame/FrameMap.h"
#include "videocut/frame/FrameProvenance.h"
#include "videocut/frame/FrameResult.h"
#include "videocut/frame/FrameStorage.h"
#include "videocut/frame/FrameTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace videocut::frame {

class VIDEOCUT_FRAME_API VideoFrame final {
public:
  VideoFrame() = default;

  [[nodiscard]] static Result<CpuWriteMap>
  AllocateCpu(const FrameDesc &desc, std::size_t alignment = 64,
              std::uint64_t generation = 1);

  [[nodiscard]] static Result<VideoFrame>
  CopyFromCpu(const FrameDesc &desc, const void *source,
              std::size_t source_size, std::size_t alignment = 64);

  [[nodiscard]] static Result<VideoFrame>
  FromGpu(const FrameDesc &desc, std::shared_ptr<const IGpuImage> image,
          std::uint64_t generation = 1);

  [[nodiscard]] static Result<VideoFrame>
  FromSurface(const FrameDesc &desc, std::shared_ptr<const ISurface> surface,
              std::uint64_t generation = 1);

  [[nodiscard]] const FrameDesc &desc() const noexcept;
  [[nodiscard]] StorageKind storageKind() const noexcept;
  [[nodiscard]] std::uint64_t contentId() const noexcept;
  [[nodiscard]] std::uint64_t generation() const noexcept;
  [[nodiscard]] std::size_t residentBytes() const noexcept;
  [[nodiscard]] const FrameProvenance *provenance() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept;

  [[nodiscard]] Result<CpuReadMap> mapRead() const;
  [[nodiscard]] Result<std::shared_ptr<const IGpuImage>> gpuImage() const;
  [[nodiscard]] Result<std::shared_ptr<const ISurface>> surface() const;
  [[nodiscard]] Result<VideoFrame> withTiming(FrameTiming timing) const;
  [[nodiscard]] Result<VideoFrame>
  withProvenance(FrameProvenance provenance) const;

  /// Return a zero-pixel-copy alias with replacement presentation identity.
  /// Storage, content/generation identity, format/layout, color and rotation
  /// are preserved; only kind and timing are replaced.
  [[nodiscard]] Result<VideoFrame>
  withPresentation(FrameKind kind, FrameTiming timing) const;

private:
  struct FrameData;

  explicit VideoFrame(std::shared_ptr<const FrameData> data) noexcept;

  [[nodiscard]] static Result<VideoFrame>
  FromCpuBuffer(FrameDesc desc, std::shared_ptr<const CpuBuffer> buffer,
                std::uint64_t generation);

  friend class CpuWriteMap;

  std::shared_ptr<const FrameData> data_;
};

} // namespace videocut::frame
