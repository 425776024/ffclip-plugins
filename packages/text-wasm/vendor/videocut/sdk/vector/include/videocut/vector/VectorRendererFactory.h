#pragma once

#include "videocut/vector/VectorRenderLane.h"

#include <memory>
#include <string>

namespace videocut::vector {

class VectorRendererFactory {
public:
    virtual ~VectorRendererFactory() = default;
    virtual VectorRenderLaneHolder CreateLane(
        const VectorRenderOptions& options,
        std::string& error) const = 0;
};

using VectorRendererFactoryHolder =
    std::shared_ptr<VectorRendererFactory>;

} // namespace videocut::vector
