#pragma once

#include "videocut/frame/FrameDesc.h"
#include "videocut/frame/FrameExport.h"
#include "videocut/frame/FrameResult.h"
#include "videocut/frame/FrameTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace videocut::frame {

class CpuWriteMap;
class AudioWriteMap;
class VideoFrame;
class AudioBuffer;

class VIDEOCUT_FRAME_API CpuBuffer final {
public:
  CpuBuffer(const CpuBuffer &) = delete;
  CpuBuffer &operator=(const CpuBuffer &) = delete;
  CpuBuffer(CpuBuffer &&) = delete;
  CpuBuffer &operator=(CpuBuffer &&) = delete;
  ~CpuBuffer() noexcept;

  [[nodiscard]] static Result<std::shared_ptr<CpuBuffer>>
  Allocate(std::size_t capacity, std::size_t alignment = 64);

  [[nodiscard]] const std::byte *data() const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t alignment() const noexcept { return alignment_; }

private:
  CpuBuffer(void *allocation, std::size_t capacity,
            std::size_t alignment) noexcept;

  [[nodiscard]] std::byte *mutableData() noexcept;

  friend class CpuWriteMap;
  friend class AudioWriteMap;
  friend class VideoFrame;
  friend class AudioBuffer;

  void *allocation_{nullptr};
  std::size_t capacity_{0};
  std::size_t alignment_{0};
};

struct CpuStorage {
  std::shared_ptr<const CpuBuffer> buffer;
};

struct GpuImageDesc {
  GpuBackend backend{GpuBackend::Unknown};
  PixelFormat format{PixelFormat::Unknown};
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint8_t plane_count{0};
};

class VIDEOCUT_FRAME_API IGpuImage {
public:
  virtual ~IGpuImage() noexcept = default;
  [[nodiscard]] virtual GpuBackend backend() const noexcept = 0;
  [[nodiscard]] virtual GpuImageDesc imageDesc() const noexcept = 0;
  [[nodiscard]] virtual std::size_t residentBytes() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t deviceGeneration() const noexcept = 0;
};

struct GpuStorage {
  std::shared_ptr<const IGpuImage> image;
};

struct SurfaceDesc {
  PixelFormat format{PixelFormat::Unknown};
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint8_t plane_count{0};
};

class VIDEOCUT_FRAME_API ISurface {
public:
  virtual ~ISurface() noexcept = default;
  [[nodiscard]] virtual SurfaceRole role() const noexcept = 0;
  [[nodiscard]] virtual SurfaceDesc surfaceDesc() const noexcept = 0;
  [[nodiscard]] virtual std::size_t residentBytes() const noexcept = 0;
};

// Optional platform-handle view for backends that can preserve a native
// decoder/encoder surface across module boundaries. Ownership remains with
// the ISurface shared_ptr; callers must never retain the raw handle beyond it.
class VIDEOCUT_FRAME_API INativeSurface : public ISurface {
public:
  ~INativeSurface() noexcept override = default;
  [[nodiscard]] virtual NativeSurfaceApi nativeApi() const noexcept = 0;
  [[nodiscard]] virtual void *nativeHandle() const noexcept = 0;
};

struct SurfaceStorage {
  std::shared_ptr<const ISurface> surface;
};

// Metadata-only leaf resources for ownership/lifetime verification and
// non-platform producers. Native backends implement IGpuImage/ISurface
// directly and keep their handles private to the backend target.
class VIDEOCUT_FRAME_API OwnedGpuImage final : public IGpuImage {
public:
  [[nodiscard]] static Result<std::shared_ptr<const OwnedGpuImage>>
  Create(GpuImageDesc desc, std::size_t resident_bytes,
         std::uint64_t device_generation, std::shared_ptr<void> lifetime = {});

  ~OwnedGpuImage() noexcept override = default;

  [[nodiscard]] GpuBackend backend() const noexcept override;
  [[nodiscard]] GpuImageDesc imageDesc() const noexcept override;
  [[nodiscard]] std::size_t residentBytes() const noexcept override;
  [[nodiscard]] std::uint64_t deviceGeneration() const noexcept override;

private:
  OwnedGpuImage(GpuImageDesc desc, std::size_t resident_bytes,
                std::uint64_t device_generation,
                std::shared_ptr<void> lifetime) noexcept;

  GpuImageDesc desc_{};
  std::size_t resident_bytes_{0};
  std::uint64_t device_generation_{0};
  std::shared_ptr<void> lifetime_;
};

class VIDEOCUT_FRAME_API OwnedSurface final : public ISurface {
public:
  [[nodiscard]] static Result<std::shared_ptr<const OwnedSurface>>
  Create(SurfaceRole role, SurfaceDesc desc, std::size_t resident_bytes,
         std::shared_ptr<void> lifetime = {});

  ~OwnedSurface() noexcept override = default;

  [[nodiscard]] SurfaceRole role() const noexcept override;
  [[nodiscard]] SurfaceDesc surfaceDesc() const noexcept override;
  [[nodiscard]] std::size_t residentBytes() const noexcept override;

private:
  OwnedSurface(SurfaceRole role, SurfaceDesc desc, std::size_t resident_bytes,
               std::shared_ptr<void> lifetime) noexcept;

  SurfaceRole role_{SurfaceRole::Unknown};
  SurfaceDesc desc_{};
  std::size_t resident_bytes_{0};
  std::shared_ptr<void> lifetime_;
};

} // namespace videocut::frame
