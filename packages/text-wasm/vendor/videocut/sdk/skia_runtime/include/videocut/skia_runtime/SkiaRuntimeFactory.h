#pragma once

#include "videocut/frame/VideoFrame.h"
#include "videocut/text/TextRendererFactory.h"
#include "videocut/vector/VectorRendererFactory.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace videocut::skia_runtime {

struct SkiaRuntimeCapabilities final {
    std::string skiaRevision;
    std::string artifactProfile;
    std::string textRendererProfile;
    std::string vectorRendererProfile;
    bool cpuRaster{true};
    bool textGpuRaster{false};
    bool textMetal{false};
    bool textNativeFragmentDerivatives{false};
    bool skParagraph{true};
    bool svg{true};
    bool lottie{true};
    bool dotLottie{false};
    bool expressions{false};
    bool externalIo{false};
};

enum class RuntimeAssetKind : std::uint8_t {
    Font = 0,
    Image,
    Vector,
    NestedAnimation,
    Mesh,
    FloatTexture,
};

struct RuntimeAsset final {
    std::string logicalId;
    std::string mediaType;
    /// Resolver-admitted identity for the complete runtime closure. For a
    /// leaf this is its byte digest; compound Vector assets may expose the
    /// sealed digest of the document plus all resident dependencies.
    std::string contentDigest;
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
};

/// Metrics used by the standalone SDFText font-resource carrier.  VideoFusion
/// TextPro normalizes a face against a 1.2-em typographic box rather than the
/// raw hhea line box that Skia exposes through SkFontMetrics.  The profile is
/// resolved from the selected immutable face, so the correction remains
/// stable for TTC faces and variable-font default instances without any
/// family or template-specific branching.
struct TextFontSdfMetricProfile final {
    std::uint32_t unitsPerEm{0U};
    std::int32_t typographicAscender{0};
    std::int32_t typographicDescender{0};
    std::int32_t typographicLineGap{0};
    std::int32_t horizontalAscender{0};
    std::int32_t horizontalDescender{0};
    std::int32_t horizontalLineGap{0};
    std::uint16_t selectionFlags{0U};
    bool typographicMetricsAvailable{false};
    bool baselineMetricsAvailable{false};
    float qtSdfScale{1.0F};
    /// Baseline correction in font-size units (em).  Callers multiply this
    /// by the effective authored font size; it is zero when either metric
    /// table is unavailable or invalid.
    float qtSdfBaselineShiftEm{0.0F};
};

/// Capability-limited byte resolver. Implementations return only allowlisted
/// assets and never perform an implicit path or network lookup.
class RuntimeAssetResolver {
public:
    virtual ~RuntimeAssetResolver() = default;
    virtual bool Resolve(
        RuntimeAssetKind kind,
        const std::string& logicalId,
        RuntimeAsset& output,
        std::string& error) const = 0;
};

inline constexpr std::uint32_t
    kRuntimeAnimatedImageSourceContractVersion = 1U;
inline constexpr std::uint32_t kRuntimePackedYuvFrameContractVersion = 1U;
// Frozen maximum R8 plane texture width for the v2 merge contract. The Y
// texture width is its decoded row stride (not merely coded width), matching
// the captured shader's yChannelStride coordinate domain.
inline constexpr std::uint32_t kRuntimePackedYuvMaximumPlaneStride =
    16'384U;
inline constexpr std::string_view kQtPackedAlphaMp4Capability =
    vector::kVideoCutPackedAlphaMp4Capability;
// This exact media type, including its profile parameter, is the admission
// marker. Plain video/mp4 remains unsupported by the Skia resource provider.
// The source exposes the immutable decoded
// YUV420p frame without presentation conversion; the Skia runtime executes
// Qt's fixed BT.601-limited Metal merge, one-texel seam guards, and float alpha
// reconstruction. The MP4 right half already carries associated RGB, so the
// merge must not multiply RGB by reconstructed alpha.
inline constexpr std::string_view kQtPackedAlphaMp4ResourceMediaType =
    vector::kVideoCutPackedAlphaMp4MediaType;

enum class RuntimeAnimatedImagePixelFormat : std::uint8_t {
    /// Byte order is R, G, B, A. RGB bytes are display-referred sRGB values;
    /// alpha is independent linear coverage rather than premultiplied color.
    Rgba8 = 1,
};

enum class RuntimeAnimatedImageAlphaType : std::uint8_t {
    Straight = 1,
};

enum class RuntimeAnimatedImageRowOrder : std::uint8_t {
    TopLeft = 1,
};

struct RuntimeAnimatedImageOpenRequest final {
    std::uint32_t contractVersion{
        kRuntimeAnimatedImageSourceContractVersion};
    std::string logicalId;
    std::string mediaType;
    std::string capability;
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
};

/// One strict, immutable decoded frame. The factory owns all decoding and
/// timeline behavior; Skia receives only neutral top-left RGBA8 bytes.
struct RuntimeAnimatedImageFrame final {
    std::uint32_t contractVersion{
        kRuntimeAnimatedImageSourceContractVersion};
    RuntimeAnimatedImagePixelFormat pixelFormat{
        RuntimeAnimatedImagePixelFormat::Rgba8};
    RuntimeAnimatedImageAlphaType alphaType{
        RuntimeAnimatedImageAlphaType::Straight};
    RuntimeAnimatedImageRowOrder rowOrder{
        RuntimeAnimatedImageRowOrder::TopLeft};
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    std::uint32_t width{0};
    std::uint32_t height{0};
    /// The immutable buffer contains exactly rowBytes * height bytes. Padding
    /// after width * 4 is permitted and row zero is the top row.
    std::uint32_t rowBytes{0};
    /// Stable source-scoped identity for all frame fields and byte content.
    /// Reusing an identity for different content violates the v1 contract.
    std::string frameIdentity;
};

/// Exact v2 transport for the decoded packed MP4 before YUV-to-RGB
/// presentation conversion. The retained VideoFrame owns all three YUV420p
/// planes and their padded row strides. Its color metadata is informational:
/// the Qt merge contract deliberately uses fixed BT.601 limited coefficients.
struct RuntimePackedYuvFrame final {
    std::uint32_t contractVersion{kRuntimePackedYuvFrameContractVersion};
    frame::VideoFrame decodedFrame;
    /// Stable identity for compressed source, stream PTS/time base, plane
    /// layout, and the versioned Qt merge contract.
    std::string frameIdentity;
};

class RuntimeAnimatedImageSource {
public:
    virtual ~RuntimeAnimatedImageSource() = default;

    /// Resolves the exact source frame for a signed source timestamp. Repeated
    /// samples and time rollback are source responsibilities; adapters must
    /// not infer cadence, loop mode, or monotonicity.
    virtual bool GetFrame(std::int64_t sourceTimeUs,
                          RuntimeAnimatedImageFrame& output,
                          std::string& error) = 0;

    /// Resolves a raw planar frame for the packed-alpha profile. Implementors
    /// must fail closed instead of silently crossing the RGBA8 boundary.
    virtual bool GetPackedYuvFrame(
        std::int64_t sourceTimeUs,
        RuntimePackedYuvFrame& output,
        std::string& error) = 0;

    /// Exact display-order source-frame count available to versioned
    /// source-index sampling. Absence is authoritative: callers must not infer
    /// a count from duration/fps or fall back to timestamp sampling.
    virtual std::optional<std::uint64_t> ExactSourceFrameCount() const noexcept
    {
        return std::nullopt;
    }

    /// Direct zero-based display-order source-frame request for the exact
    /// packed-YUV profile.
    virtual bool GetPackedYuvFrameBySourceIndex(
        std::uint64_t sourceFrameIndex,
        RuntimePackedYuvFrame& output,
        std::string& error) = 0;
};

class RuntimeAnimatedImageSourceFactory {
public:
    virtual ~RuntimeAnimatedImageSourceFactory() = default;
    /// Creates a decoder only for the exact version, media type, capability,
    /// logical ID, and immutable bytes admitted by the caller. Implementations
    /// must not perform fallback path or network lookup and return null on any
    /// unsupported or malformed request.
    virtual std::shared_ptr<RuntimeAnimatedImageSource> Create(
        const RuntimeAnimatedImageOpenRequest& request,
        std::string& error) const = 0;
};

struct SkiaRuntimeConfig final {
    std::shared_ptr<const RuntimeAssetResolver> assets;
    std::shared_ptr<const RuntimeAnimatedImageSourceFactory> animatedImages;
    bool allowSystemFonts{true};
    bool allowSystemGlyphFallback{false};
    std::vector<std::string> defaultFontFamilies;
    std::size_t maximumDiagnostics{64};
};

class SkiaRuntime {
public:
    virtual ~SkiaRuntime() = default;
    virtual const SkiaRuntimeCapabilities& capabilities() const noexcept = 0;
    virtual text::TextRendererFactoryHolder textRenderer() const noexcept = 0;
    virtual vector::VectorRendererFactoryHolder vectorRenderer() const noexcept = 0;
};

using SkiaRuntimeHolder = std::shared_ptr<SkiaRuntime>;

/// Creates the composition-root object. Exceptions never cross this boundary.
SkiaRuntimeHolder CreateSkiaRuntime(
    SkiaRuntimeConfig config,
    std::string& error) noexcept;

/// Admission-time validation used before a project Font Asset and its Text
/// mutation are committed together. No path lookup is performed.
bool ValidateTextFontBytes(const std::vector<std::uint8_t>& bytes,
                           std::uint32_t faceIndex,
                           std::string& error) noexcept;
bool ValidateTextFontBytes(const std::uint8_t* bytes,
                           std::size_t byteLength,
                           std::uint32_t faceIndex,
                           std::string& error) noexcept;

/// Resolves the selected face's data-driven TextPro SDF scale.  The function
/// validates the same byte/face boundary as ValidateTextFontBytes.  Fonts
/// without an OS/2 typographic metric pair are accepted with
/// typographicMetricsAvailable=false and qtSdfScale=1, allowing callers to
/// retain the established neutral path instead of inventing a fallback.
bool ResolveTextFontSdfMetricProfile(
    const std::vector<std::uint8_t>& bytes, std::uint32_t faceIndex,
    TextFontSdfMetricProfile& profile, std::string& error) noexcept;

/// Admission-time validation for project-managed Text texture bytes. The
/// declared media type must match the encoded image and the decode budget.
bool ValidateTextTextureBytes(const std::vector<std::uint8_t>& bytes,
                              const std::string& mediaType,
                              std::string& error) noexcept;

} // namespace videocut::skia_runtime
