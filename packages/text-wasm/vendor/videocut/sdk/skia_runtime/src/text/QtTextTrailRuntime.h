#pragma once

#include "text/NativeCommandSubmission.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace videocut::skia_runtime::internal {

inline constexpr std::uint32_t kQtTextTrailImplementationVersion = 1U;
inline constexpr std::uint32_t kQtTextTrailStateSchemaVersion = 1U;
inline constexpr char kQtTextTrailImplementationId[] =
    "videocut.text-post.qt-lumi-trail";
inline constexpr char kQtTextTrailSourceContractDigest[] =
    "sha256-tree-v1:"
    "e84eac0c16630e63f158c286c3ef2be5a4245cb5ee0b4639fe67fe444a626c11";

// This ABI is deliberately timestamp-free. Every successful Render call is
// one Qt onUpdate/render sample. Seeking is represented only by restoring an
// exact checkpoint or by resetting and replaying all samples in order.
enum class QtTextTrailDirectionContract : std::uint32_t {
  TopLeftRowsNegativeHeightViewport = 1U,
};

enum class QtTextTrailPixelFormat : std::uint32_t {
  Rgba8Unorm = 70U,
  Rgba32Float = 125U,
};

enum class QtTextTrailResetReason : std::uint32_t {
  None = 0U,
  NewLifecycle,
  DimensionsChanged,
  IncompatibleVersion,
  IncompatibleCheckpoint,
  SequentialReplay,
};

struct QtTextTrailRawRgba8ImageView final {
  const std::uint8_t *pixels{nullptr};
  std::size_t byteSize{0U};
  int width{0};
  int height{0};
  std::size_t rowBytes{0U};
  /// Borrowed bottom-left RGBA8 texture, valid through Render encoding.
  void *nativeTexture{nullptr};
};

struct QtTextTrailTextureTarget final {
  void *texture{nullptr};
  void *commandQueue{nullptr};
  NativeCommandSubmission submit;
};

struct QtTextTrailStateIdentity final {
  std::uint64_t renderGraphInstanceId{0U};
  std::uint64_t effectNodeInstanceId{0U};
  std::uint64_t lifecycleEpoch{0U};
  std::string implementationId{kQtTextTrailImplementationId};
  std::uint32_t implementationVersion{kQtTextTrailImplementationVersion};
  std::uint32_t stateSchemaVersion{kQtTextTrailStateSchemaVersion};
  std::string sourceContractDigest{kQtTextTrailSourceContractDigest};
};

struct QtTextTrailParameters final {
  float blur{0.0F};
  float weaken{0.2F};
  int hintProfile{0};
  float hintHue{0.0F};
  float hintOffset{0.0F};
  float hintMin{0.0F};
  float hintMax{1.0F};
  bool baseEnabled{true};
  std::array<float, 4> baseHint{1.0F, 1.0F, 1.0F, 1.0F};
};

struct QtTextTrailRenderRequest final {
  QtTextTrailStateIdentity identity;
  QtTextTrailRawRgba8ImageView source;
  QtTextTrailParameters parameters;
  QtTextTrailDirectionContract directionContract{
      QtTextTrailDirectionContract::TopLeftRowsNegativeHeightViewport};
  /// Native source and target must be supplied together on the same device.
  QtTextTrailTextureTarget target;
};

struct QtTextTrailPassTrace final {
  std::string name;
  QtTextTrailPixelFormat outputFormat{QtTextTrailPixelFormat::Rgba8Unorm};
  bool blendingEnabled{false};
  bool linearClampSampler{true};
  bool negativeHeightViewport{true};
  std::uint32_t primitiveTypeMetalValue{3U};
  std::uint32_t indexTypeMetalValue{1U};
  std::uint32_t elementCount{6U};
  std::uint32_t instanceCount{1U};
  std::uint32_t loadActionMetalValue{1U};
  std::uint32_t storeActionMetalValue{1U};
  std::uint32_t colorWriteMaskMetalValue{15U};
};

struct QtTextTrailRenderResult final {
  std::vector<std::uint8_t> outputPixels;
  int width{0};
  int height{0};
  std::uint64_t resourceGeneration{0U};
  std::uint64_t committedSampleOrdinal{0U};
  QtTextTrailResetReason resetReason{QtTextTrailResetReason::None};
  std::vector<QtTextTrailPassTrace> passes;
};

// TimeB is serialized as tightly packed logical top-left RGBA32Float pixels.
// All four channels are retained even though v1 writes float4(history); this
// prevents a future schema from silently narrowing the committed state.
struct QtTextTrailCheckpoint final {
  QtTextTrailStateIdentity identity;
  std::uint32_t implementationVersion{kQtTextTrailImplementationVersion};
  std::uint32_t stateSchemaVersion{kQtTextTrailStateSchemaVersion};
  std::string sourceContractDigest{kQtTextTrailSourceContractDigest};
  int width{0};
  int height{0};
  std::uint64_t committedSampleOrdinal{0U};
  std::vector<float> committedTimeB;
};

class QtTextTrailRuntime {
public:
  virtual ~QtTextTrailRuntime() = default;

  QtTextTrailRuntime(const QtTextTrailRuntime &) = delete;
  QtTextTrailRuntime &operator=(const QtTextTrailRuntime &) = delete;

  [[nodiscard]] virtual bool Render(const QtTextTrailRenderRequest &request,
                                    QtTextTrailRenderResult &result,
                                    std::string &error) = 0;

  [[nodiscard]] virtual bool SaveCheckpoint(QtTextTrailCheckpoint &checkpoint,
                                            std::string &error) const = 0;

  // Any incompatible checkpoint is rejected and clears resident history. The
  // next valid Render starts at a proven reset boundary and reports
  // IncompatibleCheckpoint.
  [[nodiscard]] virtual bool
  RestoreCheckpoint(const QtTextTrailStateIdentity &expectedIdentity,
                    const QtTextTrailCheckpoint &checkpoint,
                    std::string &error) = 0;

  // Starts the other valid random-access path: the host then calls Render for
  // every actual sample from the reset boundary through the target sample.
  [[nodiscard]] virtual bool
  ResetForSequentialReplay(const QtTextTrailStateIdentity &identity,
                           std::string &error) = 0;

  [[nodiscard]] virtual const char *BackendName() const noexcept = 0;

protected:
  QtTextTrailRuntime() = default;
};

// Returns null only when the versioned native backend is unavailable. Once a
// runtime exists, validation or execution errors are surfaced and never
// replaced with translated-copy or absolute-time approximations.
[[nodiscard]] std::unique_ptr<QtTextTrailRuntime>
CreateQtTextTrailRuntime(std::string &error);

} // namespace videocut::skia_runtime::internal
