#include "text/QtTextSoftGlowMetalRuntime.h"

namespace videocut::skia_runtime::internal {

std::unique_ptr<QtTextSoftGlowMetalRuntime>
CreateQtTextSoftGlowMetalRuntime(std::string &error) {
  error = "Qt LumiSoftGlow native Metal v1 is unavailable in this build";
  return {};
}

} // namespace videocut::skia_runtime::internal
