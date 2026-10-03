#pragma once

#include "QtTextSelectorRandom.h"
#include "videocut/text/TextAnimation.h"

namespace videocut::text {

struct TextSelectorAttributeSample final {
  double rangeStart{0.0};
  double rangeEnd{0.0};
  double offset{0.0};
  double intensity{0.0};
};

struct TextAnimatorFrameSample final {
  std::vector<TextSelectorAttributeSample> selectors;
  std::vector<double> tracks;
  std::optional<Color> fillColor;
};

// One immutable animator, selector domain and layer clock per evaluation.
// Group samples share frame-constant curves and lazily built random orders;
// nothing is retained across frames.
class TextAnimatorSampler final {
public:
  TextAnimatorSampler(const TextAnimatorSpec &animator,
                      std::size_t unitCount, double layerProgress) noexcept;

  TextUnitAnimationSample Sample(std::size_t unitIndex) noexcept;

private:
  const TextAnimatorSpec &animator_;
  std::size_t unitCount_;
  double layerProgress_;
  std::vector<QtTextSelectorRandomMap> randomOrders_;
  TextAnimatorFrameSample frameSample_;
};

} // namespace videocut::text
