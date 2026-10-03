#pragma once

#include "videocut/skia_runtime/SkiaRuntimeFactory.h"

namespace videocut::skia_runtime::internal {

text::TextRendererFactoryHolder MakeTextRendererFactory(
    const SkiaRuntimeConfig& config,
    std::string& error);

vector::VectorRendererFactoryHolder MakeVectorRendererFactory(
    const SkiaRuntimeConfig& config,
    std::string& error);

} // namespace videocut::skia_runtime::internal

