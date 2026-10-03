#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

struct WebPAuxStats;
struct WebPPicture;
struct WebPConfig;
struct WebPAnimEncoder;

enum WebPImageHint : int { WEBP_HINT_DEFAULT = 0 };
enum WebPPreset : int { WEBP_PRESET_DEFAULT = 0 };
enum WebPEncCSP : int { WEBP_YUV420 = 0 };
enum WebPEncodingError : int { VP8_ENC_OK = 0 };

using WebPWriterFunction = int (*)(const std::uint8_t *, std::size_t,
                                    const WebPPicture *);
using WebPProgressHook = int (*)(int, const WebPPicture *);

struct WebPConfig {
  int lossless;
  float quality;
  int method;
  WebPImageHint image_hint;
  int target_size;
  float target_PSNR;
  int segments;
  int sns_strength;
  int filter_strength;
  int filter_sharpness;
  int filter_type;
  int autofilter;
  int alpha_compression;
  int alpha_filtering;
  int alpha_quality;
  int pass;
  int show_compressed;
  int preprocessing;
  int partitions;
  int partition_limit;
  int emulate_jpeg_size;
  int thread_level;
  int low_memory;
  int near_lossless;
  int exact;
  int use_delta_palette;
  int use_sharp_yuv;
  int qmin;
  int qmax;
};

struct WebPPicture {
  int use_argb;
  WebPEncCSP colorspace;
  int width;
  int height;
  std::uint8_t *y;
  std::uint8_t *u;
  std::uint8_t *v;
  int y_stride;
  int uv_stride;
  std::uint8_t *a;
  int a_stride;
  std::uint32_t pad1[2];
  std::uint32_t *argb;
  int argb_stride;
  std::uint32_t pad2[3];
  WebPWriterFunction writer;
  void *custom_ptr;
  int extra_info_type;
  std::uint8_t *extra_info;
  WebPAuxStats *stats;
  WebPEncodingError error_code;
  WebPProgressHook progress_hook;
  void *user_data;
  std::uint32_t pad3[3];
  std::uint8_t *pad4;
  std::uint8_t *pad5;
  std::uint32_t pad6[8];
  void *memory_;
  void *memory_argb_;
  void *pad7[2];
};

struct WebPData {
  const std::uint8_t *bytes;
  std::size_t size;
};

struct WebPMuxAnimParams {
  std::uint32_t bgcolor;
  int loop_count;
};

struct WebPAnimEncoderOptions {
  WebPMuxAnimParams anim_params;
  int minimize_size;
  int kmin;
  int kmax;
  int allow_mixed;
  int verbose;
  std::uint32_t padding[4];
};

void WebPFree(void *value);
int WebPConfigInitInternal(WebPConfig *config, WebPPreset preset,
                           float quality, int abi_version);
int WebPPictureInitInternal(WebPPicture *picture, int abi_version);
int WebPPictureImportRGBA(WebPPicture *picture, const std::uint8_t *rgba,
                           int rgba_stride);
void WebPPictureFree(WebPPicture *picture);
int WebPAnimEncoderOptionsInitInternal(WebPAnimEncoderOptions *options,
                                       int abi_version);
WebPAnimEncoder *WebPAnimEncoderNewInternal(
    int width, int height, const WebPAnimEncoderOptions *options,
    int abi_version);
int WebPAnimEncoderAdd(WebPAnimEncoder *encoder, WebPPicture *frame,
                       int timestamp_ms, const WebPConfig *config);
int WebPAnimEncoderAssemble(WebPAnimEncoder *encoder, WebPData *output);
const char *WebPAnimEncoderGetError(WebPAnimEncoder *encoder);
void WebPAnimEncoderDelete(WebPAnimEncoder *encoder);

} // extern "C"

inline constexpr int kWebpEncoderAbiVersion = 0x0210;
inline constexpr int kWebpMuxAbiVersion = 0x0109;
