#pragma once

#include "videocut/text/TextTypes.h"

#include <cstddef>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace videocut::text {

/// Include authored appearance without changing the existing transform pivot.
/// Renderers and editing clients use the control centre as the layer origin;
/// asymmetric decorations therefore reserve matching space on the other side.
inline Rect IncludeTextControlExtent(const Rect &control,
                                     const Rect &extent) noexcept {
    if (!std::isfinite(extent.x) || !std::isfinite(extent.y) ||
        !std::isfinite(extent.width) || !std::isfinite(extent.height) ||
        extent.width <= 0.0F || extent.height <= 0.0F)
        return control;
    if (control.width <= 0.0F || control.height <= 0.0F)
        return extent;
    const float centerX = control.x + control.width * 0.5F;
    const float centerY = control.y + control.height * 0.5F;
    const float halfWidth = std::max({control.width * 0.5F,
        centerX - extent.x, extent.x + extent.width - centerX});
    const float halfHeight = std::max({control.height * 0.5F,
        centerY - extent.y, extent.y + extent.height - centerY});
    return {centerX - halfWidth, centerY - halfHeight,
            halfWidth * 2.0F, halfHeight * 2.0F};
}

struct TextLineMetrics final {
    std::string paragraphId;
    std::size_t utf8Begin{0};
    std::size_t utf8End{0};
    Rect bounds{};
    float baseline{0.0F};
    bool hardBreak{false};
};

struct TextClusterMetrics final {
    struct Box final {
        Rect bounds{};
        /// Exact renderer-owned caret geometry for the logical UTF-8
        /// boundaries. Keeping both sides explicit preserves bidi direction
        /// after shaping and avoids a second UI text layout.
        Rect utf8BeginCaret{};
        Rect utf8EndCaret{};
    };

    std::string paragraphId;
    std::string runId;
    std::size_t graphemeIndex{0};
    std::size_t utf8Begin{0};
    std::size_t utf8End{0};
    /// UTF-8 range in the flattened document text, including paragraph
    /// separators. This is the stable editing coordinate exposed to clients.
    std::size_t documentUtf8Begin{0};
    std::size_t documentUtf8End{0};
    /// Renderer-owned Unicode word boundary containing this grapheme. These
    /// offsets use the same flattened UTF-8 document coordinate as the caret
    /// range above, so canvas double-click selection never needs a second UI
    /// text shaper.
    std::size_t documentWordUtf8Begin{0};
    std::size_t documentWordUtf8End{0};
    std::size_t glyphBegin{0};
    std::size_t glyphEnd{0};
    std::vector<Box> boxes;
};

struct TextLayout final {
    std::uint32_t outputWidth{0};
    std::uint32_t outputHeight{0};
    /// Bounds before reference-canvas scaling and the sampled layer transform.
    /// These are suitable for authoring handles and deterministic hit tests.
    Rect authoredLogicalBounds{};
    Rect authoredInkBounds{};
    /// TextPro's authored, non-tight, non-uniform-height Letter rect union
    /// before sampled animator transforms.
    Rect authoredLetterBounds{};
    /// Stable editing geometry. It is never used as a render scissor.
    Rect authoredControlBounds{};
    /// Conservative local-space bounds of glyphs, appearance and animation.
    Rect authoredVisualExtent{};
    Rect authoredConservativeEnvelope{};
    Rect logicalBounds{};
    Rect inkBounds{};
    /// Current animated Letter union, retained for animation diagnostics and
    /// clients that deliberately follow sampled per-Letter transforms.
    Rect letterBounds{};
    Rect controlBounds{};
    Rect visualExtent{};
    Rect conservativeEnvelope{};
    std::vector<float> baselines;
    std::vector<TextLineMetrics> lines;
    std::vector<TextClusterMetrics> clusters;
    /// Neutral reference-canvas geometry retained for composition-owned
    /// decorations. The public editing projection still exposes only the
    /// compact bounds and normalized hit regions.
    std::vector<TextLineMetrics> authoredLines;
    std::vector<TextClusterMetrics> authoredClusters;
    bool overflow{false};
    /// Digest of the exact flattened UTF-8 text used for this layout. Clients
    /// fence edit-session geometry with it before accepting carets/selection.
    std::string textDigest;
    std::string semanticDigest;
};

} // namespace videocut::text
