#include "text/QtTextTurbulenceMetalRuntime.h"

namespace videocut::skia_runtime::internal {

bool RenderQtTextMaterialTurbulence(
    const QtTextMaterialTurbulenceUniforms &, void *, void *, void *,
    std::string &error, const NativeCommandSubmission &) {
  error = "text material turbulence requires the native Metal backend";
  return false;
}

std::unique_ptr<QtTextTurbulenceMetalRuntime>
CreateQtTextTurbulenceMetalRuntime(std::string &error) {
  error =
      "Qt LumiTurbulenceDisplacement native Metal v1 is unavailable in this build";
  return {};
}

} // namespace videocut::skia_runtime::internal
