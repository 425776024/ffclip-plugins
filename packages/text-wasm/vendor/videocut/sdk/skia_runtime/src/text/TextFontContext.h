#pragma once

#include "internal/Factories.h"
#include "internal/skia/SkiaHeaders.h"
#include "text/FontInstanceResolver.h"
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace videocut::skia_runtime::internal {

struct FontContext final {
  sk_sp<SkFontMgr> assetManager;
  sk_sp<SkFontMgr> systemManager;
  sk_sp<skia::textlayout::FontCollection> collection;
  sk_sp<SkUnicode> unicode;
  std::unordered_map<std::string, ResolvedFontInstance> typefaces;
  std::vector<text::FontRunResolutionReceipt> resolutionReceipts;
  std::vector<text::Diagnostic> diagnostics;

  void LayoutParagraph(skia::textlayout::Paragraph &paragraph, float width) const;

private:
  // FontCollection owns mutable face/paragraph caches shared by render lanes.
  mutable std::mutex paragraphMutex_;
};

SkFontStyle::Slant ToSkSlant(text::FontSlant slant) noexcept;
std::string ExactFontFamilyAlias(const text::FontReference &reference);
std::string FontResourceIdentity(const text::ResolvedRichTextView &view,
                                text::TextSourceCreationComponent sourceCreationComponent);
std::vector<text::FontRunResolutionReceipt> BuildFontResolutionReceipts(
    const text::ResolvedRichTextView &view, const FontContext &context);

class FontContextCache final {
public:
  std::shared_ptr<FontContext> Resolve(
      const std::string &key, const text::ResolvedRichTextView &view,
      text::TextSourceCreationComponent sourceCreationComponent,
      const SkiaRuntimeConfig &config,
      const text::TextAssetResolver::Holder &assets,
      std::vector<text::FontRunResolutionReceipt> *attemptReceipts,
      std::string &error);
private:
  std::mutex mutex_;
  std::unordered_map<std::string, std::weak_ptr<FontContext>> entries_;
};

} // namespace videocut::skia_runtime::internal
