#include "videocut/frame/FrameStorage.h"

#include "FrameInternal.h"

#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace videocut::frame {
namespace {

[[nodiscard]] bool IsPowerOfTwo(std::size_t value) noexcept {
  return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] Result<void> ValidateGpuImageDesc(const GpuImageDesc &desc) {
  if (desc.backend == GpuBackend::Unknown || desc.width == 0 ||
      desc.height == 0 || desc.format == PixelFormat::Unknown) {
    return Result<void>::Failure(MakeFrameError(
        FrameErrorCode::InvalidDesc, "GPU image description is incomplete"));
  }
  if (desc.plane_count != ExpectedPlaneCount(desc.format)) {
    return Result<void>::Failure(MakeFrameError(
        FrameErrorCode::InvalidDesc,
        "GPU image plane count does not match its pixel format"));
  }
  return Result<void>::Success();
}

[[nodiscard]] Result<void> ValidateSurfaceDesc(SurfaceRole role,
                                               const SurfaceDesc &desc) {
  if (role == SurfaceRole::Unknown || desc.width == 0 || desc.height == 0 ||
      desc.format == PixelFormat::Unknown) {
    return Result<void>::Failure(MakeFrameError(
        FrameErrorCode::InvalidDesc, "surface description is incomplete"));
  }
  if (desc.plane_count != ExpectedPlaneCount(desc.format)) {
    return Result<void>::Failure(
        MakeFrameError(FrameErrorCode::InvalidDesc,
                       "surface plane count does not match its pixel format"));
  }

  const bool role_matches_format =
      ((role == SurfaceRole::DecoderNv12 || role == SurfaceRole::EncoderNv12) &&
       desc.format == PixelFormat::Nv12) ||
      ((role == SurfaceRole::DecoderP010 || role == SurfaceRole::EncoderP010) &&
       desc.format == PixelFormat::P010) ||
      ((role == SurfaceRole::DecoderBgra ||
        role == SurfaceRole::PreviewBgra) &&
       desc.format == PixelFormat::Bgra8);
  if (!role_matches_format) {
    return Result<void>::Failure(
        MakeFrameError(FrameErrorCode::SurfaceRoleMismatch,
                       "surface role does not match its pixel format"));
  }
  return Result<void>::Success();
}

} // namespace

namespace detail {

std::uint64_t NextContentId() noexcept {
  static std::atomic<std::uint64_t> next_content_id{1};
  auto id = next_content_id.fetch_add(1, std::memory_order_relaxed);
  if (id == 0) {
    id = next_content_id.fetch_add(1, std::memory_order_relaxed);
  }
  return id;
}

} // namespace detail

CpuBuffer::CpuBuffer(void *allocation, std::size_t capacity,
                     std::size_t alignment) noexcept
    : allocation_(allocation), capacity_(capacity), alignment_(alignment) {}

CpuBuffer::~CpuBuffer() noexcept {
  if (allocation_ != nullptr) {
    ::operator delete(allocation_, std::align_val_t(alignment_));
  }
}

Result<std::shared_ptr<CpuBuffer>> CpuBuffer::Allocate(std::size_t capacity,
                                                       std::size_t alignment) {
  if (capacity == 0) {
    return Result<std::shared_ptr<CpuBuffer>>::Failure(
        MakeFrameError(FrameErrorCode::InvalidArgument,
                       "CPU buffer capacity must be non-zero"));
  }
  if (!IsPowerOfTwo(alignment) || alignment < alignof(void *)) {
    return Result<std::shared_ptr<CpuBuffer>>::Failure(
        MakeFrameError(FrameErrorCode::InvalidArgument,
                       "CPU buffer alignment must be a power of two and at "
                       "least pointer alignment"));
  }

  try {
    void *allocation = ::operator new(capacity, std::align_val_t(alignment));
    std::memset(allocation, 0, capacity);
    CpuBuffer *buffer = nullptr;
    try {
      buffer = new CpuBuffer(allocation, capacity, alignment);
      return Result<std::shared_ptr<CpuBuffer>>::Success(
          std::shared_ptr<CpuBuffer>(buffer));
    } catch (const std::bad_alloc &) {
      if (buffer == nullptr) {
        ::operator delete(allocation, std::align_val_t(alignment));
      }
      throw;
    }
  } catch (const std::bad_alloc &) {
    return Result<std::shared_ptr<CpuBuffer>>::Failure(MakeFrameError(
        FrameErrorCode::AllocationFailed, "CPU buffer allocation failed"));
  }
}

const std::byte *CpuBuffer::data() const noexcept {
  return static_cast<const std::byte *>(allocation_);
}

std::byte *CpuBuffer::mutableData() noexcept {
  return static_cast<std::byte *>(allocation_);
}

OwnedGpuImage::OwnedGpuImage(GpuImageDesc desc, std::size_t resident_bytes,
                             std::uint64_t device_generation,
                             std::shared_ptr<void> lifetime) noexcept
    : desc_(desc), resident_bytes_(resident_bytes),
      device_generation_(device_generation), lifetime_(std::move(lifetime)) {}

Result<std::shared_ptr<const OwnedGpuImage>>
OwnedGpuImage::Create(GpuImageDesc desc, std::size_t resident_bytes,
                      std::uint64_t device_generation,
                      std::shared_ptr<void> lifetime) {
  const auto valid = ValidateGpuImageDesc(desc);
  if (!valid) {
    return Result<std::shared_ptr<const OwnedGpuImage>>::Failure(valid.error());
  }
  if (device_generation == 0) {
    return Result<std::shared_ptr<const OwnedGpuImage>>::Failure(
        MakeFrameError(FrameErrorCode::InvalidArgument,
                       "GPU device generation must be non-zero"));
  }

  try {
    return Result<std::shared_ptr<const OwnedGpuImage>>::Success(
        std::shared_ptr<const OwnedGpuImage>(new OwnedGpuImage(
            desc, resident_bytes, device_generation, std::move(lifetime))));
  } catch (const std::bad_alloc &) {
    return Result<std::shared_ptr<const OwnedGpuImage>>::Failure(MakeFrameError(
        FrameErrorCode::AllocationFailed, "GPU image owner allocation failed"));
  }
}

GpuBackend OwnedGpuImage::backend() const noexcept { return desc_.backend; }

GpuImageDesc OwnedGpuImage::imageDesc() const noexcept { return desc_; }

std::size_t OwnedGpuImage::residentBytes() const noexcept {
  return resident_bytes_;
}

std::uint64_t OwnedGpuImage::deviceGeneration() const noexcept {
  return device_generation_;
}

OwnedSurface::OwnedSurface(SurfaceRole role, SurfaceDesc desc,
                           std::size_t resident_bytes,
                           std::shared_ptr<void> lifetime) noexcept
    : role_(role), desc_(desc), resident_bytes_(resident_bytes),
      lifetime_(std::move(lifetime)) {}

Result<std::shared_ptr<const OwnedSurface>>
OwnedSurface::Create(SurfaceRole role, SurfaceDesc desc,
                     std::size_t resident_bytes,
                     std::shared_ptr<void> lifetime) {
  const auto valid = ValidateSurfaceDesc(role, desc);
  if (!valid) {
    return Result<std::shared_ptr<const OwnedSurface>>::Failure(valid.error());
  }

  try {
    return Result<std::shared_ptr<const OwnedSurface>>::Success(
        std::shared_ptr<const OwnedSurface>(
            new OwnedSurface(role, desc, resident_bytes, std::move(lifetime))));
  } catch (const std::bad_alloc &) {
    return Result<std::shared_ptr<const OwnedSurface>>::Failure(MakeFrameError(
        FrameErrorCode::AllocationFailed, "surface owner allocation failed"));
  }
}

SurfaceRole OwnedSurface::role() const noexcept { return role_; }

SurfaceDesc OwnedSurface::surfaceDesc() const noexcept { return desc_; }

std::size_t OwnedSurface::residentBytes() const noexcept {
  return resident_bytes_;
}

} // namespace videocut::frame
