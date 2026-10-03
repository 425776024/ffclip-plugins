#pragma once

#include "videocut/text/RichTextDocument.h"
#include "videocut/text/TextAnimation.h"
#include "videocut/text/TextLayerAppearance.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace videocut::text {

struct TextBindingTarget final {
    std::string paragraphId;
    std::string runId;
};

struct TextTemplateBinding final {
    std::string id;
    std::string defaultText;
    TextBindingTarget target;
};

struct TextTemplate final {
    std::string id;
    std::uint32_t revision{1};
    std::string rendererProfile;
    std::string fallbackName;
    std::string category;
    std::vector<std::string> tags;
    std::int64_t defaultDurationTicks{600'000};
    std::vector<TextTemplateBinding> bindings;
    RichTextDocument document;
    TextLayerAppearance appearance;
    TextAnimationStack animationStack;
};

struct TextTemplateParseResult final {
    TextTemplate value;
    std::vector<Diagnostic> diagnostics;
    bool valid{false};
};

TextTemplateParseResult ParseTextTemplateJson(
    const std::string& json,
    const RichTextLimits& limits = {});

struct TextTemplateInstantiateResult final {
    RichTextDocument document;
    TextAnimationStack animationStack;
    std::vector<Diagnostic> diagnostics;
    bool valid{false};
};

TextTemplateInstantiateResult InstantiateTextTemplate(
    const TextTemplate& textTemplate,
    const std::map<std::string, std::string>& overrides = {});

} // namespace videocut::text
