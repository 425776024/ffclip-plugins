#include "text/QtTextDistortChromaMetalRuntime.h"

namespace videocut::skia_runtime::internal {

std::unique_ptr<QtTextDistortChromaMetalRuntime>
CreateQtTextDistortChromaMetalRuntime(std::string &error) {
  error = "Qt LumiDistortChroma requires the product Metal runtime";
  return {};
}

} // namespace videocut::skia_runtime::internal
