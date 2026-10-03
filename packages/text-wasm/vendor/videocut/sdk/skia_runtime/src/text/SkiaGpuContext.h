#pragma once

#include "internal/skia/SkiaHeaders.h"
#include "raster/SkiaFramePublication.h"
#include "text/NativeCommandSubmission.h"
#include "videocut/text/RichTextDocument.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace videocut::skia_runtime::internal {

class QtTextEngineCopyMetalRuntime;
class QtTextLetterMetalRuntime;
struct QtTextLetterRenderRequest;

// Platform-neutral command payload for TextPro-compatible distance-field
// generation. Positions are normalized against targetWidth/targetHeight so
// the native backend can preserve the reference Metal viewport and pixel
// center convention without depending on private Skia GPU objects.
struct TextSdfDistanceVertex final {
  float positionX{0.0F};
  float positionY{0.0F};
  float parabolaX{0.0F};
  float parabolaY{0.0F};
  float limitBegin{0.0F};
  float limitEnd{0.0F};
  float distanceScale{1.0F};
  float distanceLimit{1.0F};
};

struct TextSdfShapeVertex final {
  float positionX{0.0F};
  float positionY{0.0F};
  float parabolaX{0.0F};
  float parabolaY{1.0F};
};

// Renderer-local shaped identity carried with the generated mesh. It binds the
// atlas bytes to the exact paragraph/run/cluster/glyph that produced them and
// is deliberately independent of mutable glyph geometry.
struct TextSdfGlyphIdentity final {
  std::uint64_t stableUnitId{0U};
  std::string paragraphId;
  std::string runId;
  std::size_t paragraphUtf8Cluster{0U};
  std::size_t runUtf8Cluster{0U};
  std::size_t glyphIndexInCluster{0U};
  std::uint16_t glyphId{0U};
};

struct TextSdfGpuMesh final {
  TextSdfGlyphIdentity glyphIdentity{};
  int targetWidth{0};
  int targetHeight{0};
  int outputX{0};
  int outputY{0};
  int outputWidth{0};
  int outputHeight{0};
  std::vector<TextSdfDistanceVertex> distanceVertices;
  std::vector<TextSdfShapeVertex> shapeVertices;
};

enum class TextSdfCoordinateDomain : std::uint8_t {
  GlyphRect = 0,
  LineRect,
  TextRect,
};

struct TextSdfGpuRect final {
  float x{0.0F};
  float y{0.0F};
  float width{0.0F};
  float height{0.0F};
};

// One glyph-material draw. glyphAtlasRect is the fractional canonical SDF
// cell used only for distance UVs. glyphLocalRect is its authored-space draw
// quad, while glyphMaterialRect is the unpadded ink domain used by grapheme
// textures/gradients; lineRect and textRect share that authored coordinate
// system. The matrix maps authored coordinates into the presentation.
// A material texture, when present, has already been resolved to the glyph's
// authored atlas cell by the render lane. An editable slot's replacement mask
// is independent so texture materials can still be locally replaced.
struct TextSdfMaterialGpuRequest final {
  TextSdfGpuMesh mesh{};
  TextSdfGpuRect glyphAtlasRect{};
  TextSdfGpuRect glyphLocalRect{};
  TextSdfGpuRect glyphMaterialRect{};
  TextSdfGpuRect lineRect{};
  TextSdfGpuRect textRect{};
  TextSdfGpuRect replacementMaskRect{};
  TextSdfCoordinateDomain coordinateDomain{
      TextSdfCoordinateDomain::GlyphRect};
  std::uint32_t materialIndex{0U};
  std::array<float, 16> localToPresentation{
      1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
      0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
  // TextPro keeps this two-stage float32 vertex projection separate. The
  // algebraically equivalent localToPresentation path rounds differently at
  // glyph edges, so the render lane supplies the reference-derived packet.
  std::array<float, 16> qtLetterPositions{};
  std::array<float, 16> qtLetterMvp{
      1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
      0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
  std::array<float, 2> qtLetterOffset{};
  std::array<float, 2> qtLetterPolarOffset{};
  bool qtLetterPolarOffsetEnabled{false};
  std::array<float, 2> qtInnerShadowUvOffset{};
  bool qtInnerShadowUvOffsetEnabled{false};
  text::TextGlyphMaterialLayer material{text::TextFillLayer{}};
  sk_sp<SkImage> texture;
  sk_sp<SkImage> replacementMask;
  // TextPro's material coordinate is independent from the nominal outline
  // scale used to generate the distance field. Keep both recovered constants
  // explicit so the GPU never infers the authored atlas scale from tight
  // glyph rectangles.
  float authoredAtlasScale{1.0F};
  float materialCoordinateExtent{1.0F};
  float materialDistanceRange{172.0F};
  float smoothingScale{0.6F};
  float sdfBlurRadius{0.0F};
  float presentationOpacity{1.0F};
  // Effect-program instance color modulates the typed material after texture
  // or gradient sampling; it must never replace the authored material.
  text::Color presentationTint{1.0F, 1.0F, 1.0F, 1.0F};
  text::TextBlendMode presentationBlend{text::TextBlendMode::SourceOver};
};

// Versioned ABI for AmazingEngine's final Page RenderGroup composite. Both
// matrices retain Metal's column-major float4x4 layout and remain separate so
// the GPU preserves the captured u_MVP * u_CustomMat float32 operation order.
inline constexpr std::uint32_t
    kQtTextRenderGroupCompositeImplementationVersion = 1U;

struct QtTextRenderGroupCompositeRequest final {
  std::uint32_t implementationVersion{
      kQtTextRenderGroupCompositeImplementationVersion};
  int presentationWidth{0};
  int presentationHeight{0};
  std::array<float, 16> mvp{};
  std::array<float, 16> customMatrix{};
  float alpha{1.0F};
};

// Public, template-independent transport for Qt VideoAnimSeq follower
// sprites. imageToPresentation maps top-left image pixels into the final
// presentation surface. The source image carries the captured raw RGBA8
// associated transport and must be sampled by Metal without a Skia color
// image conversion.
inline constexpr std::uint32_t kQtTextFollowerCompositeImplementationVersion =
    1U;

struct QtTextFollowerCompositeRequest final {
  std::uint32_t implementationVersion{
      kQtTextFollowerCompositeImplementationVersion};
  int presentationWidth{0};
  int presentationHeight{0};
  std::array<float, 9> imageToPresentation{};
  float opacity{1.0F};
};

// Template-independent transport for a closed VAT mesh draw. The render lane
// resolves animation resources and projection into clip-space vertices; the
// native backend preserves the authored perspective interpolation and
// transparent source-over mesh pass.
inline constexpr std::uint32_t kTextVatMeshImplementationVersion = 1U;

struct TextVatMeshVertex final {
  float clipX{0.0F};
  float clipY{0.0F};
  float clipZ{0.0F};
  float clipW{1.0F};
  float textureU{0.0F};
  float textureV{0.0F};
};

struct TextVatMeshGpuRequest final {
  std::uint32_t implementationVersion{kTextVatMeshImplementationVersion};
  int targetWidth{0};
  int targetHeight{0};
  std::vector<TextVatMeshVertex> vertices;
  std::vector<std::uint16_t> indices;
  float opacity{1.0F};
};

class SkiaGpuContext {
public:
  virtual ~SkiaGpuContext() = default;

  SkiaGpuContext(const SkiaGpuContext &) = delete;
  SkiaGpuContext &operator=(const SkiaGpuContext &) = delete;

  [[nodiscard]] virtual sk_sp<SkSurface>
  MakeSurface(const SkImageInfo &info, std::string &error) = 0;

  [[nodiscard]] virtual PublishedFrame PublishFrame(
      SkSurface &surface, std::int64_t timestampUs, std::uint64_t generation,
      bool premultiplied, const std::function<bool()> &cancel) = 0;
  /// Explicit CPU consumers observe the same native failure receipts before
  /// reading pixels. GPU publication never invokes this wait.
  [[nodiscard]] virtual bool WaitForSubmittedWork(std::string &error) = 0;

  [[nodiscard]] virtual sk_sp<SkImage> RenderQtTextEngineCopy(
      QtTextEngineCopyMetalRuntime &runtime, const sk_sp<SkImage> &source,
      int width, int height, std::string &error) = 0;

  using NativeRgba8Pass =
      std::function<bool(void *, void *, void *,
                         const NativeCommandSubmission &, std::string &)>;
  // Raw bottom-left RGBA8 textures on the Ganesh queue. The callback commits
  // its native commands before returning; no CPU pixel transfer is required.
  [[nodiscard]] virtual sk_sp<SkImage> RenderQtTextRawPass(
      const sk_sp<SkImage> &source, int width, int height,
      const NativeRgba8Pass &render, std::string &error) = 0;

  // Letter and Ganesh share an ordered queue; the published image retains the
  // bottom-left texture until its last GPU consumer has completed.
  [[nodiscard]] virtual sk_sp<SkImage> RenderQtTextLetter(
      QtTextLetterMetalRuntime &runtime, const QtTextLetterRenderRequest &request,
      sk_sp<SkColorSpace> colorSpace, std::string &error,
      std::vector<std::uint8_t> *diagnosticPixels = nullptr) = 0;

  // Retained Metal-row atlas on the Letter/Ganesh queue. Only the packed
  // cells are populated; optional RG8 bytes are for explicit diagnostics.
  [[nodiscard]] virtual std::shared_ptr<void> RenderTextSdfAtlas(
      const TextSdfGpuMesh &mesh, const std::vector<SkIRect> &cells,
      std::string &error,
      std::vector<std::uint8_t> *diagnosticRg8 = nullptr) = 0;

  // Generates the private signed-distance target and composites the typed
  // material into destination on the GPU. This method never exposes or reads
  // back the distance target.
  [[nodiscard]] virtual bool RenderTextSdfMaterial(
      const TextSdfMaterialGpuRequest &request, SkSurface &destination,
      std::string &error) = 0;

  // pageImage and presentationTarget must resolve into this context's Metal
  // device. The Apple implementation uploads a raster page at most once, then
  // submits the exact versioned RenderGroup draw on Ganesh's own command
  // queue; GPU-backed pages and targets remain zero-copy.
  [[nodiscard]] virtual bool CompositeQtTextRenderGroup(
      const sk_sp<SkImage> &pageImage, SkSurface &presentationTarget,
      const QtTextRenderGroupCompositeRequest &request,
      std::string &error) = 0;

  [[nodiscard]] virtual bool CompositeQtTextFollower(
      const sk_sp<SkImage> &followerImage, SkSurface &presentationTarget,
      const QtTextFollowerCompositeRequest &request,
      std::string &error) = 0;

  [[nodiscard]] virtual bool RenderTextVatMesh(
      const sk_sp<SkImage> &source, const TextVatMeshGpuRequest &request,
      SkSurface &destination, std::string &error) = 0;

protected:
  SkiaGpuContext() = default;
};

[[nodiscard]] std::unique_ptr<SkiaGpuContext>
CreateSkiaGpuContext(std::string &error, std::int32_t deviceIndex = -1,
                     std::uint64_t deviceGeneration = 1);

[[nodiscard]] bool NativeTextGpuRequired() noexcept;

} // namespace videocut::skia_runtime::internal
