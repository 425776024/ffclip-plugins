#include "text/QtTextPostEffectParameters.h"
#include "text/TextExecutionParameterContract.h"
#include <algorithm>
#include <utility>

namespace videocut::skia_runtime::internal::post_effect {
const text::TextPostEffectParameter *
FindParameter(const text::TextEffectPostEffectNode &effect, const char *name) {
  const auto found =
      std::find_if(effect.parameters.begin(), effect.parameters.end(),
                   [name](const text::TextPostEffectParameter &parameter) {
                     return parameter.name == name;
                   });
  return found == effect.parameters.end() ? nullptr : &*found;
}

double SampleParameterValue(const text::TextEffectPostEffectNode &effect,
                            const char *name, const double progress,
                            const double fallback) {
  const auto *parameter = FindParameter(effect, name);
  if (!parameter)
    return fallback;
  TextPostEffectParameterConsumptionScope::Record(effect, *parameter);
  const double base = parameter->values.empty()
                          ? fallback
                          : static_cast<double>(parameter->values.front());
  return text::SampleTextKeyframeCurve(parameter->keyframes, progress, base);
}

float SampleParameter(const text::TextEffectPostEffectNode &effect, const char *name,
                      const double progress, const float fallback) {
  return static_cast<float>(SampleParameterValue(
      effect, name, progress, static_cast<double>(fallback)));
}

std::array<float, 4> VectorParameter(const text::TextEffectPostEffectNode &effect,
                                     const char *name,
                                     const std::array<float, 4> fallback) {
  const auto *parameter = FindParameter(effect, name);
  if (!parameter || parameter->values.empty())
    return fallback;
  TextPostEffectParameterConsumptionScope::Record(effect, *parameter);
  auto result = fallback;
  for (std::size_t index = 0;
       index < std::min(result.size(), parameter->values.size()); ++index) {
    result[index] = parameter->values[index];
  }
  return result;
}

void SetScalarParameter(text::TextEffectPostEffectNode &effect,
                        const char *name, const float value) {
  auto *existing = const_cast<text::TextPostEffectParameter *>(
      FindParameter(effect, name));
  if (existing) {
    existing->values = {value};
    existing->keyframes.clear();
    return;
  }
  text::TextPostEffectParameter parameter;
  parameter.name = name;
  parameter.values = {value};
  effect.parameters.push_back(std::move(parameter));
}

void SetVectorParameter(text::TextEffectPostEffectNode &effect,
                        const char *name, std::vector<float> value) {
  auto *existing = const_cast<text::TextPostEffectParameter *>(
      FindParameter(effect, name));
  if (existing) {
    existing->values = std::move(value);
    existing->keyframes.clear();
    return;
  }
  text::TextPostEffectParameter parameter;
  parameter.name = name;
  parameter.values = std::move(value);
  effect.parameters.push_back(std::move(parameter));
}

} // namespace videocut::skia_runtime::internal::post_effect
