#include "text/TextEffectFontSizeReflow.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>
#include <unordered_map>

namespace videocut::skia_runtime::internal {

bool ResolvedTextEffectLayoutMutations::Resolve(
    const text::TextEffectFramePlan &framePlan,
    const std::vector<TextEffectLayoutUnitBinding> &units,
    std::string &error) noexcept {
  auto &output = values_;
  output.clear();
  ranges_.clear();
  error.clear();
  try {
    std::unordered_map<std::uint64_t, std::size_t> unitIndex;
    unitIndex.reserve(units.size());
    output.reserve(units.size());
    for (const auto &unit : units) {
      if (unit.stableUnitId == 0U || unit.paragraphId.empty() ||
          unit.runId.empty() || unit.utf8Begin >= unit.utf8End ||
          !unitIndex.emplace(unit.stableUnitId, output.size()).second) {
        error = "text frame plan has an invalid layout-unit binding";
        output.clear();
        return false;
      }
      output.push_back({unit});
    }

    for (const auto &sample : framePlan.units) {
      if (!sample.absoluteFontSize && !sample.replacementCodepoint)
        continue;
      const auto found = unitIndex.find(sample.stableUnitId);
      if (found == unitIndex.end()) {
        error = "text frame plan targets an unknown layout unit";
        output.clear();
        return false;
      }
      auto &resolved = output[found->second];
      if (sample.absoluteFontSize) {
        if (!std::isfinite(*sample.absoluteFontSize) ||
            *sample.absoluteFontSize < 0.0F) {
          error = "text frame plan has an invalid absolute font-size sample";
          output.clear();
          return false;
        }
        resolved.absoluteFontSize.push_back(
            {text::TextPropertyCombineMode::Replace,
             *sample.absoluteFontSize});
      }
      if (sample.replacementCodepoint) {
        const auto value = *sample.replacementCodepoint;
        if (value > 0xFFFFU || (value >= 0xD800U && value <= 0xDFFFU)) {
          error = "text frame plan has an invalid replacement codepoint";
          output.clear();
          return false;
        }
        resolved.replacementCodepoint = value;
      }
    }

    for (const auto &mutation : framePlan.layoutMutations) {
      const auto found = unitIndex.find(mutation.stableUnitId);
      if (found == unitIndex.end() || !std::isfinite(mutation.value)) {
        error = "text frame plan targets an unknown layout unit";
        output.clear();
        return false;
      }
      auto &resolved = output[found->second];
      std::vector<TextEffectLayoutScalarOperation> *destination = nullptr;
      switch (mutation.kind) {
      case text::TextEffectLayoutMutationKind::AbsoluteFontSize:
        destination = &resolved.absoluteFontSize;
        break;
      case text::TextEffectLayoutMutationKind::LetterSpacing:
        destination = &resolved.letterSpacing;
        break;
      case text::TextEffectLayoutMutationKind::WordSpacing:
        destination = &resolved.wordSpacing;
        break;
      case text::TextEffectLayoutMutationKind::BaselineShift:
        destination = &resolved.baselineShift;
        break;
      case text::TextEffectLayoutMutationKind::ParagraphLineHeight:
        destination = &resolved.paragraphLineHeight;
        break;
      }
      if (!destination) {
        error = "text frame plan has an unsupported layout mutation";
        output.clear();
        return false;
      }
      if (mutation.combineMode == text::TextPropertyCombineMode::MatrixConcat ||
          mutation.combineMode == text::TextPropertyCombineMode::ColorMix) {
        error = "text frame plan has an invalid layout combine operation";
        output.clear();
        return false;
      }
      destination->push_back({mutation.combineMode, mutation.value});
    }
    BuildRangeIndex();
    return true;
  } catch (...) {
    output.clear();
    ranges_.clear();
    error = "text frame-plan layout mutation allocation failed";
    return false;
  }
}

std::optional<float> ApplyTextEffectLayoutScalarOperations(
    const float authored,
    const std::vector<TextEffectLayoutScalarOperation> &operations) noexcept {
  if (!std::isfinite(authored))
    return std::nullopt;
  float result = authored;
  for (const auto &operation : operations) {
    if (!std::isfinite(operation.value))
      return std::nullopt;
    switch (operation.combineMode) {
    case text::TextPropertyCombineMode::Replace:
      result = operation.value;
      break;
    case text::TextPropertyCombineMode::Add:
      result += operation.value;
      break;
    case text::TextPropertyCombineMode::Multiply:
      result *= operation.value;
      break;
    case text::TextPropertyCombineMode::MatrixConcat:
    case text::TextPropertyCombineMode::ColorMix:
      return std::nullopt;
    }
    if (!std::isfinite(result))
      return std::nullopt;
  }
  return result;
}

std::optional<std::vector<TextEffectVisualLineFontSizeScale>>
ResolveTextEffectVisualLineFontSizeScales(
    const std::vector<TextEffectVisualLineFontSizeSample> &samples) noexcept {
  if (samples.empty())
    return std::vector<TextEffectVisualLineFontSizeScale>{};

  struct Accumulator final {
    std::size_t visualLine{0U};
    float maximumScale{0.0F};
    bool sawTarget{false};
  };
  try {
    std::vector<Accumulator> accumulators;
    accumulators.reserve(samples.size());
    for (const auto &sample : samples) {
      if (!std::isfinite(sample.basis) || sample.basis <= 0.0F)
        return std::nullopt;
      const float target = sample.target.value_or(sample.basis);
      if (!std::isfinite(target) || target < 0.0F)
        return std::nullopt;
      const float scale = target / sample.basis;
      if (!std::isfinite(scale))
        return std::nullopt;
      auto line = std::find_if(
          accumulators.begin(), accumulators.end(),
          [&](const Accumulator &candidate) {
            return candidate.visualLine == sample.visualLine;
          });
      if (line == accumulators.end()) {
        accumulators.push_back(
            {sample.visualLine, scale, sample.target.has_value()});
      } else {
        line->maximumScale = std::max(line->maximumScale, scale);
        line->sawTarget = line->sawTarget || sample.target.has_value();
      }
    }
    std::sort(accumulators.begin(), accumulators.end(),
              [](const Accumulator &left, const Accumulator &right) {
                return left.visualLine < right.visualLine;
              });
    std::vector<TextEffectVisualLineFontSizeScale> result;
    result.reserve(accumulators.size());
    for (const auto &line : accumulators) {
      if (line.sawTarget)
        result.push_back({line.visualLine, line.maximumScale});
    }
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<TextEffectVisualLineOrigin> ResolveTextEffectVisualLineOrigin(
    const float designCenterY, const float baseLineCenterY,
    const float rebuiltLineCenterY,
    const float effectiveMaximumScale) noexcept {
  if (!std::isfinite(designCenterY) || !std::isfinite(baseLineCenterY) ||
      !std::isfinite(rebuiltLineCenterY) ||
      !std::isfinite(effectiveMaximumScale) || effectiveMaximumScale < 0.0F) {
    return std::nullopt;
  }
  const float desiredCenterY =
      designCenterY +
      effectiveMaximumScale * (baseLineCenterY - designCenterY);
  const float offsetY = desiredCenterY - rebuiltLineCenterY;
  if (!std::isfinite(desiredCenterY) || !std::isfinite(offsetY))
    return std::nullopt;
  return TextEffectVisualLineOrigin{desiredCenterY, offsetY};
}

void ResolvedTextEffectLayoutMutations::BuildRangeIndex() {
  ranges_.reserve(values_.size());
  for (std::size_t index = 0U; index < values_.size(); ++index)
    ranges_.push_back({index, values_[index].unit.utf8End});
  std::sort(ranges_.begin(), ranges_.end(),
            [&](const RangeEntry &left, const RangeEntry &right) {
              const auto &a = values_[left.index].unit;
              const auto &b = values_[right.index].unit;
              return std::tie(a.paragraphId, a.runId, a.utf8Begin) <
                     std::tie(b.paragraphId, b.runId, b.utf8Begin);
            });
  for (std::size_t index = 1U; index < ranges_.size(); ++index) {
    const auto &previous = ranges_[index - 1U];
    auto &current = ranges_[index];
    const auto &a = values_[previous.index].unit;
    const auto &b = values_[current.index].unit;
    if (a.paragraphId == b.paragraphId && a.runId == b.runId)
      current.maximumEnd = std::max(previous.maximumEnd, current.maximumEnd);
  }
}

const ResolvedTextEffectLayoutMutation *ResolvedTextEffectLayoutMutations::Find(
    const std::string &paragraphId, const std::string &runId,
    const std::size_t utf8Begin, const std::size_t utf8End) const noexcept {
  auto position = std::upper_bound(
      ranges_.begin(), ranges_.end(), std::tie(paragraphId, runId, utf8Begin),
      [&](const auto &key, const RangeEntry &entry) {
        const auto &unit = values_[entry.index].unit;
        return key < std::tie(unit.paragraphId, unit.runId, unit.utf8Begin);
      });
  std::size_t first = values_.size();
  while (position != ranges_.begin()) {
    const auto &entry = *--position;
    const auto &unit = values_[entry.index].unit;
    if (unit.paragraphId != paragraphId || unit.runId != runId ||
        entry.maximumEnd < utf8End)
      break;
    if (unit.utf8End >= utf8End)
      first = std::min(first, entry.index);
    if (first == 0U)
      break;
  }
  return first == values_.size() ? nullptr : &values_[first];
}

std::uint32_t TextEffectLayoutScalarIdentity(const float value) noexcept {
  std::uint32_t result = 0U;
  static_assert(sizeof(result) == sizeof(value));
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

} // namespace videocut::skia_runtime::internal
