#include "text/QtTextLetterMetalRuntime.h"

namespace videocut::skia_runtime::internal {

std::unique_ptr<QtTextLetterMetalRuntime>
CreateQtTextLetterMetalRuntime(std::string &error) {
  error = "Qt TextPro letter native Metal v1 is unavailable in this build";
  return {};
}

} // namespace videocut::skia_runtime::internal
