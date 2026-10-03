#pragma once
#include "videocut/text_composition/TextCompositionDocument.h"
#include <nlohmann/json.hpp>
#include <string>
namespace videocut::text_wasm {
bool DecodePackageAnimation(const nlohmann::json& effect, const nlohmann::json& animation,
    text_composition::TextCompositionDocument& composition, std::string& error);
}
