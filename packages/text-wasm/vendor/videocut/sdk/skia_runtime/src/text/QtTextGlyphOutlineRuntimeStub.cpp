#include "text/QtTextGlyphOutlineRuntime.h"

namespace videocut::skia_runtime::internal {

bool BuildQtTextGlyphOutlinePath(const QtTextGlyphOutlineRequest &,
                                 QtTextGlyphOutlineResult &result,
                                 std::string &error) {
  result = QtTextGlyphOutlineResult{};
  error = "native Qt TextPro glyph-outline extraction is unavailable";
  return false;
}

} // namespace videocut::skia_runtime::internal
