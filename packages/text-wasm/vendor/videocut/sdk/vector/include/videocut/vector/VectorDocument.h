#pragma once

#include "videocut/vector/VectorTimeMapping.h"
#include "videocut/vector/VectorTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace videocut::vector {

/// Sole packed-alpha movie contract. Plain video/mp4 is intentionally not a
/// Vector resource type: callers must opt into this profile and capability.
/// The merge copies associated right-half RGB and reconstructs alpha from the
/// left half in the native YUV/Metal path.
inline constexpr std::string_view kVideoCutPackedAlphaMp4MediaType =
    "video/mp4;profile=videocut-packed-alpha";
inline constexpr std::string_view kVideoCutPackedAlphaMp4Capability =
    "videocut-packed-alpha";

enum class VectorSourceKind : std::uint8_t {
  Svg = 0,
  LottieJson,
  DotLottie,
};

enum class VectorFit : std::uint8_t {
  Contain = 0,
  Cover,
  Fill,
  None,
};

struct VectorViewportPolicy final {
  VectorFit fit{VectorFit::Contain};
  float alignmentX{0.5F};
  float alignmentY{0.5F};
  bool preserveAspectRatio{true};
  float referenceWidth{0.0F};
  float referenceHeight{0.0F};
  float overscan{0.0F};
};

struct VectorProbe final {
  float intrinsicWidth{0.0F};
  float intrinsicHeight{0.0F};
  std::int64_t durationUs{0};
  double nativeFramesPerSecond{0.0};
  bool hasTransparency{true};
  std::vector<std::string> capabilitiesUsed;
};

struct VectorResource final {
  std::string logicalName;
  std::string mediaType;
  std::shared_ptr<const std::vector<std::uint8_t>> bytes;
};

struct VectorSource final {
  VectorSourceKind kind{VectorSourceKind::Svg};
  std::string assetId;
  std::string mediaType;
  std::shared_ptr<const std::vector<std::uint8_t>> bytes;
};

/// Renderer-neutral value kinds exposed by an editable vector template.
///
/// Discovery maps colors, text and supported scalar Lottie properties to
/// these kinds so the inspector can choose the matching control automatically.
enum class VectorFieldKind : std::uint8_t {
  Color = 0,
  Text,
  Number,
};

enum class VectorFieldRole : std::uint8_t {
  Fill = 0,
  Stroke,
  Text,
  Scalar,
  GradientStop,
  Font,
  FillOpacity,
  StrokeWidth,
  StrokeOpacity,
  CenterlineColor,
  CenterlineWidth,
  CenterlineOpacity,
  Roundness,
  ShadowColor,
  ShadowOpacity,
  ShadowDistance,
  ShadowAngle,
  GradientOpacity,
  GradientPosition,
  GradientAngle,
  TextureOpacity,
  SizeX,
  SizeY,
  GlobalOpacity,
};

enum class VectorFieldAnimatableComponent : std::uint8_t {
  Number = 1,
  Color = 2,
  Alpha = 3,
};

enum class VectorFieldUnit : std::uint8_t {
  Unitless = 1,
  Ratio = 2,
  Percent = 3,
  Pixels = 4,
  Degrees = 5,
};

struct VectorColor final {
  float red{0.0F};
  float green{0.0F};
  float blue{0.0F};
  float alpha{1.0F};

  bool operator==(const VectorColor &other) const noexcept {
    return red == other.red && green == other.green && blue == other.blue &&
           alpha == other.alpha;
  }
};

struct VectorFieldValue final {
  VectorFieldKind kind{VectorFieldKind::Color};
  VectorColor color{};
  std::string text;
  double number{0.0};

  bool operator==(const VectorFieldValue &other) const noexcept {
    if (kind != other.kind)
      return false;
    switch (kind) {
    case VectorFieldKind::Color:
      return color == other.color;
    case VectorFieldKind::Text:
      return text == other.text;
    case VectorFieldKind::Number:
      return number == other.number;
    }
    return false;
  }
};

/// A selector is interpreted only by the renderer profile that discovered it.
/// v1 uses `svg:<byte-offset>:<byte-length>` and `json:<RFC6901 pointer>`.
/// Keeping selectors opaque here leaves room for native SVG IDs, Lottie slots,
/// gradients and richer bindings in later renderer profiles.
struct VectorFieldTarget final {
  std::string selector;
};

struct VectorEditableField final {
  std::string id;
  std::string label;
  VectorFieldKind kind{VectorFieldKind::Color};
  VectorFieldRole role{VectorFieldRole::Fill};
  VectorFieldValue defaultValue{};
  std::optional<double> minimum;
  std::optional<double> maximum;
  std::optional<double> step;
  std::vector<VectorFieldTarget> targets;
  std::vector<VectorFieldAnimatableComponent> animatableComponents;
  VectorFieldUnit unit{VectorFieldUnit::Unitless};
  std::string animationGroupIdentity;
};

struct VectorFieldOverride final {
  std::string fieldId;
  VectorFieldValue value{};
};

struct VectorDocument final {
  std::uint32_t version{1};
  std::string documentId;
  VectorSource source;
  VectorProbe probe;
  std::vector<VectorResource> resources;
  VectorTimeMapping timeMapping;
  VectorViewportPolicy viewport;
  std::string rendererProfile{"videocut-skia-vector-v1"};
  std::vector<VectorEditableField> editableFields;
  std::vector<VectorFieldOverride> overrides;
};

struct VectorLimits final {
  std::size_t maximumSourceBytes{16U * 1024U * 1024U};
  std::size_t maximumResourceBytes{64U * 1024U * 1024U};
  std::size_t maximumResources{256};
  std::size_t maximumJsonDepth{128};
  std::size_t maximumJsonNodes{1'000'000};
  std::size_t maximumXmlElements{200'000};
  std::size_t maximumXmlAttributes{1'000'000};
  std::size_t maximumXmlDepth{128};
  std::size_t maximumSvgContainerDepth{32};
  std::size_t maximumSvgPathCommands{1'000'000};
  std::size_t maximumEmbeddedDataUriBytes{16U * 1024U * 1024U};
  std::size_t maximumLottieLayers{10'000};
  std::size_t maximumLottieShapes{200'000};
  std::size_t maximumLottieMasks{100'000};
  std::size_t maximumLottieEffects{100'000};
  std::size_t maximumLottieKeyframes{1'000'000};
  std::size_t maximumLottieImageAssets{4096};
  std::size_t maximumLottieImageFrames{1024};
  std::uint64_t maximumDecodedPixelFrames{268'435'456};
  std::size_t maximumLottieFontFaces{64};
  std::size_t maximumLottieGlyphs{200'000};
  std::size_t maximumStringBytes{1U * 1024U * 1024U};
  std::size_t maximumEditableFields{128};
  std::size_t maximumFieldTargets{1024};
  std::size_t maximumEditableTextBytes{64U * 1024U};
  std::size_t maximumFontFamilyBytes{256};
  std::size_t maximumFieldIdBytes{128};
  std::size_t maximumFieldLabelBytes{256};
  std::size_t maximumSelectorBytes{4096};
  std::uint32_t maximumDimension{16'384};
  std::uint64_t maximumPixels{67'108'864};
  float maximumOverscan{4096.0F};
  std::int64_t maximumDurationUs{86'400'000'000};
  double maximumFramesPerSecond{1000.0};
};

struct VectorValidationResult final {
  bool valid{false};
  std::vector<Diagnostic> diagnostics;
};

VectorValidationResult ValidateVectorDocument(const VectorDocument &document,
                                              const VectorLimits &limits = {});

} // namespace videocut::vector
