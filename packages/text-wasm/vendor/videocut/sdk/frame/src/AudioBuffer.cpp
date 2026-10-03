#include "videocut/frame/AudioBuffer.h"

#include "FrameInternal.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace videocut::frame {
namespace {

const AudioDesc kEmptyAudioDesc{};

[[nodiscard]] Result<std::uint64_t>
CheckedMultiply(std::uint64_t lhs, std::uint64_t rhs, const char *operation) {
  if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
    return Result<std::uint64_t>::Failure(MakeFrameError(
        FrameErrorCode::Overflow, std::string(operation) + " overflow"));
  }
  return Result<std::uint64_t>::Success(lhs * rhs);
}

[[nodiscard]] Result<std::size_t> AudioCapacityForDesc(const AudioDesc &desc) {
  const auto required = AudioRequiredBytes(desc);
  if (!required) {
    return Result<std::size_t>::Failure(required.error());
  }
  if (required.value() > std::numeric_limits<std::size_t>::max()) {
    return Result<std::size_t>::Failure(
        MakeFrameError(FrameErrorCode::Overflow,
                       "audio capacity exceeds platform size_t range"));
  }
  return Result<std::size_t>::Success(
      static_cast<std::size_t>(required.value()));
}

[[nodiscard]] Result<std::size_t> AudioPlaneOffset(const AudioDesc &desc,
                                                   std::uint32_t index) {
  if (index >= (desc.planar ? desc.channels : 1U)) {
    return Result<std::size_t>::Failure(
        MakeFrameError(FrameErrorCode::InvalidPlaneIndex,
                       "audio plane index is out of range"));
  }
  if (!desc.planar) {
    return Result<std::size_t>::Success(0);
  }

  const auto bytes_per_plane = CheckedMultiply(
      desc.frames, BytesPerSample(desc.format), "audio plane offset");
  if (!bytes_per_plane) {
    return Result<std::size_t>::Failure(bytes_per_plane.error());
  }
  const auto offset =
      CheckedMultiply(bytes_per_plane.value(), index, "audio plane offset");
  if (!offset) {
    return Result<std::size_t>::Failure(offset.error());
  }
  if (offset.value() > std::numeric_limits<std::size_t>::max()) {
    return Result<std::size_t>::Failure(
        MakeFrameError(FrameErrorCode::Overflow,
                       "audio plane offset exceeds platform size_t range"));
  }
  return Result<std::size_t>::Success(static_cast<std::size_t>(offset.value()));
}

[[nodiscard]] Result<std::size_t> AudioPlaneBytes(const AudioDesc &desc) {
  std::uint64_t samples = desc.frames;
  if (!desc.planar) {
    const auto interleaved_samples =
        CheckedMultiply(samples, desc.channels, "interleaved audio samples");
    if (!interleaved_samples) {
      return Result<std::size_t>::Failure(interleaved_samples.error());
    }
    samples = interleaved_samples.value();
  }
  const auto bytes = CheckedMultiply(samples, BytesPerSample(desc.format),
                                     "audio plane bytes");
  if (!bytes) {
    return Result<std::size_t>::Failure(bytes.error());
  }
  if (bytes.value() > std::numeric_limits<std::size_t>::max()) {
    return Result<std::size_t>::Failure(
        MakeFrameError(FrameErrorCode::Overflow,
                       "audio plane size exceeds platform size_t range"));
  }
  return Result<std::size_t>::Success(static_cast<std::size_t>(bytes.value()));
}

[[nodiscard]] Result<std::int64_t>
AudioFramesToTimingTicks(std::uint32_t frames, const AudioDesc &desc) {
  if (desc.sample_rate == 0 || desc.timing.time_base.num <= 0 ||
      desc.timing.time_base.den <= 0) {
    return Result<std::int64_t>::Failure(
        MakeFrameError(FrameErrorCode::InvalidDesc,
                       "audio timing conversion requires a valid rate"));
  }

  const long double ticks =
      static_cast<long double>(frames) *
      static_cast<long double>(desc.timing.time_base.den) /
      (static_cast<long double>(desc.sample_rate) *
       static_cast<long double>(desc.timing.time_base.num));
  if (ticks > static_cast<long double>(
                  std::numeric_limits<std::int64_t>::max())) {
    return Result<std::int64_t>::Failure(
        MakeFrameError(FrameErrorCode::Overflow,
                       "audio timing conversion overflow"));
  }
  return Result<std::int64_t>::Success(
      static_cast<std::int64_t>(std::llround(ticks)));
}

} // namespace

struct AudioBuffer::BufferData {
  AudioDesc desc;
  std::uint64_t content_id{0};
  std::uint64_t generation{0};
  std::shared_ptr<const CpuBuffer> buffer;
};

std::uint8_t BytesPerSample(SampleFormat format) noexcept {
  switch (format) {
  case SampleFormat::S16:
    return 2;
  case SampleFormat::S32:
  case SampleFormat::F32:
    return 4;
  case SampleFormat::F64:
    return 8;
  case SampleFormat::Unknown:
    return 0;
  }
  return 0;
}

std::uint64_t DefaultAudioChannelLayout(std::uint32_t channels) noexcept {
  switch (channels) {
  case 1:
    return 0x4U;
  case 2:
    return 0x3U;
  case 3:
    return 0x7U;
  case 4:
    return 0x107U;
  case 5:
    return 0x37U;
  case 6:
    return 0x3fU;
  case 7:
    return 0x70fU;
  case 8:
    return 0x63fU;
  default:
    return 0;
  }
}

bool AudioChannelLayoutMatches(std::uint64_t layout,
                               std::uint32_t channels) noexcept {
  if (layout == 0 || channels == 0 || channels > 64) {
    return false;
  }
  std::uint32_t count = 0;
  while (layout != 0) {
    layout &= layout - 1;
    ++count;
  }
  return count == channels;
}

Result<void> ValidateAudioDesc(const AudioDesc &desc) {
  if (BytesPerSample(desc.format) == 0) {
    return Result<void>::Failure(
        MakeFrameError(FrameErrorCode::UnsupportedFormat,
                       "audio sample format is unknown or unsupported"));
  }
  if (desc.sample_rate == 0 || desc.channels == 0 || desc.channels > 64 ||
      desc.frames == 0) {
    return Result<void>::Failure(
        MakeFrameError(FrameErrorCode::InvalidDesc,
                       "audio rate, channels, and frame count must be valid"));
  }
  if (!AudioChannelLayoutMatches(desc.channel_layout, desc.channels)) {
    return Result<void>::Failure(
        MakeFrameError(FrameErrorCode::InvalidDesc,
                       "audio channel layout does not match channel count"));
  }
  if (desc.timing.time_base.num <= 0 || desc.timing.time_base.den <= 0 ||
      desc.timing.duration < 0) {
    return Result<void>::Failure(
        MakeFrameError(FrameErrorCode::InvalidDesc, "audio timing is invalid"));
  }
  const auto required = AudioRequiredBytes(desc);
  if (!required) {
    return Result<void>::Failure(required.error());
  }
  return Result<void>::Success();
}

Result<std::uint64_t> AudioRequiredBytes(const AudioDesc &desc) {
  const auto bytes_per_sample = BytesPerSample(desc.format);
  if (bytes_per_sample == 0) {
    return Result<std::uint64_t>::Failure(
        MakeFrameError(FrameErrorCode::UnsupportedFormat,
                       "audio sample format is unknown or unsupported"));
  }
  if (desc.sample_rate == 0 || desc.channels == 0 || desc.channels > 64 ||
      desc.frames == 0) {
    return Result<std::uint64_t>::Failure(
        MakeFrameError(FrameErrorCode::InvalidDesc,
                       "audio rate, channels, and frame count must be valid"));
  }

  const auto sample_count =
      CheckedMultiply(desc.frames, desc.channels, "audio sample count");
  if (!sample_count) {
    return sample_count;
  }
  return CheckedMultiply(sample_count.value(), bytes_per_sample,
                         "audio byte size");
}

AudioReadMap::AudioReadMap(AudioDesc desc,
                           std::shared_ptr<const CpuBuffer> buffer) noexcept
    : desc_(std::move(desc)), buffer_(std::move(buffer)) {}

AudioReadMap::operator bool() const noexcept { return buffer_ != nullptr; }

std::uint32_t AudioReadMap::planeCount() const noexcept {
  return buffer_ ? (desc_.planar ? desc_.channels : 1U) : 0;
}

Result<AudioPlaneReadView> AudioReadMap::plane(std::uint32_t index) const {
  if (!buffer_) {
    return Result<AudioPlaneReadView>::Failure(MakeFrameError(
        FrameErrorCode::ResourceClosed, "audio read map is empty"));
  }
  const auto offset = AudioPlaneOffset(desc_, index);
  if (!offset) {
    return Result<AudioPlaneReadView>::Failure(offset.error());
  }
  const auto plane_bytes = AudioPlaneBytes(desc_);
  if (!plane_bytes) {
    return Result<AudioPlaneReadView>::Failure(plane_bytes.error());
  }

  const auto bytes_per_sample = BytesPerSample(desc_.format);
  const auto frame_stride =
      desc_.planar
          ? bytes_per_sample
          : static_cast<std::uint32_t>(bytes_per_sample) * desc_.channels;
  return Result<AudioPlaneReadView>::Success(AudioPlaneReadView{
      buffer_->data() + offset.value(),
      plane_bytes.value(),
      frame_stride,
      bytes_per_sample,
  });
}

AudioWriteMap::AudioWriteMap(AudioDesc desc, std::shared_ptr<CpuBuffer> buffer,
                             std::uint64_t generation) noexcept
    : desc_(std::move(desc)), buffer_(std::move(buffer)),
      generation_(generation) {}

AudioWriteMap::operator bool() const noexcept {
  return buffer_ != nullptr && !finished_;
}

std::uint32_t AudioWriteMap::planeCount() const noexcept {
  return *this ? (desc_.planar ? desc_.channels : 1U) : 0;
}

Result<AudioPlaneWriteView> AudioWriteMap::plane(std::uint32_t index) {
  if (!*this) {
    return Result<AudioPlaneWriteView>::Failure(
        MakeFrameError(FrameErrorCode::ResourceClosed,
                       "audio write map is empty or already finished"));
  }
  const auto offset = AudioPlaneOffset(desc_, index);
  if (!offset) {
    return Result<AudioPlaneWriteView>::Failure(offset.error());
  }
  const auto plane_bytes = AudioPlaneBytes(desc_);
  if (!plane_bytes) {
    return Result<AudioPlaneWriteView>::Failure(plane_bytes.error());
  }

  const auto bytes_per_sample = BytesPerSample(desc_.format);
  const auto frame_stride =
      desc_.planar
          ? bytes_per_sample
          : static_cast<std::uint32_t>(bytes_per_sample) * desc_.channels;
  return Result<AudioPlaneWriteView>::Success(AudioPlaneWriteView{
      buffer_->mutableData() + offset.value(),
      plane_bytes.value(),
      frame_stride,
      bytes_per_sample,
  });
}

Result<void> AudioWriteMap::finish(AudioBuffer &output) {
  if (!*this) {
    return Result<void>::Failure(
        MakeFrameError(FrameErrorCode::ResourceClosed,
                       "audio write map is empty or already finished"));
  }

  std::shared_ptr<const CpuBuffer> immutable_buffer = buffer_;
  auto audio = AudioBuffer::FromCpuBuffer(desc_, std::move(immutable_buffer),
                                          generation_);
  if (!audio) {
    return Result<void>::Failure(audio.error());
  }

  output = std::move(audio).value();
  buffer_.reset();
  finished_ = true;
  return Result<void>::Success();
}

AudioBuffer::AudioBuffer(std::shared_ptr<const BufferData> data) noexcept
    : data_(std::move(data)) {}

Result<AudioWriteMap> AudioBuffer::Allocate(const AudioDesc &desc,
                                            std::size_t alignment,
                                            std::uint64_t generation) {
  if (generation == 0) {
    return Result<AudioWriteMap>::Failure(
        MakeFrameError(FrameErrorCode::InvalidArgument,
                       "audio generation must be non-zero"));
  }
  const auto valid = ValidateAudioDesc(desc);
  if (!valid) {
    return Result<AudioWriteMap>::Failure(valid.error());
  }
  const auto capacity = AudioCapacityForDesc(desc);
  if (!capacity) {
    return Result<AudioWriteMap>::Failure(capacity.error());
  }
  auto buffer = CpuBuffer::Allocate(capacity.value(), alignment);
  if (!buffer) {
    return Result<AudioWriteMap>::Failure(buffer.error());
  }
  return Result<AudioWriteMap>::Success(
      AudioWriteMap(desc, std::move(buffer).value(), generation));
}

Result<AudioBuffer> AudioBuffer::Silence(const AudioDesc &desc,
                                         std::size_t alignment) {
  auto writable = Allocate(desc, alignment);
  if (!writable) {
    return Result<AudioBuffer>::Failure(writable.error());
  }

  auto write_map = std::move(writable).value();
  for (std::uint32_t plane_index = 0;
       plane_index < write_map.planeCount(); ++plane_index) {
    auto plane = write_map.plane(plane_index);
    if (!plane) {
      return Result<AudioBuffer>::Failure(plane.error());
    }
    std::memset(plane.value().data, 0, plane.value().size);
  }

  AudioBuffer output;
  auto finish = write_map.finish(output);
  if (!finish) {
    return Result<AudioBuffer>::Failure(finish.error());
  }
  return Result<AudioBuffer>::Success(std::move(output));
}

const AudioDesc &AudioBuffer::desc() const noexcept {
  return data_ ? data_->desc : kEmptyAudioDesc;
}

std::uint64_t AudioBuffer::contentId() const noexcept {
  return data_ ? data_->content_id : 0;
}

std::uint64_t AudioBuffer::generation() const noexcept {
  return data_ ? data_->generation : 0;
}

std::size_t AudioBuffer::residentBytes() const noexcept {
  return data_ && data_->buffer ? data_->buffer->capacity() : 0;
}

AudioBuffer::operator bool() const noexcept { return data_ != nullptr; }

Result<AudioReadMap> AudioBuffer::mapRead() const {
  if (!data_ || !data_->buffer) {
    return Result<AudioReadMap>::Failure(MakeFrameError(
        FrameErrorCode::ResourceClosed, "cannot map an empty audio buffer"));
  }
  return Result<AudioReadMap>::Success(
      AudioReadMap(data_->desc, data_->buffer));
}

Result<AudioWriteMap> AudioBuffer::makeWritableCopy() const {
  if (!data_ || !data_->buffer) {
    return Result<AudioWriteMap>::Failure(MakeFrameError(
        FrameErrorCode::ResourceClosed, "cannot copy an empty audio buffer"));
  }
  if (data_->generation == std::numeric_limits<std::uint64_t>::max()) {
    return Result<AudioWriteMap>::Failure(
        MakeFrameError(FrameErrorCode::Overflow, "audio generation overflow"));
  }

  auto buffer = CpuBuffer::Allocate(data_->buffer->capacity(),
                                    data_->buffer->alignment());
  if (!buffer) {
    return Result<AudioWriteMap>::Failure(buffer.error());
  }
  std::memcpy(buffer.value()->mutableData(), data_->buffer->data(),
              data_->buffer->capacity());
  return Result<AudioWriteMap>::Success(AudioWriteMap(
      data_->desc, std::move(buffer).value(), data_->generation + 1));
}

Result<AudioBuffer> AudioBuffer::Slice(std::uint32_t first_frame,
                                       std::uint32_t frame_count,
                                       std::size_t alignment) const {
  if (!data_ || !data_->buffer) {
    return Result<AudioBuffer>::Failure(MakeFrameError(
        FrameErrorCode::ResourceClosed, "cannot slice an empty audio buffer"));
  }
  if (frame_count == 0 || first_frame > data_->desc.frames ||
      frame_count > data_->desc.frames - first_frame) {
    return Result<AudioBuffer>::Failure(
        MakeFrameError(FrameErrorCode::InvalidArgument,
                       "audio slice range is outside the source buffer"));
  }

  AudioDesc output_desc = data_->desc;
  output_desc.frames = frame_count;
  const auto pts_offset = AudioFramesToTimingTicks(first_frame, data_->desc);
  if (!pts_offset) {
    return Result<AudioBuffer>::Failure(pts_offset.error());
  }
  const auto duration = AudioFramesToTimingTicks(frame_count, data_->desc);
  if (!duration) {
    return Result<AudioBuffer>::Failure(duration.error());
  }
  if (pts_offset.value() > 0 &&
      data_->desc.timing.pts >
          std::numeric_limits<std::int64_t>::max() - pts_offset.value()) {
    return Result<AudioBuffer>::Failure(
        MakeFrameError(FrameErrorCode::Overflow, "audio slice PTS overflow"));
  }
  output_desc.timing.pts += pts_offset.value();
  output_desc.timing.duration = duration.value();

  auto source_map_result = mapRead();
  if (!source_map_result) {
    return Result<AudioBuffer>::Failure(source_map_result.error());
  }
  auto write_map_result = Allocate(
      output_desc, alignment, data_->generation);
  if (!write_map_result) {
    return Result<AudioBuffer>::Failure(write_map_result.error());
  }

  auto source_map = std::move(source_map_result).value();
  auto write_map = std::move(write_map_result).value();
  const std::size_t bytes_per_sample = BytesPerSample(data_->desc.format);
  const std::size_t bytes_per_frame =
      data_->desc.planar
          ? bytes_per_sample
          : bytes_per_sample * static_cast<std::size_t>(data_->desc.channels);
  const std::size_t source_offset =
      static_cast<std::size_t>(first_frame) * bytes_per_frame;
  const std::size_t copy_size =
      static_cast<std::size_t>(frame_count) * bytes_per_frame;

  for (std::uint32_t plane_index = 0;
       plane_index < source_map.planeCount(); ++plane_index) {
    auto source_plane = source_map.plane(plane_index);
    if (!source_plane) {
      return Result<AudioBuffer>::Failure(source_plane.error());
    }
    auto output_plane = write_map.plane(plane_index);
    if (!output_plane) {
      return Result<AudioBuffer>::Failure(output_plane.error());
    }
    if (source_offset > source_plane.value().size ||
        copy_size > source_plane.value().size - source_offset ||
        copy_size > output_plane.value().size) {
      return Result<AudioBuffer>::Failure(
          MakeFrameError(FrameErrorCode::InsufficientCapacity,
                         "audio slice exceeds mapped plane capacity"));
    }
    std::memcpy(output_plane.value().data,
                source_plane.value().data + source_offset, copy_size);
  }

  AudioBuffer output;
  auto finish = write_map.finish(output);
  if (!finish) {
    return Result<AudioBuffer>::Failure(finish.error());
  }
  return Result<AudioBuffer>::Success(std::move(output));
}

Result<AudioBuffer> AudioBuffer::WithTiming(FrameTiming timing) const {
  if (!data_ || !data_->buffer) {
    return Result<AudioBuffer>::Failure(MakeFrameError(
        FrameErrorCode::ResourceClosed, "cannot retime an empty audio buffer"));
  }
  AudioDesc desc = data_->desc;
  desc.timing = timing;
  return FromCpuBuffer(std::move(desc), data_->buffer, data_->generation);
}

Result<void>
CopyAudioFrames(const AudioBuffer &source, std::uint32_t source_first_frame,
                AudioWriteMap &destination,
                std::uint32_t destination_first_frame,
                std::uint32_t frame_count) {
  if (!source || !destination) {
    return Result<void>::Failure(
        MakeFrameError(FrameErrorCode::ResourceClosed,
                       "audio copy requires readable and writable buffers"));
  }
  if (frame_count == 0) {
    return Result<void>::Success();
  }

  const auto &source_desc = source.desc();
  const auto &destination_desc = destination.desc();
  if (source_desc.format != destination_desc.format ||
      source_desc.sample_rate != destination_desc.sample_rate ||
      source_desc.channels != destination_desc.channels ||
      source_desc.channel_layout != destination_desc.channel_layout ||
      source_desc.planar != destination_desc.planar) {
    return Result<void>::Failure(
        MakeFrameError(FrameErrorCode::StorageMismatch,
                       "audio copy layouts do not match"));
  }
  if (source_first_frame > source_desc.frames ||
      frame_count > source_desc.frames - source_first_frame ||
      destination_first_frame > destination_desc.frames ||
      frame_count > destination_desc.frames - destination_first_frame) {
    return Result<void>::Failure(
        MakeFrameError(FrameErrorCode::InvalidArgument,
                       "audio copy range exceeds source or destination"));
  }

  const auto source_map_result = source.mapRead();
  if (!source_map_result) {
    return Result<void>::Failure(source_map_result.error());
  }
  const auto source_map = std::move(source_map_result).value();
  const std::size_t bytes_per_sample = BytesPerSample(source_desc.format);
  const std::size_t bytes_per_frame =
      source_desc.planar
          ? bytes_per_sample
          : bytes_per_sample * static_cast<std::size_t>(source_desc.channels);
  const std::size_t source_offset =
      static_cast<std::size_t>(source_first_frame) * bytes_per_frame;
  const std::size_t destination_offset =
      static_cast<std::size_t>(destination_first_frame) * bytes_per_frame;
  const std::size_t copy_size =
      static_cast<std::size_t>(frame_count) * bytes_per_frame;

  for (std::uint32_t plane_index = 0;
       plane_index < source_map.planeCount(); ++plane_index) {
    const auto source_plane = source_map.plane(plane_index);
    if (!source_plane) {
      return Result<void>::Failure(source_plane.error());
    }
    auto destination_plane = destination.plane(plane_index);
    if (!destination_plane) {
      return Result<void>::Failure(destination_plane.error());
    }
    if (source_offset > source_plane.value().size ||
        copy_size > source_plane.value().size - source_offset ||
        destination_offset > destination_plane.value().size ||
        copy_size >
            destination_plane.value().size - destination_offset) {
      return Result<void>::Failure(
          MakeFrameError(FrameErrorCode::InsufficientCapacity,
                         "audio copy exceeds mapped plane capacity"));
    }
    std::memcpy(destination_plane.value().data + destination_offset,
                source_plane.value().data + source_offset, copy_size);
  }
  return Result<void>::Success();
}

Result<AudioBuffer>
AudioBuffer::FromCpuBuffer(AudioDesc desc,
                           std::shared_ptr<const CpuBuffer> buffer,
                           std::uint64_t generation) {
  if (!buffer || generation == 0) {
    return Result<AudioBuffer>::Failure(MakeFrameError(
        FrameErrorCode::InvalidArgument,
        "audio buffer requires storage and non-zero generation"));
  }
  const auto valid = ValidateAudioDesc(desc);
  if (!valid) {
    return Result<AudioBuffer>::Failure(valid.error());
  }
  const auto capacity = AudioRequiredBytes(desc);
  if (!capacity) {
    return Result<AudioBuffer>::Failure(capacity.error());
  }
  if (capacity.value() > buffer->capacity()) {
    return Result<AudioBuffer>::Failure(
        MakeFrameError(FrameErrorCode::InsufficientCapacity,
                       "audio storage is smaller than AudioDesc requires"));
  }

  try {
    auto data = std::make_shared<BufferData>();
    data->desc = std::move(desc);
    data->content_id = detail::NextContentId();
    data->generation = generation;
    data->buffer = std::move(buffer);
    return Result<AudioBuffer>::Success(AudioBuffer(std::move(data)));
  } catch (const std::bad_alloc &) {
    return Result<AudioBuffer>::Failure(MakeFrameError(
        FrameErrorCode::AllocationFailed, "audio buffer allocation failed"));
  }
}

} // namespace videocut::frame
