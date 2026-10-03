#pragma once

#include "videocut/text/TextEffectFramePlan.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>

namespace videocut::skia_runtime::internal::text_lane {

// Freeze the final, retargeted plan for one render call. Consumers share its
// unit index; neither the indexed IDs nor their storage can change afterward.
class TextRenderFramePlan final {
public:
  explicit TextRenderFramePlan(text::TextEffectFramePlan &&plan)
      : plan_(std::move(plan)) {
    unitsById_.reserve(plan_.units.size());
    for (std::size_t index = 0U; index < plan_.units.size(); ++index)
      unitsById_.try_emplace(plan_.units[index].stableUnitId, index);
  }

  TextRenderFramePlan(const TextRenderFramePlan &) = delete;
  TextRenderFramePlan &operator=(const TextRenderFramePlan &) = delete;

  const text::TextEffectFramePlan &value() const noexcept { return plan_; }

  const text::TextEffectUnitFramePlan *
  FindUnit(const std::uint64_t stableUnitId) const noexcept {
    const auto found = unitsById_.find(stableUnitId);
    return found == unitsById_.end() ? nullptr : &plan_.units[found->second];
  }

private:
  text::TextEffectFramePlan plan_;
  // Keep the first occurrence, matching the original ordered linear search.
  std::unordered_map<std::uint64_t, std::size_t> unitsById_;
};

} // namespace videocut::skia_runtime::internal::text_lane
