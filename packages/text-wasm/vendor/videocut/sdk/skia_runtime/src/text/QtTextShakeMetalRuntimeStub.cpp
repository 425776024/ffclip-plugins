#include "text/QtTextShakeMetalRuntime.h"

namespace videocut::skia_runtime::internal {

std::unique_ptr<QtTextShakeMetalRuntime>
CreateQtTextShakeMetalRuntime(std::string &error) {
  error = "Qt LumiSShake native Metal v1 is unavailable in this build";
  return {};
}

} // namespace videocut::skia_runtime::internal
