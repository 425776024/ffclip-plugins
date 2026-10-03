#pragma once

#include "videocut/frame/FrameDesc.h"
#include "videocut/frame/FrameExport.h"
#include "videocut/frame/FrameResult.h"
#include "videocut/frame/FrameStorage.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace videocut::frame {

class VideoFrame;

struct PlaneReadView {
  const std::byte *data{nullptr};
  std::size_t size{0};
  std::uint32_t row_stride{0};
  std::uint16_t pixel_stride{0};
};

struct PlaneWriteView {
  std::byte *data{nullptr};
  std::size_t size{0};
  std::uint32_t row_stride{0};
  std::uint16_t pixel_stride{0};
};

class VIDEOCUT_FRAME_API CpuReadMap final {
public:
  CpuReadMap() = default;

  [[nodiscard]] explicit operator bool() const noexcept;
  [[nodiscard]] const FrameDesc &desc() const noexcept { return desc_; }
  [[nodiscard]] std::uint8_t planeCount() const noexcept;
  [[nodiscard]] Result<PlaneReadView> plane(std::uint8_t index) const;

private:
  CpuReadMap(FrameDesc desc, std::shared_ptr<const CpuBuffer> buffer) noexcept;

  friend class VideoFrame;

  FrameDesc desc_{};
  std::shared_ptr<const CpuBuffer> buffer_;
};

class VIDEOCUT_FRAME_API CpuWriteMap final {
public:
  CpuWriteMap() = default;
  CpuWriteMap(const CpuWriteMap &) = delete;
  CpuWriteMap &operator=(const CpuWriteMap &) = delete;
  CpuWriteMap(CpuWriteMap &&) noexcept = default;
  CpuWriteMap &operator=(CpuWriteMap &&) noexcept = default;
  ~CpuWriteMap() noexcept = default;

  [[nodiscard]] explicit operator bool() const noexcept;
  [[nodiscard]] const FrameDesc &desc() const noexcept { return desc_; }
  [[nodiscard]] std::uint8_t planeCount() const noexcept;
  [[nodiscard]] Result<PlaneWriteView> plane(std::uint8_t index);

  // Seals the mutable allocation into a new immutable VideoFrame. A write map
  // is single-use; abandoning it simply releases the uncommitted allocation.
  [[nodiscard]] Result<void> finish(VideoFrame &output);

private:
  CpuWriteMap(FrameDesc desc, std::shared_ptr<CpuBuffer> buffer,
              std::uint64_t generation) noexcept;

  friend class VideoFrame;

  FrameDesc desc_{};
  std::shared_ptr<CpuBuffer> buffer_;
  std::uint64_t generation_{0};
  bool finished_{false};
};

} // namespace videocut::frame
