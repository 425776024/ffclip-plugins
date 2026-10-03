#include "internal/Factories.h"

#include "internal/skia/SkiaHeaders.h"
#include "raster/SkiaFramePublication.h"
#include "text/SkiaGpuContext.h"
#include "resources/SkiaResourceProvider.h"
#include "videocut/vector/VectorCustomization.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <exception>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace videocut::skia_runtime::internal {
namespace {

constexpr const char *kVectorProfile = "videocut-skia-vector-v1";
constexpr int kLottieSupersampleScale = 2;
constexpr int kLottieRasterApron = 2;
constexpr std::string_view kQtFollowerDirectConverter{
    "packed-alpha-associated-direct-frame-lottie"};

using videocut::vector::Diagnostic;
using videocut::vector::DiagnosticSeverity;

Diagnostic MakeDiagnostic(std::string code, DiagnosticSeverity severity,
                          std::string stage, std::string subject,
                          std::string message) {
  return {
      std::move(code),    severity,           std::move(stage),
      std::move(subject), std::move(message),
  };
}

bool IsCanceled(const vector::CancelCheck &cancel) noexcept {
  if (!cancel)
    return false;
  try {
    return cancel();
  } catch (...) {
    // Cancellation is an external lifecycle fence. A faulty callback must
    // fail closed instead of escaping Render or terminating a noexcept path.
    return true;
  }
}

enum class RenderAbortReason : std::uint8_t {
  None = 0,
  Canceled,
  DeadlineExceeded,
};

RenderAbortReason
AbortReason(const vector::VectorRenderRequest &request) noexcept {
  if (IsCanceled(request.cancel))
    return RenderAbortReason::Canceled;
  if (request.deadline && std::chrono::steady_clock::now() >= *request.deadline)
    return RenderAbortReason::DeadlineExceeded;
  return RenderAbortReason::None;
}

void ApplyAbort(vector::VectorRenderResult &result,
                const RenderAbortReason reason, std::string subject,
                std::string stage) {
  result.image = {};
  result.originX = 0;
  result.originY = 0;
  result.rasterToOutputScaleX = 1.0F;
  result.rasterToOutputScaleY = 1.0F;
  result.logicalBounds = {};
  result.authoredLocalBounds = {};
  result.authoredLocalBoundsValid = false;
  result.inkBounds = {};
  result.inkBoundsValid = false;
  result.sampledPhase = vector::VectorAnimationPhase::Intrinsic;
  result.sampledPhaseLocalUs = 0;
  result.sampledSourceUs = 0;
  if (reason == RenderAbortReason::DeadlineExceeded) {
    result.status = vector::VectorStatusCode::DeadlineExceeded;
    result.diagnostics.clear();
    result.diagnostics.push_back(MakeDiagnostic(
        "vector.render.deadline_exceeded", DiagnosticSeverity::Error,
        std::move(stage), std::move(subject),
        "vector render exceeded its publication deadline"));
    return;
  }
  result.status = vector::VectorStatusCode::Canceled;
}

class LottieLogger final : public skottie::Logger {
public:
  explicit LottieLogger(std::size_t maximum) : maximum_(maximum) {}

  void log(Level level, const char message[], const char *) override {
    if (diagnostics_.size() >= maximum_)
      return;
    diagnostics_.push_back(MakeDiagnostic(
        level == Level::kError ? "vector.lottie.parse_error"
                               : "vector.lottie.parse_warning",
        level == Level::kError ? DiagnosticSeverity::Error
                               : DiagnosticSeverity::Warning,
        "parse", {}, message ? message : "Skottie parser diagnostic"));
  }

  const std::vector<Diagnostic> &diagnostics() const noexcept {
    return diagnostics_;
  }

private:
  std::size_t maximum_{0};
  std::vector<Diagnostic> diagnostics_;
};

using Json = nlohmann::json;

enum class LottieSlotValueKind : std::uint8_t {
  Scalar,
  Color,
  Vec2,
};

struct LottieSlotFieldTarget final {
  std::string fieldId;
  std::optional<std::size_t> vectorComponent;
};

struct LottieSlotBinding final {
  std::string slotId;
  LottieSlotValueKind kind{LottieSlotValueKind::Scalar};
  std::vector<LottieSlotFieldTarget> targets;
  float baselineScalar{0.0F};
  SkColor baselineColor{SK_ColorTRANSPARENT};
  SkV2 baselineVec2{0.0F, 0.0F};
};

struct SvgBindingSpec final {
  std::string fieldId;
  std::string nodeId;
  std::string attribute;
};

struct SvgAttributeBinding final {
  std::string fieldId;
  sk_sp<SkSVGNode> node;
  std::string attribute;
};

struct LottieObserverRoute final {
  std::vector<std::string> colorFieldIds;
  std::vector<std::string> opacityFieldIds;
  std::vector<std::string> textFillFieldIds;
  std::vector<std::string> textStrokeFieldIds;
};

struct LottieColorHandleBinding final {
  std::vector<std::string> fieldIds;
  std::unique_ptr<skottie::ColorPropertyHandle> handle;
  SkColor baseline{SK_ColorTRANSPARENT};
};

struct LottieOpacityHandleBinding final {
  std::vector<std::string> fieldIds;
  std::unique_ptr<skottie::OpacityPropertyHandle> handle;
  float baseline{1.0F};
};

struct LottieTextHandleBinding final {
  std::vector<std::string> fillFieldIds;
  std::vector<std::string> strokeFieldIds;
  std::unique_ptr<skottie::TextPropertyHandle> handle;
  skottie::TextPropertyValue baseline;
};

struct CompiledVectorAnimationBindings final {
  std::vector<LottieSlotBinding> lottieSlots;
  std::vector<SvgBindingSpec> svgSpecs;
  std::vector<SvgAttributeBinding> svgAttributes;
  std::unordered_map<std::string, LottieObserverRoute> observerRoutes;
  std::vector<LottieColorHandleBinding> colorHandles;
  std::vector<LottieOpacityHandleBinding> opacityHandles;
  std::vector<LottieTextHandleBinding> textHandles;
  std::unordered_map<std::string, std::string> unsupportedFields;
  sk_sp<skottie::SlotManager> slotManager;
};

class VectorPropertyObserver final : public skottie::PropertyObserver {
public:
  explicit VectorPropertyObserver(CompiledVectorAnimationBindings &bindings)
      : bindings_(bindings) {}

  void onColorProperty(
      const char nodeName[],
      const LazyHandle<skottie::ColorPropertyHandle> &lazyHandle) override {
    const auto route = FindRoute(nodeName);
    if (!route || route->colorFieldIds.empty())
      return;
    auto handle = lazyHandle();
    if (!handle)
      return;
    const auto baseline = handle->get();
    bindings_.colorHandles.push_back(
        {route->colorFieldIds, std::move(handle), baseline});
  }

  void onOpacityProperty(
      const char nodeName[],
      const LazyHandle<skottie::OpacityPropertyHandle> &lazyHandle) override {
    const auto route = FindRoute(nodeName);
    if (!route || route->opacityFieldIds.empty())
      return;
    auto handle = lazyHandle();
    if (!handle)
      return;
    const auto baseline = handle->get();
    bindings_.opacityHandles.push_back(
        {route->opacityFieldIds, std::move(handle), baseline});
  }

  void onTextProperty(
      const char nodeName[],
      const LazyHandle<skottie::TextPropertyHandle> &lazyHandle) override {
    const auto route = FindRoute(nodeName);
    if (!route ||
        (route->textFillFieldIds.empty() && route->textStrokeFieldIds.empty()))
      return;
    auto handle = lazyHandle();
    if (!handle)
      return;
    auto baseline = handle->get();
    bindings_.textHandles.push_back({route->textFillFieldIds,
                                     route->textStrokeFieldIds,
                                     std::move(handle), std::move(baseline)});
  }

private:
  const LottieObserverRoute *FindRoute(const char nodeName[]) const noexcept {
    if (!nodeName)
      return nullptr;
    const auto found = bindings_.observerRoutes.find(nodeName);
    return found == bindings_.observerRoutes.end() ? nullptr : &found->second;
  }

  CompiledVectorAnimationBindings &bindings_;
};

struct ParsedDocument final {
  struct QtFollowerDirectFrameAdmission final {
    std::uint32_t contractVersion{1U};
    std::string resourceLogicalId;
    std::uint32_t renderSize{0U};
    std::uint32_t packedWidth{0U};
    std::uint64_t frameCount{0U};
    std::int64_t durationUs{0};
    double framesPerSecond{0.0};
  };

  vector::VectorDocument document;
  sk_sp<ImmutableResourceProvider> baseResources;
  sk_sp<skresources::ResourceProvider> resources;
  sk_sp<SkFontMgr> fontManager;
  sk_sp<SkSVGDOM> svg;
  sk_sp<skottie::Animation> lottie;
  float sourceWidth{0.0F};
  float sourceHeight{0.0F};
  std::int64_t durationUs{0};
  double framesPerSecond{0.0};
  std::vector<Diagnostic> diagnostics;
  CompiledVectorAnimationBindings animationBindings;
  std::optional<QtFollowerDirectFrameAdmission> qtFollowerDirectFrame;
};

bool JsonNumberEquals(const nlohmann::json &value,
                      const double expected) noexcept {
  if (!value.is_number())
    return false;
  const double actual = value.get<double>();
  return std::isfinite(actual) && actual == expected;
}

bool JsonVec3Equals(const nlohmann::json &value, const double x,
                    const double y, const double z) noexcept {
  return value.is_array() && value.size() == 3U &&
         JsonNumberEquals(value[0], x) && JsonNumberEquals(value[1], y) &&
         JsonNumberEquals(value[2], z);
}

bool JsonStaticPropertyEquals(const nlohmann::json &value,
                              const nlohmann::json &expected) noexcept {
  return value.is_object() && value.value("a", -1) == 0 &&
         value.contains("k") && value["k"] == expected;
}

bool AdmitPackedAlphaFollowerDirectFrame(
    const vector::VectorDocument &document,
    std::optional<ParsedDocument::QtFollowerDirectFrameAdmission> &admission,
    std::string &error) {
  admission.reset();
  if (document.source.kind != vector::VectorSourceKind::LottieJson ||
      !document.source.bytes || document.source.bytes->empty()) {
    return true;
  }
  try {
    const auto root = nlohmann::json::parse(document.source.bytes->begin(),
                                            document.source.bytes->end(),
                                            nullptr, false);
    if (root.is_discarded() || !root.is_object())
      return true; // The ordinary Skottie parser owns generic diagnostics.
    const auto meta = root.find("meta");
    if (meta == root.end() || !meta->is_object())
      return true;
    const auto follower = meta->find("videocut_packed_alpha");
    if (follower == meta->end() || !follower->is_object())
      return true;
    const auto runtime = follower->find("runtime");
    if (runtime == follower->end() || !runtime->is_string() ||
        runtime->get<std::string>() != kQtFollowerDirectConverter) {
      error = "packed-alpha follower runtime identity is invalid";
      return false;
    }

    const auto reject = [&](std::string message) {
      error = "packed-alpha follower direct-frame admission failed: " +
              std::move(message);
      return false;
    };
    // Product loading deterministically discovers editable Lottie properties
    // before the lane sees the document. Those declarations are metadata and
    // do not mutate this strictly validated identity graph. Authored values
    // remain forbidden here, and Render separately rejects every non-empty
    // runtime override set before taking the direct publication path.
    if (!document.overrides.empty())
      return reject("identity follower cannot carry authored overrides");
    const auto capability = std::find(
        document.probe.capabilitiesUsed.begin(),
        document.probe.capabilitiesUsed.end(), kQtPackedAlphaMp4Capability);
    if (capability == document.probe.capabilitiesUsed.end())
      return reject("the packed-alpha capability is absent");

    const auto unsignedField = [&](const char *name,
                                   std::uint64_t &output) {
      const auto found = follower->find(name);
      if (found == follower->end() || !found->is_number_unsigned())
        return false;
      output = found->get<std::uint64_t>();
      return true;
    };
    std::uint64_t packedWidth = 0U;
    std::uint64_t renderSize = 0U;
    std::uint64_t frameCount = 0U;
    std::uint64_t durationUs = 0U;
    std::uint64_t sourceByteLength = 0U;
    if (!unsignedField("packed_width", packedWidth) ||
        !unsignedField("render_size", renderSize) ||
        !unsignedField("frame_count", frameCount) ||
        !unsignedField("duration_us", durationUs) ||
        !unsignedField("byte_length", sourceByteLength) ||
        renderSize == 0U ||
        renderSize > std::numeric_limits<std::uint32_t>::max() ||
        packedWidth != renderSize * 2U || frameCount == 0U ||
        frameCount > 9'007'199'254'740'991ULL ||
        durationUs >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return reject("numeric metadata is incomplete or inconsistent");
    }
    const auto fpsValue = follower->find("fps");
    const auto mediaType = follower->find("media_type");
    const auto packing = follower->find("packing");
    if (fpsValue == follower->end() || !fpsValue->is_number() ||
        !std::isfinite(fpsValue->get<double>()) ||
        fpsValue->get<double>() <= 0.0 || mediaType == follower->end() ||
        !mediaType->is_string() ||
        mediaType->get<std::string>() != kQtPackedAlphaMp4ResourceMediaType ||
        packing == follower->end() || !packing->is_string() ||
        packing->get<std::string>() != "alpha-left-color-right") {
      return reject("media, packing, or frame-rate metadata is invalid");
    }
    const double fps = fpsValue->get<double>();
    const auto expectedDuration = static_cast<std::int64_t>(
        std::llround(static_cast<double>(frameCount) / fps * 1'000'000.0));
    if (expectedDuration != static_cast<std::int64_t>(durationUs))
      return reject("duration does not equal frame_count/fps");

    if (!root.contains("w") || !root.contains("h") ||
        !root.contains("fr") || !root.contains("ip") ||
        !root.contains("op") || !JsonNumberEquals(root["w"], renderSize) ||
        !JsonNumberEquals(root["h"], renderSize) ||
        !JsonNumberEquals(root["fr"], fps) ||
        !JsonNumberEquals(root["ip"], 0.0) ||
        !JsonNumberEquals(root["op"], frameCount) ||
        root.value("ddd", -1) != 0) {
      return reject("Lottie root is not the admitted identity canvas");
    }
    const auto assets = root.find("assets");
    const auto layers = root.find("layers");
    if (assets == root.end() || !assets->is_array() || assets->size() != 1U ||
        layers == root.end() || !layers->is_array() || layers->size() != 1U)
      return reject("identity follower requires exactly one asset and layer");
    const auto &asset = (*assets)[0];
    const auto &layer = (*layers)[0];
    if (!asset.is_object() || !layer.is_object() ||
        asset.value("id", std::string{}) != "image_0" ||
        asset.value("u", std::string{"invalid"}) != "" ||
        asset.value("e", -1) != 0 ||
        !asset.contains("w") || !asset.contains("h") ||
        !JsonNumberEquals(asset["w"], renderSize) ||
        !JsonNumberEquals(asset["h"], renderSize)) {
      return reject("image asset is not an unscaled logical-size source");
    }
    const std::string logicalId = asset.value("p", std::string{});
    if (logicalId.empty() || layer.value("ty", -1) != 2 ||
        layer.value("refId", std::string{}) != "image_0" ||
        layer.value("sr", 0.0) != 1.0 || layer.value("bm", -1) != 0 ||
        !layer.contains("ip") || !layer.contains("op") ||
        !layer.contains("st") || !JsonNumberEquals(layer["ip"], 0.0) ||
        !JsonNumberEquals(layer["op"], frameCount) ||
        !JsonNumberEquals(layer["st"], 0.0)) {
      return reject("image layer timing or blend mode is not identity");
    }
    const auto ks = layer.find("ks");
    if (ks == layer.end() || !ks->is_object() ||
        !ks->contains("o") || !ks->contains("r") || !ks->contains("p") ||
        !ks->contains("a") || !ks->contains("s") ||
        !JsonStaticPropertyEquals((*ks)["o"], 100) ||
        !JsonStaticPropertyEquals((*ks)["r"], 0) ||
        !JsonStaticPropertyEquals(
            (*ks)["p"], nlohmann::json::array(
                            {renderSize / 2.0, renderSize / 2.0, 0.0})) ||
        !JsonStaticPropertyEquals(
            (*ks)["a"], nlohmann::json::array(
                            {renderSize / 2.0, renderSize / 2.0, 0.0})) ||
        !JsonStaticPropertyEquals((*ks)["s"],
                                  nlohmann::json::array({100, 100, 100}))) {
      return reject("image-layer transform is not exact identity");
    }

    const auto resource = std::find_if(
        document.resources.begin(), document.resources.end(),
        [&](const vector::VectorResource &candidate) {
          return candidate.logicalName == logicalId;
        });
    if (resource == document.resources.end() ||
        resource->mediaType != kQtPackedAlphaMp4ResourceMediaType ||
        !resource->bytes || resource->bytes->size() != sourceByteLength)
      return reject("immutable resource identity/extent does not close");
    if (std::count_if(document.resources.begin(), document.resources.end(),
                      [&](const vector::VectorResource &candidate) {
                        return candidate.mediaType ==
                               kQtPackedAlphaMp4ResourceMediaType;
                      }) != 1) {
      return reject("direct follower requires exactly one packed resource");
    }
    if (document.probe.intrinsicWidth != static_cast<float>(renderSize) ||
        document.probe.intrinsicHeight != static_cast<float>(renderSize) ||
        document.probe.durationUs != static_cast<std::int64_t>(durationUs) ||
        document.probe.nativeFramesPerSecond != fps) {
      return reject("VectorDocument probe disagrees with follower metadata");
    }

    ParsedDocument::QtFollowerDirectFrameAdmission admitted;
    admitted.resourceLogicalId = logicalId;
    admitted.renderSize = static_cast<std::uint32_t>(renderSize);
    admitted.packedWidth = static_cast<std::uint32_t>(packedWidth);
    admitted.frameCount = frameCount;
    admitted.durationUs = static_cast<std::int64_t>(durationUs);
    admitted.framesPerSecond = fps;
    admission = std::move(admitted);
    error.clear();
    return true;
  } catch (const std::exception &exception) {
    error = std::string("packed-alpha follower admission raised: ") +
            exception.what();
    return false;
  }
}

constexpr std::string_view kSvgSelectorPrefix{"svg:"};
constexpr std::string_view kJsonSelectorPrefix{"json:"};
constexpr std::string_view kJsonGradientSelectorPrefix{"json-gradient:"};

bool EndsWith(const std::string &value, const std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
             0;
}

bool ParseSize(const std::string &value, std::size_t &parsed) noexcept {
  const char *begin = value.data();
  const char *end = begin + value.size();
  const auto result = std::from_chars(begin, end, parsed);
  return result.ec == std::errc{} && result.ptr == end;
}

struct ParsedSvgBindingSelector final {
  std::size_t offset{0};
  std::size_t length{0};
  std::size_t tagOffset{0};
  std::string attribute;
};

bool ParseSvgBindingSelector(const std::string &selector,
                             ParsedSvgBindingSelector &parsed) {
  if (selector.compare(0, kSvgSelectorPrefix.size(), kSvgSelectorPrefix) != 0)
    return false;
  std::array<std::string, 4> parts;
  std::size_t partIndex = 0;
  std::size_t begin = kSvgSelectorPrefix.size();
  while (partIndex < parts.size()) {
    const auto separator = selector.find(':', begin);
    if (separator == std::string::npos) {
      parts[partIndex++] = selector.substr(begin);
      break;
    }
    parts[partIndex++] = selector.substr(begin, separator - begin);
    begin = separator + 1;
  }
  if (partIndex < 2 || !ParseSize(parts[0], parsed.offset) ||
      !ParseSize(parts[1], parsed.length))
    return false;
  if (partIndex >= 4 && ParseSize(parts[2], parsed.tagOffset))
    parsed.attribute = std::move(parts[3]);
  return true;
}

std::string EncodeSvgBindingSelector(const ParsedSvgBindingSelector &target) {
  std::string selector = std::string(kSvgSelectorPrefix) +
                         std::to_string(target.offset) + ':' +
                         std::to_string(target.length);
  if (!target.attribute.empty())
    selector += ':' + std::to_string(target.tagOffset) + ':' + target.attribute;
  return selector;
}

struct XmlAttributeRange final {
  std::string name;
  std::string value;
  std::size_t valueOffset{0};
  std::size_t valueLength{0};
};

std::size_t FindSvgTagEnd(const std::string &source,
                          const std::size_t tagOffset) noexcept {
  char quote = '\0';
  for (std::size_t cursor = tagOffset; cursor < source.size(); ++cursor) {
    const char character = source[cursor];
    if (quote != '\0') {
      if (character == quote)
        quote = '\0';
    } else if (character == '\'' || character == '"') {
      quote = character;
    } else if (character == '>') {
      return cursor;
    }
  }
  return std::string::npos;
}

std::vector<XmlAttributeRange>
ParseSvgTagAttributes(const std::string &source, const std::size_t tagOffset,
                      const std::size_t tagEnd) {
  std::vector<XmlAttributeRange> attributes;
  std::size_t cursor = tagOffset + 1;
  while (cursor < tagEnd &&
         !std::isspace(static_cast<unsigned char>(source[cursor])) &&
         source[cursor] != '/' && source[cursor] != '>')
    ++cursor;
  while (cursor < tagEnd) {
    while (cursor < tagEnd &&
           std::isspace(static_cast<unsigned char>(source[cursor])))
      ++cursor;
    if (cursor >= tagEnd || source[cursor] == '/')
      break;
    const auto nameBegin = cursor;
    while (cursor < tagEnd &&
           !std::isspace(static_cast<unsigned char>(source[cursor])) &&
           source[cursor] != '=' && source[cursor] != '/' &&
           source[cursor] != '>')
      ++cursor;
    std::string name = source.substr(nameBegin, cursor - nameBegin);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](const unsigned char character) {
                     return static_cast<char>(std::tolower(character));
                   });
    while (cursor < tagEnd &&
           std::isspace(static_cast<unsigned char>(source[cursor])))
      ++cursor;
    if (cursor >= tagEnd || source[cursor] != '=')
      continue;
    ++cursor;
    while (cursor < tagEnd &&
           std::isspace(static_cast<unsigned char>(source[cursor])))
      ++cursor;
    if (cursor >= tagEnd)
      break;
    const char quote = source[cursor] == '\'' || source[cursor] == '"'
                           ? source[cursor++]
                           : '\0';
    const auto valueBegin = cursor;
    if (quote != '\0') {
      while (cursor < tagEnd && source[cursor] != quote)
        ++cursor;
    } else {
      while (cursor < tagEnd &&
             !std::isspace(static_cast<unsigned char>(source[cursor])) &&
             source[cursor] != '>')
        ++cursor;
    }
    attributes.push_back({std::move(name),
                          source.substr(valueBegin, cursor - valueBegin),
                          valueBegin, cursor - valueBegin});
    if (quote != '\0' && cursor < tagEnd)
      ++cursor;
  }
  return attributes;
}

bool ResolveSvgSelectorMetadata(const std::string &source,
                                ParsedSvgBindingSelector &target) {
  if (!target.attribute.empty())
    return target.tagOffset < source.size();
  if (target.offset >= source.size())
    return false;
  target.tagOffset = source.rfind('<', target.offset);
  if (target.tagOffset == std::string::npos)
    return false;
  const auto tagEnd = FindSvgTagEnd(source, target.tagOffset);
  if (tagEnd == std::string::npos || target.offset >= tagEnd)
    return false;
  const auto attributes =
      ParseSvgTagAttributes(source, target.tagOffset, tagEnd);
  const auto found = std::find_if(
      attributes.begin(), attributes.end(), [&](const auto &attribute) {
        return attribute.valueOffset == target.offset &&
               attribute.valueLength == target.length;
      });
  if (found == attributes.end())
    return false;
  target.attribute = found->name;
  return true;
}

bool PrepareSvgBindingDocument(const vector::VectorDocument &authored,
                               vector::VectorDocument &instanceDocument,
                               CompiledVectorAnimationBindings &bindings,
                               std::string &error) {
  const auto &sourceBytes = *authored.source.bytes;
  const std::string source(reinterpret_cast<const char *>(sourceBytes.data()),
                           sourceBytes.size());
  struct NodePlan final {
    std::size_t tagOffset{0};
    std::size_t insertionOffset{0};
    std::string nodeId;
    std::string insertion;
  };
  std::unordered_map<std::size_t, NodePlan> nodes;
  std::vector<std::pair<std::string, ParsedSvgBindingSelector>> targets;
  for (const auto &field : authored.editableFields) {
    if (field.animatableComponents.empty())
      continue;
    for (const auto &declared : field.targets) {
      ParsedSvgBindingSelector target;
      if (!ParseSvgBindingSelector(declared.selector, target) ||
          !ResolveSvgSelectorMetadata(source, target)) {
        bindings.unsupportedFields.emplace(
            field.id, "SVG selector does not identify a mutable DOM attribute");
        continue;
      }
      const auto tagEnd = FindSvgTagEnd(source, target.tagOffset);
      if (tagEnd == std::string::npos) {
        bindings.unsupportedFields.emplace(field.id,
                                           "SVG selector tag is truncated");
        continue;
      }
      auto node = nodes.find(target.tagOffset);
      if (node == nodes.end()) {
        const auto attributes =
            ParseSvgTagAttributes(source, target.tagOffset, tagEnd);
        const auto id = std::find_if(
            attributes.begin(), attributes.end(), [](const auto &attribute) {
              return attribute.name == "id" && !attribute.value.empty();
            });
        NodePlan plan;
        plan.tagOffset = target.tagOffset;
        if (id != attributes.end()) {
          plan.nodeId = id->value;
        } else {
          std::size_t nameEnd = target.tagOffset + 1;
          while (nameEnd < tagEnd &&
                 !std::isspace(static_cast<unsigned char>(source[nameEnd])) &&
                 source[nameEnd] != '/' && source[nameEnd] != '>')
            ++nameEnd;
          plan.insertionOffset = nameEnd;
          plan.nodeId =
              "__vc_vector_binding_" + std::to_string(nodes.size() + 1);
          plan.insertion = " id=\"" + plan.nodeId + "\"";
        }
        node = nodes.emplace(target.tagOffset, std::move(plan)).first;
      }
      bindings.svgSpecs.push_back(
          {field.id, node->second.nodeId, target.attribute});
      targets.emplace_back(declared.selector, std::move(target));
    }
  }

  std::vector<NodePlan *> insertions;
  for (auto &[tagOffset, node] : nodes)
    if (!node.insertion.empty())
      insertions.push_back(&node);
  std::sort(insertions.begin(), insertions.end(),
            [](const auto *left, const auto *right) {
              return left->insertionOffset > right->insertionOffset;
            });
  std::string rewritten = source;
  for (const auto *insertion : insertions)
    rewritten.insert(insertion->insertionOffset, insertion->insertion);

  const auto adjustedOffset = [&](const std::size_t original) {
    std::size_t adjusted = original;
    for (const auto *insertion : insertions)
      if (insertion->insertionOffset <= original)
        adjusted += insertion->insertion.size();
    return adjusted;
  };
  instanceDocument = authored;
  instanceDocument.source.bytes =
      std::make_shared<const std::vector<std::uint8_t>>(rewritten.begin(),
                                                        rewritten.end());
  for (auto &field : instanceDocument.editableFields) {
    for (auto &declared : field.targets) {
      const auto found = std::find_if(
          targets.begin(), targets.end(), [&](const auto &candidate) {
            return candidate.first == declared.selector;
          });
      if (found == targets.end())
        continue;
      auto target = found->second;
      target.offset = adjustedOffset(target.offset);
      target.tagOffset = adjustedOffset(target.tagOffset);
      declared.selector = EncodeSvgBindingSelector(target);
    }
  }
  error.clear();
  return true;
}

Json *JsonAt(Json &root, const std::string &path) noexcept {
  try {
    return &root.at(Json::json_pointer(path));
  } catch (...) {
    return nullptr;
  }
}

const Json *JsonAt(const Json &root, const std::string &path) noexcept {
  try {
    return &root.at(Json::json_pointer(path));
  } catch (...) {
    return nullptr;
  }
}

std::string LottieObserverName(const std::size_t index) {
  return "__vc_vector_observer_" + std::to_string(index + 1);
}

bool CompileLottieAnimationBindings(
    const vector::VectorDocument &document,
    const std::shared_ptr<const std::vector<std::uint8_t>> &instanceSource,
    CompiledVectorAnimationBindings &bindings,
    std::shared_ptr<const std::vector<std::uint8_t>> &compiledSource,
    sk_sp<skottie::PropertyObserver> &observer, std::string &error) {
  struct SlotDraft final {
    std::string propertyPath;
    std::string slotId;
    LottieSlotValueKind kind{LottieSlotValueKind::Scalar};
    bool existingSlot{false};
    std::vector<LottieSlotFieldTarget> targets;
  };
  try {
    Json source = Json::parse(instanceSource->begin(), instanceSource->end());
    if (!source.is_object()) {
      error = "Lottie root is not an object";
      return false;
    }
    Json &slots = source["slots"];
    if (!slots.is_object())
      slots = Json::object();
    std::vector<SlotDraft> drafts;
    std::unordered_map<std::string, std::size_t> draftByPath;
    std::unordered_map<std::string, std::string> observerNameByOwner;

    const auto attachObserverRoute =
        [&](const std::string &ownerPath,
            const vector::VectorEditableField &field, const bool textRoute) {
          Json *owner = JsonAt(source, ownerPath);
          if (!owner || !owner->is_object())
            return;
          auto name = observerNameByOwner.find(ownerPath);
          if (name == observerNameByOwner.end()) {
            const std::string generated =
                LottieObserverName(observerNameByOwner.size());
            (*owner)["nm"] = generated;
            name = observerNameByOwner.emplace(ownerPath, generated).first;
          }
          auto &route = bindings.observerRoutes[name->second];
          const auto appendUnique = [](std::vector<std::string> &target,
                                       const std::string &fieldId) {
            if (std::find(target.begin(), target.end(), fieldId) ==
                target.end())
              target.push_back(fieldId);
          };
          switch (field.role) {
          case vector::VectorFieldRole::Fill:
            if (textRoute)
              appendUnique(route.textFillFieldIds, field.id);
            else
              appendUnique(route.colorFieldIds, field.id);
            break;
          case vector::VectorFieldRole::Stroke:
            if (textRoute)
              appendUnique(route.textStrokeFieldIds, field.id);
            else
              appendUnique(route.colorFieldIds, field.id);
            break;
          case vector::VectorFieldRole::CenterlineColor:
          case vector::VectorFieldRole::ShadowColor:
            appendUnique(route.colorFieldIds, field.id);
            break;
          case vector::VectorFieldRole::GlobalOpacity:
          case vector::VectorFieldRole::TextureOpacity:
            appendUnique(route.opacityFieldIds, field.id);
            break;
          default:
            break;
          }
        };

    for (const auto &field : document.editableFields) {
      if (field.animatableComponents.empty())
        continue;
      bool fieldBound = false;
      for (const auto &target : field.targets) {
        if (target.selector.compare(0, kJsonGradientSelectorPrefix.size(),
                                    kJsonGradientSelectorPrefix) == 0) {
          bindings.unsupportedFields.emplace(
              field.id,
              "pinned Skottie exposes no mutable packed-gradient stop handle");
          continue;
        }
        if (target.selector.compare(0, kJsonSelectorPrefix.size(),
                                    kJsonSelectorPrefix) != 0)
          continue;
        const std::string selectedPath =
            target.selector.substr(kJsonSelectorPrefix.size());
        if ((EndsWith(selectedPath, "/fc") || EndsWith(selectedPath, "/sc")) &&
            selectedPath.find("/t/d/") != std::string::npos) {
          const auto documentOffset = selectedPath.find("/t/d/");
          attachObserverRoute(selectedPath.substr(0, documentOffset), field,
                              true);
          fieldBound = true;
          continue;
        }

        std::string propertyPath = selectedPath;
        std::optional<std::size_t> vectorComponent;
        LottieSlotValueKind kind = LottieSlotValueKind::Scalar;
        if (field.kind == vector::VectorFieldKind::Number) {
          const auto componentMarker = selectedPath.rfind("/k/");
          if (componentMarker != std::string::npos) {
            std::size_t component = 0;
            if (!ParseSize(selectedPath.substr(componentMarker + 3),
                           component) ||
                component > 1) {
              bindings.unsupportedFields.emplace(
                  field.id, "Lottie vector component selector is invalid");
              continue;
            }
            propertyPath = selectedPath.substr(0, componentMarker);
            vectorComponent = component;
            kind = LottieSlotValueKind::Vec2;
          } else if (EndsWith(selectedPath, "/k")) {
            propertyPath.resize(propertyPath.size() - 2);
            kind = LottieSlotValueKind::Scalar;
          } else {
            bindings.unsupportedFields.emplace(
                field.id, "Lottie numeric selector has no property owner");
            continue;
          }
        } else if (field.kind == vector::VectorFieldKind::Color) {
          kind = LottieSlotValueKind::Color;
        } else {
          continue;
        }

        const Json *property = JsonAt(source, propertyPath);
        if (!property || !property->is_object()) {
          bindings.unsupportedFields.emplace(
              field.id, "Lottie selector does not resolve to a slot property");
          continue;
        }
        auto draft = draftByPath.find(propertyPath);
        if (draft == draftByPath.end()) {
          SlotDraft created;
          created.propertyPath = propertyPath;
          created.kind = kind;
          const auto sid = property->find("sid");
          if (sid != property->end() && sid->is_string()) {
            created.slotId = sid->get<std::string>();
            created.existingSlot = true;
          } else {
            do {
              created.slotId =
                  "__vc_vector_slot_" + std::to_string(drafts.size() + 1);
            } while (slots.contains(created.slotId));
          }
          drafts.push_back(std::move(created));
          draft = draftByPath.emplace(propertyPath, drafts.size() - 1).first;
        }
        auto &created = drafts[draft->second];
        if (created.kind != kind) {
          bindings.unsupportedFields.emplace(
              field.id, "Lottie property has conflicting slot value kinds");
          continue;
        }
        created.targets.push_back({field.id, vectorComponent});
        const auto ownerSeparator = propertyPath.rfind('/');
        if (ownerSeparator != std::string::npos)
          attachObserverRoute(propertyPath.substr(0, ownerSeparator), field,
                              false);
        fieldBound = true;
      }
      if (!fieldBound && bindings.unsupportedFields.find(field.id) ==
                             bindings.unsupportedFields.end())
        bindings.unsupportedFields.emplace(
            field.id, "Lottie field has no compiled runtime binding");
    }

    for (auto &draft : drafts) {
      if (!draft.existingSlot) {
        Json *property = JsonAt(source, draft.propertyPath);
        if (!property || !property->is_object()) {
          for (const auto &target : draft.targets)
            bindings.unsupportedFields.emplace(
                target.fieldId, "Lottie slot property became stale");
          continue;
        }
        slots[draft.slotId] = Json{{"p", *property}};
        *property = Json{{"sid", draft.slotId}};
      }
      auto binding =
          std::find_if(bindings.lottieSlots.begin(), bindings.lottieSlots.end(),
                       [&](const auto &candidate) {
                         return candidate.slotId == draft.slotId &&
                                candidate.kind == draft.kind;
                       });
      if (binding == bindings.lottieSlots.end()) {
        bindings.lottieSlots.push_back({draft.slotId, draft.kind});
        binding = std::prev(bindings.lottieSlots.end());
      }
      for (auto &target : draft.targets) {
        const auto duplicate = std::find_if(
            binding->targets.begin(), binding->targets.end(),
            [&](const auto &existing) {
              return existing.fieldId == target.fieldId &&
                     existing.vectorComponent == target.vectorComponent;
            });
        if (duplicate == binding->targets.end())
          binding->targets.push_back(std::move(target));
      }
    }
    const std::string encoded = source.dump();
    compiledSource = std::make_shared<const std::vector<std::uint8_t>>(
        encoded.begin(), encoded.end());
    observer = sk_make_sp<VectorPropertyObserver>(bindings);
    error.clear();
    return true;
  } catch (const std::exception &exception) {
    error = exception.what();
    return false;
  }
}

bool CompileVectorAnimationBindings(
    const vector::VectorDocument &document,
    CompiledVectorAnimationBindings &bindings,
    std::shared_ptr<const std::vector<std::uint8_t>> &instanceSource,
    sk_sp<skottie::PropertyObserver> &observer, std::string &warning,
    std::string &error) {
  warning.clear();
  error.clear();
  vector::VectorDocument instanceDocument;
  if (document.source.kind == vector::VectorSourceKind::Svg) {
    if (!PrepareSvgBindingDocument(document, instanceDocument, bindings, error))
      return false;
  } else {
    instanceDocument = document;
  }
  instanceSource = vector::BuildVectorInstanceSource(instanceDocument, warning);
  if (!instanceSource) {
    error = warning.empty() ? "vector instance source is unavailable" : warning;
    return false;
  }
  if (document.source.kind != vector::VectorSourceKind::LottieJson)
    return true;
  std::shared_ptr<const std::vector<std::uint8_t>> compiledSource;
  if (!CompileLottieAnimationBindings(document, instanceSource, bindings,
                                      compiledSource, observer, error))
    return false;
  instanceSource = std::move(compiledSource);
  return true;
}

std::shared_ptr<ParsedDocument> ParseDocument(vector::VectorDocument document,
                                              const SkiaRuntimeConfig &config,
                                              std::string &error) {
  auto parsed = std::make_shared<ParsedDocument>();
  parsed->document = std::move(document);
  if (!AdmitPackedAlphaFollowerDirectFrame(
          parsed->document, parsed->qtFollowerDirectFrame, error)) {
    return {};
  }
  const bool requiresTypefaceClosure =
      std::find(parsed->document.probe.capabilitiesUsed.begin(),
                parsed->document.probe.capabilitiesUsed.end(),
                "embedded-font-closure-required") !=
      parsed->document.probe.capabilitiesUsed.end();
  parsed->baseResources = sk_make_sp<ImmutableResourceProvider>(
      parsed->document.resources, requiresTypefaceClosure,
      config.animatedImages);
  if (config.allowSystemFonts && !requiresTypefaceClosure)
    parsed->fontManager = MakeSystemFontManager();
  parsed->resources = skresources::DataURIResourceProviderProxy::Make(
      parsed->baseResources, skresources::ImageDecodeStrategy::kLazyDecode,
      parsed->fontManager);
  if (!parsed->resources)
    parsed->resources = parsed->baseResources;

  std::string customizationWarning;
  std::shared_ptr<const std::vector<std::uint8_t>> instanceSource;
  sk_sp<skottie::PropertyObserver> propertyObserver;
  if (!CompileVectorAnimationBindings(
          parsed->document, parsed->animationBindings, instanceSource,
          propertyObserver, customizationWarning, error)) {
    return {};
  }
  if (!customizationWarning.empty()) {
    parsed->diagnostics.push_back(MakeDiagnostic(
        "vector.customization.fallback", DiagnosticSeverity::Warning,
        "customize", parsed->document.source.assetId,
        std::move(customizationWarning)));
  }
  const auto &bytes = *instanceSource;
  if (parsed->document.source.kind == vector::VectorSourceKind::Svg) {
    SkMemoryStream stream(bytes.data(), bytes.size(), false);
    SkSVGDOM::Builder builder;
    builder.setResourceProvider(parsed->resources);
    builder.setTextShapingFactory(SkShapers::BestAvailable());
    if (parsed->fontManager)
      builder.setFontManager(parsed->fontManager);
    parsed->svg = builder.make(stream);
    if (!parsed->svg) {
      error = "SkSVG document parsing failed";
      return {};
    }
    for (const auto &spec : parsed->animationBindings.svgSpecs) {
      const auto node = parsed->svg->findNodeById(spec.nodeId.c_str());
      if (!node || !*node) {
        parsed->animationBindings.unsupportedFields.emplace(
            spec.fieldId, "compiled SVG node handle is unavailable");
        continue;
      }
      parsed->animationBindings.svgAttributes.push_back(
          {spec.fieldId, *node, spec.attribute});
    }
    const SkSize intrinsic = parsed->svg->containerSize();
    parsed->sourceWidth = parsed->document.probe.intrinsicWidth > 0.0F
                              ? parsed->document.probe.intrinsicWidth
                              : intrinsic.width();
    parsed->sourceHeight = parsed->document.probe.intrinsicHeight > 0.0F
                               ? parsed->document.probe.intrinsicHeight
                               : intrinsic.height();
  } else if (parsed->document.source.kind ==
             vector::VectorSourceKind::LottieJson) {
    auto logger = sk_make_sp<LottieLogger>(config.maximumDiagnostics);
    skottie::Animation::Builder builder(
        skottie::Animation::Builder::kDeferImageLoading |
        skottie::Animation::Builder::kPreferEmbeddedFonts);
    builder.setResourceProvider(parsed->resources);
    builder.setLogger(logger);
    if (propertyObserver)
      builder.setPropertyObserver(std::move(propertyObserver));
    builder.setTextShapingFactory(SkShapers::BestAvailable());
    if (parsed->fontManager)
      builder.setFontManager(parsed->fontManager);
    parsed->lottie = builder.make(reinterpret_cast<const char *>(bytes.data()),
                                  bytes.size());
    const auto &lottieDiagnostics = logger->diagnostics();
    parsed->diagnostics.insert(parsed->diagnostics.end(),
                               lottieDiagnostics.begin(),
                               lottieDiagnostics.end());
    if (!parsed->lottie) {
      error = "Skottie document parsing failed";
      return {};
    }
    // PropertyObserver handles are published while Builder is still wiring
    // the scene graph, before the first animator sync. Freeze baselines only
    // after that initial seek; pre-seek handle values can be zero/transparent
    // placeholders and would corrupt the first same-timestamp restore.
    parsed->lottie->seekFrameTime(0.0);
    for (auto &binding : parsed->animationBindings.colorHandles)
      binding.baseline = binding.handle->get();
    for (auto &binding : parsed->animationBindings.opacityHandles)
      binding.baseline = binding.handle->get();
    for (auto &binding : parsed->animationBindings.textHandles)
      binding.baseline = binding.handle->get();

    parsed->animationBindings.slotManager = builder.getSlotManager();
    std::vector<LottieSlotBinding> publishedSlots;
    std::unordered_set<std::string> publishedSlotFields;
    publishedSlots.reserve(parsed->animationBindings.lottieSlots.size());
    for (auto &binding : parsed->animationBindings.lottieSlots) {
      bool available = false;
      if (parsed->animationBindings.slotManager) {
        const SkString slotId(binding.slotId.c_str());
        switch (binding.kind) {
        case LottieSlotValueKind::Scalar:
          if (const auto value =
                  parsed->animationBindings.slotManager->getScalarSlot(
                      slotId)) {
            binding.baselineScalar = *value;
            available = true;
          }
          break;
        case LottieSlotValueKind::Color:
          if (const auto value =
                  parsed->animationBindings.slotManager->getColorSlot(slotId)) {
            binding.baselineColor = *value;
            available = true;
          }
          break;
        case LottieSlotValueKind::Vec2:
          if (const auto value =
                  parsed->animationBindings.slotManager->getVec2Slot(slotId)) {
            binding.baselineVec2 = *value;
            available = true;
          }
          break;
        }
      }
      if (!available) {
        for (const auto &target : binding.targets)
          parsed->animationBindings.unsupportedFields.emplace(
              target.fieldId,
              "Skottie did not publish the compiled property slot");
      } else {
        for (const auto &target : binding.targets)
          publishedSlotFields.insert(target.fieldId);
        publishedSlots.push_back(std::move(binding));
      }
    }
    // A syntactically valid Lottie property is not necessarily consumed by
    // the pinned Skottie runtime (for example an extension property ignored
    // by the parser). Keep those fields in the typed unsupported map so an
    // explicit sample still fails closed, but never let an unpublished slot
    // participate in baseline restore for an otherwise immutable render.
    parsed->animationBindings.lottieSlots = std::move(publishedSlots);

    // A property can be exposed through both a native slot and an observer
    // handle. The slot is the typed, writable contract; retaining the alias
    // would let a stale observer baseline overwrite the slot immediately
    // after restoration. Remove only published-slot fields from observer
    // routes. Unpublished slots remain typed-unsupported and observer-only
    // fields retain their fallback handle.
    const auto removePublishedSlotFields = [&](auto &fieldIds) {
      fieldIds.erase(
          std::remove_if(fieldIds.begin(), fieldIds.end(),
                         [&](const std::string &fieldId) {
                           return publishedSlotFields.find(fieldId) !=
                                  publishedSlotFields.end();
                         }),
          fieldIds.end());
    };
    for (auto &binding : parsed->animationBindings.colorHandles)
      removePublishedSlotFields(binding.fieldIds);
    parsed->animationBindings.colorHandles.erase(
        std::remove_if(parsed->animationBindings.colorHandles.begin(),
                       parsed->animationBindings.colorHandles.end(),
                       [](const auto &binding) {
                         return binding.fieldIds.empty();
                       }),
        parsed->animationBindings.colorHandles.end());
    for (auto &binding : parsed->animationBindings.opacityHandles)
      removePublishedSlotFields(binding.fieldIds);
    parsed->animationBindings.opacityHandles.erase(
        std::remove_if(parsed->animationBindings.opacityHandles.begin(),
                       parsed->animationBindings.opacityHandles.end(),
                       [](const auto &binding) {
                         return binding.fieldIds.empty();
                       }),
        parsed->animationBindings.opacityHandles.end());

    std::unordered_set<std::string> observedTextFields;
    for (const auto &binding : parsed->animationBindings.textHandles) {
      observedTextFields.insert(binding.fillFieldIds.begin(),
                                binding.fillFieldIds.end());
      observedTextFields.insert(binding.strokeFieldIds.begin(),
                                binding.strokeFieldIds.end());
    }
    for (const auto &[name, route] : parsed->animationBindings.observerRoutes) {
      for (const auto &fieldId : route.textFillFieldIds)
        if (observedTextFields.find(fieldId) == observedTextFields.end())
          parsed->animationBindings.unsupportedFields.emplace(
              fieldId,
              "Skottie did not publish the compiled text property handle");
      for (const auto &fieldId : route.textStrokeFieldIds)
        if (observedTextFields.find(fieldId) == observedTextFields.end())
          parsed->animationBindings.unsupportedFields.emplace(
              fieldId,
              "Skottie did not publish the compiled text property handle");
    }
    parsed->sourceWidth = parsed->lottie->size().width();
    parsed->sourceHeight = parsed->lottie->size().height();
    parsed->durationUs = static_cast<std::int64_t>(
        std::llround(parsed->lottie->duration() * 1'000'000.0));
    parsed->framesPerSecond = parsed->lottie->fps();
    if (parsed->qtFollowerDirectFrame) {
      const auto &direct = *parsed->qtFollowerDirectFrame;
      if (parsed->sourceWidth != static_cast<float>(direct.renderSize) ||
          parsed->sourceHeight != static_cast<float>(direct.renderSize) ||
          parsed->durationUs != direct.durationUs ||
          parsed->framesPerSecond != direct.framesPerSecond) {
        error = "packed-alpha follower Skottie probe disagreement";
        return {};
      }
    }
  } else {
    error = "dotLottie is not enabled in renderer profile v1";
    return {};
  }
  if (parsed->baseResources->resourceFailureObserved()) {
    error =
        "vector document referenced a missing or invalid immutable resource";
    return {};
  }

  if (!(parsed->sourceWidth > 0.0F) || !(parsed->sourceHeight > 0.0F) ||
      !std::isfinite(parsed->sourceWidth) ||
      !std::isfinite(parsed->sourceHeight)) {
    parsed->sourceWidth = parsed->document.viewport.referenceWidth > 0.0F
                              ? parsed->document.viewport.referenceWidth
                              : 100.0F;
    parsed->sourceHeight = parsed->document.viewport.referenceHeight > 0.0F
                               ? parsed->document.viewport.referenceHeight
                               : 100.0F;
    parsed->diagnostics.push_back(MakeDiagnostic(
        "vector.probe.intrinsic_size_fallback", DiagnosticSeverity::Warning,
        "probe", parsed->document.source.assetId,
        "intrinsic size was unavailable; the declared viewport fallback is "
        "used"));
  }
  return parsed;
}

const vector::VectorRuntimeFieldOverride *FindRuntimeOverride(
    const vector::VectorRuntimeOverrideSet &overrides,
    const std::string &fieldId,
    const vector::VectorRuntimeOverrideComponent component) noexcept {
  const auto found = std::find_if(
      overrides.values.begin(), overrides.values.end(),
      [&](const auto &sampled) {
        return sampled.fieldId == fieldId && sampled.component == component;
      });
  return found == overrides.values.end() ? nullptr : &*found;
}

std::uint8_t VectorColorByte(const float value) noexcept {
  return static_cast<std::uint8_t>(
      std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

SkColor ToSkColor(const vector::VectorColor &color) noexcept {
  return SkColorSetARGB(
      VectorColorByte(color.alpha), VectorColorByte(color.red),
      VectorColorByte(color.green), VectorColorByte(color.blue));
}

SkColor ApplyColorRuntimeOverrides(
    const SkColor baseline, const std::string &fieldId,
    const vector::VectorRuntimeOverrideSet &overrides) noexcept {
  std::uint8_t red = SkColorGetR(baseline);
  std::uint8_t green = SkColorGetG(baseline);
  std::uint8_t blue = SkColorGetB(baseline);
  std::uint8_t alpha = SkColorGetA(baseline);
  if (const auto *sampled = FindRuntimeOverride(
          overrides, fieldId, vector::VectorRuntimeOverrideComponent::Color)) {
    red = VectorColorByte(sampled->value.color.red);
    green = VectorColorByte(sampled->value.color.green);
    blue = VectorColorByte(sampled->value.color.blue);
  }
  if (const auto *sampled = FindRuntimeOverride(
          overrides, fieldId, vector::VectorRuntimeOverrideComponent::Alpha))
    alpha = VectorColorByte(static_cast<float>(sampled->value.number));
  return SkColorSetARGB(alpha, red, green, blue);
}

std::string SvgFieldValueText(const vector::VectorEditableField &field,
                              const vector::VectorFieldValue &value) {
  if (value.kind == vector::VectorFieldKind::Color) {
    std::ostringstream stream;
    stream << '#' << std::uppercase << std::hex << std::setfill('0')
           << std::setw(2) << static_cast<int>(VectorColorByte(value.color.red))
           << std::setw(2)
           << static_cast<int>(VectorColorByte(value.color.green))
           << std::setw(2)
           << static_cast<int>(VectorColorByte(value.color.blue));
    if (VectorColorByte(value.color.alpha) != 255)
      stream << std::setw(2)
             << static_cast<int>(VectorColorByte(value.color.alpha));
    return stream.str();
  }
  if (value.kind == vector::VectorFieldKind::Text)
    return value.text;
  std::ostringstream stream;
  stream << std::setprecision(12) << value.number;
  switch (field.unit) {
  case vector::VectorFieldUnit::Percent:
    stream << '%';
    break;
  case vector::VectorFieldUnit::Pixels:
    stream << "px";
    break;
  case vector::VectorFieldUnit::Degrees:
    stream << "deg";
    break;
  case vector::VectorFieldUnit::Unitless:
  case vector::VectorFieldUnit::Ratio:
    break;
  }
  return stream.str();
}

bool ApplyVectorAnimationBindings(
    ParsedDocument &parsed, const std::int64_t sourceTimeUs,
    const vector::VectorRuntimeOverrideSet &overrides, std::string &error) {
  vector::VectorDocument resolved;
  if (!vector::ApplyVectorRuntimeOverrides(parsed.document, overrides, resolved,
                                           error))
    return false;
  for (const auto &sampled : overrides.values) {
    const auto unsupported =
        parsed.animationBindings.unsupportedFields.find(sampled.fieldId);
    if (unsupported != parsed.animationBindings.unsupportedFields.end()) {
      error = unsupported->second + ": " + sampled.fieldId;
      return false;
    }
  }

  if (parsed.svg) {
    for (const auto &binding : parsed.animationBindings.svgAttributes) {
      const auto *field =
          vector::FindVectorEditableField(resolved, binding.fieldId);
      if (!field || !binding.node) {
        error = "compiled SVG binding became stale: " + binding.fieldId;
        return false;
      }
      const auto value = vector::EffectiveVectorFieldValue(resolved, *field);
      const std::string encoded = SvgFieldValueText(*field, value);
      if (!binding.node->parseAndSetAttribute(binding.attribute.c_str(),
                                              encoded.c_str())) {
        error =
            "SVG node rejected compiled attribute binding: " + binding.fieldId;
        return false;
      }
    }
    error.clear();
    return true;
  }

  if (!parsed.lottie) {
    error = "compiled vector animation has no render document";
    return false;
  }
  auto &bindings = parsed.animationBindings;
  if (!bindings.slotManager && !bindings.lottieSlots.empty()) {
    error = "compiled Skottie slot manager is unavailable";
    return false;
  }

  for (const auto &binding : bindings.lottieSlots) {
    const SkString slotId(binding.slotId.c_str());
    bool restored = false;
    switch (binding.kind) {
    case LottieSlotValueKind::Scalar:
      restored =
          bindings.slotManager->setScalarSlot(slotId, binding.baselineScalar);
      break;
    case LottieSlotValueKind::Color:
      restored =
          bindings.slotManager->setColorSlot(slotId, binding.baselineColor);
      break;
    case LottieSlotValueKind::Vec2:
      restored =
          bindings.slotManager->setVec2Slot(slotId, binding.baselineVec2);
      break;
    }
    if (!restored) {
      error =
          "compiled Skottie slot could not restore baseline: " + binding.slotId;
      return false;
    }
  }
  for (auto &binding : bindings.colorHandles)
    binding.handle->set(binding.baseline);
  for (auto &binding : bindings.opacityHandles)
    binding.handle->set(binding.baseline);
  for (auto &binding : bindings.textHandles)
    binding.handle->set(binding.baseline);

  parsed.lottie->seekFrameTime(static_cast<double>(sourceTimeUs) / 1'000'000.0);

  std::unordered_set<std::string> slotFields;
  for (const auto &binding : bindings.lottieSlots) {
    const SkString slotId(binding.slotId.c_str());
    bool changed = false;
    switch (binding.kind) {
    case LottieSlotValueKind::Scalar: {
      auto current = bindings.slotManager->getScalarSlot(slotId);
      if (!current) {
        error = "compiled Skottie scalar slot is unavailable";
        return false;
      }
      float value = *current;
      for (const auto &target : binding.targets) {
        slotFields.insert(target.fieldId);
        if (const auto *sampled = FindRuntimeOverride(
                overrides, target.fieldId,
                vector::VectorRuntimeOverrideComponent::Number)) {
          value = static_cast<float>(sampled->value.number);
          changed = true;
        }
      }
      if (changed && !bindings.slotManager->setScalarSlot(slotId, value)) {
        error = "compiled Skottie scalar slot update failed";
        return false;
      }
      break;
    }
    case LottieSlotValueKind::Color: {
      auto current = bindings.slotManager->getColorSlot(slotId);
      if (!current) {
        error = "compiled Skottie color slot is unavailable";
        return false;
      }
      SkColor value = *current;
      for (const auto &target : binding.targets) {
        slotFields.insert(target.fieldId);
        if (FindRuntimeOverride(
                overrides, target.fieldId,
                vector::VectorRuntimeOverrideComponent::Color) ||
            FindRuntimeOverride(
                overrides, target.fieldId,
                vector::VectorRuntimeOverrideComponent::Alpha)) {
          value = ApplyColorRuntimeOverrides(value, target.fieldId, overrides);
          changed = true;
        }
      }
      if (changed && !bindings.slotManager->setColorSlot(slotId, value)) {
        error = "compiled Skottie color slot update failed";
        return false;
      }
      break;
    }
    case LottieSlotValueKind::Vec2: {
      auto current = bindings.slotManager->getVec2Slot(slotId);
      if (!current) {
        error = "compiled Skottie vector slot is unavailable";
        return false;
      }
      SkV2 value = *current;
      for (const auto &target : binding.targets) {
        slotFields.insert(target.fieldId);
        const auto *sampled =
            FindRuntimeOverride(overrides, target.fieldId,
                                vector::VectorRuntimeOverrideComponent::Number);
        if (!sampled || !target.vectorComponent)
          continue;
        if (*target.vectorComponent == 0)
          value.x = static_cast<float>(sampled->value.number);
        else
          value.y = static_cast<float>(sampled->value.number);
        changed = true;
      }
      if (changed && !bindings.slotManager->setVec2Slot(slotId, value)) {
        error = "compiled Skottie vector slot update failed";
        return false;
      }
      break;
    }
    }
  }

  for (auto &binding : bindings.colorHandles) {
    SkColor value = binding.handle->get();
    bool changed = false;
    for (const auto &fieldId : binding.fieldIds) {
      if (slotFields.find(fieldId) != slotFields.end())
        continue;
      if (FindRuntimeOverride(overrides, fieldId,
                              vector::VectorRuntimeOverrideComponent::Color) ||
          FindRuntimeOverride(overrides, fieldId,
                              vector::VectorRuntimeOverrideComponent::Alpha)) {
        value = ApplyColorRuntimeOverrides(value, fieldId, overrides);
        changed = true;
      }
    }
    if (changed)
      binding.handle->set(value);
  }
  for (auto &binding : bindings.opacityHandles) {
    for (const auto &fieldId : binding.fieldIds) {
      if (slotFields.find(fieldId) != slotFields.end())
        continue;
      const auto *sampled = FindRuntimeOverride(
          overrides, fieldId, vector::VectorRuntimeOverrideComponent::Number);
      if (!sampled)
        continue;
      float value = static_cast<float>(sampled->value.number);
      if (const auto *field =
              vector::FindVectorEditableField(parsed.document, fieldId);
          field && field->unit == vector::VectorFieldUnit::Percent)
        value *= 0.01F;
      binding.handle->set(value);
    }
  }
  for (auto &binding : bindings.textHandles) {
    auto value = binding.handle->get();
    bool changed = false;
    for (const auto &fieldId : binding.fillFieldIds)
      if (FindRuntimeOverride(overrides, fieldId,
                              vector::VectorRuntimeOverrideComponent::Color) ||
          FindRuntimeOverride(overrides, fieldId,
                              vector::VectorRuntimeOverrideComponent::Alpha)) {
        value.fFillColor =
            ApplyColorRuntimeOverrides(value.fFillColor, fieldId, overrides);
        changed = true;
      }
    for (const auto &fieldId : binding.strokeFieldIds)
      if (FindRuntimeOverride(overrides, fieldId,
                              vector::VectorRuntimeOverrideComponent::Color) ||
          FindRuntimeOverride(overrides, fieldId,
                              vector::VectorRuntimeOverrideComponent::Alpha)) {
        value.fStrokeColor =
            ApplyColorRuntimeOverrides(value.fStrokeColor, fieldId, overrides);
        changed = true;
      }
    if (changed)
      binding.handle->set(value);
  }
  error.clear();
  return true;
}

struct Destination final {
  float left{0.0F};
  float top{0.0F};
  float width{0.0F};
  float height{0.0F};
};

struct RasterRegion final {
  int left{0};
  int top{0};
  int right{0};
  int bottom{0};

  int width() const noexcept { return right - left; }
  int height() const noexcept { return bottom - top; }
  bool empty() const noexcept { return right <= left || bottom <= top; }
};

RasterRegion ResolveRasterRegion(const Destination &destination,
                                 const std::uint32_t outputWidth,
                                 const std::uint32_t outputHeight) {
  const double destinationLeft = static_cast<double>(destination.left);
  const double destinationTop = static_cast<double>(destination.top);
  const double destinationRight =
      destinationLeft + static_cast<double>(destination.width);
  const double destinationBottom =
      destinationTop + static_cast<double>(destination.height);
  if (!std::isfinite(destination.left) || !std::isfinite(destination.top) ||
      !std::isfinite(destinationRight) || !std::isfinite(destinationBottom)) {
    return {0, 0, static_cast<int>(outputWidth),
            static_cast<int>(outputHeight)};
  }

  const double visibleLeft = std::max(0.0, destinationLeft);
  const double visibleTop = std::max(0.0, destinationTop);
  const double visibleRight =
      std::min(static_cast<double>(outputWidth), destinationRight);
  const double visibleBottom =
      std::min(static_cast<double>(outputHeight), destinationBottom);
  if (visibleRight <= visibleLeft || visibleBottom <= visibleTop)
    return {};

  return {
      static_cast<int>(std::floor(visibleLeft)) - kLottieRasterApron,
      static_cast<int>(std::floor(visibleTop)) - kLottieRasterApron,
      static_cast<int>(std::ceil(visibleRight)) + kLottieRasterApron,
      static_cast<int>(std::ceil(visibleBottom)) + kLottieRasterApron,
  };
}

bool RgbaSurfaceBytes(const std::uint32_t width, const std::uint32_t height,
                      std::size_t &bytes) noexcept {
  if (width == 0 || height == 0 ||
      width > std::numeric_limits<std::size_t>::max() / height) {
    return false;
  }
  const std::size_t pixels = static_cast<std::size_t>(width) * height;
  if (pixels > std::numeric_limits<std::size_t>::max() / 4U)
    return false;
  bytes = pixels * 4U;
  return true;
}

bool PublishQtFollowerAssociatedFrame(
    const QtFollowerDirectFrame &source, const std::int64_t timestampUs,
    const std::uint64_t generation, const vector::CancelCheck &cancel,
    frame::VideoFrame &output, std::string &error) {
  output = {};
  if (source.contractVersion != kQtFollowerDirectFrameContractVersion ||
      source.width == 0U || source.height == 0U ||
      source.width > std::numeric_limits<std::uint32_t>::max() / 4U ||
      source.rowBytes != static_cast<std::size_t>(source.width) * 4U ||
      !source.pixels ||
      source.height > std::numeric_limits<std::size_t>::max() /
                          source.rowBytes ||
      source.pixels->size() != source.rowBytes * source.height ||
      source.frameIdentity.empty() || source.exactSourceFrameCount == 0U) {
    error = "Qt follower associated direct frame is invalid";
    return false;
  }
  if (IsCanceled(cancel)) {
    error = "canceled";
    return false;
  }
  frame::ColorInfo color;
  color.primaries = frame::ColorPrimaries::Unknown;
  color.transfer = frame::TransferFunction::Srgb;
  color.matrix = frame::MatrixCoefficients::Identity;
  color.range = frame::ColorRange::Full;
  // Preserve the recovered merge texture's authored association. The
  // compositor admits premultiplied Sprite2D overlays explicitly and selects
  // its associated linear/source-over path from this metadata. Converting to
  // straight RGBA8 and associating again loses low-alpha RGB to quantization.
  color.alpha = frame::AlphaMode::Premultiplied;
  color.model = frame::ColorModel::Rgb;
  const auto descriptor = frame::MakePackedFrameDesc(
      frame::FrameKind::Image, frame::PixelFormat::Rgba8, source.width,
      source.height, color, {timestampUs, 1, {1, 1'000'000}});
  if (!descriptor) {
    error = descriptor.error().message();
    return false;
  }
  auto allocation = frame::VideoFrame::AllocateCpu(descriptor.value(), 64,
                                                     generation);
  if (!allocation) {
    error = allocation.error().message();
    return false;
  }
  auto writable = std::move(allocation).value();
  const auto plane = writable.plane(0U);
  if (!plane || plane.value().row_stride < source.rowBytes) {
    error = plane ? "Qt follower publication row stride is invalid"
                  : plane.error().message();
    return false;
  }
  for (std::uint32_t row = 0U; row < source.height; ++row) {
    if (IsCanceled(cancel)) {
      error = "canceled";
      return false;
    }
    std::memcpy(
        plane.value().data +
            static_cast<std::size_t>(row) * plane.value().row_stride,
        source.pixels->data() + static_cast<std::size_t>(row) * source.rowBytes,
        source.rowBytes);
  }
  const auto finished = writable.finish(output);
  if (!finished) {
    error = finished.error().message();
    output = {};
    return false;
  }
  error.clear();
  return true;
}

bool WantsLottieSupersampling(
    const vector::VectorRenderRequest &request) noexcept {
  return request.delivery ==
             vector::VectorRasterDelivery::DeferredCompositeRaster &&
         (request.quality == vector::RenderQuality::PreviewActive ||
          request.quality == vector::RenderQuality::Export);
}

Destination ResolveDestination(float sourceWidth, float sourceHeight,
                               std::uint32_t outputWidth,
                               std::uint32_t outputHeight,
                               const vector::VectorViewportPolicy &viewport) {
  const float targetWidth = static_cast<float>(outputWidth);
  const float targetHeight = static_cast<float>(outputHeight);
  float scaleX = targetWidth / sourceWidth;
  float scaleY = targetHeight / sourceHeight;
  if (viewport.preserveAspectRatio && viewport.fit != vector::VectorFit::Fill) {
    float scale = 1.0F;
    switch (viewport.fit) {
    case vector::VectorFit::Contain:
      scale = std::min(scaleX, scaleY);
      break;
    case vector::VectorFit::Cover:
      scale = std::max(scaleX, scaleY);
      break;
    case vector::VectorFit::None:
      scale = 1.0F;
      break;
    case vector::VectorFit::Fill:
      break;
    }
    scaleX = scale;
    scaleY = scale;
  }
  const float width = sourceWidth * scaleX;
  const float height = sourceHeight * scaleY;
  const float overscan = std::max(0.0F, viewport.overscan);
  return {
      (targetWidth - width) * viewport.alignmentX - overscan,
      (targetHeight - height) * viewport.alignmentY - overscan,
      width + overscan * 2.0F,
      height + overscan * 2.0F,
  };
}

int ResolveSupersampleScale(
    const vector::VectorRenderRequest &request) noexcept {
  float presentation = request.presentationScale;
  if (!std::isfinite(presentation) || presentation < 1.0F)
    presentation = 1.0F;
  if (presentation > 4.0F)
    presentation = 4.0F;
  int floorScale = kLottieSupersampleScale;
  if (request.minimumSupersampleScale > 0)
    floorScale =
        std::clamp(request.minimumSupersampleScale, kLottieSupersampleScale, 4);
  const int boosted = static_cast<int>(
      std::ceil(static_cast<float>(floorScale) * presentation - 1.0e-6F));
  return std::clamp(boosted, floorScale, 4);
}

struct RenderKey final {
  std::uint64_t generation{0};
  std::string runtimeOverrideDigest;
  std::int64_t sourceTimeUs{0};
  std::optional<std::uint64_t> sourceFrameIndex;
  std::uint32_t width{0};
  std::uint32_t height{0};
  vector::VectorViewportPolicy viewport{};
  vector::RenderQuality quality{vector::RenderQuality::PreviewActive};
  vector::VectorRasterDelivery delivery{
      vector::VectorRasterDelivery::ResolvedFrame};
  int supersampleScale{kLottieSupersampleScale};
  bool gpuConsumer{false};
  std::int32_t gpuDeviceIndex{-1};
  std::uint64_t gpuDeviceGeneration{1};

  bool operator==(const RenderKey &other) const noexcept {
    return generation == other.generation &&
           runtimeOverrideDigest == other.runtimeOverrideDigest &&
           sourceTimeUs == other.sourceTimeUs &&
           sourceFrameIndex == other.sourceFrameIndex &&
           width == other.width &&
           height == other.height && viewport.fit == other.viewport.fit &&
           viewport.alignmentX == other.viewport.alignmentX &&
           viewport.alignmentY == other.viewport.alignmentY &&
           viewport.preserveAspectRatio == other.viewport.preserveAspectRatio &&
           viewport.referenceWidth == other.viewport.referenceWidth &&
           viewport.referenceHeight == other.viewport.referenceHeight &&
           viewport.overscan == other.viewport.overscan &&
           quality == other.quality && delivery == other.delivery &&
           supersampleScale == other.supersampleScale &&
           gpuConsumer == other.gpuConsumer &&
           gpuDeviceIndex == other.gpuDeviceIndex &&
           gpuDeviceGeneration == other.gpuDeviceGeneration;
  }
};

class VectorLane final : public vector::VectorRenderLane {
public:
  VectorLane(vector::VectorRenderOptions options, SkiaRuntimeConfig config)
      : options_(std::move(options)), config_(std::move(config)) {}

  void PurgeCache() noexcept override { cached_.reset(); }

  bool ReplaceDocument(vector::VectorDocument document,
                       std::string &error) override {
    error.clear();
    if (!document.source.bytes && config_.assets) {
      RuntimeAsset resolved;
      if (!config_.assets->Resolve(RuntimeAssetKind::Vector,
                                   document.source.assetId, resolved, error))
        return false;
      if (!resolved.bytes || resolved.bytes->empty() ||
          (!resolved.mediaType.empty() &&
           resolved.mediaType != document.source.mediaType)) {
        error = "managed vector source identity did not match: " +
                document.source.assetId;
        return false;
      }
      document.source.bytes = std::move(resolved.bytes);
    }
    for (auto &resource : document.resources) {
      if (resource.bytes || !config_.assets)
        continue;
      RuntimeAsset resolved;
      const bool imageResource =
          resource.mediaType.compare(0, 6, "image/") == 0 ||
          resource.mediaType == kQtPackedAlphaMp4ResourceMediaType;
      const auto kind = imageResource ? RuntimeAssetKind::Image
                                      : RuntimeAssetKind::NestedAnimation;
      if (!config_.assets->Resolve(kind, resource.logicalName, resolved, error))
        return false;
      if (!resolved.bytes || resolved.bytes->empty() ||
          (!resolved.mediaType.empty() &&
           resolved.mediaType != resource.mediaType)) {
        error = "managed vector resource identity did not match: " +
                resource.logicalName;
        return false;
      }
      resource.bytes = std::move(resolved.bytes);
    }
    const auto validation = vector::ValidateVectorDocument(document);
    if (!validation.valid) {
      error = validation.diagnostics.empty()
                  ? "vector document is invalid"
                  : validation.diagnostics.front().message;
      return false;
    }
    if (document.rendererProfile != options_.rendererProfile ||
        document.rendererProfile != kVectorProfile) {
      error = "vector renderer profile is unsupported";
      return false;
    }
    auto parsed = ParseDocument(std::move(document), config_, error);
    if (!parsed)
      return false;
    parsed_ = std::move(parsed);
    ++generation_;
    if (generation_ == 0)
      generation_ = 1;
    supersampleRouteLogged_ = false;
    supersampleFallbackLogged_ = false;
    cached_.reset();
    return true;
  }

  std::uint64_t generation() const noexcept override { return generation_; }

  vector::VectorRenderResult
  Render(const vector::VectorRenderRequest &request) override {
    vector::VectorRenderResult result;
    result.documentGeneration = generation_;
    if (!parsed_) {
      result.status = vector::VectorStatusCode::NotReady;
      result.diagnostics.push_back(MakeDiagnostic(
          "vector.document.not_ready", DiagnosticSeverity::Error, "render", {},
          "no vector document has been installed"));
      return result;
    }
    // Frame selection is request-scoped. Clearing first also guarantees that
    // ordinary animated resources keep the seconds-based ImageAsset ABI.
    parsed_->baseResources->ClearSourceFrameSelection();
    if (const auto abort = AbortReason(request);
        abort != RenderAbortReason::None) {
      ApplyAbort(result, abort, parsed_->document.source.assetId, "render");
      return result;
    }
    auto normalizedOverrides = request.runtimeOverrides;
    std::string runtimeOverrideError;
    if (!vector::FinalizeVectorRuntimeOverrides(normalizedOverrides,
                                                runtimeOverrideError) ||
        (!request.runtimeOverrides.digest.empty() &&
         request.runtimeOverrides.digest != normalizedOverrides.digest)) {
      result.status = vector::VectorStatusCode::InvalidDocument;
      result.diagnostics.push_back(MakeDiagnostic(
          "vector.customization.runtime_override_invalid",
          DiagnosticSeverity::Error, "customize",
          parsed_->document.source.assetId,
          runtimeOverrideError.empty()
              ? "vector runtime override digest does not match its values"
              : std::move(runtimeOverrideError)));
      return result;
    }
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(request.outputWidth) *
        static_cast<std::uint64_t>(request.outputHeight);
    std::size_t outputSurfaceBytes = 0;
    if (request.outputWidth == 0 || request.outputHeight == 0 ||
        request.outputWidth > options_.maximumWidth ||
        request.outputHeight > options_.maximumHeight ||
        request.outputWidth >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max() -
                                       kLottieRasterApron * 2) ||
        request.outputHeight >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max() -
                                       kLottieRasterApron * 2) ||
        pixels > options_.maximumPixels ||
        !RgbaSurfaceBytes(request.outputWidth, request.outputHeight,
                          outputSurfaceBytes) ||
        outputSurfaceBytes > options_.maximumSurfaceBytes ||
        outputSurfaceBytes > options_.maximumWorkingBytes / 2U) {
      result.status = vector::VectorStatusCode::BudgetExceeded;
      result.diagnostics.push_back(MakeDiagnostic(
          "vector.surface.budget_exceeded", DiagnosticSeverity::Error, "raster",
          parsed_->document.source.assetId,
          "requested vector surface exceeds the lane budget"));
      return result;
    }

    std::int64_t sourceTimeUs = 0;
    std::optional<std::uint64_t> sourceFrameIndex;
    vector::VectorAnimationPhase sampledPhase =
        vector::VectorAnimationPhase::Intrinsic;
    std::int64_t sampledPhaseLocalUs =
        std::max<std::int64_t>(0, request.clipLocalTimeUs);
    if (request.assetTimeUs && request.animationPhases) {
      result.status = vector::VectorStatusCode::InvalidDocument;
      result.diagnostics.push_back(MakeDiagnostic(
          "vector.time.sample_ambiguous", DiagnosticSeverity::Error, "seek",
          parsed_->document.source.assetId,
          "direct vector asset time and typed phase time are mutually "
          "exclusive"));
      return result;
    }
    if (request.assetFrameSample &&
        (!request.assetTimeUs || request.animationPhases || !parsed_->lottie)) {
      result.status = vector::VectorStatusCode::InvalidDocument;
      result.diagnostics.push_back(MakeDiagnostic(
          "vector.frame_index.sample_ambiguous",
          DiagnosticSeverity::Error, "seek",
          parsed_->document.source.assetId,
          "Qt ByProgress source-frame sampling requires one direct Lottie "
          "asset clock and no typed phase clock"));
      return result;
    }
    if (parsed_->lottie && request.animationPhases) {
      const auto withinDuration =
          [&](const vector::VectorAnimationPhaseSegment &segment) {
            return segment.sourceInUs >= 0 &&
                   segment.sourceOutUs > segment.sourceInUs &&
                   segment.sourceOutUs <= parsed_->durationUs;
          };
      const auto &phases = *request.animationPhases;
      if (!withinDuration(phases.loop) ||
          (phases.enter && !withinDuration(*phases.enter)) ||
          (phases.exit && !withinDuration(*phases.exit))) {
        result.status = vector::VectorStatusCode::InvalidDocument;
        result.diagnostics.push_back(MakeDiagnostic(
            "vector.time.phase_envelope_failed", DiagnosticSeverity::Error,
            "seek", parsed_->document.source.assetId,
            "every typed animation phase segment must stay inside the parsed "
            "asset duration"));
        return result;
      }
    }
    if (request.assetTimeUs) {
      const auto directAssetTimeUs = *request.assetTimeUs;
      if (directAssetTimeUs < 0) {
        result.status = vector::VectorStatusCode::InvalidDocument;
        result.diagnostics.push_back(MakeDiagnostic(
            "vector.time.direct_sample_failed", DiagnosticSeverity::Error,
            "seek", parsed_->document.source.assetId,
            "direct vector asset time must be non-negative"));
        return result;
      }
      if (parsed_->lottie) {
        if (parsed_->durationUs <= 0) {
          result.status = vector::VectorStatusCode::InvalidDocument;
          result.diagnostics.push_back(MakeDiagnostic(
              "vector.time.direct_sample_failed", DiagnosticSeverity::Error,
              "seek", parsed_->document.source.assetId,
              "direct vector asset time requires a positive parsed asset "
              "duration"));
          return result;
        }
        if (directAssetTimeUs >= parsed_->durationUs) {
          result.status = vector::VectorStatusCode::InvalidDocument;
          result.diagnostics.push_back(MakeDiagnostic(
              "vector.time.direct_sample_failed", DiagnosticSeverity::Error,
              "seek", parsed_->document.source.assetId,
              "direct vector asset time is outside the half-open asset "
              "duration"));
          return result;
        }
        sourceTimeUs = directAssetTimeUs;
      } else if (directAssetTimeUs != 0) {
        result.status = vector::VectorStatusCode::InvalidDocument;
        result.diagnostics.push_back(MakeDiagnostic(
            "vector.time.direct_sample_failed", DiagnosticSeverity::Error,
            "seek", parsed_->document.source.assetId,
            "a static vector asset accepts only zero direct asset time"));
        return result;
      }
    } else if (parsed_->lottie && request.animationPhases) {
      const auto sample = vector::MapVectorAnimationPhaseTime(
          *request.animationPhases, request.clipLocalTimeUs);
      if (!sample.valid || sample.sourceUs < 0 ||
          sample.sourceUs >= parsed_->durationUs) {
        result.status = vector::VectorStatusCode::InvalidDocument;
        result.diagnostics.push_back(MakeDiagnostic(
            "vector.time.phase_sample_failed", DiagnosticSeverity::Error,
            "seek", parsed_->document.source.assetId,
            sample.valid ? "vector animation phase sampled outside the "
                           "parsed asset duration"
                         : sample.error));
        return result;
      }
      sourceTimeUs = sample.sourceUs;
      sampledPhase = sample.phase;
      sampledPhaseLocalUs = sample.phaseLocalUs;
    } else if (parsed_->lottie) {
      const auto sample = vector::MapVectorTime(parsed_->document.timeMapping,
                                                request.clipLocalTimeUs);
      if (!sample.valid) {
        result.status = vector::VectorStatusCode::InvalidDocument;
        result.diagnostics.push_back(MakeDiagnostic(
            "vector.time.sample_failed", DiagnosticSeverity::Error, "seek",
            parsed_->document.source.assetId, sample.error));
        return result;
      }
      sourceTimeUs = sample.sourceUs;
    } else if (request.animationPhases) {
      result.status = vector::VectorStatusCode::InvalidDocument;
      result.diagnostics.push_back(MakeDiagnostic(
          "vector.time.phase_static_unsupported", DiagnosticSeverity::Error,
          "seek", parsed_->document.source.assetId,
          "typed animation phases require an animated vector document"));
      return result;
    }
    if (request.assetFrameSample) {
      std::uint64_t selected = 0U;
      bool selectedFrame = false;
      if (parsed_->qtFollowerDirectFrame) {
        const auto &sample = *request.assetFrameSample;
        const auto frameCount = parsed_->qtFollowerDirectFrame->frameCount;
        selectedFrame = vector::ResolveQtByProgressSourceFrame(
            sample.contractVersion, sample.progress, frameCount, selected);
        if (!selectedFrame) {
          runtimeOverrideError =
              "Qt follower direct-frame ByProgress sample is invalid";
        }
      } else {
        selectedFrame = parsed_->baseResources->SelectQtByProgressSourceFrame(
            request.assetFrameSample->contractVersion,
            request.assetFrameSample->progress, selected,
            runtimeOverrideError);
      }
      if (!selectedFrame) {
        result.status = vector::VectorStatusCode::InvalidDocument;
        result.diagnostics.push_back(MakeDiagnostic(
            "vector.frame_index.admission_failed",
            DiagnosticSeverity::Error, "seek",
            parsed_->document.source.assetId,
            runtimeOverrideError.empty()
                ? "Qt ByProgress source-frame admission is unavailable"
                : std::move(runtimeOverrideError)));
        return result;
      }
      sourceFrameIndex = selected;
      result.sampledSourceFrameIndex = selected;
    }
    if (parsed_->qtFollowerDirectFrame) {
      const auto &direct = *parsed_->qtFollowerDirectFrame;
      const Destination destination = ResolveDestination(
          parsed_->sourceWidth, parsed_->sourceHeight, request.outputWidth,
          request.outputHeight, request.viewport);
      const auto exactIdentityDestination = [](const float actual,
                                               const float expected) {
        return std::isfinite(actual) && actual == expected;
      };
      if (!normalizedOverrides.values.empty() || request.animationPhases ||
          request.outputWidth != direct.renderSize ||
          request.outputHeight != direct.renderSize ||
          !exactIdentityDestination(destination.left, 0.0F) ||
          !exactIdentityDestination(destination.top, 0.0F) ||
          !exactIdentityDestination(destination.width,
                                    static_cast<float>(direct.renderSize)) ||
          !exactIdentityDestination(destination.height,
                                    static_cast<float>(direct.renderSize))) {
        result.status = vector::VectorStatusCode::UnsupportedFeature;
        result.diagnostics.push_back(MakeDiagnostic(
            "vector.qt_follower.direct_frame_nonidentity_request",
            DiagnosticSeverity::Error, "publish",
            parsed_->document.source.assetId,
            "packed-alpha follower direct-frame only admits its exact logical-size "
            "identity publication; customization, phase envelopes and "
            "intermediate vector scaling are forbidden"));
        return result;
      }
      QtFollowerDirectFrame directFrame;
      if (!parsed_->baseResources->ResolveQtFollowerDirectFrame(
              direct.resourceLogicalId, sourceTimeUs, sourceFrameIndex,
              directFrame, runtimeOverrideError)) {
        result.status = vector::VectorStatusCode::MissingResource;
        result.diagnostics.push_back(MakeDiagnostic(
            "vector.qt_follower.direct_frame_resolve_failed",
            DiagnosticSeverity::Error, "resolve",
            parsed_->document.source.assetId,
            runtimeOverrideError.empty()
                ? "Qt follower direct associated frame is unavailable"
                : std::move(runtimeOverrideError)));
        return result;
      }
      if (directFrame.width != direct.renderSize ||
          directFrame.height != direct.renderSize ||
          directFrame.exactSourceFrameCount != direct.frameCount) {
        result.status = vector::VectorStatusCode::MissingResource;
        result.diagnostics.push_back(MakeDiagnostic(
            "vector.qt_follower.direct_frame_identity_mismatch",
            DiagnosticSeverity::Error, "resolve",
            parsed_->document.source.assetId,
            "decoded frame geometry/count disagrees with admitted metadata"));
        return result;
      }
      if (!PublishQtFollowerAssociatedFrame(
              directFrame, request.clipLocalTimeUs, generation_,
              request.cancel, result.image, runtimeOverrideError)) {
        if (runtimeOverrideError == "canceled") {
          ApplyAbort(result, RenderAbortReason::Canceled,
                     parsed_->document.source.assetId, "publish");
        } else {
          result.status = vector::VectorStatusCode::Failed;
          result.diagnostics.push_back(MakeDiagnostic(
              "vector.qt_follower.direct_frame_publish_failed",
              DiagnosticSeverity::Error, "publish",
              parsed_->document.source.assetId,
              runtimeOverrideError.empty()
                  ? "Qt follower associated frame publication failed"
                  : std::move(runtimeOverrideError)));
        }
        return result;
      }
      result.status = vector::VectorStatusCode::Ok;
      result.sampledPhase = sampledPhase;
      result.sampledPhaseLocalUs = sampledPhaseLocalUs;
      result.sampledSourceUs = sourceTimeUs;
      result.sampledSourceFrameIndex = sourceFrameIndex;
      result.originX = 0;
      result.originY = 0;
      result.rasterToOutputScaleX = 1.0F;
      result.rasterToOutputScaleY = 1.0F;
      result.logicalBounds = {0.0F, 0.0F,
                              static_cast<float>(direct.renderSize),
                              static_cast<float>(direct.renderSize)};
      result.authoredLocalBounds = result.logicalBounds;
      result.authoredLocalBoundsValid = true;
      result.inkBounds = {};
      result.inkBoundsValid = false;
      result.diagnostics = parsed_->diagnostics;
      result.diagnostics.push_back(MakeDiagnostic(
          "vector.qt_follower.direct_frame",
          DiagnosticSeverity::Information, "publish",
          parsed_->document.source.assetId,
          "published associated RGBA8 from packed-alpha identity " +
              directFrame.frameIdentity + " without Skottie rasterization"));
      result.documentGeneration = generation_;
      if (const auto abort = AbortReason(request);
          abort != RenderAbortReason::None) {
        ApplyAbort(result, abort, parsed_->document.source.assetId, "publish");
      }
      return result;
    }
    if (!ApplyVectorAnimationBindings(*parsed_, sourceTimeUs,
                                      normalizedOverrides,
                                      runtimeOverrideError)) {
      result.status = vector::VectorStatusCode::InvalidDocument;
      result.diagnostics.push_back(MakeDiagnostic(
          "vector.customization.runtime_binding_failed",
          DiagnosticSeverity::Error, "customize",
          parsed_->document.source.assetId, std::move(runtimeOverrideError)));
      return result;
    }
    const int preferredSupersample = ResolveSupersampleScale(request);
    const RenderKey key{
        generation_,         normalizedOverrides.digest, sourceTimeUs,
        sourceFrameIndex,    request.outputWidth,        request.outputHeight,
        request.viewport,    request.quality,             request.delivery,
        preferredSupersample,
        request.gpuConsumer, request.gpuDeviceIndex, request.gpuDeviceGeneration,
    };
    if (cached_ && cached_->first == key) {
      auto cachedResult = cached_->second;
      cachedResult.sampledPhase = sampledPhase;
      cachedResult.sampledPhaseLocalUs = sampledPhaseLocalUs;
      cachedResult.sampledSourceFrameIndex = sourceFrameIndex;
      if (cachedResult.image &&
          cachedResult.image.desc().timing.pts != request.clipLocalTimeUs) {
        auto timing = cachedResult.image.desc().timing;
        timing.pts = request.clipLocalTimeUs;
        const auto retimed = cachedResult.image.withTiming(timing);
        if (retimed)
          cachedResult.image = retimed.value();
      }
      if (const auto abort = AbortReason(request);
          abort != RenderAbortReason::None) {
        ApplyAbort(cachedResult, abort, parsed_->document.source.assetId,
                   "publish");
      }
      return cachedResult;
    }

    // Do not retain the previous full-frame publication while allocating a
    // supersampled working surface for the next frame.
    cached_.reset();

    try {
      const Destination destination = ResolveDestination(
          parsed_->sourceWidth, parsed_->sourceHeight, request.outputWidth,
          request.outputHeight, request.viewport);
      const RasterRegion rasterRegion = ResolveRasterRegion(
          destination, request.outputWidth, request.outputHeight);
      if (const auto abort = AbortReason(request);
          abort != RenderAbortReason::None) {
        ApplyAbort(result, abort, parsed_->document.source.assetId, "raster");
        return result;
      }

      const auto renderDocument =
          [&](SkCanvas *canvas, const float rasterScaleX,
              const float rasterScaleY, const int originX,
              const int originY) -> RenderAbortReason {
        if (const auto abort = AbortReason(request);
            abort != RenderAbortReason::None)
          return abort;
        canvas->save();
        canvas->scale(rasterScaleX, rasterScaleY);
        canvas->translate(-static_cast<float>(originX),
                          -static_cast<float>(originY));
        canvas->translate(destination.left, destination.top);
        canvas->scale(destination.width / parsed_->sourceWidth,
                      destination.height / parsed_->sourceHeight);
        if (parsed_->svg) {
          parsed_->svg->setContainerSize(
              SkSize::Make(parsed_->sourceWidth, parsed_->sourceHeight));
          parsed_->svg->render(canvas);
        } else {
          if (const auto abort = AbortReason(request);
              abort != RenderAbortReason::None) {
            canvas->restore();
            return abort;
          }
          parsed_->lottie->render(canvas);
        }
        canvas->restore();
        return AbortReason(request);
      };

      sk_sp<SkSurface> rasterSurface;
      // Lottie's straight RGBA8 coverage is an explicit CPU raster contract.
      // SVG has no such constraint and stays on the consuming GPU domain.
      const bool gpuRaster = request.gpuConsumer && !parsed_->lottie;
      std::string gpuError;
      if (gpuRaster && (!gpuContext_ ||
          gpuDeviceIndex_ != request.gpuDeviceIndex ||
          gpuDeviceGeneration_ != request.gpuDeviceGeneration)) {
        gpuContext_ = CreateSkiaGpuContext(gpuError, request.gpuDeviceIndex,
                                           request.gpuDeviceGeneration);
        gpuDeviceIndex_ = request.gpuDeviceIndex;
        gpuDeviceGeneration_ = request.gpuDeviceGeneration;
      }
      if (gpuRaster && !gpuContext_)
        return Failure(std::move(result), "vector.gpu.unavailable", "raster",
                       std::move(gpuError));
      const auto makeSurface = [&](const SkImageInfo &info) {
        return gpuRaster ? gpuContext_->MakeSurface(info, gpuError)
                         : SkSurfaces::Raster(info);
      };
      std::uint32_t rasterWidth = request.outputWidth;
      std::uint32_t rasterHeight = request.outputHeight;
      int rasterOriginX = 0;
      int rasterOriginY = 0;
      float rasterScaleX = 1.0F;
      float rasterScaleY = 1.0F;
      bool supersampled = false;
      std::string supersampleFallback;
      // Keep Lottie at four bytes per pixel while retaining edge colour:
      // Skia stores the final coverage directly as straight RGBA8, instead of
      // quantizing premultiplied RGBA8 and trying to recover RGB afterwards.
      const SkAlphaType rasterAlphaType =
          parsed_->lottie ? kUnpremul_SkAlphaType : kPremul_SkAlphaType;
      if (parsed_->lottie && WantsLottieSupersampling(request) &&
          !rasterRegion.empty()) {
        // Prefer the settle/static floor, then fall back to the live 2× path
        // before abandoning supersample entirely when the working set is tight.
        int supersampleCandidates[2] = {preferredSupersample,
                                        kLottieSupersampleScale};
        const int candidateCount =
            preferredSupersample == kLottieSupersampleScale ? 1 : 2;
        for (int candidateIndex = 0; candidateIndex < candidateCount;
             ++candidateIndex) {
          const int inkScale = supersampleCandidates[candidateIndex];
          const bool dimensionsFit =
              rasterRegion.width() <=
                  std::numeric_limits<int>::max() / inkScale &&
              rasterRegion.height() <=
                  std::numeric_limits<int>::max() / inkScale;
          const int highWidth =
              dimensionsFit ? rasterRegion.width() * inkScale : 0;
          const int highHeight =
              dimensionsFit ? rasterRegion.height() * inkScale : 0;
          std::size_t highSurfaceBytes = 0;
          const bool byteCountValid =
              dimensionsFit &&
              RgbaSurfaceBytes(static_cast<std::uint32_t>(highWidth),
                               static_cast<std::uint32_t>(highHeight),
                               highSurfaceBytes);
          const bool withinWorkingBudget =
              byteCountValid &&
              highSurfaceBytes <= options_.maximumSurfaceBytes &&
              highSurfaceBytes <= options_.maximumWorkingBytes / 2U;
          if (!withinWorkingBudget) {
            supersampleFallback =
                "the supersampled Lottie surface exceeds the working-set "
                "budget; the frame used the direct raster path";
            continue;
          }
          const auto highInfo =
              SkImageInfo::Make(highWidth, highHeight, kRGBA_8888_SkColorType,
                                rasterAlphaType, SkColorSpace::MakeSRGB());
          rasterSurface = SkSurfaces::Raster(highInfo);
          if (!rasterSurface) {
            supersampleFallback =
                "Skia could not allocate the supersampled Lottie surface; "
                "the frame used the direct raster path";
            continue;
          }
          rasterSurface->getCanvas()->clear(SK_ColorTRANSPARENT);
          if (const auto abort = renderDocument(
                  rasterSurface->getCanvas(), static_cast<float>(inkScale),
                  static_cast<float>(inkScale), rasterRegion.left,
                  rasterRegion.top);
              abort != RenderAbortReason::None) {
            ApplyAbort(result, abort, parsed_->document.source.assetId,
                       "render");
            return result;
          }
          supersampled = true;
          rasterWidth = static_cast<std::uint32_t>(highWidth);
          rasterHeight = static_cast<std::uint32_t>(highHeight);
          rasterOriginX = rasterRegion.left;
          rasterOriginY = rasterRegion.top;
          rasterScaleX = static_cast<float>(inkScale);
          rasterScaleY = static_cast<float>(inkScale);
          if (!supersampleRouteLogged_) {
            supersampleRouteLogged_ = true;
            std::fprintf(stderr,
                         "[VECTOR_RASTER] phase=supersample_publish asset=%s "
                         "quality=%u output=%ux%u crop=%dx%d scale=%d "
                         "resolve=deferred_gpu_coverage high_bytes=%zu "
                         "working_budget=%zu\n",
                         parsed_->document.source.assetId.c_str(),
                         static_cast<unsigned>(request.quality),
                         request.outputWidth, request.outputHeight,
                         rasterRegion.width(), rasterRegion.height(), inkScale,
                         highSurfaceBytes, options_.maximumWorkingBytes);
          }
          break;
        }
      }

      if (!supersampled) {
        if (!supersampleFallback.empty() && !supersampleFallbackLogged_) {
          supersampleFallbackLogged_ = true;
          std::fprintf(stderr,
                       "[VECTOR_RASTER] phase=fallback asset=%s quality=%u "
                       "output=%ux%u working_budget=%zu reason=%s\n",
                       parsed_->document.source.assetId.c_str(),
                       static_cast<unsigned>(request.quality),
                       request.outputWidth, request.outputHeight,
                       options_.maximumWorkingBytes,
                       supersampleFallback.c_str());
        }
        const bool deferredCrop =
            request.delivery ==
                vector::VectorRasterDelivery::DeferredCompositeRaster &&
            !rasterRegion.empty();
        if (deferredCrop) {
          const auto cropInfo =
              SkImageInfo::Make(rasterRegion.width(), rasterRegion.height(),
                                kRGBA_8888_SkColorType, rasterAlphaType,
                                SkColorSpace::MakeSRGB());
          rasterSurface = makeSurface(cropInfo);
          if (!rasterSurface) {
            return Failure(std::move(result),
                           "vector.surface.allocation_failed", "raster",
                           "Skia raster surface allocation failed");
          }
          rasterSurface->getCanvas()->clear(SK_ColorTRANSPARENT);
          if (const auto abort =
                  renderDocument(rasterSurface->getCanvas(), 1.0F, 1.0F,
                                 rasterRegion.left, rasterRegion.top);
              abort != RenderAbortReason::None) {
            ApplyAbort(result, abort, parsed_->document.source.assetId,
                       "render");
            return result;
          }
          rasterWidth = static_cast<std::uint32_t>(rasterRegion.width());
          rasterHeight = static_cast<std::uint32_t>(rasterRegion.height());
          rasterOriginX = rasterRegion.left;
          rasterOriginY = rasterRegion.top;
        } else {
          const auto outputInfo = SkImageInfo::Make(
              static_cast<int>(request.outputWidth),
              static_cast<int>(request.outputHeight), kRGBA_8888_SkColorType,
              rasterAlphaType, SkColorSpace::MakeSRGB());
          rasterSurface = makeSurface(outputInfo);
          if (!rasterSurface) {
            return Failure(std::move(result),
                           "vector.surface.allocation_failed", "raster",
                           "Skia raster surface allocation failed");
          }
          rasterSurface->getCanvas()->clear(SK_ColorTRANSPARENT);
          if (const auto abort =
                  renderDocument(rasterSurface->getCanvas(), 1.0F, 1.0F, 0, 0);
              abort != RenderAbortReason::None) {
            ApplyAbort(result, abort, parsed_->document.source.assetId,
                       "render");
            return result;
          }
        }
      }
      if (parsed_->baseResources->resourceFailureObserved()) {
        result.status = vector::VectorStatusCode::MissingResource;
        result.diagnostics.push_back(MakeDiagnostic(
            "vector.resource.resolve_failed", DiagnosticSeverity::Error,
            "resolve", parsed_->document.source.assetId,
            "vector rendering referenced a missing or invalid immutable "
            "resource"));
        return result;
      }
      if (const auto abort = AbortReason(request);
          abort != RenderAbortReason::None) {
        ApplyAbort(result, abort, parsed_->document.source.assetId, "publish");
        return result;
      }

      std::optional<RenderAbortReason> publicationAbort;
      const vector::CancelCheck publicationFence = [&request,
                                                    &publicationAbort] {
        const auto abort = AbortReason(request);
        if (abort != RenderAbortReason::None)
          publicationAbort = abort;
        return abort != RenderAbortReason::None;
      };
      auto published = gpuRaster
          ? gpuContext_->PublishFrame(*rasterSurface, request.clipLocalTimeUs,
                                       generation_, false, publicationFence)
          : PublishStraightRgba(
          *rasterSurface, rasterWidth, rasterHeight, request.clipLocalTimeUs,
          generation_, publicationFence);
      if (!published.frame) {
        if (published.error == "canceled") {
          ApplyAbort(result,
                     publicationAbort.value_or(RenderAbortReason::Canceled),
                     parsed_->document.source.assetId, "publish");
          return result;
        }
        return Failure(std::move(result), "vector.frame.publish_failed",
                       "publish", published.error);
      }
      result.status = vector::VectorStatusCode::Ok;
      result.image = std::move(published.frame);
      result.sampledPhase = sampledPhase;
      result.sampledPhaseLocalUs = sampledPhaseLocalUs;
      result.sampledSourceUs = sourceTimeUs;
      result.originX = rasterOriginX;
      result.originY = rasterOriginY;
      result.rasterToOutputScaleX = 1.0F / rasterScaleX;
      result.rasterToOutputScaleY = 1.0F / rasterScaleY;
      result.logicalBounds = {
          destination.left,
          destination.top,
          destination.width,
          destination.height,
      };
      result.authoredLocalBounds = {
          0.0F,
          0.0F,
          parsed_->sourceWidth,
          parsed_->sourceHeight,
      };
      result.authoredLocalBoundsValid = true;
      result.inkBounds = {};
      result.inkBoundsValid = false;
      result.diagnostics = parsed_->diagnostics;
      if (!supersampleFallback.empty()) {
        result.diagnostics.push_back(MakeDiagnostic(
            "vector.raster.supersample_fallback", DiagnosticSeverity::Warning,
            "raster", parsed_->document.source.assetId,
            std::move(supersampleFallback)));
      }
      result.documentGeneration = generation_;
      if (const auto abort = AbortReason(request);
          abort != RenderAbortReason::None) {
        ApplyAbort(result, abort, parsed_->document.source.assetId, "publish");
        return result;
      }
      if (options_.maximumCachedFrames > 0 && options_.maximumCacheBytes > 0 &&
          result.image.residentBytes() <= options_.maximumCacheBytes) {
        cached_ = std::make_pair(key, result);
      }
      return result;
    } catch (const std::exception &exception) {
      if (const auto abort = AbortReason(request);
          abort != RenderAbortReason::None) {
        ApplyAbort(result, abort, parsed_->document.source.assetId, "render");
        return result;
      }
      return Failure(std::move(result), "vector.render.exception", "render",
                     exception.what());
    } catch (...) {
      if (const auto abort = AbortReason(request);
          abort != RenderAbortReason::None) {
        ApplyAbort(result, abort, parsed_->document.source.assetId, "render");
        return result;
      }
      return Failure(std::move(result), "vector.render.unknown_failure",
                     "render", "unknown Skia vector rendering failure");
    }
  }

private:
  vector::VectorRenderResult Failure(vector::VectorRenderResult result,
                                     std::string code, std::string stage,
                                     std::string message) const {
    result.status = vector::VectorStatusCode::Failed;
    result.diagnostics.push_back(MakeDiagnostic(
        std::move(code), DiagnosticSeverity::Error, std::move(stage),
        parsed_ ? parsed_->document.source.assetId : std::string{},
        std::move(message)));
    return result;
  }

  vector::VectorRenderOptions options_;
  SkiaRuntimeConfig config_;
  std::shared_ptr<ParsedDocument> parsed_;
  std::uint64_t generation_{0};
  std::unique_ptr<SkiaGpuContext> gpuContext_;
  std::int32_t gpuDeviceIndex_{-1};
  std::uint64_t gpuDeviceGeneration_{1};
  bool supersampleRouteLogged_{false};
  bool supersampleFallbackLogged_{false};
  std::optional<std::pair<RenderKey, vector::VectorRenderResult>> cached_;
};

class VectorFactory final : public vector::VectorRendererFactory {
public:
  explicit VectorFactory(SkiaRuntimeConfig config)
      : config_(std::move(config)) {}

  vector::VectorRenderLaneHolder
  CreateLane(const vector::VectorRenderOptions &options,
             std::string &error) const override {
    error.clear();
    if (options.rendererProfile != kVectorProfile) {
      error = "unsupported vector renderer profile";
      return {};
    }
    if (options.maximumWidth == 0 || options.maximumHeight == 0 ||
        options.maximumPixels == 0 || options.maximumSurfaceBytes == 0) {
      error = "vector render lane budget is invalid";
      return {};
    }
    if (options.maximumWorkingBytes == 0) {
      error = "vector render lane working-set budget is invalid";
      return {};
    }
    return std::make_shared<VectorLane>(options, config_);
  }

private:
  SkiaRuntimeConfig config_;
};

} // namespace

vector::VectorRendererFactoryHolder
MakeVectorRendererFactory(const SkiaRuntimeConfig &config, std::string &error) {
  error.clear();
  return std::make_shared<VectorFactory>(config);
}

} // namespace videocut::skia_runtime::internal
