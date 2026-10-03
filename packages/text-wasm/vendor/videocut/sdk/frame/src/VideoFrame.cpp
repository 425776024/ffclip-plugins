#include "videocut/frame/VideoFrame.h"

#include "FrameInternal.h"

#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <variant>

namespace videocut::frame {
namespace {

const FrameDesc kEmptyFrameDesc{};

[[nodiscard]] Result<std::size_t> CpuCapacityForDesc(const FrameDesc &desc) {
  const auto required = RequiredBufferBytes(desc);
  if (!required) {
    return Result<std::size_t>::Failure(required.error());
  }
  if (required.value() > std::numeric_limits<std::size_t>::max()) {
    return Result<std::size_t>::Failure(
        MakeFrameError(FrameErrorCode::Overflow,
                       "frame capacity exceeds platform size_t range"));
  }
  return Result<std::size_t>::Success(
      static_cast<std::size_t>(required.value()));
}

[[nodiscard]] bool SurfaceRoleMatchesFormat(SurfaceRole role,
                                            PixelFormat format) noexcept {
  return ((role == SurfaceRole::DecoderNv12 ||
           role == SurfaceRole::EncoderNv12) &&
          format == PixelFormat::Nv12) ||
         ((role == SurfaceRole::DecoderP010 ||
           role == SurfaceRole::EncoderP010) &&
          format == PixelFormat::P010) ||
         ((role == SurfaceRole::DecoderBgra ||
           role == SurfaceRole::PreviewBgra) &&
          format == PixelFormat::Bgra8);
}

} // namespace

struct VideoFrame::FrameData {
  FrameDesc desc;
  std::uint64_t content_id{0};
  std::uint64_t generation{0};
  std::shared_ptr<const FrameProvenance> provenance;
  std::variant<CpuStorage, GpuStorage, SurfaceStorage> storage;
};

CpuReadMap::CpuReadMap(FrameDesc desc,
                       std::shared_ptr<const CpuBuffer> buffer) noexcept
    : desc_(std::move(desc)), buffer_(std::move(buffer)) {}

CpuReadMap::operator bool() const noexcept { return buffer_ != nullptr; }

std::uint8_t CpuReadMap::planeCount() const noexcept {
  return buffer_ ? desc_.plane_count : 0;
}

Result<PlaneReadView> CpuReadMap::plane(std::uint8_t index) const {
  if (!buffer_) {
    return Result<PlaneReadView>::Failure(MakeFrameError(
        FrameErrorCode::ResourceClosed, "CPU read map is empty"));
  }
  if (index >= desc_.plane_count) {
    return Result<PlaneReadView>::Failure(
        MakeFrameError(FrameErrorCode::InvalidPlaneIndex,
                       "CPU read plane index is out of range"));
  }

  const auto &layout = desc_.planes[index];
  const auto offset = static_cast<std::size_t>(layout.offset);
  return Result<PlaneReadView>::Success(PlaneReadView{
      buffer_->data() + offset,
      static_cast<std::size_t>(layout.size),
      layout.row_stride,
      layout.pixel_stride,
  });
}

CpuWriteMap::CpuWriteMap(FrameDesc desc, std::shared_ptr<CpuBuffer> buffer,
                         std::uint64_t generation) noexcept
    : desc_(std::move(desc)), buffer_(std::move(buffer)),
      generation_(generation) {}

CpuWriteMap::operator bool() const noexcept {
  return buffer_ != nullptr && !finished_;
}

std::uint8_t CpuWriteMap::planeCount() const noexcept {
  return *this ? desc_.plane_count : 0;
}

Result<PlaneWriteView> CpuWriteMap::plane(std::uint8_t index) {
  if (!*this) {
    return Result<PlaneWriteView>::Failure(
        MakeFrameError(FrameErrorCode::ResourceClosed,
                       "CPU write map is empty or already finished"));
  }
  if (index >= desc_.plane_count) {
    return Result<PlaneWriteView>::Failure(
        MakeFrameError(FrameErrorCode::InvalidPlaneIndex,
                       "CPU write plane index is out of range"));
  }

  const auto &layout = desc_.planes[index];
  const auto offset = static_cast<std::size_t>(layout.offset);
  return Result<PlaneWriteView>::Success(PlaneWriteView{
      buffer_->mutableData() + offset,
      static_cast<std::size_t>(layout.size),
      layout.row_stride,
      layout.pixel_stride,
  });
}

Result<void> CpuWriteMap::finish(VideoFrame &output) {
  if (!*this) {
    return Result<void>::Failure(
        MakeFrameError(FrameErrorCode::ResourceClosed,
                       "CPU write map is empty or already finished"));
  }

  std::shared_ptr<const CpuBuffer> immutable_buffer = buffer_;
  auto frame = VideoFrame::FromCpuBuffer(desc_, std::move(immutable_buffer),
                                         generation_);
  if (!frame) {
    return Result<void>::Failure(frame.error());
  }

  output = std::move(frame).value();
  buffer_.reset();
  finished_ = true;
  return Result<void>::Success();
}

VideoFrame::VideoFrame(std::shared_ptr<const FrameData> data) noexcept
    : data_(std::move(data)) {}

Result<CpuWriteMap> VideoFrame::AllocateCpu(const FrameDesc &desc,
                                            std::size_t alignment,
                                            std::uint64_t generation) {
  if (generation == 0) {
    return Result<CpuWriteMap>::Failure(
        MakeFrameError(FrameErrorCode::InvalidArgument,
                       "CPU frame generation must be non-zero"));
  }
  const auto capacity = CpuCapacityForDesc(desc);
  if (!capacity) {
    return Result<CpuWriteMap>::Failure(capacity.error());
  }
  auto buffer = CpuBuffer::Allocate(capacity.value(), alignment);
  if (!buffer) {
    return Result<CpuWriteMap>::Failure(buffer.error());
  }
  return Result<CpuWriteMap>::Success(
      CpuWriteMap(desc, std::move(buffer).value(), generation));
}

Result<VideoFrame> VideoFrame::CopyFromCpu(const FrameDesc &desc,
                                           const void *source,
                                           std::size_t source_size,
                                           std::size_t alignment) {
  const auto capacity = CpuCapacityForDesc(desc);
  if (!capacity) {
    return Result<VideoFrame>::Failure(capacity.error());
  }
  if (source == nullptr) {
    return Result<VideoFrame>::Failure(MakeFrameError(
        FrameErrorCode::InvalidArgument, "CPU frame source cannot be null"));
  }
  if (source_size < capacity.value()) {
    return Result<VideoFrame>::Failure(MakeFrameError(
        FrameErrorCode::InsufficientCapacity,
        "CPU frame source is smaller than the described plane ranges"));
  }

  auto buffer = CpuBuffer::Allocate(capacity.value(), alignment);
  if (!buffer) {
    return Result<VideoFrame>::Failure(buffer.error());
  }
  std::memcpy(buffer.value()->mutableData(), source, capacity.value());
  std::shared_ptr<const CpuBuffer> immutable_buffer = std::move(buffer).value();
  return FromCpuBuffer(desc, std::move(immutable_buffer), 1);
}

Result<VideoFrame> VideoFrame::FromGpu(const FrameDesc &desc,
                                       std::shared_ptr<const IGpuImage> image,
                                       std::uint64_t generation) {
  const auto valid = ValidateFrameDesc(desc);
  if (!valid) {
    return Result<VideoFrame>::Failure(valid.error());
  }
  if (!image || generation == 0) {
    return Result<VideoFrame>::Failure(
        MakeFrameError(FrameErrorCode::InvalidArgument,
                       "GPU frame requires an image and non-zero generation"));
  }

  const auto image_desc = image->imageDesc();
  if (image_desc.backend == GpuBackend::Unknown ||
      image->backend() != image_desc.backend ||
      image_desc.width != desc.width || image_desc.height != desc.height ||
      image_desc.format != desc.format ||
      image_desc.plane_count != desc.plane_count) {
    return Result<VideoFrame>::Failure(
        MakeFrameError(FrameErrorCode::StorageMismatch,
                       "GPU image description does not match FrameDesc"));
  }
  if (image->deviceGeneration() == 0) {
    return Result<VideoFrame>::Failure(
        MakeFrameError(FrameErrorCode::DeviceGenerationMismatch,
                       "GPU image has an invalid device generation"));
  }

  try {
    auto data = std::make_shared<FrameData>();
    data->desc = desc;
    data->content_id = detail::NextContentId();
    data->generation = generation;
    data->storage = GpuStorage{std::move(image)};
    return Result<VideoFrame>::Success(VideoFrame(std::move(data)));
  } catch (const std::bad_alloc &) {
    return Result<VideoFrame>::Failure(MakeFrameError(
        FrameErrorCode::AllocationFailed, "GPU frame allocation failed"));
  }
}

Result<VideoFrame>
VideoFrame::FromSurface(const FrameDesc &desc,
                        std::shared_ptr<const ISurface> surface,
                        std::uint64_t generation) {
  const auto valid = ValidateFrameDesc(desc);
  if (!valid) {
    return Result<VideoFrame>::Failure(valid.error());
  }
  if (!surface || generation == 0) {
    return Result<VideoFrame>::Failure(MakeFrameError(
        FrameErrorCode::InvalidArgument,
        "surface frame requires a surface and non-zero generation"));
  }

  const auto surface_desc = surface->surfaceDesc();
  if (surface_desc.width != desc.width || surface_desc.height != desc.height ||
      surface_desc.format != desc.format ||
      surface_desc.plane_count != desc.plane_count) {
    return Result<VideoFrame>::Failure(
        MakeFrameError(FrameErrorCode::StorageMismatch,
                       "surface description does not match FrameDesc"));
  }
  if (!SurfaceRoleMatchesFormat(surface->role(), desc.format)) {
    return Result<VideoFrame>::Failure(
        MakeFrameError(FrameErrorCode::SurfaceRoleMismatch,
                       "surface role does not match FrameDesc format"));
  }

  try {
    auto data = std::make_shared<FrameData>();
    data->desc = desc;
    data->content_id = detail::NextContentId();
    data->generation = generation;
    data->storage = SurfaceStorage{std::move(surface)};
    return Result<VideoFrame>::Success(VideoFrame(std::move(data)));
  } catch (const std::bad_alloc &) {
    return Result<VideoFrame>::Failure(MakeFrameError(
        FrameErrorCode::AllocationFailed, "surface frame allocation failed"));
  }
}

const FrameDesc &VideoFrame::desc() const noexcept {
  return data_ ? data_->desc : kEmptyFrameDesc;
}

StorageKind VideoFrame::storageKind() const noexcept {
  if (!data_) {
    return StorageKind::Empty;
  }
  if (std::holds_alternative<CpuStorage>(data_->storage)) {
    return StorageKind::Cpu;
  }
  if (std::holds_alternative<GpuStorage>(data_->storage)) {
    return StorageKind::Gpu;
  }
  return StorageKind::Surface;
}

std::uint64_t VideoFrame::contentId() const noexcept {
  return data_ ? data_->content_id : 0;
}

std::uint64_t VideoFrame::generation() const noexcept {
  return data_ ? data_->generation : 0;
}

std::size_t VideoFrame::residentBytes() const noexcept {
  if (!data_) {
    return 0;
  }
  if (const auto *cpu = std::get_if<CpuStorage>(&data_->storage)) {
    return cpu->buffer ? cpu->buffer->capacity() : 0;
  }
  if (const auto *gpu = std::get_if<GpuStorage>(&data_->storage)) {
    return gpu->image ? gpu->image->residentBytes() : 0;
  }
  const auto &surface_storage = std::get<SurfaceStorage>(data_->storage);
  return surface_storage.surface ? surface_storage.surface->residentBytes() : 0;
}

const FrameProvenance *VideoFrame::provenance() const noexcept {
  return data_ && data_->provenance ? data_->provenance.get() : nullptr;
}

VideoFrame::operator bool() const noexcept { return data_ != nullptr; }

Result<CpuReadMap> VideoFrame::mapRead() const {
  if (!data_) {
    return Result<CpuReadMap>::Failure(MakeFrameError(
        FrameErrorCode::ResourceClosed, "cannot map an empty frame"));
  }
  const auto *cpu = std::get_if<CpuStorage>(&data_->storage);
  if (cpu == nullptr || !cpu->buffer) {
    return Result<CpuReadMap>::Failure(MakeFrameError(
        FrameErrorCode::NotCpuMappable, "frame storage is not CPU-mappable"));
  }
  return Result<CpuReadMap>::Success(CpuReadMap(data_->desc, cpu->buffer));
}

Result<std::shared_ptr<const IGpuImage>> VideoFrame::gpuImage() const {
  if (!data_) {
    return Result<std::shared_ptr<const IGpuImage>>::Failure(
        MakeFrameError(FrameErrorCode::ResourceClosed,
                       "cannot access GPU storage on an empty frame"));
  }
  const auto *gpu = std::get_if<GpuStorage>(&data_->storage);
  if (gpu == nullptr || !gpu->image) {
    return Result<std::shared_ptr<const IGpuImage>>::Failure(MakeFrameError(
        FrameErrorCode::StorageMismatch, "frame does not contain GPU storage"));
  }
  return Result<std::shared_ptr<const IGpuImage>>::Success(gpu->image);
}

Result<std::shared_ptr<const ISurface>> VideoFrame::surface() const {
  if (!data_) {
    return Result<std::shared_ptr<const ISurface>>::Failure(
        MakeFrameError(FrameErrorCode::ResourceClosed,
                       "cannot access surface storage on an empty frame"));
  }
  const auto *surface_storage = std::get_if<SurfaceStorage>(&data_->storage);
  if (surface_storage == nullptr || !surface_storage->surface) {
    return Result<std::shared_ptr<const ISurface>>::Failure(
        MakeFrameError(FrameErrorCode::StorageMismatch,
                       "frame does not contain surface storage"));
  }
  return Result<std::shared_ptr<const ISurface>>::Success(
      surface_storage->surface);
}

Result<VideoFrame> VideoFrame::withTiming(FrameTiming timing) const {
  if (!data_) {
    return Result<VideoFrame>::Failure(MakeFrameError(
        FrameErrorCode::ResourceClosed, "cannot retime an empty frame"));
  }

  FrameDesc desc = data_->desc;
  desc.timing = timing;
  const auto valid = ValidateFrameDesc(desc);
  if (!valid) {
    return Result<VideoFrame>::Failure(valid.error());
  }

  try {
    auto data = std::make_shared<FrameData>(*data_);
    data->desc = std::move(desc);
    if (data->provenance) {
      auto provenance =
          std::make_shared<FrameProvenance>(*data->provenance);
      provenance->presentation_timing = timing;
      data->provenance = std::move(provenance);
    }
    return Result<VideoFrame>::Success(VideoFrame(std::move(data)));
  } catch (const std::bad_alloc &) {
    return Result<VideoFrame>::Failure(MakeFrameError(
        FrameErrorCode::AllocationFailed, "retimed frame allocation failed"));
  }
}

Result<VideoFrame>
VideoFrame::withProvenance(FrameProvenance provenance) const {
  if (!data_) {
    return Result<VideoFrame>::Failure(MakeFrameError(
        FrameErrorCode::ResourceClosed,
        "cannot attach provenance to an empty frame"));
  }

  provenance.presentation_timing = data_->desc.timing;
  try {
    auto data = std::make_shared<FrameData>(*data_);
    data->provenance =
        std::make_shared<const FrameProvenance>(std::move(provenance));
    return Result<VideoFrame>::Success(VideoFrame(std::move(data)));
  } catch (const std::bad_alloc &) {
    return Result<VideoFrame>::Failure(MakeFrameError(
        FrameErrorCode::AllocationFailed,
        "frame provenance allocation failed"));
  }
}

Result<VideoFrame>
VideoFrame::withPresentation(FrameKind kind, FrameTiming timing) const {
  if (!data_) {
    return Result<VideoFrame>::Failure(MakeFrameError(
        FrameErrorCode::ResourceClosed,
        "cannot change presentation metadata on an empty frame"));
  }

  FrameDesc desc = data_->desc;
  desc.kind = kind;
  desc.timing = timing;
  const auto valid = ValidateFrameDesc(desc);
  if (!valid) {
    return Result<VideoFrame>::Failure(valid.error());
  }

  try {
    // FrameData's storage variant only contains shared storage owners. Copying
    // it creates a descriptor alias without copying or mapping pixel data and
    // deliberately retains content_id and generation.
    auto data = std::make_shared<FrameData>(*data_);
    data->desc = std::move(desc);
    if (data->provenance) {
      auto provenance =
          std::make_shared<FrameProvenance>(*data->provenance);
      provenance->presentation_timing = timing;
      data->provenance = std::move(provenance);
    }
    return Result<VideoFrame>::Success(VideoFrame(std::move(data)));
  } catch (const std::bad_alloc &) {
    return Result<VideoFrame>::Failure(MakeFrameError(
        FrameErrorCode::AllocationFailed,
        "presentation metadata alias allocation failed"));
  }
}

Result<VideoFrame>
VideoFrame::FromCpuBuffer(FrameDesc desc,
                          std::shared_ptr<const CpuBuffer> buffer,
                          std::uint64_t generation) {
  if (!buffer || generation == 0) {
    return Result<VideoFrame>::Failure(
        MakeFrameError(FrameErrorCode::InvalidArgument,
                       "CPU frame requires a buffer and non-zero generation"));
  }
  const auto valid = ValidateFrameCapacity(desc, buffer->capacity());
  if (!valid) {
    return Result<VideoFrame>::Failure(valid.error());
  }

  try {
    auto data = std::make_shared<FrameData>();
    data->desc = std::move(desc);
    data->content_id = detail::NextContentId();
    data->generation = generation;
    data->storage = CpuStorage{std::move(buffer)};
    return Result<VideoFrame>::Success(VideoFrame(std::move(data)));
  } catch (const std::bad_alloc &) {
    return Result<VideoFrame>::Failure(MakeFrameError(
        FrameErrorCode::AllocationFailed, "CPU frame allocation failed"));
  }
}

} // namespace videocut::frame
