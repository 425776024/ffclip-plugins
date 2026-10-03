#include "text/QtTextTrailRuntime.h"

namespace videocut::skia_runtime::internal {

std::unique_ptr<QtTextTrailRuntime>
CreateQtTextTrailRuntime(std::string &error) {
  error = "Qt LumiTrail Metal v1 is unavailable in this build";
  return {};
}

} // namespace videocut::skia_runtime::internal
