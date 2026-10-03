#include "text/QtTextDustMetalRuntime.h"

namespace videocut::skia_runtime::internal {

std::unique_ptr<QtTextDustMetalRuntime>
CreateQtTextDustMetalRuntime(std::string &error) {
  error = "native Qt LumiDust Metal runtime is unavailable on this platform";
  return {};
}

} // namespace videocut::skia_runtime::internal
