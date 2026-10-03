#pragma once

#include <cstdint>

namespace videocut::frame {

enum class FrameKind : std::uint8_t {
  Unknown = 0,
  Video,
  Image,
  Mask,
};

enum class PixelFormat : std::uint16_t {
  Unknown = 0,
  Bgra8 = 1,
  Rgba8 = 2,
  BgraF32 = 3,
  RgbaF32 = 4,
  Nv12 = 5,
  P010 = 6,
  Yuv420p = 7,
  Alpha8 = 8,
  AlphaF32 = 9,

  // Type-generic, canonical image layouts. DataType selects the scalar
  // representation; these formats deliberately do not describe arbitrary
  // compute-buffer layout details.
  GrayPacked = 10,
  AlphaPacked = 11,
  RgbPacked = 12,
  BgrPacked = 13,
  RgbaPacked = 14,
  BgraPacked = 15,
  HslPacked = 16,
  HsvPacked = 17,
  LabPacked = 18,
  Yuv420Planar = 19,
  Yuv422Planar = 20,
  Yuv440Planar = 21,
  Yuv444Planar = 22,
};

enum class DataType : std::uint8_t {
  Unknown = 0,
  U8,
  U16,
  F16,
  F32,
};

enum class ColorPrimaries : std::uint8_t {
  Unknown = 0,
  Bt601,
  Bt709,
  Bt2020,
  DisplayP3,
};

enum class TransferFunction : std::uint8_t {
  Unknown = 0,
  Linear,
  Srgb,
  Bt709,
  Pq,
  Hlg,
};

/// Numeric scale of RGB samples whose transfer function is Linear.
/// ReferenceWhite203Nits is the maintained BT.2100 working convention used
/// by HDR composition; 1.0 represents 203 cd/m² and values above 1.0 retain
/// highlight headroom.
enum class LinearLightScale : std::uint8_t {
  Unspecified = 0,
  ReferenceWhite203Nits,
};

enum class MatrixCoefficients : std::uint8_t {
  Unknown = 0,
  Identity,
  Bt601,
  Bt709,
  Bt2020Ncl,
};

enum class ColorRange : std::uint8_t {
  Unspecified = 0,
  Limited,
  Full,
};

enum class AlphaMode : std::uint8_t {
  Opaque = 0,
  Straight,
  Premultiplied,
};

enum class ColorModel : std::uint8_t {
  Unknown = 0,
  Rgb = 1,
  Gray = 2,
  Alpha = 3,
  Yuv = 4,
  Hsl = 5,
  Hsv = 6,
  Lab = 7,
};

enum class LabWhiteReference : std::uint8_t {
  Unspecified = 0,
  D50 = 1,
  D65 = 2,
};

// Matrix presets used by the maintained RGB <-> Lab conversion path. These
// values describe the conversion encoding, not a display ICC profile.
enum class LabColorSystem : std::uint8_t {
  Unspecified = 0,
  Srgb = 1,
  AdobeRgb = 2,
  AppleRgb = 3,
  BruceRgb = 4,
  Pal = 5,
  Ntsc = 6,
  Smpte = 7,
  Cie = 8,
};

// Counter-clockwise display rotation applied to the stored pixel orientation.
enum class Rotation : std::uint8_t {
  R0 = 0,
  R90,
  R180,
  R270,
};

enum class StorageKind : std::uint8_t {
  Empty = 0,
  Cpu,
  Gpu,
  Surface,
};

enum class GpuBackend : std::uint8_t {
  Unknown = 0,
  Vulkan,
  Metal,
  Other,
};

enum class SurfaceRole : std::uint8_t {
  Unknown = 0,
  DecoderNv12,
  DecoderP010,
  DecoderBgra,
  PreviewBgra,
  EncoderNv12,
  EncoderP010,
};

enum class NativeSurfaceApi : std::uint8_t {
  Unknown = 0,
  CoreVideoPixelBuffer,
  IOSurface,
  MetalTexture,
  D3D11Texture2D,
  D3D12Resource,
  Win32SharedHandle,
  DmaBuf,
};

} // namespace videocut::frame
