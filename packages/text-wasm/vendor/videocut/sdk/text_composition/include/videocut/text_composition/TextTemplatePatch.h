#pragma once

#include "videocut/text_composition/TextCompositionDocument.h"
#include "videocut/text/TextProperty.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace videocut::text_composition {

enum class TextTemplateChannel : std::uint8_t {
  Content = 0,
  Typography,
  GlyphFills,
  GlyphStrokes,
  GlyphShadows,
  GlyphGlows,
  InlineDecorations,
  Paragraph,
  Layout,
  Bubble,
  Frame,
  CustomBackdrops,
  Bend,
  Path,
  SdfMaterial,
  Animations,
  VectorDecorations,
  TimedText,
  Resources,
};

enum class TextTemplateChannelMode : std::uint8_t {
  Keep = 0,
  Merge,
  Replace,
  Clear,
};

struct TextTemplateChannelRule final {
  TextTemplateChannel channel{TextTemplateChannel::Content};
  TextTemplateChannelMode mode{TextTemplateChannelMode::Keep};
  std::string contentSlotId;
  std::string targetLayerId;
  std::string semanticRole;
};

struct TextTemplateTextBinding final {
  std::string bindingId;
  std::string contentSlotId;
  std::string defaultText;
};

struct TextTemplatePatch final {
  std::string templateId;
  std::string packageDigest;
  std::vector<TextTemplateChannelRule> rules;
  std::vector<TextTemplateTextBinding> textBindings;
  TextCompositionDocument value{};
};

/// Record each appearance slot touched by an admitted template. A slot not
/// addressed by the patch keeps its previous origin.
void RecordTextTemplateSlotOrigins(TextCompositionDocument &document,
                                   const TextTemplatePatch &patch);

/// Release resources required by the previous document but no longer
/// referenced by any channel in the updated document.
void PruneResourcesNoLongerRequired(TextCompositionDocument &document,
                                    const TextCompositionDocument &before);

enum class TextTemplateAppearanceSlot : std::uint8_t { Bubble, Flower };

struct TextTemplateApplicationResult final {
  bool valid{false};
  bool changed{false};
  std::vector<TextTemplateChannel> changedChannels;
  std::vector<text::TextPropertyAddress> overwrittenProperties;
  std::vector<Diagnostic> diagnostics;
};

TextTemplateApplicationResult ClearTextTemplateAppearanceSlot(
    TextCompositionDocument &document, TextTemplateAppearanceSlot slot,
    const std::vector<text::TextPropertyAddress> &animatedProperties = {});

struct TextTemplateParseResult final {
  bool valid{false};
  TextTemplatePatch value{};
  std::vector<Diagnostic> diagnostics;
};

std::string_view TextTemplateDecorationIdentitySlot(
    const TextTemplatePatch &patch) noexcept;

TextTemplateParseResult ParseTextTemplateJson(
    const std::string &json, const TextCompositionLimits &limits = {});

TextTemplateApplicationResult ApplyTextTemplatePatch(
    TextCompositionDocument &document, const TextTemplatePatch &patch,
    const std::map<std::string, std::string> &textOverrides = {},
    const TextCompositionLimits &limits = {},
    std::string_view decorationIdentityNamespace = {},
    const std::vector<text::TextPropertyAddress> &animatedProperties = {});

/// Promotes package-local decoration identities into one durable composition
/// namespace. The operation is deterministic and idempotent and updates both
/// authored bindings and animation references as one closure.
void NamespaceTextTemplateDecorationIdentities(
    TextCompositionDocument &document, std::string_view identityNamespace,
    std::string_view slot);

} // namespace videocut::text_composition
