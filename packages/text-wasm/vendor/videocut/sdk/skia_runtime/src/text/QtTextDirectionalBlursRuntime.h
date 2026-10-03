#pragma once

#include "text/NativeCommandSubmission.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace videocut::skia_runtime::internal {

inline constexpr std::uint32_t kQtTextDirectionalBlursImplementationVersion =
    1U;
inline constexpr std::uint32_t kQtTextDirectionalBlursContractSchemaVersion =
    1U;
inline constexpr char kQtTextDirectionalBlursImplementationId[] =
    "videocut.text-post.qt-lumi-directional-blurs-alpha-outline";

// sha256-contract-v1 is the digest of the audited source/capture closure
// documented by QtTextDirectionalBlursContractProbe. The shader and Lua
// contracts derive every RT size and coordinate from the current Page; the
// original 755x437 capture remains a fixture, not an execution restriction.
inline constexpr char kQtTextDirectionalBlursSourceContractDigest[] =
    "sha256-contract-v1:"
    "2584577c2a4af3fb3bf4f2326a41103ff6cab70a43a237e27e750998e9177d00";

// DirectionalBlurs with directionNum=1 is a different Qt render topology,
// not a revision of the fused DirectionalBlurs -> AlphaOutline chain above.
// Qt routes CameraDirectionalBlurs_1 directly to OutputTex, so the native
// executor below materializes only EngineCopy + DirectionalGaussian.
inline constexpr std::uint32_t
    kQtTextDirectionalBlurStandaloneImplementationVersion = 1U;
inline constexpr std::uint32_t
    kQtTextDirectionalBlurStandaloneContractSchemaVersion = 1U;
inline constexpr char kQtTextDirectionalBlurStandaloneImplementationId[] =
    "videocut.text-post.qt-lumi-directional-blur";
inline constexpr char kQtTextDirectionalBlurStandaloneSourceTreeDigestA[] =
    "sha256-tree-v1:"
    "a00f919b44f5641bdffdd5e63956230b03c94a278ce02df33af12bc71f1b5f62";
inline constexpr char kQtTextDirectionalBlurStandaloneSourceTreeDigestB[] =
    "sha256-tree-v1:"
    "a13c7d2337924f8a0cdaaa37cb32dfbb6a3578d7bd33b1e9b9b786ef0e86b51d";
inline constexpr std::size_t kQtTextDirectionalBlurStandalonePassCount = 2U;

inline constexpr int kQtTextDirectionalBlursPageWidth = 755;
inline constexpr int kQtTextDirectionalBlursPageHeight = 437;
inline constexpr std::size_t kQtTextDirectionalBlursPassCount = 7U;

enum class QtTextDirectionalBlursDirectionContract : std::uint32_t {
  TopLeftRowsNegativeHeightViewport = 1U,
};

enum class QtTextDirectionalBlursPixelFormat : std::uint32_t {
  Rgba8Unorm = 70U,
};

enum class QtTextDirectionalBlursBorderType : std::int32_t {
  Normal = 0,
  Black = 1,
  Mirror = 2,
};

enum class QtTextDirectionalBlursBlendMode : std::int32_t {
  Screen = 0,
  Add = 1,
  Mean = 2,
};

struct QtTextDirectionalBlursIdentity final {
  std::string implementationId{kQtTextDirectionalBlursImplementationId};
  std::uint32_t implementationVersion{
      kQtTextDirectionalBlursImplementationVersion};
  std::uint32_t contractSchemaVersion{
      kQtTextDirectionalBlursContractSchemaVersion};
  std::string sourceContractDigest{kQtTextDirectionalBlursSourceContractDigest};
};

struct QtTextDirectionalBlursRawRgba8ImageView final {
  const std::uint8_t *pixels{nullptr};
  std::size_t byteSize{0U};
  int width{0};
  int height{0};
  std::size_t rowBytes{0U};
  // Borrowed bottom-left RGBA8 texture for the explicit native target path.
  void *nativeTexture{nullptr};
};

struct QtTextDirectionalBlursParameters final {
  // Values are kept in binary64 until the material/Metal boundary.
  double blurIntensity{0.0};
  double angleDegrees{0.0};
  std::int32_t directionNum{4};
  double exposure{1.0};
  double quality{0.5};
  double spaceDither{0.0};
  QtTextDirectionalBlursBorderType borderType{
      QtTextDirectionalBlursBorderType::Normal};
  QtTextDirectionalBlursBlendMode blendMode{
      QtTextDirectionalBlursBlendMode::Mean};
};

struct QtTextAlphaOutlineParameters final {
  double offsetX{0.0};
  double offsetY{0.0};
  double size{1.0};
  double scaleX{1.0};
  double scaleY{1.0};
  std::array<double, 4> outlineColor{0.2, 0.2, 0.2, 1.0};
  double intensity{1.0};
};

struct QtTextDirectionalBlursContractRequest final {
  QtTextDirectionalBlursIdentity identity;
  int pageWidth{kQtTextDirectionalBlursPageWidth};
  int pageHeight{kQtTextDirectionalBlursPageHeight};
  QtTextDirectionalBlursParameters directional;
  QtTextAlphaOutlineParameters alphaOutline;
  QtTextDirectionalBlursDirectionContract directionContract{
      QtTextDirectionalBlursDirectionContract::
          TopLeftRowsNegativeHeightViewport};
};

// The layout of these three structures is the captured Metal setBytes ABI.
// Contract probes assert every phase-25 word rather than only the formulas.
struct alignas(16) QtTextDirectionalBlursUniforms final {
  float screenParams[4]{};
  float sample{0.0F};
  float sigma{0.0F};
  float spaceDither{0.0F};
  float stepX{0.0F};
  float stepY{0.0F};
  std::int32_t borderType{0};
  std::int32_t directionNum{0};
  float exposure{0.0F};
};

struct QtTextDirectionalBlursBlendUniforms final {
  std::int32_t blendMode{0};
  std::int32_t directionNum{0};
  float exposure{0.0F};
};

struct alignas(16) QtTextAlphaOutlineUniforms final {
  float offsetX{0.0F};
  float offsetY{0.0F};
  float ratio{0.0F};
  float size{0.0F};
  float scaleX{0.0F};
  float scaleY{0.0F};
  float alignmentPadding0[2]{};
  float outlineColor[4]{};
  float intensity{0.0F};
  float alignmentPadding1[3]{};
};

struct QtTextDirectionalBlursPassTrace final {
  std::string name;
  int inputWidth{0};
  int inputHeight{0};
  int outputWidth{0};
  int outputHeight{0};
  std::uint32_t inputTextureCount{0U};
  QtTextDirectionalBlursPixelFormat outputFormat{
      QtTextDirectionalBlursPixelFormat::Rgba8Unorm};
  std::uint32_t storageModeMetalValue{2U};
  bool blendingEnabled{false};
  bool normalizedCoordinates{true};
  bool linearMinFilter{true};
  bool linearMagFilter{true};
  bool mipFilterDisabled{true};
  bool clampToEdgeS{true};
  bool clampToEdgeT{true};
  bool negativeHeightViewport{true};
  bool innerTextureFlip{false};
  bool indexed{true};
  std::uint32_t primitiveTypeMetalValue{3U};
  std::uint32_t indexTypeMetalValue{1U};
  std::uint32_t cullModeMetalValue{0U};
  std::uint32_t frontFacingWindingMetalValue{0U};
  std::uint32_t elementCount{6U};
  std::uint32_t instanceCount{1U};
  std::uint32_t loadActionMetalValue{2U};
  std::uint32_t storeActionMetalValue{1U};
  std::uint32_t colorWriteMaskMetalValue{15U};
  std::array<float, 4> clearColor{0.0F, 0.0F, 0.0F, 0.0F};
};

struct QtTextDirectionalBlursDerivedContract final {
  int pageWidth{0};
  int pageHeight{0};
  int downsampleWidth{0};
  int downsampleHeight{0};
  double downsampleScale{0.0};
  double sampleNumberBeforeQuality{0.0};
  double qualityParameter{0.0};
  double sampleNumber{0.0};
  double baseSize{0.0};
  double xScale{0.0};
  double yScale{0.0};
  double radius{0.0};
  double sigma{0.0};
  double sampleDistance{0.0};
  std::array<QtTextDirectionalBlursUniforms, 4> directionalUniforms{};
  QtTextDirectionalBlursBlendUniforms blendUniforms{};
  QtTextAlphaOutlineUniforms alphaOutlineUniforms{};
  std::array<QtTextDirectionalBlursPassTrace,
             kQtTextDirectionalBlursPassCount>
      passes{};
};

struct QtTextDirectionalBlurStandaloneIdentity final {
  std::string implementationId{
      kQtTextDirectionalBlurStandaloneImplementationId};
  std::uint32_t implementationVersion{
      kQtTextDirectionalBlurStandaloneImplementationVersion};
  std::uint32_t contractSchemaVersion{
      kQtTextDirectionalBlurStandaloneContractSchemaVersion};
  std::string sourceContractDigest{
      kQtTextDirectionalBlurStandaloneSourceTreeDigestA};
};

struct QtTextDirectionalBlurStandaloneParameters final {
  // Authored numbers remain binary64 until the Metal material boundary.
  double blurIntensity{0.0};
  double angleDegrees{0.0};
  std::int32_t directionNum{1};
  double exposure{1.0};
  double quality{0.5};
  double spaceDither{0.0};
  QtTextDirectionalBlursBorderType borderType{
      QtTextDirectionalBlursBorderType::Normal};
  // Authored and persisted by Qt, but deliberately unused when directionNum
  // is one because CameraBlend is hidden in that branch.
  QtTextDirectionalBlursBlendMode blendMode{
      QtTextDirectionalBlursBlendMode::Mean};
};

struct QtTextDirectionalBlurStandaloneContractRequest final {
  QtTextDirectionalBlurStandaloneIdentity identity;
  int pageWidth{0};
  int pageHeight{0};
  QtTextDirectionalBlurStandaloneParameters directional;
  QtTextDirectionalBlursDirectionContract directionContract{
      QtTextDirectionalBlursDirectionContract::
          TopLeftRowsNegativeHeightViewport};
};

struct QtTextDirectionalBlurStandaloneDerivedContract final {
  int pageWidth{0};
  int pageHeight{0};
  int downsampleWidth{0};
  int downsampleHeight{0};
  double downsampleScale{0.0};
  double sampleNumberBeforeQuality{0.0};
  double qualityParameter{0.0};
  double sampleNumber{0.0};
  double baseSize{0.0};
  double xScale{0.0};
  double yScale{0.0};
  double radius{0.0};
  double sigma{0.0};
  double sampleDistance{0.0};
  QtTextDirectionalBlursUniforms directionalUniforms{};
  std::array<float, 4> textureFlip{0.0F, 0.0F, 0.0F, 0.0F};
  std::array<QtTextDirectionalBlursPassTrace,
             kQtTextDirectionalBlurStandalonePassCount>
      passes{};
};

[[nodiscard]] bool ResolveQtTextDirectionalBlurStandaloneContract(
    const QtTextDirectionalBlurStandaloneContractRequest &request,
    QtTextDirectionalBlurStandaloneDerivedContract &derived,
    std::string &error);

// Resolves the exact native material contract without touching a GPU. Unknown
// implementation identities, dimensions, options, or authored domains fail;
// v1 never substitutes a translated-copy approximation.
[[nodiscard]] bool ResolveQtTextDirectionalBlursContract(
    const QtTextDirectionalBlursContractRequest &request,
    QtTextDirectionalBlursDerivedContract &derived, std::string &error);

struct QtTextDirectionalBlursRenderRequest final {
  QtTextDirectionalBlursContractRequest contract;
  QtTextDirectionalBlursRawRgba8ImageView source;
};

struct QtTextDirectionalBlursRenderResult final {
  std::vector<std::uint8_t> outputPixels;
  int width{0};
  int height{0};
  QtTextDirectionalBlursDerivedContract contract;
};

struct QtTextDirectionalBlurStandaloneRenderRequest final {
  QtTextDirectionalBlurStandaloneContractRequest contract;
  QtTextDirectionalBlursRawRgba8ImageView source;
};

struct QtTextDirectionalBlurStandaloneRenderResult final {
  std::vector<std::uint8_t> outputPixels;
  int width{0};
  int height{0};
  QtTextDirectionalBlurStandaloneDerivedContract contract;
};

class QtTextDirectionalBlursRuntime {
public:
  virtual ~QtTextDirectionalBlursRuntime() = default;

  QtTextDirectionalBlursRuntime(const QtTextDirectionalBlursRuntime &) = delete;
  QtTextDirectionalBlursRuntime &
  operator=(const QtTextDirectionalBlursRuntime &) = delete;

  [[nodiscard]] virtual bool
  Render(const QtTextDirectionalBlursRenderRequest &request,
         QtTextDirectionalBlursRenderResult &result, std::string &error,
         const NativeRgba8TextureTarget *target = nullptr) = 0;

  [[nodiscard]] virtual bool RenderStandalone(
      const QtTextDirectionalBlurStandaloneRenderRequest &request,
      QtTextDirectionalBlurStandaloneRenderResult &result,
      std::string &error,
         const NativeRgba8TextureTarget *target = nullptr) = 0;

  [[nodiscard]] virtual const char *BackendName() const noexcept = 0;

protected:
  QtTextDirectionalBlursRuntime() = default;
};

// Returns null only when the audited Metal backend is unavailable. Once a
// runtime exists, contract or execution errors are surfaced verbatim and are
// never replaced by the historical translated-copy fallback.
[[nodiscard]] std::unique_ptr<QtTextDirectionalBlursRuntime>
CreateQtTextDirectionalBlursRuntime(std::string &error);

} // namespace videocut::skia_runtime::internal
