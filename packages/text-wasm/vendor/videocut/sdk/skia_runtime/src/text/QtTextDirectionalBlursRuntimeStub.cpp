#include "text/QtTextDirectionalBlursRuntime.h"

namespace videocut::skia_runtime::internal {

std::unique_ptr<QtTextDirectionalBlursRuntime>
CreateQtTextDirectionalBlursRuntime(std::string &error) {
  error = "Qt DirectionalBlurs exact runtime requires the Metal backend";
  return {};
}

} // namespace videocut::skia_runtime::internal
