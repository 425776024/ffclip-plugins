#include "videocut/skia_runtime/SkiaRuntimeFactory.h"

#include "internal/Factories.h"

#include <exception>
#include <memory>
#include <utility>

#ifndef VIDEOCUT_SKIA_REVISION
#define VIDEOCUT_SKIA_REVISION "unknown"
#endif

#ifndef VIDEOCUT_SKIA_RENDERER_PROFILE
#define VIDEOCUT_SKIA_RENDERER_PROFILE "unknown"
#endif

namespace videocut::skia_runtime {
namespace internal {

SkiaRuntimeCapabilities MakeCapabilities()
{
    SkiaRuntimeCapabilities result;
    result.skiaRevision = VIDEOCUT_SKIA_REVISION;
    result.artifactProfile = VIDEOCUT_SKIA_RENDERER_PROFILE;
    result.textRendererProfile = "videocut-skia-text-v1";
    result.vectorRendererProfile = "videocut-skia-vector-v1";
#if defined(VIDEOCUT_SKIA_METAL_RUNTIME)
    result.textGpuRaster = true;
    result.textMetal = true;
    result.textNativeFragmentDerivatives = true;
#endif
    return result;
}

} // namespace internal
namespace {

class Runtime final : public SkiaRuntime {
public:
    Runtime(
        SkiaRuntimeCapabilities capabilities,
        text::TextRendererFactoryHolder textRenderer,
        vector::VectorRendererFactoryHolder vectorRenderer)
        : capabilities_(std::move(capabilities))
        , textRenderer_(std::move(textRenderer))
        , vectorRenderer_(std::move(vectorRenderer))
    {
    }

    const SkiaRuntimeCapabilities& capabilities() const noexcept override
    {
        return capabilities_;
    }

    text::TextRendererFactoryHolder textRenderer() const noexcept override
    {
        return textRenderer_;
    }

    vector::VectorRendererFactoryHolder vectorRenderer() const noexcept override
    {
        return vectorRenderer_;
    }

private:
    SkiaRuntimeCapabilities capabilities_;
    text::TextRendererFactoryHolder textRenderer_;
    vector::VectorRendererFactoryHolder vectorRenderer_;
};

} // namespace

SkiaRuntimeHolder CreateSkiaRuntime(
    SkiaRuntimeConfig config,
    std::string& error) noexcept
{
    error.clear();
    try
    {
        auto textRenderer =
            internal::MakeTextRendererFactory(config, error);
        if (!textRenderer)
            return {};
        auto vectorRenderer =
            internal::MakeVectorRendererFactory(config, error);
        if (!vectorRenderer)
            return {};
        return std::make_shared<Runtime>(
            internal::MakeCapabilities(),
            std::move(textRenderer),
            std::move(vectorRenderer));
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
    }
    catch (...)
    {
        error = "unknown Skia runtime initialization failure";
    }
    return {};
}

} // namespace videocut::skia_runtime
