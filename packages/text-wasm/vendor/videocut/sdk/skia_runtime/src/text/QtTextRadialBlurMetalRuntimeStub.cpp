#include "text/QtTextRadialBlurMetalRuntime.h"

namespace videocut::skia_runtime::internal {

std::unique_ptr<QtTextRadialBlurMetalRuntime>
CreateQtTextRadialBlurMetalRuntime(std::string &error) {
  error = "Qt LumiRadialBlur native Metal v1 is unavailable in this build";
  return {};
}

} // namespace videocut::skia_runtime::internal
