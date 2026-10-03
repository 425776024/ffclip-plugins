#include "text/QtTextGaussianMetalRuntime.h"

namespace videocut::skia_runtime::internal {

std::unique_ptr<QtTextGaussianMetalRuntime>
CreateQtTextGaussianMetalRuntime(std::string &error) {
  error = "Qt LumiGaussianBlur native Metal v1 is unavailable in this build";
  return {};
}

} // namespace videocut::skia_runtime::internal
