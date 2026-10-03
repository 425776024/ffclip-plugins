#pragma once

#include "videocut/text/TextEffectFramePlan.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace videocut::text::internal {

// One invariant layout per program stage, shared by every component read.
struct EllipticRowLayoutCache final {
  std::optional<std::array<double, 4>> parameters;
  std::vector<std::array<double, 4>> units;
};

bool EvaluateEllipticRowLayout(
    const TextEffectFrameInput &frame, const std::array<double, 4> &parameters,
    std::size_t unitIndex, std::size_t component,
    EllipticRowLayoutCache &cache, double &value, std::string &error);

} // namespace videocut::text::internal
