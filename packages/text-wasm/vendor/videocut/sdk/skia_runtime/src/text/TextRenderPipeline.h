#pragma once

// Private render-pipeline contracts. No editor or tools dependency.
#include "text/TextExecutionParameterContract.h"
#include "text/TextRuntimeShader.h"
#include "internal/Factories.h"

#include "internal/skia/SkiaHeaders.h"
#include "text/TextFontContext.h"
#include "text/TextRenderIdentity.h"
#include "text/TextRenderFramePlan.h"
#include "codec/CanonicalRgbaAdapter.h"
#include "raster/SkiaFramePublication.h"
#include "resources/SkiaResourceProvider.h"
#include "text/FontInstanceResolver.h"
#include "text/FlowerAppearanceComposition.h"
#include "text/QtTextLetterMetalRuntime.h"
#include "text/QtTextProPostEffectPipeline.h"
#include "text/QtTextTurbulenceMetalRuntime.h"
#include "text/SkiaGpuContext.h"
#include "text/TextEffectGeometryContract.h"
#include "text/TextEffectFontSizeReflow.h"
#include "text/TextGlyphOutlineCapture.h"
#include "text/TextProLetterTransformF32.h"
#include "text/TextSdfMeshBuilder.h"

#include "videocut/text/TextEffectEvaluator.h"
#include "videocut/vector/VectorDigest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace videocut::skia_runtime::internal::text_lane {
using videocut::text::Diagnostic;

using videocut::text::DiagnosticSeverity;

using Paragraph = skia::textlayout::Paragraph;

using ParagraphBuilder = skia::textlayout::ParagraphBuilder;

using SkParagraphStyle = skia::textlayout::ParagraphStyle;

using SkTextStyle = skia::textlayout::TextStyle;

constexpr std::size_t kMaximumDecodedAssetBytes = 64U * 1024U * 1024U;

constexpr std::size_t kMaximumDecodedAssetPixels = 64U * 1024U * 1024U;

std::uint64_t NextTextPostEffectGraphInstanceId() noexcept;

Diagnostic MakeDiagnostic(std::string code, DiagnosticSeverity severity,
                          std::string stage, std::string subject,
                          std::string message);

bool IsCanceled(const text::CancelCheck &cancel);

std::uint8_t ColorByte(const float value);

SkColor ToSkColor(const text::Color &color, const float opacity = 1.0F);

SkBlendMode ToSkBlendMode(const text::TextBlendMode mode) noexcept;

SkFontStyle ToSkFontStyle(const text::FontSpec &font);

std::string TextureKey(const text::TextureReference &reference);

bool IsSvgMediaType(const std::string_view mediaType) noexcept;

bool IsLottieMediaType(const std::string_view mediaType) noexcept;

bool IsStreamingMediaType(const std::string_view mediaType) noexcept;

RuntimeAssetKind RuntimeKindForMediaType(const std::string_view mediaType);

RuntimeAssetKind RuntimeKindForFrameResource(
    const text::TextEffectResourceKind kind) noexcept;

text::TextAssetKind ProjectKindForFrameResource(
    const text::TextEffectResourceKind kind) noexcept;

struct ResolvedVisualAsset final {
  text::TextureReference reference{};
  std::string mediaType;
  std::shared_ptr<const std::vector<std::uint8_t>> bytes;
  sk_sp<SkImage> raster;
  sk_sp<SkSVGDOM> svg;
  sk_sp<skottie::Animation> lottie;
  sk_sp<skresources::ResourceProvider> lottieResources;
  sk_sp<ImmutableResourceProvider> immutableLottieResources;
  std::string qtFollowerResourceId;
  std::uint64_t qtFollowerFrameCount{0U};
  std::uint32_t qtFollowerRenderSize{0U};
  std::shared_ptr<RuntimeAnimatedImageSource> stream;
  float intrinsicWidth{0.0F};
  float intrinsicHeight{0.0F};
  std::int64_t durationUs{0};
  std::int64_t sampledTimeUs{std::numeric_limits<std::int64_t>::min()};
  sk_sp<SkImage> sampledFrame;
  std::string sampledFrameIdentity;
};

enum class RasterDecodeUsage : std::uint8_t {
  Default = 0,
  QtStretchableBubble,
};

class TextVisualAssetStore final {
public:
  TextVisualAssetStore(const SkiaRuntimeConfig &config,
                       text::TextAssetResolver::Holder projectAssets);

  void Reset();

  [[nodiscard]] std::size_t residentBytes() const noexcept;

  ResolvedVisualAsset *Resolve(const text::TextureReference &reference,
                               std::string &error,
                               const RasterDecodeUsage usage =
                                   RasterDecodeUsage::Default);

  ResolvedVisualAsset *ResolveUnqualified(const std::string &assetId,
                                          const std::string &digest,
                                          std::string &error);

  sk_sp<SkImage> SampleRaster(ResolvedVisualAsset &asset,
                              const std::int64_t localTimeUs,
                              std::string &sampleIdentity,
                              std::string &error);

  sk_sp<SkImage> SampleQtFollowerDirectRaster(
      ResolvedVisualAsset &asset, const std::int64_t localTimeUs,
      std::string &sampleIdentity, std::string &error);

  bool AdmitOpaque(const std::string &assetId, const std::string &digest,
                   const RuntimeAssetKind builtinKind,
                   const text::TextAssetKind projectKind,
                   std::string &identity, std::string &error,
                   std::shared_ptr<const std::vector<std::uint8_t>>
                       *resolvedBytes = nullptr,
                   std::string *resolvedMediaType = nullptr,
                   const bool allowKindFallback = true) const;

private:
  bool ResolveBytes(
      const text::TextureReference &reference,
      std::shared_ptr<const std::vector<std::uint8_t>> &bytes,
      std::string &mediaType, std::string &error) const;

  bool Parse(ResolvedVisualAsset &asset, const RasterDecodeUsage usage,
             std::string &error) const;

  const SkiaRuntimeConfig &config_;
  text::TextAssetResolver::Holder projectAssets_;
  std::unordered_map<std::string, std::unique_ptr<ResolvedVisualAsset>> assets_;
  std::size_t residentBytes_{0U};
};

SkTileMode ToSkTileMode(const text::PaintSpread spread) noexcept;

SkRect MaterialCoordinateBounds(const text::TextMaterialCoordinates &coordinates,
                                const SkRect &layoutBounds,
                                const SkRect &textBounds,
                                const SkRect &graphemeBounds);

sk_sp<SkShader> MakeGradientShader(
    const std::vector<text::GradientStop> &stops, const bool radial,
    const float startX, const float startY, const float endX,
    const float endY, const float centerX, const float centerY,
    const float radius, const text::PaintSpread spread,
    const text::GradientSampling sampling, const SkRect &bounds);

sk_sp<SkImage> ResolveVisualTextureFrame(
    const text::TextureReference &reference, const std::int64_t localTimeUs,
    TextVisualAssetStore &assets, std::string &sampleIdentity,
    std::string &error);

sk_sp<SkImage> ResolveTextureMaterialFrame(
    const text::TextureTextMaterial &material,
    const std::size_t materialUnitIndex, const std::int64_t localTimeUs,
    TextVisualAssetStore &assets, std::string &sampleIdentity,
    std::string &error);

bool ConfigureTextMaterialPaint(
    const text::TextMaterialBinding &binding, const SkRect &layoutBounds,
    const SkRect &textBounds, const SkRect &graphemeBounds,
    const std::int64_t localTimeUs, TextVisualAssetStore &assets,
    SkPaint &paint, std::string &sampleIdentity, std::string &error,
    const std::size_t materialUnitIndex = 0U);

std::vector<std::size_t> GraphemeOffsets(SkUnicode &unicode,
                                         const std::string &utf8,
                                         const std::string &locale);

std::size_t Utf16UnitsForUtf8Prefix(const std::string &utf8,
                                    const std::size_t offset) noexcept;

std::uint64_t StableUnitFallbackIdentity(
    const TextEffectLayoutUnitBinding &unit) noexcept;

bool BuildLayoutUnitBindings(
    const text::ResolvedRichTextView &view, SkUnicode &unicode,
    const text::TextEffectFramePlan &framePlan,
    std::vector<TextEffectLayoutUnitBinding> &units, std::string &error);

const text::RichTextRun *FindRun(const text::ResolvedRichTextView &view,
                                 const std::string &paragraphId,
                                 const std::string &runId) noexcept;

bool ApplySampledProperties(const text::TextEffectFramePlan &framePlan,
                            text::TextRenderDocument &document,
                            std::vector<Diagnostic> &diagnostics,
                            std::string &error);

SkParagraphStyle MakeParagraphStyle(const text::ParagraphStyle &source,
                                   const bool applyRoundingHack);

SkTextStyle MakeBaseTextStyle(
    const text::TextStyle &source, const FontContext &fonts,
    const ResolvedTextEffectLayoutMutation *mutation,
    const float globalScale = 1.0F,
    const std::string_view locale = "und",
    const float paragraphLineHeight = 1.0F);

struct UnitTextBoxGeometry final {
  SkRect bounds{SkRect::MakeEmpty()};
  bool rightToLeft{false};
};

struct UnitGeometry final {
  TextEffectLayoutUnitBinding binding{};
  std::size_t paragraphIndex{0U};
  std::size_t paragraphUtf16Begin{0U};
  std::size_t paragraphUtf16End{0U};
  std::size_t paragraphUtf8Begin{0U};
  std::size_t paragraphUtf8End{0U};
  SkRect localBounds{SkRect::MakeEmpty()};
  SkRect localLayoutBounds{SkRect::MakeEmpty()};
  SkRect authoredBounds{SkRect::MakeEmpty()};
  SkRect layoutBounds{SkRect::MakeEmpty()};
  float canvasCellFontSize{0.0F};
  // TextPro keeps three independent Letter geometry classes. Layout bounds
  // drive selector topology, the unpadded outline drives native anchors, and
  // the fixed 30px canonical-SDF expansion is the script-facing Letter rect.
  SkRect tightInkBounds{SkRect::MakeEmpty()};
  SkRect scriptBounds{SkRect::MakeEmpty()};
  SkRect uniformTightAnchorBounds{SkRect::MakeEmpty()};
  std::vector<UnitTextBoxGeometry> textBoxes;
  SkPoint letterInitialPosition{SkPoint::Make(0.0F, 0.0F)};
  // Native absolute font-size rebuilding retains a widget-local visual-line
  // origin. The shaped paragraph remains immutable; SDF and isolated glyph
  // draws consume this translation before their animator matrix.
  float visualReflowOffsetY{0.0F};
  std::size_t documentUtf8Begin{0U};
  std::size_t documentUtf8End{0U};
  std::size_t documentWordUtf8Begin{0U};
  std::size_t documentWordUtf8End{0U};
};

struct ParagraphLayout final {
  const text::RichTextParagraph *source{nullptr};
  // Shaping is complete before publication. Base/final layouts share the
  // paragraph within one locked render call; their editable geometry is copied.
  std::shared_ptr<Paragraph> measurement;
  float x{0.0F};
  float y{0.0F};
  float width{0.0F};
  float height{0.0F};
  SkRect materialLayoutBounds{SkRect::MakeEmpty()};
  SkRect materialTextBounds{SkRect::MakeEmpty()};
  std::size_t documentUtf8Offset{0U};
  std::vector<std::size_t> unitIndexes;
};

struct ResolvedLayout final {
  std::vector<ParagraphLayout> paragraphs;
  std::vector<UnitGeometry> units;
  text::TextLayout publicLayout{};
  SkRect logicalBounds{SkRect::MakeEmpty()};
  SkRect inkBounds{SkRect::MakeEmpty()};
  SkRect glyphInkBounds{SkRect::MakeEmpty()};
  SkRect controlBounds{SkRect::MakeEmpty()};
  SkRect visualBounds{SkRect::MakeEmpty()};
  SkMatrix writingTransform{SkMatrix::I()};
  float shapingScale{1.0F};
  float fitWriterOffsetX{0.0F};
};

void RecomputeUniformTightAnchorBounds(
    const text::ResolvedRichTextView &view, ResolvedLayout &layout);

std::unique_ptr<Paragraph> BuildMeasurementParagraph(
    const text::RichTextParagraph &paragraph, const FontContext &fonts,
    const std::vector<TextEffectLayoutUnitBinding> &units,
    const ResolvedTextEffectLayoutMutations &mutations,
    const float shapingScale, const bool applyRoundingHack,
    std::string &error);

SkMatrix ResolveWritingTransform(const text::ResolvedRichTextView &view,
                                 const SkRect &horizontalBounds);

SkRect MapRect(const SkMatrix &matrix, const SkRect &rect);

std::string FlattenDocumentText(const text::ResolvedRichTextView &view);

bool BuildLayout(const text::ResolvedRichTextView &view,
                 const text::TextEffectFramePlan &framePlan,
                 const FontContext &fonts, ResolvedLayout &output,
                 std::string &error);

bool CanReuseBaseLayout(const ResolvedLayout &base,
                       const text::TextEffectFramePlan &basePlan,
                       const text::TextEffectFramePlan &finalPlan);

std::string GlyphContributorIdentity(const SkTypeface *typeface,
                                     const FontContext &fonts);

bool AttachGlyphContributorIdentities(
    const ResolvedLayout &layout, const FontContext &fonts,
    std::vector<text::FontRunResolutionReceipt> &receipts,
    std::string &error);

void MergeFramePlan(text::TextEffectFramePlan &destination,
                    const text::TextEffectFramePlan &source);

std::size_t VisualLineForUnit(const ResolvedLayout &layout,
                              const UnitGeometry &unit);

bool ApplyTextEffectVisualLineReflow(
    const text::ResolvedRichTextView &view,
    const text::TextEffectFramePlan &framePlan,
    const ResolvedLayout &baseLayout, ResolvedLayout &rebuiltLayout,
    std::string &error);

int TextEffectUnitType(const text::RichTextRun *run,
                       const UnitGeometry &unit, SkUnicode &unicode);

std::uint32_t TextEffectUnitUnicodeCodepoint(
    const text::RichTextRun *run, const UnitGeometry &unit) noexcept;

text::Color RepresentativeMaterialColor(
    const text::TextMaterialBinding &binding);

text::Color RepresentativeRunColor(const text::RichTextRun *run);

TextEffectCoordinateBridge ResolveDocumentTextEffectBridge(
    const text::ResolvedRichTextView &view,
    const text::TextLayerAppearance &appearance);

text::TextEffectRect ReferenceEffectRect(const SkRect &bounds) noexcept;

text::TextEffectFrameInput BuildTextEffectEvaluationFrame(
    const ResolvedLayout &layout, const double progress,
    const std::int64_t localTimeUs,
    const text::ResolvedRichTextView &view,
    const text::TextLayerAppearance &appearance, SkUnicode &unicode);

bool BuildTextEffectEvaluationResources(
    const text::TextAnimationStack &animations,
    const text::TextEffectFramePlan &externalPlan,
    const bool suppressAnimation, TextVisualAssetStore &assets,
    std::vector<text::TextEffectEvaluationResource> &resources,
    std::string &error);

SkRect ToSkRect(const text::TextEffectRect &rect);

SkRect ToSkRect(const text::Rect &rect);

SkRect ExpandByInsets(const SkRect &source, const text::Insets &insets);

SkRect ExpandByOutsets(const SkRect &source,
                       const TextVisualOutsets &outsets);

std::string GlyphLayerId(const text::TextGlyphMaterialLayer &layer);

text::TextBlendMode
GlyphLayerBlend(const text::TextGlyphMaterialLayer &layer);

struct ResolvedGlyphMaterialPass final {
  std::string passId;
  std::string layerId;
  std::string paragraphId;
  std::string runId;
  std::int32_t zOrder{0};
  text::TextBlendMode blend{text::TextBlendMode::SourceOver};
  text::TextPropertyCombineMode combineMode{
      text::TextPropertyCombineMode::Replace};
  text::TextGlyphMaterialLayer material{ text::TextFillLayer{} };
  std::vector<std::uint64_t> stableUnitIds;
  std::size_t authoredOrder{0U};
  bool nestedShadowStroke{false};
};

std::vector<ResolvedGlyphMaterialPass> ResolveGlyphMaterialPasses(
    const text::ResolvedRichTextView &view,
    const text::TextEffectFramePlan &framePlan);

bool PassTargetsUnit(const ResolvedGlyphMaterialPass &pass,
                     const UnitGeometry &unit);

const text::TextGlyphMaterialLayer *FindAuthoredGlyphLayer(
    const text::RichTextRun &run, const std::string &layerId) noexcept;

void ApplyLayerGeometry(const text::TextGlyphMaterialLayer &layer,
                        SkPaint &paint);

const text::TextMaterialBinding &
GlyphLayerMaterial(const text::TextGlyphMaterialLayer &layer);

class TextMaterialExecutionScope final {
public:
  explicit TextMaterialExecutionScope(text::TextExecutionEvidence *evidence)
      : previous_(active_) { active_ = evidence; }
  ~TextMaterialExecutionScope() { active_ = previous_; }
  TextMaterialExecutionScope(const TextMaterialExecutionScope &) = delete;
  TextMaterialExecutionScope &operator=(const TextMaterialExecutionScope &) = delete;

  static void Record(const std::string_view layerId,
                     const std::string_view capability,
                     const std::string_view executor,
                     const std::size_t submittedDraws = 1U) {
    if (!active_ || submittedDraws == 0U)
      return;
    auto &items = active_->materials;
    const auto found = std::find_if(items.begin(), items.end(), [&](const auto &item) {
      return item.layerId == layerId && item.capability == capability &&
             item.executor == executor;
    });
    if (found != items.end())
      found->submittedDraws += submittedDraws;
    else
      items.push_back({std::string(layerId), std::string(capability),
                       std::string(executor), submittedDraws});
  }

  static bool Enabled() noexcept { return active_ != nullptr; }

  static void RecordLayer(const text::TextGlyphMaterialLayer &layer,
                          const text::TextSdfMaterial &sdf,
                          const bool nestedStroke,
                          const std::string_view executor = "skia-metal-sdf-material") {
    if (!active_)
      return;
    const auto record = [&](const std::string_view capability) {
      std::visit([&](const auto &value) {
        Record(value.layerId, capability, executor);
      }, layer);
    };
    record("sdf:enabled");
    if (sdf.sourceCreationComponent == text::TextSourceCreationComponent::SdfText)
      record("sdf-source:sdf_text");
    std::visit([&](const auto &value) {
      using Layer = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<Layer, text::TextFillLayer>)
        record("sdf-layer:fill");
      else if constexpr (std::is_same_v<Layer, text::TextStrokeLayer>)
        record(nestedStroke ? "sdf-nested-stroke:stroke" : "sdf-layer:stroke");
      else if constexpr (std::is_same_v<Layer, text::TextShadowLayer>) {
        record("sdf-layer:shadow");
        record(value.kind == text::TextShadowKind::Inner
                   ? "sdf-shadow:inner" : "sdf-shadow:outer");
      }
    }, layer);
    const auto &paint = ResolveTextMaterial(GlyphLayerMaterial(layer));
    if (std::holds_alternative<text::SolidTextMaterial>(paint))
      record(nestedStroke ? "sdf-nested-stroke-paint:solid" : "sdf-paint:solid");
    else if (std::holds_alternative<text::LinearGradientTextMaterial>(paint))
      record(nestedStroke ? "sdf-nested-stroke-paint:linear_gradient" : "sdf-paint:linear_gradient");
    else if (std::holds_alternative<text::TextureTextMaterial>(paint))
      record(nestedStroke ? "sdf-nested-stroke-paint:texture" : "sdf-paint:texture");
  }

private:
  inline static thread_local text::TextExecutionEvidence *active_{nullptr};
  text::TextExecutionEvidence *previous_;
};

text::TextMaterialBinding &
MutableGlyphLayerMaterial(text::TextGlyphMaterialLayer &layer);

std::optional<float>
FactorTextProRenderGroupLayerOpacity(text::TextGlyphMaterialLayer &layer);

const text::TextMaterialCoordinates *GlyphMaterialCoordinates(
    const text::TextGlyphMaterialLayer &layer) noexcept;

TextSdfCoordinateDomain SdfCoordinateDomain(
    const text::TextGlyphMaterialLayer &layer) noexcept;

SkRect SdfTextCoordinateBounds(const text::TextGlyphMaterialLayer &layer,
                               const ResolvedLayout &layout,
                               const ParagraphLayout &paragraph) noexcept;

void ClearLegacySdfCoordinateOutset(
    text::TextGlyphMaterialLayer &layer) noexcept;

bool ResolveSdfMaterialImages(
    const text::TextGlyphMaterialLayer &layer,
    const std::size_t materialUnitIndex, const std::int64_t localTimeUs,
    TextVisualAssetStore &assets, sk_sp<SkImage> &texture,
    sk_sp<SkImage> &replacementMask, std::vector<std::string> &assetSamples,
    std::string &error);

TextSdfGpuRect ToTextSdfRect(const SkRect &rect,
                             const float originX,
                             const float originY) noexcept;

// Keys borrow IDs from the unchanged layout owned by the current recording.
struct GlyphPassLayoutState final {
  using UnitKey =
      std::tuple<std::string_view, std::string_view, std::size_t, std::size_t>;
  struct Entry final {
    std::size_t unitIndex;
    // Disengaged until first lookup; mutations.size() represents no match.
    std::optional<std::size_t> mutationIndex;
  };
  ResolvedTextEffectLayoutMutations mutations;
  std::map<UnitKey, Entry> units;
};

bool BuildGlyphPassParagraph(
    const text::RichTextParagraph &paragraph,
    const ResolvedGlyphMaterialPass &pass, const ResolvedLayout &layout,
    const text::TextEffectFramePlan &framePlan,
    std::optional<GlyphPassLayoutState> &layoutState,
    const FontContext &fonts,
    const std::int64_t localTimeUs, TextVisualAssetStore &assets,
    std::unique_ptr<Paragraph> &output, std::vector<std::string> &assetSamples,
    std::string &error, const bool captureOnly = false);

void ApplyUnitTransforms(SkCanvas *canvas,
                         const text::TextEffectUnitFramePlan *plan);

int BeginUnitPaint(SkCanvas *canvas);

SkRect SdfLineBoundsForUnit(const ResolvedLayout &layout,
                            const UnitGeometry &unit) noexcept;

float StoreTextProBinary32(const float value) noexcept;

float MultiplyTextProBinary32(const float left, const float right) noexcept;

float AddTextProBinary32(const float left, const float right) noexcept;

float SubtractTextProBinary32(const float left, const float right) noexcept;

float DivideTextProBinary32(const float numerator,
                            const float denominator) noexcept;

struct QtTextTypedLetterVertex final {
  float position[3];
  float instanceColor[4];
  float lineRect[4];
  float sdfTexcoord[4];
  float sdfTexcoordMinMax[4];
  float blurSdfTexcoordMinMax[4];
  float smoothBoldIndex[4];
};

static_assert(sizeof(QtTextTypedLetterVertex) ==
              kQtTextLetterVertexStride);

// Pixel executor payload retained by a typed glyph-material component.  The
// Letter vertices stay in the authored/reference 576-unit domain; Page
// RenderGroup execution supplies the final target allocation and MVP.  This
// avoids materializing the source at presentation resolution before the
// post-effect Page target evaluates fwidth.
struct DeferredQtTextNativeLetterBatch final {
  std::shared_ptr<void> atlasTexture;
  // Populated only when the existing packet diagnostic is enabled.
  std::vector<std::uint8_t> atlasRg8MetalRows;
  int atlasWidth{0};
  int atlasHeight{0};
  std::size_t atlasRowBytes{0U};
  std::vector<QtTextTypedLetterVertex> vertices;
  std::vector<std::uint16_t> indices;
  // One stable unit identity per four-vertex Letter quad.  Keeping this
  // alongside the packet lets independent RenderGroup ranges submit the
  // exact native subset instead of clipping a presentation-sized snapshot.
  std::vector<std::uint64_t> unitStableIds;
  float materialColor[4]{1.0F, 1.0F, 1.0F, 1.0F};
  float referenceWidth{0.0F};
  float referenceHeight{0.0F};
  std::int64_t sampledTimeUs{0};
  SkRect fixedLetterBounds{SkRect::MakeEmpty()};
};

// Frozen P0E adapter name.  The payload remains typed (atlas/Letter packet
// batches) and is intentionally separate from any presentation-sized picture.
struct QtTextNativePageSource final {
  std::vector<DeferredQtTextNativeLetterBatch> batches;
  bool containsRasterGlyphs{false};
};

// Native Letter packets are an optimization over the portable Skia material
// path.  Build the packet speculatively and roll it back whenever the pixel
// executor cannot publish a frame; otherwise a failed native attempt would
// leave a page source that later replays as a blank/error picture.
struct DeferredQtTextNativeBatchCheckpoint final {
  QtTextNativePageSource *source{nullptr};
  std::size_t size{0U};
  bool committed{false};

  ~DeferredQtTextNativeBatchCheckpoint() {
    if (source && !committed && source->batches.size() > size)
      source->batches.resize(size);
  }
};

// Validate and freeze the typed packet before it is attached to a component.
// This is the current-contract replacement for the old selector-driven page
// builder: all glyph selection/atlas generation has already happened in the
// FramePlan/SDF pass, so this adapter only admits immutable pixel data.
std::optional<QtTextNativePageSource> BuildQtTextNativePageSource(
    std::vector<DeferredQtTextNativeLetterBatch> batches,
    const float referenceWidth, const float referenceHeight);

TextProLetterMatrixF32
ToTextProLetterMatrixBinary32(const SkM44 &matrix) noexcept;

bool RenderSdfParagraphMaterial(
    Paragraph &paragraph, const ResolvedGlyphMaterialPass &pass,
    const ResolvedLayout &layout, const ParagraphLayout &placement,
    const TextRenderFramePlan &renderPlan,
    const text::TextLayerAppearance &appearance,
    const float sourceToReferenceScale, const float referenceWidth,
    const float referenceHeight, const std::int64_t localTimeUs,
    const SkM44 &authoredToSurface,
    SkiaGpuContext &gpuContext, SkSurface &surface,
    TextVisualAssetStore &assets, std::vector<std::string> &assetSamples,
    QtTextNativePageSource *nativePageSource,
    SkRect &authoredMaterialBounds,
    std::string &error);

struct QtTextFollowerComponentDraw final {
  sk_sp<SkImage> image;
  SkMatrix imageToAuthored;
  float opacity{1.0F};
};

struct RenderComponent final {
  std::string id;
  text::TextEffectCompositeItemKind kind{
      text::TextEffectCompositeItemKind::GlyphMaterial};
  std::int32_t zOrder{0};
  text::TextBlendMode blend{text::TextBlendMode::SourceOver};
  sk_sp<SkPicture> picture;
  std::string identity;
  std::size_t authoredOrder{0U};
  std::shared_ptr<const QtTextNativePageSource> nativePageSource;
  std::optional<QtTextFollowerComponentDraw> nativeFollower;
  // Prepared picture commands; published only when this picture is submitted
  // to a GPU target. Recording or resolving an asset alone is not execution.
  std::vector<text::TextMaterialExecutionEvidence> recordedResourceDraws;
  SkRect authoredMaterialBounds{SkRect::MakeEmpty()};
};

struct TextExecutionGraphHistoryState final {
  sk_sp<SkPicture> picture;
  std::uint64_t revision{0U};
  std::int64_t lastTimeUs{std::numeric_limits<std::int64_t>::min()};
  bool presentationDomain{false};
};

bool ExecutionGraphRequiresExplicitRasterExecutor(
    const std::vector<text::TextEffectExecutionNodeFramePlan> &nodes,
    const std::vector<std::string> *externallyCompositedResourceIds = nullptr);

bool RecordNativeSdfGlyphMaterialComponents(
    const text::ResolvedRichTextView &view,
    const text::TextLayerAppearance &appearance,
    const ResolvedLayout &layout,
    const TextRenderFramePlan &renderPlan, const FontContext &fonts,
    const std::int64_t localTimeUs, const SkRect &recordingBounds,
    const SkMatrix &presentationMatrix, const SkIRect &presentationCrop,
    SkiaGpuContext &gpuContext, TextVisualAssetStore &assets,
    std::vector<RenderComponent> &components,
    std::string &resourceIdentity, std::string &error);

bool RecordGlyphMaterialComponents(
    const text::ResolvedRichTextView &view,
    const text::TextLayerAppearance &appearance,
    const ResolvedLayout &layout,
    const TextRenderFramePlan &renderPlan, const FontContext &fonts,
    const std::int64_t localTimeUs, const SkRect &recordingBounds,
    const SkMatrix &presentationMatrix, const SkIRect &presentationCrop,
    SkiaGpuContext *gpuContext, TextVisualAssetStore &assets,
    std::vector<RenderComponent> &components,
    std::vector<Diagnostic> &diagnostics, std::string &resourceIdentity,
    std::string &error);

void DrawTextBoxBackground(SkCanvas *canvas,
                           const text::TextBoxBackground &background,
                           const SkRect &sourceBounds,
                           const float opacity = 1.0F);

sk_sp<SkPicture> RecordSemanticBackgrounds(
    const text::ResolvedRichTextView &view, const ResolvedLayout &layout,
    const TextRenderFramePlan &renderPlan,
    const SkRect &recordingBounds);

void DrawDecorationStroke(SkCanvas *canvas,
                          const text::TextDecorationLine &line,
                          const text::TextWritingMode writingMode,
                          const SkRect &unitBounds, SkPaint paint);

bool RecordInlineDecorationComponents(
    const text::ResolvedRichTextView &view, const ResolvedLayout &layout,
    const TextRenderFramePlan &renderPlan,
    const std::int64_t localTimeUs, const SkRect &recordingBounds,
    TextVisualAssetStore &assets, std::vector<RenderComponent> &components,
    std::string &identity, std::string &error);

SkRect BackdropFitBounds(const ResolvedLayout &layout,
                         const text::TextBackdropFitPolicy fit);

SkRect ApplyAuthoredExtent(const SkRect &source,
                           const text::TextBackdropSource &backdrop,
                           const text::TextAlignment horizontalAlignment,
                           const text::TextDirection direction,
                           const bool preserveQtTypesettingHeadroom);

SkPath MakeRoundedBackdropPath(const text::RoundedRectBackdrop &source,
                               const SkRect &bounds);

std::size_t DrawImageNineWithDestinationInsets(
    SkCanvas *canvas, const SkImage *image, const SkIRect &sourceCenter,
    const SkRect &destination, const text::Insets &destinationInsets,
    const SkSamplingOptions &sampling, const SkPaint &paint);

std::size_t DrawImageNineTiled(
    SkCanvas *canvas, const SkImage *image, const SkIRect &sourceCenter,
    const SkRect &destination, const text::Insets &destinationInsets,
    const float sourceScaleX, const float sourceScaleY,
    const SkSamplingOptions &sampling, const SkPaint &paint);

std::int64_t PositiveModulo(const std::int64_t value,
                            const std::int64_t modulus) noexcept;

std::int64_t ResolveAnimatedBackdropTime(
    const text::AnimatedBackdrop &source, const std::int64_t localTimeUs,
    const text::TextEffectFramePlan &framePlan,
    const std::string &layerId) noexcept;

SkRect TransformBackdropBounds(const SkRect &bounds,
                               const text::TextBackdropTransform &transform,
                               const SkRect &anchorBounds);

SkRect TransformBackdropBounds(
    const SkRect &bounds, const text::TextBackdropTransform &transform);

void ApplyBackdropTransform(SkCanvas *canvas, const SkRect &bounds,
                            const text::TextBackdropTransform &transform);

bool DrawResolvedVisualAsset(SkCanvas *canvas, ResolvedVisualAsset &asset,
                             const SkRect &bounds,
                             const std::int64_t sourceTimeUs,
                             TextVisualAssetStore &assets,
                             std::string &sampleIdentity,
                             std::string &error);

bool DrawBackdropSource(
    SkCanvas *canvas, const text::TextBackdropSource &source,
    const SkRect &bounds, const std::int64_t sourceTimeUs,
    const text::TextBackdropChannel channel,
    const text::TextEffectSize &sourceIntrinsicSize,
    TextVisualAssetStore &assets,
    std::string &sampleIdentity,
    std::vector<Diagnostic> &diagnostics, const std::string &subject,
    std::string &error,
    std::vector<text::TextMaterialExecutionEvidence> *recordedDraws);

struct ResolvedBackdropPass final {
  std::string passId;
  std::string layerId;
  std::int32_t zOrder{-100};
  text::TextBackdropSource source{ text::RoundedRectBackdrop{} };
  text::TextBackdropChannel channel{text::TextBackdropChannel::Frame};
  std::optional<text::TextMaterialBinding> materialOverride;
  text::TextBackdropTransform transform{};
  SkRect bounds{SkRect::MakeEmpty()};
  text::TextEffectUnitRange basedRange{};
  SkRect basedRect{SkRect::MakeEmpty()};
  text::TextEffectSize sourceIntrinsicSize{};
  text::TextBackdropFitMode fitMode{text::TextBackdropFitMode::Stretch};
  float sourcePivotX{0.5F};
  float sourcePivotY{0.5F};
  TextVisualOutsets expand{};
  TextVisualOutsets sourceOutsets{};
  std::int64_t sourceTimeUs{0};
  std::size_t authoredOrder{0U};
  bool inheritsSharedUnitState{false};
};

struct SharedUnitFrameState final {
  text::TextEffectMatrix4x4 localToText{};
  float opacity{1.0F};
};

std::optional<SharedUnitFrameState> ResolveSharedUnitFrameState(
    const ResolvedLayout &layout,
    const TextRenderFramePlan &renderPlan);

TextVisualOutsets ToVisualOutsets(
    const text::TextEffectOutsets &value) noexcept;

SkRect ResolveFrameBackdropBounds(const text::TextEffectBackdropPass &pass,
                                  const ResolvedLayout &layout);

TextVisualOutsets ResolvedBackdropOutsets(
    const ResolvedBackdropPass &pass) noexcept;

void AddResolvedBackdropGeometryIdentity(IdentityBuilder &identity,
                                         const ResolvedBackdropPass &pass);

std::vector<ResolvedBackdropPass> ResolveBackdropPasses(
    const text::TextRenderDocument &document, const ResolvedLayout &layout,
    const text::TextRenderRequest &request,
    const text::TextEffectFramePlan &framePlan);

bool RecordBackdropComponents(
    const text::TextRenderDocument &document, const ResolvedLayout &layout,
    const text::TextRenderRequest &request,
    const TextRenderFramePlan &renderPlan,
    const SkRect &recordingBounds, TextVisualAssetStore &assets,
    std::vector<RenderComponent> &components,
    std::vector<Diagnostic> &diagnostics, std::string &identity,
    std::string &error);

void AddDecorationPassGeometryIdentity(
    IdentityBuilder &identity, const text::TextEffectDecorationPass &pass);

std::optional<SkRect> MapEffectBounds(
    const SkRect &bounds,
    const text::TextEffectMatrix4x4 &transform);

bool RecordDecorationComponents(
    const text::TextEffectFramePlan &framePlan, const SkRect &recordingBounds,
    TextVisualAssetStore &assets, std::vector<RenderComponent> &components,
    std::vector<Diagnostic> &diagnostics, std::string &identity,
    std::string &error);

sk_sp<SkPicture> ComposePictures(const std::vector<RenderComponent> &components,
                                 const SkRect &recordingBounds);

struct PageRenderGroupDomain final {
  text::TextEffectRenderGroupExecutionPlan execution;
  SkRect resolvedFixedGeometryBounds{SkRect::MakeEmpty()};
  SkMatrix authoredToDevice{SkMatrix::I()};
  SkMatrix deviceToAuthored{SkMatrix::I()};
  SkRect deviceTargetBounds{SkRect::MakeEmpty()};
  SkRect localBounds{SkRect::MakeEmpty()};
  float rasterScaleX{1.0F};
  float rasterScaleY{1.0F};
  int targetWidth{0};
  int targetHeight{0};
};

struct QtTextRenderGroupCompositeDraw final {
  sk_sp<SkImage> pageImage;
  QtTextRenderGroupCompositeRequest request;
};

sk_sp<SkImage> RenderDeferredQtTextNativeLetterBatch(
    const DeferredQtTextNativeLetterBatch &batch,
    const PageRenderGroupDomain &domain,
    const std::unordered_set<std::uint64_t> *unitFilter,
    SkiaGpuContext &gpuContext,
    std::string &error);

sk_sp<SkPicture> RenderQtTextNativePageSource(
    const QtTextNativePageSource &source,
    const PageRenderGroupDomain &domain,
    const std::unordered_set<std::uint64_t> *unitFilter,
    SkiaGpuContext *gpuContext,
    std::string &error);

SkRect ExpandRenderGroupBounds(
    const SkRect &bounds, const text::TextRenderGroupSpec &group);

std::optional<PageRenderGroupDomain> ResolvePageRenderGroupDomain(
    const text::TextEffectFramePlan &framePlan,
    const SkMatrix &presentationMatrix,
    const SkRect &fixedPageLetterBounds);

bool ContainsComponentId(const std::vector<std::string> &ids,
                         const std::string &id);

bool PageRenderGroupIncludesComponent(
    const text::TextEffectRenderGroupExecutionPlan &execution,
    const std::unordered_set<std::string> &afterEffectDecorations,
    const RenderComponent &component);

std::vector<RenderComponent> ResolvePageRenderGroupComponents(
    const PageRenderGroupDomain &domain,
    const text::TextEffectFramePlan &framePlan,
    const std::vector<RenderComponent> &components);

// Non-Page TextPro groups are independent unit/range render targets. Keep
// their source slices separate from the global post-effect picture so the
// authored topology and per-range raster quantization remain observable.
struct IndependentUnitSource final {
  std::size_t layoutIndex{0U};
  std::uint64_t stableUnitId{0U};
  sk_sp<SkPicture> picture;
  SkRect geometryBounds{SkRect::MakeEmpty()};
  SkRect visualBounds{SkRect::MakeEmpty()};
  SkRect fixedRenderGroupBounds{SkRect::MakeEmpty()};
};

struct IndependentRangeSource final {
  sk_sp<SkPicture> picture;
  SkRect geometryBounds{SkRect::MakeEmpty()};
  SkRect visualBounds{SkRect::MakeEmpty()};
  SkRect fixedRenderGroupBounds{SkRect::MakeEmpty()};
};

struct IndependentRenderGroupTopology final {
  std::vector<std::size_t> orderedUnitIndexes;
  std::vector<text::ResolvedTextRenderGroupRange> ranges;
};

IndependentRenderGroupTopology BuildIndependentRenderGroupTopology(
    const ResolvedLayout &layout,
    const text::TextEffectRenderGroupExecutionPlan &execution);

std::vector<IndependentUnitSource> BuildIndependentUnitSources(
    const TextRenderFramePlan &renderPlan,
    const text::TextEffectRenderGroupExecutionPlan &execution,
    const ResolvedLayout &layout, const std::vector<RenderComponent> &components,
    const SkRect &recordingBounds);

IndependentRangeSource BuildIndependentRangeSource(
    const std::vector<IndependentUnitSource> &units,
    const std::vector<std::size_t> &orderedUnitIndexes,
    const text::ResolvedTextRenderGroupRange &range,
    const SkRect &recordingBounds);

sk_sp<SkPicture> MakePageLocalPicture(
    const sk_sp<SkPicture> &source, const PageRenderGroupDomain &domain);

sk_sp<SkPicture> PromotePageLocalPictureToPresentation(
    const sk_sp<SkPicture> &source, const PageRenderGroupDomain &domain,
    const SkRect &presentationBounds);

std::vector<RenderComponent> MakePageLocalComponents(
    const std::vector<RenderComponent> &components,
    const PageRenderGroupDomain &domain, SkiaGpuContext *gpuContext,
    std::string &error);

std::optional<QtTextRenderGroupCompositeRequest>
BuildQtTextRenderGroupCompositeRequest(
    const PageRenderGroupDomain &domain, const int presentationWidth,
    const int presentationHeight, const float referenceWidth,
    const float referenceHeight);

struct MaterializedPageRenderGroup final {
  sk_sp<SkPicture> authoredPicture;
  sk_sp<SkImage> pageImage;
};

MaterializedPageRenderGroup MaterializePageRenderGroup(
    const sk_sp<SkPicture> &source, const PageRenderGroupDomain &domain,
    SkiaGpuContext &gpuContext, std::string &error);

const RenderComponent *FindComponent(
    const std::vector<RenderComponent> &components,
    const std::string &id,
    const text::TextEffectCompositeItemKind kind) noexcept;

std::uint64_t StableNodeIdentity(const std::string &nodeId) noexcept;

bool ExecutionGraphRequiresStablePageDomain(
    const std::vector<text::TextEffectExecutionNodeFramePlan> &nodes);

bool ExecutionGraphContainsMediaParticle(
    const std::vector<text::TextEffectExecutionNodeFramePlan> &nodes);

bool FramePlanRequiresStablePageDomain(
    const text::TextEffectFramePlan &framePlan);

bool ExecutionGraphUsesCaptionCanvas(
    const std::vector<text::TextEffectExecutionNodeFramePlan> &nodes);

std::optional<PageRenderGroupDomain>
ResolveExecutionGraphPageRenderGroupDomain(
    const text::TextEffectFramePlan &framePlan,
    const text::TextAnimationStack &animations,
    const SkMatrix &presentationMatrix,
    const SkRect &fixedPageLetterBounds);

const text::TextEffectExecutionNodeFramePlan *
ResolvePresentationSceneMaterial(
    const std::vector<text::TextEffectExecutionNodeFramePlan> &nodes);

const text::TextEffectExecutionNodeFramePlan *
FindSdfSourceCanvasMaterial(
    const std::vector<text::TextEffectExecutionNodeFramePlan> &nodes,
    std::string &error);

std::optional<PageRenderGroupDomain> ResolveSdfSourceAttachmentDomain(
    const text::TextEffectExecutionNodeFramePlan &material,
    const float referenceWidth, const float referenceHeight,
    const float referenceGuard,
    const text::TextEffectFramePlan &framePlan,
    const text::TextAnimationStack &animations,
    const SkMatrix &presentationMatrix);

bool BuildTextEffectExecutionEvaluationInputs(
    const text::TextEffectExecutionGraph &graph,
    const text::TextRenderRequest &request,
    const std::uint64_t graphInstanceId,
    text::EffectRuntimeClockStore &clockStore,
    const std::unordered_map<std::string, TextExecutionGraphHistoryState>
        &histories,
    std::vector<text::TextEffectEvaluationStateInput> &stateInputs,
    std::vector<text::TextEffectEvaluationHistoryInput> &historyInputs,
    std::string &error);

double ResolvePostEffectProgress(const text::TextEffectFramePlan &framePlan,
                                 const std::string &nodeId,
                                 const std::int64_t localTimeUs,
                                 const std::int64_t durationUs) noexcept;

bool PostEffectConsumesRuntimeClock(
    const text::TextPostEffectKind kind) noexcept;

bool BindPostEffectRuntimeClock(
    const text::TextRenderRequest &request,
    const text::TextEffectPostEffectNode &node,
    text::EffectRuntimeClockStore &clockStore,
    QtTextPostEffectStateContext &context, std::string &error);

sk_sp<SkPicture> ResolveNodeInputPicture(
    const text::TextEffectPostEffectNode &node,
    const std::vector<RenderComponent> &components,
    const std::unordered_map<std::string, sk_sp<SkPicture>> &postOutputs,
    const sk_sp<SkPicture> &current, const SkRect &recordingBounds);

sk_sp<SkPicture> MixRenderGroupIntensity(const sk_sp<SkPicture> &source,
                                         const sk_sp<SkPicture> &effected,
                                         const SkRect &recordingBounds,
                                         const float authoredIntensity);

void RecordPostEffectExecution(
    text::TextExecutionEvidence *evidence, const std::string &nodeId,
    const QtTextPostEffectStateContext &stateContext,
    const QtTextProPostEffectResult &result);

bool ExecutePostEffectGraph(
    const TextRenderFramePlan &renderPlan,
    const text::TextRenderRequest &request,
    const text::TextAnimationStack &animations,
    const ResolvedLayout &layout,
    const std::vector<RenderComponent> &components,
    const SkRect &recordingBounds, const SkMatrix &presentationMatrix,
    const SkRect &fixedPageLetterBounds,
    const float referenceWidth, const float referenceHeight,
    const std::uint64_t graphInstanceId, const std::uint64_t lifecycleEpoch,
    text::EffectRuntimeClockStore &runtimeClockStore,
    SkiaGpuContext *gpuContext,
    std::unordered_map<std::string, sk_sp<SkPicture>> &outputs,
    sk_sp<SkPicture> &current,
    std::vector<QtTextRenderGroupCompositeDraw> &nativeComposites,
    std::vector<Diagnostic> &diagnostics,
    text::TextExecutionEvidence *executionEvidence,
    std::string &identity, std::string &error);

sk_sp<SkPicture> EmptyExecutionGraphPicture(const SkRect &bounds);

sk_sp<SkPicture> MaterializeExecutionGraphPicture(
    const sk_sp<SkPicture> &source, const SkRect &bounds,
    SkiaGpuContext *gpuContext, std::string &error);

struct ExecutionGraphRaster final {
  SkIRect bounds{SkIRect::MakeEmpty()};
  sk_sp<SkImage> image;
};

bool MaterializeExecutionGraphRaster(
    const sk_sp<SkPicture> &source, const SkRect &bounds,
    SkiaGpuContext *gpuContext, ExecutionGraphRaster &raster,
    std::string &error);

sk_sp<SkPicture> ExecutionGraphPictureFromRaster(
    const sk_sp<SkImage> &image, const SkIRect &rasterBounds,
    const SkRect &recordingBounds, std::string &error);

struct ResidentFloatTexture final {
  sk_sp<SkImage> image;
  std::shared_ptr<const std::vector<std::uint8_t>> bytes;
  std::size_t payloadOffset{0U};
  std::uint32_t width{0U};
  std::uint32_t height{0U};
};

struct ResidentVatMeshVertex final {
  std::array<float, 3U> position{};
  std::array<float, 2U> uv0{};
  std::array<float, 2U> uv1{};
  std::array<float, 2U> uv2{};
  std::array<float, 2U> uv3{};
};

struct ResidentVatMesh final {
  std::vector<ResidentVatMeshVertex> vertices;
  std::vector<std::uint16_t> indices;
};

std::uint32_t ReadResidentUint32(
    const std::vector<std::uint8_t> &bytes, const std::size_t offset) noexcept;

std::uint16_t ReadResidentUint16(
    const std::vector<std::uint8_t> &bytes, const std::size_t offset) noexcept;

float ReadResidentFloat32(const std::vector<std::uint8_t> &bytes,
                          const std::size_t offset) noexcept;

template <std::size_t Size>
std::optional<std::size_t>
FindResidentSignature(const std::vector<std::uint8_t> &bytes,
                      const std::array<std::uint8_t, Size> &signature,
                      const std::size_t begin,
                      const std::size_t end) noexcept {
  if (begin > bytes.size() || end > bytes.size() || begin > end ||
      Size > end - begin) {
    return std::nullopt;
  }
  const auto found = std::search(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                                 bytes.begin() + static_cast<std::ptrdiff_t>(end),
                                 signature.begin(), signature.end());
  if (found == bytes.begin() + static_cast<std::ptrdiff_t>(end))
    return std::nullopt;
  return static_cast<std::size_t>(std::distance(bytes.begin(), found));
}

bool DecodeResidentVatMesh(
    const std::shared_ptr<const std::vector<std::uint8_t>> &bytes,
    ResidentVatMesh &output, std::string &error);

bool DecodeResidentFloatTexture(
    const std::shared_ptr<const std::vector<std::uint8_t>> &bytes,
    ResidentFloatTexture &output, std::string &error);

std::array<float, 4U>
SampleResidentFloatTexture(const ResidentFloatTexture &texture, const float u,
                           const float v) noexcept;

struct ExecutionMaterialViewport final {
  SkISize size;
  std::array<float, 2> textOrigin;
  std::array<float, 2> textExtent;
  SkRect localBounds;
  float scaleX{1.0F};
  float scaleY{1.0F};
};

std::optional<ExecutionMaterialViewport> ResolveExecutionMaterialViewport(
    const ExecutionGraphRaster &raster, const PageRenderGroupDomain *pageDomain,
    const SkSize viewportSize, std::string &error);

sk_sp<SkPicture> RecordExecutionMaterialViewport(
    const sk_sp<SkImage> &image, const ExecutionMaterialViewport &viewport);

template <typename Configure>
sk_sp<SkImage> ExecuteExecutionMaterialProgram(
    const sk_sp<SkImage> &source, SkiaGpuContext *gpuContext,
    const TextRuntimeProgram &program, Configure configure,
    std::string &error, const SkISize outputSize = SkISize::MakeEmpty()) {
  if (!source || !program.effect) {
    error = program.error.empty()
                ? "text execution material program is unavailable"
                : "text execution material program failed to compile: " +
                      program.error;
    return {};
  }
  const auto info = SkImageInfo::Make(
      outputSize.isEmpty() ? source->width() : outputSize.width(),
      outputSize.isEmpty() ? source->height() : outputSize.height(),
      kRGBA_8888_SkColorType,
      kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
  auto surface = gpuContext ? gpuContext->MakeSurface(info, error)
                            : SkSurfaces::Raster(info);
  if (!surface) {
    if (error.empty())
      error = "text execution material target allocation failed";
    return {};
  }
  SkRuntimeEffectBuilder builder(program.effect);
  builder.child("inputTexture") = source->makeRawShader(
      SkTileMode::kClamp, SkTileMode::kClamp,
      SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone));
  configure(builder);
  auto shader = builder.makeShader();
  if (!shader) {
    error = "text execution material shader creation failed";
    return {};
  }
  SkPaint paint;
  paint.setBlendMode(SkBlendMode::kSrc);
  paint.setShader(std::move(shader));
  surface->getCanvas()->drawPaint(paint);
  auto image = surface->makeImageSnapshot();
  if (!image)
    error = "text execution material target snapshot failed";
  return image;
}

template <typename Configure>
sk_sp<SkPicture> RecordExecutionMaterialProgram(
    const ExecutionGraphRaster &raster, const SkRect &recordingBounds,
    const TextRuntimeProgram &program, Configure configure,
    std::string &error, const SkRect *outputBounds = nullptr) {
  if (!raster.image || !program.effect) {
    error = "text execution material program is unavailable: " + program.error;
    return {};
  }
  SkRuntimeEffectBuilder builder(program.effect);
  builder.child("inputTexture") = raster.image->makeRawShader(
      SkTileMode::kClamp, SkTileMode::kClamp,
      SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone));
  configure(builder);
  auto shader = builder.makeShader();
  if (!shader) {
    error = "text execution material shader creation failed";
    return {};
  }
  // Evaluate at the consumer's pixel centres. The next explicit material/RT
  // can materialize this picture; the final composite needs no extra raster.
  SkPictureRecorder recorder;
  auto *canvas = recorder.beginRecording(outputBounds ? *outputBounds : recordingBounds);
  canvas->translate(static_cast<float>(raster.bounds.left()),
                    static_cast<float>(raster.bounds.top()));
  SkPaint paint;
  paint.setShader(std::move(shader));
  auto drawBounds = outputBounds ? *outputBounds : SkRect::Make(raster.bounds);
  drawBounds.offset(-static_cast<float>(raster.bounds.left()),
                    -static_cast<float>(raster.bounds.top()));
  canvas->drawRect(drawBounds, paint);
  return recorder.finishRecordingAsPicture();
}

std::array<float, 3U> ResidentCross(const std::array<float, 3U> &left,
                                    const std::array<float, 3U> &right) noexcept;

sk_sp<SkImage> ExecuteResidentVatRbdMesh(
    const sk_sp<SkImage> &source, SkiaGpuContext *gpuContext,
    const ResidentVatMesh &mesh, const ResidentFloatTexture &positionTexture,
    const ResidentFloatTexture &rotationTexture, const SkIRect &rasterBounds,
    const SkRect &textBounds, const std::array<float, 2U> &textSize,
    const float animationTime, const float textureRotationDegrees,
    const float fade,
    const float frameCount, const float houdiniFps, const float displayFrame,
    const float autoPlayback, const float interframeInterpolation,
    const float animateFirstFrame, const std::array<float, 3U> &boundMinimum,
    const std::array<float, 3U> &boundMaximum,
    const float cameraFovDegrees, const float modelDepthScale,
    std::string &error);

sk_sp<SkPicture> DepthExecutionGraphPicture(
    const sk_sp<SkPicture> &source, const SkRect &bounds);

sk_sp<SkPicture> ComposeExecutionGraphInputs(
    const std::string &nodeId, const std::vector<std::string> &inputIds,
    const std::unordered_map<std::string, sk_sp<SkPicture>> &outputs,
    const SkRect &bounds, bool &resolved, std::string &error);

struct DirectLetterExecutionPlan final {
  bool supported{false};
  SkMatrix authoredTransform{SkMatrix::I()};
  float opacity{1.0F};
};

DirectLetterExecutionPlan ResolveDirectLetterExecutionPlan(
    const std::vector<text::TextEffectExecutionNodeFramePlan> &nodes,
    const SkRect &pivotBounds);

void AddExecutionGraphParameterIdentity(
    IdentityBuilder &identity,
    const text::TextEffectExecutionParameterSample &parameter);

void AddExecutionCameraIdentity(
    IdentityBuilder &identity,
    const std::optional<text::TextEffectExecutionCamera> &camera);

sk_sp<SkPicture> ExecuteClosedExecutionMaterial(
    const sk_sp<SkPicture> &source,
    const text::TextEffectExecutionNodeFramePlan &node,
    const std::vector<sk_sp<SkPicture>> &orderedInputs,
    const std::unordered_map<std::string, sk_sp<SkImage>> &sampledMediaInputs,
    const std::optional<std::uint64_t> stableUnitId,
    const text::TextEffectFramePlan &framePlan, TextVisualAssetStore &assets,
    const PageRenderGroupDomain *pageDomain, const SkRect &recordingBounds,
    const float referenceToExecutionScaleX,
    const float referenceToExecutionScaleY,
    const float projectionCanvasHeight, const SkSize viewportSize,
    SkiaGpuContext *gpuContext,
    std::string &error);

SkRect ResolveExecutionMaterialUnitBounds(
    const UnitGeometry &unit, const TextRenderFramePlan &renderPlan,
    const PageRenderGroupDomain *pageDomain,
    const std::optional<text::TextEffectExecutionStaticAffine> &staticAffine);

sk_sp<SkPicture> ExecuteClosedExecutionMaterialDomains(
    const sk_sp<SkPicture> &source,
    const text::TextEffectExecutionNodeFramePlan &node,
    const std::vector<sk_sp<SkPicture>> &orderedInputs,
    const std::unordered_map<std::string, sk_sp<SkImage>> &sampledMediaInputs,
    const TextRenderFramePlan &renderPlan, const ResolvedLayout &layout,
    const PageRenderGroupDomain *pageDomain, const SkRect &recordingBounds,
    const float referenceToExecutionScaleX,
    const float referenceToExecutionScaleY,
    const float projectionCanvasHeight, const SkSize viewportSize,
    TextVisualAssetStore &assets,
    SkiaGpuContext *gpuContext,
    std::string &error);

void OverridePostEffectScalarParameter(
    text::TextEffectPostEffectNode &effect, const std::string_view name,
    const float value, const bool appendWhenMissing);

bool ApplyPostEffectExecutionParameters(
    const text::TextEffectExecutionNodeFramePlan &node,
    text::TextEffectPostEffectNode &effect, double &progress,
    std::string &error);

sk_sp<SkPicture> ApplyExecutionGraphStaticAffine(
    const sk_sp<SkPicture> &source,
    const text::TextEffectExecutionStaticAffine &affine,
    const SkRect &recordingBounds);

sk_sp<SkPicture> ApplyExecutionSceneCloneSample(
    const sk_sp<SkPicture> &source,
    const text::TextEffectExecutionNodeFramePlan &node,
    const std::optional<std::uint64_t> stableUnitId,
    const SkPoint pivot, const PageRenderGroupDomain *pageDomain,
    const SkRect &recordingBounds);

sk_sp<SkPicture> ExecuteExecutionSceneCloneParameters(
    const sk_sp<SkPicture> &source,
    const text::TextEffectExecutionNodeFramePlan &node,
    const TextRenderFramePlan &renderPlan, const ResolvedLayout &layout,
    const PageRenderGroupDomain *pageDomain, const SkRect &recordingBounds,
    const SkPoint pageScenePivot,
    const std::unordered_map<std::uint64_t, sk_sp<SkPicture>> *unitSources,
    std::string &error);

sk_sp<SkPicture> ExecuteSameFrameHistoryFold(
    const std::vector<sk_sp<SkPicture>> &orderedInputs,
    const SkRect &recordingBounds, const float sourceGain,
    const float feedbackGain, const std::optional<float> finalSourceMix,
    SkiaGpuContext *gpuContext, std::string &error);

bool ExecuteTextEffectExecutionGraph(
    const TextRenderFramePlan &renderPlan,
    const text::TextRenderRequest &request,
    const std::vector<RenderComponent> &components,
    const std::vector<RenderComponent> &materialComponents,
    const ResolvedLayout &layout, const PageRenderGroupDomain *pageDomain,
    const SkRect &recordingBounds, const SkPoint pageScenePivot,
    const float referenceToExecutionScaleX,
    const float referenceToExecutionScaleY, const float referenceWidth,
    const float referenceHeight, const float sourceToReferenceScale,
    TextVisualAssetStore &assets,
    const std::uint64_t graphInstanceId, SkiaGpuContext *gpuContext,
    std::unordered_map<std::string, TextExecutionGraphHistoryState> &histories,
    std::unordered_map<std::string, sk_sp<SkPicture>> &outputs,
    sk_sp<SkPicture> &terminal, bool &terminalUsesPresentation,
    text::TextExecutionEvidence *executionEvidence,
    const text::TextExecutionNodeEvidence *sourceAttachmentEvidence,
    std::string &identity, std::string &error);

sk_sp<SkPicture> ResolveFinalComposite(
    const text::TextEffectFramePlan &framePlan,
    const std::vector<RenderComponent> &components,
    const std::unordered_map<std::string, sk_sp<SkPicture>> &postOutputs,
    const sk_sp<SkPicture> &postDefault, const SkRect &recordingBounds,
    std::string &identity);

struct GranularFrameIdentities final {
  std::string resources;
  std::string layout;
  std::string glyphs;
  std::string backdrops;
  std::string postEffects;
  std::string executionGraph;
  std::string composite;
};

std::string CombineIdentity(const std::string &declared,
                            const std::string &resolved);

void AddTextureIdentity(IdentityBuilder &identity,
                        const text::TextureReference &texture,
                        const std::int64_t localTimeUs);

void AddInsetsIdentity(IdentityBuilder &identity,
                       const text::Insets &insets);

void AddMaterialCoordinatesIdentity(
    IdentityBuilder &identity,
    const text::TextMaterialCoordinates &coordinates);

void AddGradientStopsIdentity(
    IdentityBuilder &identity,
    const std::vector<text::GradientStop> &stops);

void AddTextBoxBackgroundIdentity(
    IdentityBuilder &identity, const text::TextBoxBackground &background);

void AddMaterialIdentity(IdentityBuilder &identity,
                         const text::TextMaterialBinding &binding,
                         const std::int64_t localTimeUs);

void AddGlyphLayerIdentity(IdentityBuilder &identity,
                           const text::TextGlyphMaterialLayer &layer,
                           const std::int64_t localTimeUs);

void AddFontSpecIdentity(IdentityBuilder &identity,
                         const text::FontSpec &font);

void AddLayoutBoxIdentity(IdentityBuilder &identity,
                          const text::LayoutBox &box);

void AddParagraphStyleIdentity(IdentityBuilder &identity,
                               const text::ParagraphStyle &style);

std::string LayoutIdentity(const text::TextRenderDocument &document,
                           const text::TextEffectFramePlan &framePlan,
                           const std::uint64_t generation,
                           const std::string &fontIdentity);

std::string GlyphFrameIdentity(const text::TextRenderDocument &document,
                               const text::TextEffectFramePlan &framePlan,
                               const std::string &layoutIdentity,
                               const std::string &resourceIdentity,
                               const std::int64_t localTimeUs);

void AddKeyframeIdentity(IdentityBuilder &identity,
                         const text::TextKeyframe &keyframe);

void AddBackdropTransformIdentity(
    IdentityBuilder &identity,
    const text::TextBackdropTransform &transform);

std::string DecorationFrameIdentity(
    const text::TextEffectFramePlan &framePlan);

std::string PostEffectFrameIdentity(
    const text::TextEffectFramePlan &framePlan,
    const text::TextRenderRequest &request);

void AddExecutionGraphNodeFrameIdentity(
    IdentityBuilder &identity,
    const text::TextEffectExecutionNodeFramePlan &node);

std::string ExecutionGraphFrameIdentity(
    const text::TextEffectFramePlan &framePlan);

std::string CompositeOrderIdentity(
    const text::TextEffectFramePlan &framePlan);

std::string AppearanceFrameIdentity(
    const text::TextLayerAppearance &appearance);

std::string PresentationIdentity(const text::TextRenderRequest &request);

std::optional<SkRect> MapEffectBounds(
    const SkRect &source, const text::TextEffectMatrix4x4 &transform);

SkRect MapUnitFrameBounds(const UnitGeometry &unit,
                          const text::TextEffectUnitFramePlan *plan);

TextVisualOutsets ResolveAllGlyphOutsets(
    const text::ResolvedRichTextView &view,
    const text::TextEffectFramePlan &framePlan);

text::Rect ResolveAuthoredControlBounds(
    const text::TextRenderDocument &document, const ResolvedLayout &layout,
    const SkRect &writerLetterBounds);

SkRect ResolveRecordingBounds(const text::TextRenderDocument &document,
                              const ResolvedLayout &layout,
                              const text::TextRenderRequest &request,
                              const TextRenderFramePlan &renderPlan);

void ApplyPresentationTransform(SkCanvas *canvas,
                                const text::ResolvedRichTextView &view,
                                const text::TextRenderRequest &request,
                                const SkRect &logicalBounds);

SkMatrix ResolvePresentationMatrix(const text::ResolvedRichTextView &view,
                                   const text::TextRenderRequest &request,
                                   const SkRect &logicalBounds);

struct BendGeometry final {
  bool enabled{false};
  float amount{0.0F};
  float left{0.0F};
  float width{1.0F};
  float height{1.0F};
};

struct PathGeometry final {
  bool enabled{false};
  text::TextPathOverflow overflow{text::TextPathOverflow::Clip};
  bool loop{false};
  bool rotateToTangent{true};
  bool keepUpright{true};
  float length{0.0F};
  float sourceLeft{0.0F};
  float sourceRight{1.0F};
  float sourceBaseline{0.0F};
  float startDistance{0.0F};
  float baselineOffset{0.0F};
  float horizontalScale{1.0F};
  SkPath path;
  std::unique_ptr<SkPathMeasure> measure;
};

struct PathSample final {
  SkPoint position{SkPoint::Make(0.0F, 0.0F)};
  SkVector tangent{SkVector::Make(1.0F, 0.0F)};
  float angleDegrees{0.0F};
};

SkPoint AuthoredPathPoint(const text::TextPathPoint &point,
                          const text::LayoutBox &layoutBox) noexcept;

PathGeometry BuildPathGeometry(const text::TextPath &authored,
                               const text::LayoutBox &layoutBox,
                               const float sourceLeft,
                               const float sourceRight,
                               const float sourceBaseline);

bool SamplePath(const PathGeometry &path, const float sourceX,
                PathSample &output);

bool MapPathPoint(const PathGeometry &path, const float sourceX,
                  const float sourceY, SkPoint &output);

bool DrawPictureOnPath(SkCanvas *canvas, const SkPicture &picture,
                       const SkRect &sourceInk, const SkRect &sourceBounds,
                       const PathGeometry &path);

SkRect MapPathRect(const SkMatrix &matrix, const SkRect &source,
                   const PathGeometry &path);

float MapPathY(const SkMatrix &matrix, const float x, const float y,
               const PathGeometry &path);

float BendOffsetY(const BendGeometry &bend, const float x) noexcept;

SkRect MapBentRect(const SkMatrix &matrix, const SkRect &source,
                   const BendGeometry &bend);

float MapBentY(const SkMatrix &matrix, const float x, const float y,
               const BendGeometry &bend);

SkRect MapDeformedRect(const SkMatrix &matrix, const SkRect &source,
                       const BendGeometry &bend,
                       const PathGeometry &path);

float MapDeformedY(const SkMatrix &matrix, const float x, const float y,
                   const BendGeometry &bend, const PathGeometry &path);

void MapLayoutToPresentation(text::TextLayout &layout,
                             const SkMatrix &matrix,
                             const SkRect &authoredVisual,
                             const SkRect &liveLetterBounds,
                             const BendGeometry &bend,
                             const PathGeometry &path,
                             const std::uint32_t outputWidth,
                             const std::uint32_t outputHeight);

bool IsFinitePresentation(const text::TextRenderRequest &request) noexcept;

void AddBackdropTransformIdentity(
    IdentityBuilder &identity,
    const text::TextBackdropTransform &transform);

void AddBackdropSourceIdentity(
    IdentityBuilder &identity, const text::TextBackdropSource &source,
    const std::int64_t sourceTimeUs);

std::string BackdropFrameIdentity(
    const text::TextRenderDocument &document, const ResolvedLayout &layout,
    const text::TextRenderRequest &request,
    const text::TextEffectFramePlan &framePlan);

} // namespace videocut::skia_runtime::internal::text_lane
