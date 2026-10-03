#pragma once

#include "videocut/text/TextRenderLane.h"

#include <memory>
#include <string>

namespace videocut::text {

class TextRendererFactory {
public:
    virtual ~TextRendererFactory() = default;
    virtual TextRenderLaneHolder CreateLane(
        const TextRenderOptions& options,
        std::string& error) const = 0;
};

using TextRendererFactoryHolder = std::shared_ptr<TextRendererFactory>;

} // namespace videocut::text
