#include "videocut/frame/FrameDesc.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>

namespace videocut::frame {
namespace {

struct PlaneGeometry {
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint16_t pixel_stride{0};
};

[[nodiscard]] Result<std::uint64_t>
CheckedAdd(std::uint64_t lhs, std::uint64_t rhs, const char *operation) {
  if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
    return Result<std::uint64_t>::Failure(MakeFrameError(
        FrameErrorCode::Overflow, std::string(operation) + " overflow"));
  }
  return Result<std::uint64_t>::Success(lhs + rhs);
}

[[nodiscard]] Result<std::uint64_t>
CheckedMultiply(std::uint64_t lhs, std::uint64_t rhs, const char *operation) {
  if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
    return Result<std::uint64_t>::Failure(MakeFrameError(
        FrameErrorCode::Overflow, std::string(operation) + " overflow"));
  }
  return Result<std::uint64_t>::Success(lhs * rhs);
}

[[nodiscard]] Result<std::uint64_t> AlignUp(std::uint64_t value,
                                            std::uint64_t alignment) {
  if (alignment == 0) {
    return Result<std::uint64_t>::Failure(MakeFrameError(
        FrameErrorCode::InvalidArgument, "row alignment must be non-zero"));
  }
  const auto remainder = value % alignment;
  if (remainder == 0) {
    return Result<std::uint64_t>::Success(value);
  }
  return CheckedAdd(value, alignment - remainder, "row alignment");
}

[[nodiscard]] bool IsKnownKind(FrameKind kind) noexcept {
  switch (kind) {
  case FrameKind::Video:
  case FrameKind::Image:
  case FrameKind::Mask:
    return true;
  case FrameKind::Unknown:
    return false;
  }
  return false;
}

[[nodiscard]] bool IsKnownRotation(Rotation rotation) noexcept {
  switch (rotation) {
  case Rotation::R0:
  case Rotation::R90:
  case Rotation::R180:
  case Rotation::R270:
    return true;
  }
  return false;
}

[[nodiscard]] constexpr std::uint16_t ScalarBytes(DataType type) noexcept {
  switch (type) {
  case DataType::U8:
    return 1;
  case DataType::U16:
  case DataType::F16:
    return 2;
  case DataType::F32:
    return 4;
  case DataType::Unknown:
    return 0;
  }
  return 0;
}

[[nodiscard]] constexpr bool HasPixelAlpha(PixelFormat format) noexcept {
  switch (format) {
  case PixelFormat::Bgra8:
  case PixelFormat::Rgba8:
  case PixelFormat::BgraF32:
  case PixelFormat::RgbaF32:
  case PixelFormat::RgbaPacked:
  case PixelFormat::BgraPacked:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] constexpr bool IsKnownColorModel(ColorModel model) noexcept {
  return model >= ColorModel::Unknown && model <= ColorModel::Lab;
}

[[nodiscard]] constexpr bool
IsKnownLabWhite(LabWhiteReference white) noexcept {
  return white >= LabWhiteReference::Unspecified &&
         white <= LabWhiteReference::D65;
}

[[nodiscard]] constexpr bool
IsKnownLabSystem(LabColorSystem system) noexcept {
  return system >= LabColorSystem::Unspecified &&
         system <= LabColorSystem::Cie;
}

[[nodiscard]] constexpr bool
IsKnownLinearLightScale(LinearLightScale scale) noexcept {
  return scale >= LinearLightScale::Unspecified &&
         scale <= LinearLightScale::ReferenceWhite203Nits;
}

[[nodiscard]] constexpr bool IsKnownPrimaries(ColorPrimaries value) noexcept {
  return value >= ColorPrimaries::Unknown &&
         value <= ColorPrimaries::DisplayP3;
}

[[nodiscard]] constexpr bool
IsKnownTransfer(TransferFunction value) noexcept {
  return value >= TransferFunction::Unknown && value <= TransferFunction::Hlg;
}

[[nodiscard]] constexpr bool
IsKnownMatrix(MatrixCoefficients value) noexcept {
  return value >= MatrixCoefficients::Unknown &&
         value <= MatrixCoefficients::Bt2020Ncl;
}

[[nodiscard]] constexpr bool IsKnownRange(ColorRange value) noexcept {
  return value >= ColorRange::Unspecified && value <= ColorRange::Full;
}

[[nodiscard]] constexpr bool IsKnownAlpha(AlphaMode value) noexcept {
  return value >= AlphaMode::Opaque && value <= AlphaMode::Premultiplied;
}

[[nodiscard]] Result<void> ValidateColorInfo(const FrameDesc &desc) {
  const auto &color = desc.color;
  if (!IsKnownPrimaries(color.primaries) ||
      !IsKnownTransfer(color.transfer) || !IsKnownMatrix(color.matrix) ||
      !IsKnownRange(color.range) || !IsKnownAlpha(color.alpha) ||
      !IsKnownColorModel(color.model) || !IsKnownLabWhite(color.lab_white) ||
      !IsKnownLabSystem(color.lab_system) ||
      !IsKnownLinearLightScale(color.linear_light_scale)) {
    return Result<void>::Failure(MakeFrameError(
        FrameErrorCode::InvalidDesc, "frame color metadata is out of range"));
  }

  const ColorModel expected = ColorModelForPixelFormat(desc.format);
  if (color.model != expected) {
    return Result<void>::Failure(MakeFrameError(
        FrameErrorCode::InvalidDesc,
        "frame color model does not match its pixel format"));
  }

  if (color.model != ColorModel::Lab &&
      (color.lab_white != LabWhiteReference::Unspecified ||
       color.lab_system != LabColorSystem::Unspecified)) {
    return Result<void>::Failure(MakeFrameError(
        FrameErrorCode::InvalidDesc,
        "Lab encoding metadata is only valid for Lab frames"));
  }

  if (color.linear_light_scale != LinearLightScale::Unspecified &&
      (color.transfer != TransferFunction::Linear ||
       color.primaries != ColorPrimaries::Bt2020 ||
       color.model != ColorModel::Rgb ||
       (desc.format != PixelFormat::RgbaF32 &&
        desc.format != PixelFormat::BgraF32))) {
    return Result<void>::Failure(MakeFrameError(
        FrameErrorCode::InvalidDesc,
        "reference-white linear scale requires float RGB BT.2020"));
  }

  if (!HasPixelAlpha(desc.format) && color.alpha != AlphaMode::Opaque) {
    return Result<void>::Failure(MakeFrameError(
        FrameErrorCode::InvalidDesc,
        "frames without pixel alpha must use opaque alpha mode"));
  }

  switch (color.model) {
  case ColorModel::Rgb:
  case ColorModel::Gray:
  case ColorModel::Hsl:
  case ColorModel::Hsv:
    if (color.matrix != MatrixCoefficients::Identity ||
        color.range != ColorRange::Full) {
      return Result<void>::Failure(MakeFrameError(
          FrameErrorCode::InvalidDesc,
          "RGB-derived frames require identity matrix and full range"));
    }
    break;
  case ColorModel::Alpha:
    if (color.primaries != ColorPrimaries::Unknown ||
        color.transfer != TransferFunction::Unknown ||
        color.matrix != MatrixCoefficients::Identity ||
        color.range != ColorRange::Full ||
        color.alpha != AlphaMode::Opaque) {
      return Result<void>::Failure(MakeFrameError(
          FrameErrorCode::InvalidDesc,
          "alpha planes cannot carry display color encoding"));
    }
    break;
  case ColorModel::Yuv:
    if ((color.matrix != MatrixCoefficients::Bt601 &&
         color.matrix != MatrixCoefficients::Bt709 &&
         color.matrix != MatrixCoefficients::Bt2020Ncl) ||
        (color.range != ColorRange::Limited &&
         color.range != ColorRange::Full)) {
      return Result<void>::Failure(MakeFrameError(
          FrameErrorCode::InvalidDesc,
          "YUV frames require an explicit YUV matrix and range"));
    }
    break;
  case ColorModel::Lab:
    if (color.transfer != TransferFunction::Linear ||
        color.matrix != MatrixCoefficients::Identity ||
        color.range != ColorRange::Full ||
        color.lab_white == LabWhiteReference::Unspecified ||
        color.lab_system == LabColorSystem::Unspecified) {
      return Result<void>::Failure(MakeFrameError(
          FrameErrorCode::InvalidDesc,
          "Lab frames require linear identity encoding, full range, white "
          "reference, and color-system preset"));
    }
    break;
  case ColorModel::Unknown:
    return Result<void>::Failure(MakeFrameError(
        FrameErrorCode::InvalidDesc,
        "frame color model must be explicit"));
  }
  return Result<void>::Success();
}

void FillCanonicalColorDefaults(PixelFormat format, ColorInfo &color) noexcept {
  const ColorModel model = ColorModelForPixelFormat(format);
  if (color.model == ColorModel::Unknown) color.model = model;

  if (model != ColorModel::Yuv) {
    if (color.matrix == MatrixCoefficients::Unknown) {
      color.matrix = MatrixCoefficients::Identity;
    }
    if (color.range == ColorRange::Unspecified) color.range = ColorRange::Full;
  }
  if (model == ColorModel::Lab &&
      color.transfer == TransferFunction::Unknown) {
    color.transfer = TransferFunction::Linear;
  }
}

[[nodiscard]] PlaneGeometry GeometryForPlane(PixelFormat format,
                                             DataType data_type,
                                             std::uint32_t width,
                                             std::uint32_t height,
                                             std::uint8_t plane) noexcept {
  const auto half_width =
      static_cast<std::uint32_t>((width / 2U) + (width % 2U));
  const auto half_height =
      static_cast<std::uint32_t>((height / 2U) + (height % 2U));

  const std::uint16_t scalar = ScalarBytes(data_type);
  switch (format) {
  case PixelFormat::Bgra8:
  case PixelFormat::Rgba8:
    return PlaneGeometry{width, height, 4};
  case PixelFormat::BgraF32:
  case PixelFormat::RgbaF32:
    return PlaneGeometry{width, height, 16};
  case PixelFormat::Nv12:
    return plane == 0 ? PlaneGeometry{width, height, 1}
                      : PlaneGeometry{half_width, half_height, 2};
  case PixelFormat::P010:
    return plane == 0 ? PlaneGeometry{width, height, 2}
                      : PlaneGeometry{half_width, half_height, 4};
  case PixelFormat::Yuv420p:
    return plane == 0 ? PlaneGeometry{width, height, 1}
                      : PlaneGeometry{half_width, half_height, 1};
  case PixelFormat::Alpha8:
    return PlaneGeometry{width, height, 1};
  case PixelFormat::AlphaF32:
    return PlaneGeometry{width, height, 4};
  case PixelFormat::GrayPacked:
  case PixelFormat::AlphaPacked:
    return PlaneGeometry{width, height, scalar};
  case PixelFormat::RgbPacked:
  case PixelFormat::BgrPacked:
  case PixelFormat::HslPacked:
  case PixelFormat::HsvPacked:
  case PixelFormat::LabPacked:
    return PlaneGeometry{width, height,
                         static_cast<std::uint16_t>(3U * scalar)};
  case PixelFormat::RgbaPacked:
  case PixelFormat::BgraPacked:
    return PlaneGeometry{width, height,
                         static_cast<std::uint16_t>(4U * scalar)};
  case PixelFormat::Yuv420Planar:
    return plane == 0 ? PlaneGeometry{width, height, scalar}
                      : PlaneGeometry{half_width, half_height, scalar};
  case PixelFormat::Yuv422Planar:
    return plane == 0 ? PlaneGeometry{width, height, scalar}
                      : PlaneGeometry{half_width, height, scalar};
  case PixelFormat::Yuv440Planar:
    return plane == 0 ? PlaneGeometry{width, height, scalar}
                      : PlaneGeometry{width, half_height, scalar};
  case PixelFormat::Yuv444Planar:
    return PlaneGeometry{width, height, scalar};
  case PixelFormat::Unknown:
    break;
  }
  return {};
}

[[nodiscard]] Result<void> ValidatePlane(const PlaneLayout &plane,
                                         const PlaneGeometry &geometry,
                                         std::uint8_t index) {
  if (plane.pixel_stride < geometry.pixel_stride) {
    return Result<void>::Failure(MakeFrameError(
        FrameErrorCode::InvalidDesc,
        "plane " + std::to_string(index) + " pixel stride is too small"));
  }

  const auto minimum_row =
      CheckedMultiply(geometry.width, plane.pixel_stride, "plane row size");
  if (!minimum_row) {
    return Result<void>::Failure(minimum_row.error());
  }
  if (minimum_row.value() > std::numeric_limits<std::uint32_t>::max() ||
      plane.row_stride < minimum_row.value()) {
    return Result<void>::Failure(MakeFrameError(
        FrameErrorCode::InvalidDesc,
        "plane " + std::to_string(index) + " row stride is too small"));
  }

  const auto minimum_size =
      CheckedMultiply(plane.row_stride, geometry.height, "plane byte size");
  if (!minimum_size) {
    return Result<void>::Failure(minimum_size.error());
  }
  if (plane.size < minimum_size.value()) {
    return Result<void>::Failure(MakeFrameError(
        FrameErrorCode::InvalidDesc,
        "plane " + std::to_string(index) + " byte size is too small"));
  }

  const auto end = CheckedAdd(plane.offset, plane.size, "plane range");
  if (!end) {
    return Result<void>::Failure(end.error());
  }
  return Result<void>::Success();
}

} // namespace

Result<std::int64_t> RescaleTimestamp(std::int64_t value,
                                      Rational source,
                                      Rational destination) {
  return RescaleTimestamp(value, source, destination,
                          TimestampRounding::Nearest);
}

Result<std::int64_t> RescaleTimestamp(std::int64_t value,
                                      Rational source,
                                      Rational destination,
                                      TimestampRounding rounding) {
  if (source.num <= 0 || source.den <= 0 || destination.num <= 0 ||
      destination.den <= 0) {
    return Result<std::int64_t>::Failure(MakeFrameError(
        FrameErrorCode::InvalidArgument,
        "timestamp time bases must be positive"));
  }
  const __int128 numerator =
      static_cast<__int128>(value) * source.num * destination.den;
  const __int128 denominator =
      static_cast<__int128>(source.den) * destination.num;
  __int128 rounded = numerator / denominator;
  const __int128 remainder = numerator % denominator;
  switch (rounding) {
  case TimestampRounding::Nearest:
    if (remainder != 0) {
      const __int128 magnitude =
          remainder < 0 ? -remainder : remainder;
      if (magnitude * 2 >= denominator) {
        rounded += numerator < 0 ? -1 : 1;
      }
    }
    break;
  case TimestampRounding::Down:
    if (remainder < 0) {
      --rounded;
    }
    break;
  case TimestampRounding::Up:
    if (remainder > 0) {
      ++rounded;
    }
    break;
  default:
    return Result<std::int64_t>::Failure(MakeFrameError(
        FrameErrorCode::InvalidArgument,
        "timestamp rounding mode is invalid"));
  }
  if (rounded < std::numeric_limits<std::int64_t>::min() ||
      rounded > std::numeric_limits<std::int64_t>::max()) {
    return Result<std::int64_t>::Failure(MakeFrameError(
        FrameErrorCode::Overflow, "timestamp rescale overflow"));
  }
  return Result<std::int64_t>::Success(static_cast<std::int64_t>(rounded));
}

std::uint8_t ExpectedPlaneCount(PixelFormat format) noexcept {
  switch (format) {
  case PixelFormat::Bgra8:
  case PixelFormat::Rgba8:
  case PixelFormat::BgraF32:
  case PixelFormat::RgbaF32:
  case PixelFormat::Alpha8:
  case PixelFormat::AlphaF32:
  case PixelFormat::GrayPacked:
  case PixelFormat::AlphaPacked:
  case PixelFormat::RgbPacked:
  case PixelFormat::BgrPacked:
  case PixelFormat::RgbaPacked:
  case PixelFormat::BgraPacked:
  case PixelFormat::HslPacked:
  case PixelFormat::HsvPacked:
  case PixelFormat::LabPacked:
    return 1;
  case PixelFormat::Nv12:
  case PixelFormat::P010:
    return 2;
  case PixelFormat::Yuv420p:
  case PixelFormat::Yuv420Planar:
  case PixelFormat::Yuv422Planar:
  case PixelFormat::Yuv440Planar:
  case PixelFormat::Yuv444Planar:
    return 3;
  case PixelFormat::Unknown:
    return 0;
  }
  return 0;
}

DataType PixelDataType(PixelFormat format) noexcept {
  switch (format) {
  case PixelFormat::Bgra8:
  case PixelFormat::Rgba8:
  case PixelFormat::Nv12:
  case PixelFormat::Yuv420p:
  case PixelFormat::Alpha8:
    return DataType::U8;
  case PixelFormat::BgraF32:
  case PixelFormat::RgbaF32:
    return DataType::F32;
  case PixelFormat::P010:
    return DataType::U16;
  case PixelFormat::AlphaF32:
    return DataType::F32;
  case PixelFormat::GrayPacked:
  case PixelFormat::AlphaPacked:
  case PixelFormat::RgbPacked:
  case PixelFormat::BgrPacked:
  case PixelFormat::RgbaPacked:
  case PixelFormat::BgraPacked:
  case PixelFormat::HslPacked:
  case PixelFormat::HsvPacked:
  case PixelFormat::LabPacked:
  case PixelFormat::Yuv420Planar:
  case PixelFormat::Yuv422Planar:
  case PixelFormat::Yuv440Planar:
  case PixelFormat::Yuv444Planar:
  case PixelFormat::Unknown:
    return DataType::Unknown;
  }
  return DataType::Unknown;
}

bool IsPixelDataTypeAllowed(PixelFormat format, DataType data_type) noexcept {
  const DataType fixed = PixelDataType(format);
  if (fixed != DataType::Unknown) return data_type == fixed;

  switch (format) {
  case PixelFormat::GrayPacked:
  case PixelFormat::AlphaPacked:
  case PixelFormat::RgbPacked:
  case PixelFormat::BgrPacked:
  case PixelFormat::RgbaPacked:
  case PixelFormat::BgraPacked:
  case PixelFormat::Yuv420Planar:
  case PixelFormat::Yuv422Planar:
  case PixelFormat::Yuv440Planar:
  case PixelFormat::Yuv444Planar:
    return data_type == DataType::U8 || data_type == DataType::U16 ||
           data_type == DataType::F16 || data_type == DataType::F32;
  case PixelFormat::HslPacked:
  case PixelFormat::HsvPacked:
  case PixelFormat::LabPacked:
    return data_type == DataType::F16 || data_type == DataType::F32;
  case PixelFormat::Unknown:
  case PixelFormat::Bgra8:
  case PixelFormat::Rgba8:
  case PixelFormat::BgraF32:
  case PixelFormat::RgbaF32:
  case PixelFormat::Nv12:
  case PixelFormat::P010:
  case PixelFormat::Yuv420p:
  case PixelFormat::Alpha8:
  case PixelFormat::AlphaF32:
    return false;
  }
  return false;
}

ColorModel ColorModelForPixelFormat(PixelFormat format) noexcept {
  switch (format) {
  case PixelFormat::Bgra8:
  case PixelFormat::Rgba8:
  case PixelFormat::BgraF32:
  case PixelFormat::RgbaF32:
  case PixelFormat::RgbPacked:
  case PixelFormat::BgrPacked:
  case PixelFormat::RgbaPacked:
  case PixelFormat::BgraPacked:
    return ColorModel::Rgb;
  case PixelFormat::GrayPacked:
    return ColorModel::Gray;
  case PixelFormat::Alpha8:
  case PixelFormat::AlphaF32:
  case PixelFormat::AlphaPacked:
    return ColorModel::Alpha;
  case PixelFormat::Nv12:
  case PixelFormat::P010:
  case PixelFormat::Yuv420p:
  case PixelFormat::Yuv420Planar:
  case PixelFormat::Yuv422Planar:
  case PixelFormat::Yuv440Planar:
  case PixelFormat::Yuv444Planar:
    return ColorModel::Yuv;
  case PixelFormat::HslPacked:
    return ColorModel::Hsl;
  case PixelFormat::HsvPacked:
    return ColorModel::Hsv;
  case PixelFormat::LabPacked:
    return ColorModel::Lab;
  case PixelFormat::Unknown:
    return ColorModel::Unknown;
  }
  return ColorModel::Unknown;
}

Result<void> ValidateFrameDesc(const FrameDesc &desc) {
  if (!IsKnownKind(desc.kind)) {
    return Result<void>::Failure(
        MakeFrameError(FrameErrorCode::InvalidDesc, "frame kind is unknown"));
  }
  if (desc.width == 0 || desc.height == 0) {
    return Result<void>::Failure(MakeFrameError(
        FrameErrorCode::InvalidDesc, "frame dimensions must be non-zero"));
  }

  const auto expected_planes = ExpectedPlaneCount(desc.format);
  if (expected_planes == 0) {
    return Result<void>::Failure(
        MakeFrameError(FrameErrorCode::UnsupportedFormat,
                       "pixel format is unknown or unsupported"));
  }
  if (desc.plane_count != expected_planes) {
    return Result<void>::Failure(
        MakeFrameError(FrameErrorCode::InvalidDesc,
                       "frame plane count does not match pixel format"));
  }
  if (!IsPixelDataTypeAllowed(desc.format, desc.data_type)) {
    return Result<void>::Failure(
        MakeFrameError(FrameErrorCode::InvalidDesc,
                       "frame data type does not match pixel format"));
  }
  const auto valid_color = ValidateColorInfo(desc);
  if (!valid_color) return valid_color;
  if (desc.timing.time_base.num <= 0 || desc.timing.time_base.den <= 0) {
    return Result<void>::Failure(
        MakeFrameError(FrameErrorCode::InvalidDesc,
                       "time base numerator and denominator must be positive"));
  }
  if (desc.timing.duration < 0) {
    return Result<void>::Failure(MakeFrameError(
        FrameErrorCode::InvalidDesc, "frame duration cannot be negative"));
  }
  if (!IsKnownRotation(desc.rotation)) {
    return Result<void>::Failure(MakeFrameError(FrameErrorCode::InvalidDesc,
                                                "frame rotation is invalid"));
  }

  for (std::uint8_t index = 0; index < desc.plane_count; ++index) {
    const auto geometry =
        GeometryForPlane(desc.format, desc.data_type, desc.width, desc.height,
                         index);
    const auto plane_result =
        ValidatePlane(desc.planes[index], geometry, index);
    if (!plane_result) {
      return plane_result;
    }
  }

  for (std::uint8_t index = desc.plane_count; index < kMaxFramePlanes;
       ++index) {
    const auto &plane = desc.planes[index];
    if (plane.offset != 0 || plane.size != 0 || plane.row_stride != 0 ||
        plane.pixel_stride != 0) {
      return Result<void>::Failure(
          MakeFrameError(FrameErrorCode::InvalidDesc,
                         "unused plane layout must be zero-initialized"));
    }
  }

  return Result<void>::Success();
}

Result<std::uint64_t> RequiredBufferBytes(const FrameDesc &desc) {
  const auto valid = ValidateFrameDesc(desc);
  if (!valid) {
    return Result<std::uint64_t>::Failure(valid.error());
  }

  std::uint64_t required = 0;
  for (std::uint8_t index = 0; index < desc.plane_count; ++index) {
    const auto end = CheckedAdd(desc.planes[index].offset,
                                desc.planes[index].size, "frame capacity");
    if (!end) {
      return end;
    }
    required = std::max(required, end.value());
  }
  return Result<std::uint64_t>::Success(required);
}

Result<void> ValidateFrameCapacity(const FrameDesc &desc,
                                   std::uint64_t capacity) {
  const auto required = RequiredBufferBytes(desc);
  if (!required) {
    return Result<void>::Failure(required.error());
  }
  if (capacity < required.value()) {
    return Result<void>::Failure(MakeFrameError(
        FrameErrorCode::InsufficientCapacity,
        "CPU buffer is smaller than the described plane ranges"));
  }
  return Result<void>::Success();
}

namespace {

Result<FrameDesc> MakePackedFrameDescImpl(
    FrameKind kind, PixelFormat format, DataType data_type,
    std::uint32_t width, std::uint32_t height, ColorInfo color,
    FrameTiming timing, Rotation rotation, std::uint32_t row_alignment) {
  FrameDesc desc;
  desc.kind = kind;
  desc.format = format;
  desc.data_type = data_type;
  desc.width = width;
  desc.height = height;
  desc.plane_count = ExpectedPlaneCount(format);
  FillCanonicalColorDefaults(format, color);
  desc.color = color;
  desc.timing = timing;
  desc.rotation = rotation;

  if (desc.plane_count == 0) {
    return Result<FrameDesc>::Failure(MakeFrameError(
        FrameErrorCode::UnsupportedFormat,
        "cannot make a packed description for an unknown pixel format"));
  }

  std::uint64_t next_offset = 0;
  for (std::uint8_t index = 0; index < desc.plane_count; ++index) {
    const auto geometry =
        GeometryForPlane(format, data_type, width, height, index);
    const auto raw_row = CheckedMultiply(geometry.width, geometry.pixel_stride,
                                         "packed row size");
    if (!raw_row) {
      return Result<FrameDesc>::Failure(raw_row.error());
    }
    const auto aligned_row = AlignUp(raw_row.value(), row_alignment);
    if (!aligned_row) {
      return Result<FrameDesc>::Failure(aligned_row.error());
    }
    if (aligned_row.value() > std::numeric_limits<std::uint32_t>::max()) {
      return Result<FrameDesc>::Failure(MakeFrameError(
          FrameErrorCode::Overflow, "packed row stride exceeds uint32 range"));
    }

    const auto plane_size = CheckedMultiply(
        aligned_row.value(), geometry.height, "packed plane size");
    if (!plane_size) {
      return Result<FrameDesc>::Failure(plane_size.error());
    }

    desc.planes[index] = PlaneLayout{
        next_offset,
        plane_size.value(),
        static_cast<std::uint32_t>(aligned_row.value()),
        geometry.pixel_stride,
    };

    const auto next =
        CheckedAdd(next_offset, plane_size.value(), "packed planes");
    if (!next) {
      return Result<FrameDesc>::Failure(next.error());
    }
    next_offset = next.value();
  }

  const auto valid = ValidateFrameDesc(desc);
  if (!valid) {
    return Result<FrameDesc>::Failure(valid.error());
  }
  return Result<FrameDesc>::Success(std::move(desc));
}

} // namespace

Result<FrameDesc> MakePackedFrameDesc(FrameKind kind, PixelFormat format,
                                      std::uint32_t width, std::uint32_t height,
                                      ColorInfo color, FrameTiming timing,
                                      Rotation rotation,
                                      std::uint32_t row_alignment) {
  const DataType data_type = PixelDataType(format);
  if (data_type == DataType::Unknown) {
    return Result<FrameDesc>::Failure(MakeFrameError(
        FrameErrorCode::InvalidArgument,
        "type-generic pixel formats require an explicit data type"));
  }
  return MakePackedFrameDescImpl(kind, format, data_type, width, height, color,
                                 timing, rotation, row_alignment);
}

Result<FrameDesc> MakePackedFrameDesc(
    FrameKind kind, PixelFormat format, DataType data_type,
    std::uint32_t width, std::uint32_t height, ColorInfo color,
    FrameTiming timing, Rotation rotation, std::uint32_t row_alignment) {
  return MakePackedFrameDescImpl(kind, format, data_type, width, height, color,
                                 timing, rotation, row_alignment);
}

} // namespace videocut::frame
