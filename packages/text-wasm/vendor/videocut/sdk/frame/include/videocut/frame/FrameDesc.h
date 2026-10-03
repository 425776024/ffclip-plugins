#pragma once

#include "videocut/frame/FrameExport.h"
#include "videocut/frame/FrameResult.h"
#include "videocut/frame/FrameTypes.h"

#include <array>
#include <cstdint>

namespace videocut::frame {

inline constexpr std::uint8_t kMaxFramePlanes = 4;

struct PlaneLayout {
  std::uint64_t offset{0};
  std::uint64_t size{0};
  std::uint32_t row_stride{0};
  std::uint16_t pixel_stride{0};
};

struct Rational {
  std::int32_t num{1};
  std::int32_t den{1};
};

struct FrameTiming {
  std::int64_t pts{0};
  std::int64_t duration{0};
  Rational time_base{};
};

enum class TimestampRounding : std::uint8_t {
  Nearest = 0,
  Down,
  Up,
};

[[nodiscard]] VIDEOCUT_FRAME_API Result<std::int64_t>
RescaleTimestamp(std::int64_t value, Rational source,
                 Rational destination);

[[nodiscard]] VIDEOCUT_FRAME_API Result<std::int64_t>
RescaleTimestamp(std::int64_t value, Rational source,
                 Rational destination, TimestampRounding rounding);

struct ColorInfo {
  ColorPrimaries primaries{ColorPrimaries::Unknown};
  TransferFunction transfer{TransferFunction::Unknown};
  MatrixCoefficients matrix{MatrixCoefficients::Unknown};
  ColorRange range{ColorRange::Unspecified};
  AlphaMode alpha{AlphaMode::Opaque};
  ColorModel model{ColorModel::Unknown};
  LabWhiteReference lab_white{LabWhiteReference::Unspecified};
  LabColorSystem lab_system{LabColorSystem::Unspecified};
  LinearLightScale linear_light_scale{LinearLightScale::Unspecified};
};

struct FrameDesc {
  FrameKind kind{FrameKind::Unknown};
  PixelFormat format{PixelFormat::Unknown};
  DataType data_type{DataType::Unknown};
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint8_t plane_count{0};
  std::array<PlaneLayout, kMaxFramePlanes> planes{};
  ColorInfo color{};
  FrameTiming timing{};
  Rotation rotation{Rotation::R0};
};

[[nodiscard]] VIDEOCUT_FRAME_API std::uint8_t
ExpectedPlaneCount(PixelFormat format) noexcept;

[[nodiscard]] VIDEOCUT_FRAME_API DataType PixelDataType(PixelFormat format) noexcept;

[[nodiscard]] VIDEOCUT_FRAME_API bool
IsPixelDataTypeAllowed(PixelFormat format, DataType data_type) noexcept;

[[nodiscard]] VIDEOCUT_FRAME_API ColorModel
ColorModelForPixelFormat(PixelFormat format) noexcept;

[[nodiscard]] VIDEOCUT_FRAME_API Result<void>
ValidateFrameDesc(const FrameDesc &desc);

[[nodiscard]] VIDEOCUT_FRAME_API Result<std::uint64_t>
RequiredBufferBytes(const FrameDesc &desc);

[[nodiscard]] VIDEOCUT_FRAME_API Result<void>
ValidateFrameCapacity(const FrameDesc &desc, std::uint64_t capacity);

[[nodiscard]] VIDEOCUT_FRAME_API Result<FrameDesc>
MakePackedFrameDesc(FrameKind kind, PixelFormat format, std::uint32_t width,
                    std::uint32_t height, ColorInfo color = {},
                    FrameTiming timing = {}, Rotation rotation = Rotation::R0,
                    std::uint32_t row_alignment = 1);

[[nodiscard]] VIDEOCUT_FRAME_API Result<FrameDesc>
MakePackedFrameDesc(FrameKind kind, PixelFormat format, DataType data_type,
                    std::uint32_t width, std::uint32_t height,
                    ColorInfo color = {}, FrameTiming timing = {},
                    Rotation rotation = Rotation::R0,
                    std::uint32_t row_alignment = 1);

} // namespace videocut::frame
