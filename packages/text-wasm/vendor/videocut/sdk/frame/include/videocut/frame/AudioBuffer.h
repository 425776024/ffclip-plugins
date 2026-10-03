#pragma once

#include "videocut/frame/FrameDesc.h"
#include "videocut/frame/FrameExport.h"
#include "videocut/frame/FrameResult.h"
#include "videocut/frame/FrameStorage.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace videocut::frame {

enum class SampleFormat : std::uint8_t {
  Unknown = 0,
  S16,
  S32,
  F32,
  F64,
};

struct AudioDesc {
  SampleFormat format{SampleFormat::Unknown};
  std::uint32_t sample_rate{0};
  std::uint32_t channels{0};
  std::uint64_t channel_layout{0};
  std::uint32_t frames{0};
  bool planar{false};
  FrameTiming timing{};
};

[[nodiscard]] VIDEOCUT_FRAME_API std::uint8_t
BytesPerSample(SampleFormat format) noexcept;

[[nodiscard]] VIDEOCUT_FRAME_API std::uint64_t
DefaultAudioChannelLayout(std::uint32_t channels) noexcept;

[[nodiscard]] VIDEOCUT_FRAME_API bool
AudioChannelLayoutMatches(std::uint64_t layout,
                          std::uint32_t channels) noexcept;

[[nodiscard]] VIDEOCUT_FRAME_API Result<void>
ValidateAudioDesc(const AudioDesc &desc);

[[nodiscard]] VIDEOCUT_FRAME_API Result<std::uint64_t>
AudioRequiredBytes(const AudioDesc &desc);

struct AudioPlaneReadView {
  const std::byte *data{nullptr};
  std::size_t size{0};
  std::uint32_t frame_stride{0};
  std::uint8_t sample_stride{0};
};

struct AudioPlaneWriteView {
  std::byte *data{nullptr};
  std::size_t size{0};
  std::uint32_t frame_stride{0};
  std::uint8_t sample_stride{0};
};

class AudioBuffer;

class VIDEOCUT_FRAME_API AudioReadMap final {
public:
  AudioReadMap() = default;

  [[nodiscard]] explicit operator bool() const noexcept;
  [[nodiscard]] const AudioDesc &desc() const noexcept { return desc_; }
  [[nodiscard]] std::uint32_t planeCount() const noexcept;
  [[nodiscard]] Result<AudioPlaneReadView> plane(std::uint32_t index) const;

private:
  AudioReadMap(AudioDesc desc,
               std::shared_ptr<const CpuBuffer> buffer) noexcept;

  friend class AudioBuffer;

  AudioDesc desc_{};
  std::shared_ptr<const CpuBuffer> buffer_;
};

class VIDEOCUT_FRAME_API AudioWriteMap final {
public:
  AudioWriteMap() = default;
  AudioWriteMap(const AudioWriteMap &) = delete;
  AudioWriteMap &operator=(const AudioWriteMap &) = delete;
  AudioWriteMap(AudioWriteMap &&) noexcept = default;
  AudioWriteMap &operator=(AudioWriteMap &&) noexcept = default;
  ~AudioWriteMap() noexcept = default;

  [[nodiscard]] explicit operator bool() const noexcept;
  [[nodiscard]] const AudioDesc &desc() const noexcept { return desc_; }
  [[nodiscard]] std::uint32_t planeCount() const noexcept;
  [[nodiscard]] Result<AudioPlaneWriteView> plane(std::uint32_t index);
  [[nodiscard]] Result<void> finish(AudioBuffer &output);

private:
  AudioWriteMap(AudioDesc desc, std::shared_ptr<CpuBuffer> buffer,
                std::uint64_t generation) noexcept;

  friend class AudioBuffer;

  AudioDesc desc_{};
  std::shared_ptr<CpuBuffer> buffer_;
  std::uint64_t generation_{0};
  bool finished_{false};
};

class VIDEOCUT_FRAME_API AudioBuffer final {
public:
  AudioBuffer() = default;

  [[nodiscard]] static Result<AudioWriteMap>
  Allocate(const AudioDesc &desc, std::size_t alignment = 64,
           std::uint64_t generation = 1);

  [[nodiscard]] static Result<AudioBuffer>
  Silence(const AudioDesc &desc, std::size_t alignment = 64);

  [[nodiscard]] const AudioDesc &desc() const noexcept;
  [[nodiscard]] std::uint64_t contentId() const noexcept;
  [[nodiscard]] std::uint64_t generation() const noexcept;
  [[nodiscard]] std::size_t residentBytes() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept;

  [[nodiscard]] Result<AudioReadMap> mapRead() const;
  [[nodiscard]] Result<AudioWriteMap> makeWritableCopy() const;
  [[nodiscard]] Result<AudioBuffer>
  Slice(std::uint32_t first_frame, std::uint32_t frame_count,
        std::size_t alignment = 64) const;
  [[nodiscard]] Result<AudioBuffer>
  WithTiming(FrameTiming timing) const;

private:
  struct BufferData;

  explicit AudioBuffer(std::shared_ptr<const BufferData> data) noexcept;

  [[nodiscard]] static Result<AudioBuffer>
  FromCpuBuffer(AudioDesc desc, std::shared_ptr<const CpuBuffer> buffer,
                std::uint64_t generation);

  friend class AudioWriteMap;

  std::shared_ptr<const BufferData> data_;
};

[[nodiscard]] VIDEOCUT_FRAME_API Result<void>
CopyAudioFrames(const AudioBuffer &source, std::uint32_t source_first_frame,
                AudioWriteMap &destination,
                std::uint32_t destination_first_frame,
                std::uint32_t frame_count);

} // namespace videocut::frame
