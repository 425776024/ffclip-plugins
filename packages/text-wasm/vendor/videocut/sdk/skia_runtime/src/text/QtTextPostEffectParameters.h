#pragma once
#include "videocut/text/TextEffectFramePlan.h"
#include <array>
#include <vector>

namespace videocut::skia_runtime::internal::post_effect {
const text::TextPostEffectParameter *FindParameter(
    const text::TextEffectPostEffectNode &effect, const char *name);
double SampleParameterValue(const text::TextEffectPostEffectNode &effect,
                           const char *name, double progress, double fallback);
float SampleParameter(const text::TextEffectPostEffectNode &effect,
                      const char *name, double progress, float fallback);
std::array<float, 4> VectorParameter(const text::TextEffectPostEffectNode &effect,
                                   const char *name, std::array<float, 4> fallback);
void SetScalarParameter(text::TextEffectPostEffectNode &effect,
                        const char *name, float value);
void SetVectorParameter(text::TextEffectPostEffectNode &effect,
                        const char *name, std::vector<float> value);
} // namespace videocut::skia_runtime::internal::post_effect
