#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace videocut::text {

// Matches JavaScript Number.prototype.toString() for the decimal seed string
// consumed by template_sticker's seedrandom(seed.toString()) path.
std::string QtTextSelectorJsNumberToString(double value);

// David Bau seedrandom's ARC4 generator as embedded by template_sticker. This
// intentionally implements the seeded-string path only; entropy, global-state,
// and autoseed options are unrelated to TextSelector.randomSort.
class QtTextSelectorSeedRandom final {
public:
  explicit QtTextSelectorSeedRandom(double numberSeed);
  explicit QtTextSelectorSeedRandom(std::u16string_view seedString);

  double Next() noexcept;

private:
  std::uint64_t Generate(std::size_t count) noexcept;
  void Discard(std::size_t count) noexcept;

  std::array<std::uint8_t, 256> state_{};
  std::uint8_t i_ = 0;
  std::uint8_t j_ = 0;
};

enum class QtTextSelectorRandomBasis {
  Letter,
  Word,
  Line,
};

struct QtTextSelectorIndexRange {
  std::size_t startIndex = 0;
  std::size_t endIndex = 0;
};

// Each vector position is a JavaScript randomMap property. nullopt represents
// both an absent property and an explicitly assigned undefined value; those are
// observationally identical to TextSelector's subsequent indexed reads.
using QtTextSelectorRandomMap = std::vector<std::optional<std::size_t>>;

// Reproduces BaseSelector._getRandomMap, including its non-zero-start
// double-offset swap target and the asymmetric Letter line-break check. The
// lineBreakMask is indexed by absolute letter index; missing entries are false.
// Word and Line modes deliberately ignore the mask, as the JavaScript does.
QtTextSelectorRandomMap BuildQtTextSelectorRandomMap(
    double numberSeed, const std::vector<QtTextSelectorIndexRange> &ranges,
    QtTextSelectorRandomBasis basedOn,
    const std::vector<bool> &lineBreakMask = {});

} // namespace videocut::text
