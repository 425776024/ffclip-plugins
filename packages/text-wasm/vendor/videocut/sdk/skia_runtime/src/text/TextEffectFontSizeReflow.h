#pragma once

#include "videocut/text/TextEffectFramePlan.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace videocut::skia_runtime::internal {

/// Stable bridge from a frame-plan unit identity to one renderer-owned
/// grapheme. The bridge is rebuilt with layout and never persisted.
struct TextEffectLayoutUnitBinding final {
  std::uint64_t stableUnitId{0U};
  std::string paragraphId;
  std::string runId;
  std::size_t utf8Begin{0U};
  std::size_t utf8End{0U};
};

struct TextEffectLayoutScalarOperation final {
  text::TextPropertyCombineMode combineMode{
      text::TextPropertyCombineMode::Replace};
  float value{0.0F};
};

struct ResolvedTextEffectLayoutMutation final {
  TextEffectLayoutUnitBinding unit{};
  std::optional<std::uint32_t> replacementCodepoint;
  std::vector<TextEffectLayoutScalarOperation> absoluteFontSize;
  std::vector<TextEffectLayoutScalarOperation> letterSpacing;
  std::vector<TextEffectLayoutScalarOperation> wordSpacing;
  std::vector<TextEffectLayoutScalarOperation> baselineShift;
  std::vector<TextEffectLayoutScalarOperation> paragraphLineHeight;
};

/// One effective absolute font-size input assigned to a rebuilt visual line.
/// A missing target leaves the authored basis at scale one, but still
/// participates in the line maximum used by SkParagraph's line box.
struct TextEffectVisualLineFontSizeSample final {
  std::size_t visualLine{0U};
  float basis{0.0F};
  std::optional<float> target;
};

struct TextEffectVisualLineFontSizeScale final {
  std::size_t visualLine{0U};
  float effectiveMaximumScale{1.0F};
};

struct TextEffectVisualLineOrigin final {
  float desiredCenterY{0.0F};
  float offsetY{0.0F};
};

/// Resolves the sole public TextEffectFramePlan layout stream. Unknown stable
/// unit identities and illegal combine operations fail closed; no private
/// animation frame-plan or compositor-scale substitute is constructed.
class ResolvedTextEffectLayoutMutations final {
public:
  ResolvedTextEffectLayoutMutations() = default;
  ResolvedTextEffectLayoutMutations(
      const ResolvedTextEffectLayoutMutations &) = default;
  ResolvedTextEffectLayoutMutations(
      ResolvedTextEffectLayoutMutations &&other) noexcept {
    Swap(other);
  }
  ResolvedTextEffectLayoutMutations &operator=(
      ResolvedTextEffectLayoutMutations other) noexcept {
    Swap(other);
    return *this;
  }

  [[nodiscard]] bool Resolve(
      const text::TextEffectFramePlan &framePlan,
      const std::vector<TextEffectLayoutUnitBinding> &units,
      std::string &error) noexcept;

  /// Returns the first containing range in binding order, including overlaps.
  [[nodiscard]] const ResolvedTextEffectLayoutMutation *Find(
      const std::string &paragraphId, const std::string &runId,
      std::size_t utf8Begin, std::size_t utf8End) const noexcept;

  std::size_t size() const noexcept { return values_.size(); }
  const ResolvedTextEffectLayoutMutation *data() const noexcept {
    return values_.data();
  }
  const ResolvedTextEffectLayoutMutation &operator[](std::size_t index) const {
    return values_[index];
  }

private:
  struct RangeEntry final {
    std::size_t index;
    std::size_t maximumEnd;
  };
  void Swap(ResolvedTextEffectLayoutMutations &other) noexcept {
    values_.swap(other.values_);
    ranges_.swap(other.ranges_);
  }
  void BuildRangeIndex();
  std::vector<ResolvedTextEffectLayoutMutation> values_;
  // Sorted by paragraph/run/start, with prefix maximum ends within each run.
  // Indices keep copies and moves independent of vector/string addresses.
  std::vector<RangeEntry> ranges_;
};

[[nodiscard]] std::optional<float> ApplyTextEffectLayoutScalarOperations(
    float authored,
    const std::vector<TextEffectLayoutScalarOperation> &operations) noexcept;

/// Recovered TextPro line-local font-size origin contract. These helpers are
/// kept separate from layout mutation resolution so rebuilding glyphs never
/// turns the sampled font size into a compositor scale.
[[nodiscard]] std::optional<std::vector<TextEffectVisualLineFontSizeScale>>
ResolveTextEffectVisualLineFontSizeScales(
    const std::vector<TextEffectVisualLineFontSizeSample> &samples) noexcept;

[[nodiscard]] std::optional<TextEffectVisualLineOrigin>
ResolveTextEffectVisualLineOrigin(float designCenterY, float baseLineCenterY,
                                  float rebuiltLineCenterY,
                                  float effectiveMaximumScale) noexcept;

/// Exact float-bit identity used by the glyph/layout cache. Decimal strings
/// are deliberately not used for sampled shaping values.
[[nodiscard]] std::uint32_t
TextEffectLayoutScalarIdentity(float value) noexcept;

} // namespace videocut::skia_runtime::internal
