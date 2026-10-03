#pragma once
#include "videocut/text_composition/TextCompositionDocument.h"
#include "videocut/text/TextRenderLane.h"
#include <nlohmann/json.hpp>
namespace videocut::text_wasm {
void ValidateBrowserComposition(const text_composition::TextCompositionDocument& document);
nlohmann::json SampleBrowserComposition(const text_composition::TextCompositionDocument& document,
    const text::TextLayout& layout, std::int64_t timeUs, unsigned width, unsigned height);
}
