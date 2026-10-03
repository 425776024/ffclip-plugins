#include "text/QtTextEngineCopyMetalRuntime.h"

namespace videocut::skia_runtime::internal {

std::unique_ptr<QtTextEngineCopyMetalRuntime>
CreateQtTextEngineCopyMetalRuntime(std::string &error) {
  error = "Qt EngineCopy native Metal v1 is unavailable in this build";
  return {};
}

} // namespace videocut::skia_runtime::internal
