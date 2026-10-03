#pragma once

#include "videocut/text_composition/TextCompositionDocument.h"
#include "videocut/text_composition/TextInvalidation.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace videocut::text_composition {

/// Renderer-independent geometry contract. Control geometry is stable editor
/// state and never acts as a pixel scissor; visual extents may exceed it.
struct TextVisualExtentPlan final {
  text::Rect layoutBounds{};
  text::Rect controlBounds{};
  text::Rect inkBounds{};
  text::Rect visualExtent{};
  text::Rect conservativeEnvelope{};
};

/// Stateless absolute clocks for one normalization request. Source timestamps
/// use source-local media ticks; composition timestamps use absolute
/// microseconds. All authored intervals are half-open.
struct TextCompositionSnapshotSample final {
  std::int64_t absoluteCompositionUs{0};
  std::int64_t compositionBeginUs{0};
  std::int64_t compositionEndUs{0};
  std::int64_t sourceLocalTicks{0};
  std::int64_t sourceWindowBeginTicks{0};
  std::int64_t sourceWindowEndTicks{0};
};

struct TextCompositionSnapshot final {
  std::string contentIdentity;
  std::string resourceIdentity;
  std::string layoutIdentity;
  std::string glyphMaterialIdentity;
  std::string backdropIdentity;
  std::string animationIdentity;
  std::string decorationIdentity;
  std::string postEffectIdentity;
  std::string executionGraphIdentity;
  std::string compositeIdentity;
  TextResourceKey resourceKey{};
  TextLayoutKey layoutKey{};
  TextGlyphKey glyphKey{};
  TextBackdropKey backdropKey{};
  TextPostEffectKey postEffectKey{};
  TextCompositeKey compositeKey{};
  text::ResolvedRichTextView resolvedText{};
  text::TextLayerAppearance appearance{};
  text::TextAnimationStack animations{};
  std::vector<text::TimedTextSpan> timedSpans;
  std::vector<TextResourceReference> resources;
  TextVisualExtentPlan visualExtentPlan{};
  std::vector<VectorDecorationBinding> decorations;
  std::vector<std::string> disabledDecorationIds;
  bool decorationFreeFastPath{false};
};

struct TextCompositionSnapshotResult final {
  bool valid{false};
  /// A valid gap/out-of-window sample has no snapshot and sets this flag.
  bool emptyAtSample{false};
  std::optional<TextCompositionSnapshot> snapshot;
  std::vector<Diagnostic> diagnostics;
};

/// Pure renderer-neutral normalization. It has no previous-frame state and
/// emits only local RichTextDocument/layout inputs. Timeline/output transforms
/// remain the TextCompositionClip/compositor's responsibility.
TextCompositionSnapshotResult BuildTextCompositionSnapshot(
    const TextCompositionDocument &document,
    const TextCompositionSnapshotSample &sample,
    const TextCompositionLimits &limits = {});

/// Keep animation-owned inline decorations in the text lane, excluding bindings
/// already rendered by the composition compositor (including disabled bindings).
/// The full animation remains available for decoration placement.
text::TextAnimationStack BuildTextLaneAnimations(
    const text::TextAnimationStack &animations,
    const std::vector<VectorDecorationBinding> &compositionDecorations);

} // namespace videocut::text_composition
