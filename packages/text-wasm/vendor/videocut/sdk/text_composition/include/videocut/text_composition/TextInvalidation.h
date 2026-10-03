#pragma once

#include "videocut/text/TextProperty.h"

#include <string>
#include <vector>

namespace videocut::text_composition {

using TextInvalidationImpact = text::TextInvalidationImpact;

struct TextResourceKey final {
  std::string digest;
};

struct TextLayoutKey final {
  std::string digest;
};

struct TextGlyphKey final {
  std::string digest;
};

struct TextBackdropKey final {
  std::string digest;
};

struct TextPostEffectKey final {
  std::string digest;
};

struct TextCompositeKey final {
  std::string digest;
};

struct TextInvalidationPlan final {
  TextInvalidationImpact impact{TextInvalidationImpact::None};
  std::vector<std::string> paragraphIds;
  std::vector<std::string> runIds;
  std::vector<std::string> glyphLayerIds;
  std::vector<std::string> backdropLayerIds;
};

TextInvalidationPlan
PlanTextInvalidation(const text::TextPropertyPatch &patch) noexcept;

} // namespace videocut::text_composition
