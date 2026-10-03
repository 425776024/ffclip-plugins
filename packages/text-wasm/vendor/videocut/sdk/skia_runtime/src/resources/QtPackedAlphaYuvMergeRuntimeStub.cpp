#include "resources/QtPackedAlphaYuvMergeRuntime.h"

namespace videocut::skia_runtime::internal {

std::unique_ptr<QtPackedAlphaYuvMergeRuntime>
CreateQtPackedAlphaYuvMergeRuntime(std::string &error) {
  error = "Qt packed-alpha YUV merge Metal v1 is unavailable in this build";
  return {};
}

} // namespace videocut::skia_runtime::internal
