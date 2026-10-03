#pragma once

#include "videocut/frame/VideoFrame.h"
#include "videocut/text/EffectRuntimeClock.h"
#include "videocut/text/RichTextDocument.h"
#include "videocut/text/TextAnimation.h"
#include "videocut/text/TextEffectFramePlan.h"
#include "videocut/text/TextExecutionEvidence.h"
#include "videocut/text/TextLayerAppearance.h"
#include "videocut/text/TextLayout.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace videocut::text {

enum class TextAssetKind : std::uint8_t {
    Font = 1,
    Image = 2,
    Mesh = 3,
    FloatTexture = 4,
};

struct TextAssetBytes final {
    TextAssetKind kind{TextAssetKind::Font};
    std::string logicalId;
    std::string mediaType;
    std::string contentDigest;
    std::uint64_t byteLength{0};
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
};

/// Snapshot-scoped, capability-limited Text resource closure. Implementations
/// may only return bytes admitted into the exact authored render revision;
/// paths, bookmarks and platform locators never cross this interface.
class TextAssetResolver {
public:
    using Holder = std::shared_ptr<const TextAssetResolver>;

    virtual ~TextAssetResolver() = default;
    virtual bool Resolve(TextAssetKind kind, const std::string& logicalId,
                         const std::string& expectedContentDigest,
                         TextAssetBytes& output,
                         std::string& error) const = 0;
};

/// Storage precision of the lane's final publication surface. This is an
/// explicit renderer contract rather than a side effect of the byte budget:
/// callers may reserve enough memory for transient RenderGroup surfaces while
/// still matching an RGBA8 reference renderer's final quantization boundary.
enum class TextPublicationFormat : std::uint8_t {
    Rgba8 = 0,
    Rgba16Float,
};

struct TextRenderOptions final {
    std::uint32_t maximumWidth{8192};
    std::uint32_t maximumHeight{8192};
    std::size_t maximumSurfaceBytes{256U * 1024U * 1024U};
    std::size_t maximumCacheBytes{256U * 1024U * 1024U};
    std::uint32_t maximumCachedFrames{32};
    TextPublicationFormat publicationFormat{TextPublicationFormat::Rgba8};
    std::string rendererProfile{"videocut-skia-text-v1"};
    TextAssetResolver::Holder assets;
};

/// Sampled post-layout state. It is deliberately separate from typography:
/// resizing a layer never rewrites font metrics or scales a cached bitmap.
struct TextLayerPresentation final {
    float positionX{0.0F};
    float positionY{0.0F};
    float scaleX{1.0F};
    float scaleY{1.0F};
    float rotationDegrees{0.0F};
    float opacity{1.0F};
    bool flipHorizontal{false};
    bool flipVertical{false};
};

/// Disposable normalized input installed by a composition snapshot. It owns
/// no project identity or revision and is replaced atomically.
struct TextRenderDocument final {
    ResolvedRichTextView text{};
    TextLayerAppearance appearance{};
    TextAnimationStack animations{};
    std::vector<TimedTextSpan> timedSpans;
};

/// Identity-free runtime command bound by the render lane to every stateful
/// post-effect node/render-group instance. It is transient playback/export
/// execution state and is never serialized into a template or project.
struct TextEffectRuntimeClockControl final {
    EffectRuntimeClockEvent event{EffectRuntimeClockEvent::Begin};
    std::uint64_t lifecycleEpoch{0U};
    std::int64_t advanceUs{0};
    std::optional<std::int64_t> restoreElapsedUs;
    std::uint64_t restoreStateRevision{1U};
};

enum class TextRasterDelivery : std::uint8_t {
    StraightFrame = 0,
    PremultipliedComposite,
};

struct TextRenderRequest final {
    std::int64_t localTimeUs{0};
    std::int64_t durationUs{0};
    std::uint32_t outputWidth{0};
    std::uint32_t outputHeight{0};
    RenderQuality quality{RenderQuality::PreviewActive};
    /// Keep RGBA8 associated while a downstream compositor will add more
    /// layers. The default public frame remains straight-alpha.
    TextRasterDelivery delivery{TextRasterDelivery::StraightFrame};
    /// GPU consumers receive canonical GPU storage with its producer fence.
    /// CPU callers retain the explicit readback delivery used by snapshots.
    bool gpuConsumer{false};
    std::int32_t gpuDeviceIndex{-1};
    std::uint64_t gpuDeviceGeneration{1};
    TextLayerPresentation presentation{};
    TextEffectFramePlan framePlan{};
    /// Canonical FramePlan resource IDs whose pixels are owned by a downstream
    /// compositor. Execution nodes still validate and fingerprint these
    /// resources, but must not draw a second copy in the text surface.
    std::vector<std::string> externallyCompositedResourceIds;
    std::optional<TextEffectRuntimeClockControl> effectRuntimeClock;
    bool suppressAnimation{false};
    /// Transparent output-space padding reserved for downstream GPU effects.
    /// It does not alter typography, layout metrics, or hit-test geometry.
    float externalEffectOutset{0.0F};
    CancelCheck cancel;
    bool captureExecutionEvidence{false};
};

struct FontRunResolutionReceipt final {
    enum class Status : std::uint8_t {
        Resolved = 0,
        Failed,
    };

    enum class FailureStage : std::uint8_t {
        None = 0,
        Primary,
        ExplicitFallback,
    };

    std::string paragraphId;
    std::string runId;
    FontReference requestedPrimary;
    std::string resolvedPrimaryIdentity;
    std::vector<std::string> resolvedFallbackIdentities;
    std::vector<FontAxisRange> supportedAxes;
    bool primarySubstituted{false};
    bool systemGlyphFallbackAllowed{false};
    /// Exact typeface identities observed in shaped glyph runs for this
    /// authored run, in first-use order. This is visible-frame evidence and
    /// may include an implicit system fallback that is not in the authored
    /// fallback chain. Whitespace-only/empty runs may have no contributors.
    std::vector<std::string> glyphContributorIdentities;
    /// Number of shaped glyphs whose final Skia glyph ID is zero. This is
    /// visible-frame missing-glyph evidence, not a prediction from cmap data.
    std::uint32_t missingGlyphCount{0U};
    /// Pre-raster resolution outcome. A failed receipt is produced while the
    /// document is installed, before a layout or visible surface can exist.
    Status status{Status::Resolved};
    FailureStage failureStage{FailureStage::None};
    /// Present only when failureStage is ExplicitFallback. It is the authored
    /// zero-based fallback index and never a renderer/provider lookup index.
    std::optional<std::uint32_t> failedFallbackIndex;
    /// Stable machine-readable reason. Human diagnostics remain separate so
    /// provider errors, paths, or locators cannot leak through this receipt.
    std::string failureCode;
};

struct TextDocumentInstallReceipt final {
    /// Attempt evidence is available on both success and failure. On failure
    /// it contains every fully resolved prior run followed by the exact run
    /// whose primary or explicit fallback failed.
    std::vector<FontRunResolutionReceipt> fontResolutions;
};

struct TextRenderResult final {
    TextStatusCode status{TextStatusCode::Failed};
    frame::VideoFrame image;
    std::int32_t originX{0};
    std::int32_t originY{0};
    Rect logicalBounds{};
    Rect inkBounds{};
    TextLayout layout;
    /// Snapshot- and frame-specific font resolution evidence. UI and export
    /// diagnostics consume this receipt; they never re-resolve platform fonts.
    std::vector<FontRunResolutionReceipt> fontResolutions;
    std::uint64_t documentGeneration{0};
    std::vector<Diagnostic> diagnostics;
    std::optional<TextExecutionEvidence> executionEvidence;

    explicit operator bool() const noexcept
    {
        return status == TextStatusCode::Ok
            && static_cast<bool>(image);
    }
};

/// Thread-confined renderer. Preview and export must own separate instances.
class TextRenderLane {
public:
    virtual ~TextRenderLane() = default;
    bool ReplaceDocument(TextRenderDocument document, std::string& error)
    {
        TextDocumentInstallReceipt ignored;
        return ReplaceDocument(std::move(document), ignored, error);
    }
    virtual bool ReplaceDocument(TextRenderDocument document,
                                 TextDocumentInstallReceipt& receipt,
                                 std::string& error) = 0;
    virtual std::uint64_t generation() const noexcept = 0;
    virtual TextRenderResult Render(
        const TextRenderRequest& request) = 0;
    [[nodiscard]] virtual std::size_t cachedBytes() const noexcept = 0;
    virtual void PurgeCache() noexcept = 0;
};

using TextRenderLaneHolder = std::shared_ptr<TextRenderLane>;

} // namespace videocut::text
