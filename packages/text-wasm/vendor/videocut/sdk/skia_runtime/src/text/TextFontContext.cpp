#include "text/TextFontContext.h"
#include "text/TextRenderIdentity.h"
#include "resources/SkiaResourceProvider.h"
#include "videocut/vector/VectorDigest.h"
#include <algorithm>
#include <iterator>

namespace videocut::skia_runtime::internal {
using text::Diagnostic;
using text::DiagnosticSeverity;

SkFontStyle::Slant ToSkSlant(const text::FontSlant slant) noexcept {
  switch (slant) {
  case text::FontSlant::Italic:
    return SkFontStyle::kItalic_Slant;
  case text::FontSlant::Oblique:
    return SkFontStyle::kOblique_Slant;
  case text::FontSlant::Upright:
    return SkFontStyle::kUpright_Slant;
  }
  return SkFontStyle::kUpright_Slant;
}

std::string ExactFontFamilyAlias(const std::string_view referenceKey) {
  return "videocut-font-instance-" +
         videocut::vector::Sha256Digest(
             reinterpret_cast<const std::uint8_t *>(referenceKey.data()),
             referenceKey.size());
}

std::string ExactFontFamilyAlias(const text::FontReference &reference) {
  return ExactFontFamilyAlias(
      FontInstanceResolver::ReferenceCacheKey(reference));
}


std::string StableFontInstanceFailureCode(
    const FontInstanceFailureCode code) {
  using Code = FontInstanceFailureCode;
  switch (code) {
  case Code::InvalidContentIdentity:
    return "font-instance-invalid-content-identity";
  case Code::InvalidFaceIndex:
    return "font-instance-invalid-face-index";
  case Code::BaseTypefaceUnavailable:
    return "font-instance-base-typeface-unavailable";
  case Code::InvalidAxisTag:
    return "font-instance-invalid-axis-tag";
  case Code::NonFiniteAxisValue:
    return "font-instance-non-finite-axis-value";
  case Code::DuplicateAxis:
    return "font-instance-duplicate-axis";
  case Code::AxisCapacityExceeded:
    return "font-instance-axis-capacity-exceeded";
  case Code::TypefaceAxisMetadataUnavailable:
    return "font-instance-axis-metadata-unavailable";
  case Code::TypefaceAxisMetadataInvalid:
    return "font-instance-axis-metadata-invalid";
  case Code::UnknownAxis:
    return "font-instance-unknown-axis";
  case Code::AxisValueOutOfRange:
    return "font-instance-axis-value-out-of-range";
  case Code::CloneFailed:
    return "font-instance-clone-failed";
  case Code::CloneVerificationFailed:
    return "font-instance-clone-verification-failed";
  }
  return "font-instance-resolution-failed";
}

sk_sp<SkTypeface> MatchDefaultTypeface(
    const text::FontReference &reference, const SkiaRuntimeConfig &config,
    const sk_sp<SkFontMgr> &systemManager) {
  if (!config.allowSystemFonts || !systemManager)
    return {};
  const SkFontStyle style(reference.weight, reference.width,
                          ToSkSlant(reference.slant));
  for (const auto &family : config.defaultFontFamilies) {
    if (family.empty())
      continue;
    if (auto typeface = systemManager->matchFamilyStyle(family.c_str(), style))
      return typeface;
  }
  return systemManager->matchFamilyStyle(nullptr, style);
}

bool ResolveFont(
    const text::FontReference &reference, const SkiaRuntimeConfig &config,
    const text::TextAssetResolver::Holder &textAssets,
    const sk_sp<SkFontMgr> &dataManager,
    const sk_sp<SkFontMgr> &systemManager,
    std::unordered_map<std::string, ResolvedFontInstance> &typefaces,
    std::vector<Diagnostic> &diagnostics, std::string &failureCode,
    std::string &error) {
  failureCode.clear();
  const auto key = FontInstanceResolver::ReferenceCacheKey(reference);
  if (typefaces.find(key) != typefaces.end())
    return true;

  sk_sp<SkTypeface> baseTypeface;
  sk_sp<SkData> fontData;
  std::string contentIdentity;
  std::string resolutionError;
  bool substituted = false;
  if (reference.kind == text::FontSourceKind::System) {
    if (config.allowSystemFonts && systemManager) {
      baseTypeface = systemManager->matchFamilyStyle(
          reference.family.c_str(),
          SkFontStyle(reference.weight, reference.width,
                      ToSkSlant(reference.slant)));
    }
    if (!baseTypeface) {
      resolutionError = "system font is unavailable";
      failureCode = "system-font-unavailable";
    }
  } else if (reference.kind == text::FontSourceKind::Builtin) {
    RuntimeAsset asset;
    if (!config.assets ||
        !config.assets->Resolve(RuntimeAssetKind::Font, reference.assetId,
                                asset, resolutionError) ||
        asset.logicalId != reference.assetId || !asset.bytes ||
        asset.bytes->empty()) {
      if (resolutionError.empty())
        resolutionError = "built-in font identity did not resolve";
      failureCode = "builtin-font-asset-unavailable";
    } else {
      fontData =
          SkData::MakeWithCopy(asset.bytes->data(), asset.bytes->size());
      contentIdentity = videocut::vector::Sha256Digest(asset.bytes->data(),
                                                       asset.bytes->size());
    }
  } else {
    text::TextAssetBytes asset;
    if (!textAssets ||
        !textAssets->Resolve(text::TextAssetKind::Font, reference.assetId,
                             reference.digest, asset, resolutionError) ||
        asset.kind != text::TextAssetKind::Font ||
        asset.logicalId != reference.assetId ||
        asset.contentDigest != reference.digest || !asset.bytes ||
        asset.bytes->empty() || asset.byteLength != asset.bytes->size()) {
      if (resolutionError.empty())
        resolutionError = "project font identity did not resolve";
      failureCode = "project-font-asset-unavailable";
    } else {
      fontData =
          SkData::MakeWithCopy(asset.bytes->data(), asset.bytes->size());
      contentIdentity = asset.contentDigest;
    }
  }

  if (!baseTypeface && !fontData) {
    if (reference.kind != text::FontSourceKind::System ||
        !reference.allowSystemGlyphFallback) {
      error = resolutionError + ": " + reference.family;
      if (failureCode.empty())
        failureCode = "font-resource-unavailable";
      return false;
    }
    baseTypeface = MatchDefaultTypeface(reference, config, systemManager);
    if (!baseTypeface) {
      error = resolutionError + "; configured fallback is unavailable";
      failureCode = "configured-default-font-unavailable";
      return false;
    }
    diagnostics.push_back(Diagnostic{
        "text.font.default_fallback", DiagnosticSeverity::Warning,
        "resource", reference.family,
        resolutionError + "; using the configured default font"});
    substituted = true;
  }

  if (!fontData) {
    SkString postscript;
    if (baseTypeface && baseTypeface->getPostScriptName(&postscript) &&
        !postscript.isEmpty()) {
      contentIdentity = "system:" + std::string(postscript.c_str());
    } else {
      contentIdentity = "system:" + reference.family;
    }
  }
  if (!reference.faceFingerprint.empty())
    contentIdentity += "|face:" + reference.faceFingerprint;

  auto instance =
      fontData
          ? FontInstanceResolver::ResolveFromData(reference, dataManager,
                                                  fontData, contentIdentity)
          : FontInstanceResolver::ResolveFromTypeface(
                reference, std::move(baseTypeface), contentIdentity);
  if (!instance) {
    failureCode = instance.failure
                      ? StableFontInstanceFailureCode(instance.failure->code)
                      : "font-instance-resolution-failed";
    error = instance.failure
                ? instance.failure->Describe(reference.family)
                : "font instance resolution failed: " + reference.family;
    return false;
  }
  instance.substituted = substituted;
  typefaces.emplace(key, std::move(instance));
  return true;
}

std::string FontResourceIdentity(
    const text::ResolvedRichTextView &view,
    const text::TextSourceCreationComponent sourceCreationComponent) {
  std::vector<std::string> keys;
  for (const auto &paragraph : view.paragraphs) {
    for (const auto &run : paragraph.runs) {
      keys.push_back(
          FontInstanceResolver::ReferenceCacheKey(run.style.font.primary));
      for (const auto &fallback : run.style.font.fallbacks)
        keys.push_back(FontInstanceResolver::ReferenceCacheKey(fallback));
    }
  }
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  IdentityBuilder identity;
  identity.Add(sourceCreationComponent);
  for (const auto &key : keys)
    identity.AddString(key);
  return identity.Finish();
}

text::FontRunResolutionReceipt AuthoredFontResolutionReceipt(
    const text::RichTextParagraph &paragraph,
    const text::RichTextRun &run) {
  text::FontRunResolutionReceipt receipt;
  receipt.paragraphId = paragraph.paragraphId;
  receipt.runId = run.runId;
  receipt.requestedPrimary = run.style.font.primary;
  receipt.systemGlyphFallbackAllowed =
      run.style.font.allowSystemGlyphFallback ||
      run.style.font.primary.allowSystemGlyphFallback;
  return receipt;
}

std::shared_ptr<FontContext>
BuildFontContext(const text::ResolvedRichTextView &view,
                 const text::TextSourceCreationComponent sourceCreationComponent,
                 const SkiaRuntimeConfig &config,
                 const text::TextAssetResolver::Holder &textAssets,
                 std::vector<text::FontRunResolutionReceipt> &attemptReceipts,
                 std::string &error) {
  attemptReceipts.clear();
  auto context = std::make_shared<FontContext>();
  context->unicode = SkUnicodes::ICU::Make();
  if (!context->unicode) {
    error = "SkUnicode ICU initialization failed";
    return {};
  }
  if (config.allowSystemFonts)
    context->systemManager = MakeSystemFontManager();
  // SDFText's source layout uses CoreText metrics on macOS. FreeType's
  // fixed-point font scale slightly changes even a full-em advance, which
  // then changes dimension-derived particle seeds. Load the same admitted
  // font bytes through the native backend; system fallback remains governed
  // independently by config below.
#if defined(__APPLE__)
  auto dataManager =
      sourceCreationComponent == text::TextSourceCreationComponent::SdfText
          ? MakeSystemFontManager()
          : SkFontMgr_New_Custom_Data({});
#else
  static_cast<void>(sourceCreationComponent);
  auto dataManager = SkFontMgr_New_Custom_Data({});
#endif
  if (!dataManager) {
    error = "Skia data font manager initialization failed";
    return {};
  }

  for (const auto &paragraph : view.paragraphs) {
    for (const auto &run : paragraph.runs) {
      auto receipt = AuthoredFontResolutionReceipt(paragraph, run);
      std::string failureCode;
      const auto resolve = [&](const text::FontReference &reference) {
        return ResolveFont(reference, config, textAssets, dataManager,
                           context->systemManager, context->typefaces,
                           context->diagnostics, failureCode, error);
      };
      if (!resolve(run.style.font.primary)) {
        receipt.status = text::FontRunResolutionReceipt::Status::Failed;
        receipt.failureStage =
            text::FontRunResolutionReceipt::FailureStage::Primary;
        receipt.failureCode = failureCode.empty()
                                  ? "font-primary-resolution-failed"
                                  : std::move(failureCode);
        attemptReceipts.push_back(std::move(receipt));
        return {};
      }
      const auto primary = context->typefaces.find(
          FontInstanceResolver::ReferenceCacheKey(run.style.font.primary));
      if (primary == context->typefaces.end()) {
        error = "resolved primary font instance is unavailable";
        receipt.status = text::FontRunResolutionReceipt::Status::Failed;
        receipt.failureStage =
            text::FontRunResolutionReceipt::FailureStage::Primary;
        receipt.failureCode = "font-primary-instance-missing";
        attemptReceipts.push_back(std::move(receipt));
        return {};
      }
      receipt.resolvedPrimaryIdentity = primary->second.identity;
      receipt.supportedAxes = primary->second.supportedAxes;
      receipt.primarySubstituted = primary->second.substituted;
      receipt.resolvedFallbackIdentities.reserve(
          run.style.font.fallbacks.size());
      for (std::size_t fallbackIndex = 0U;
           fallbackIndex < run.style.font.fallbacks.size(); ++fallbackIndex) {
        const auto &fallback = run.style.font.fallbacks[fallbackIndex];
        if (!resolve(fallback)) {
          receipt.status = text::FontRunResolutionReceipt::Status::Failed;
          receipt.failureStage =
              text::FontRunResolutionReceipt::FailureStage::ExplicitFallback;
          receipt.failedFallbackIndex =
              static_cast<std::uint32_t>(fallbackIndex);
          receipt.failureCode = failureCode.empty()
                                    ? "font-fallback-resolution-failed"
                                    : std::move(failureCode);
          attemptReceipts.push_back(std::move(receipt));
          return {};
        }
        const auto resolved = context->typefaces.find(
            FontInstanceResolver::ReferenceCacheKey(fallback));
        if (resolved == context->typefaces.end()) {
          error = "resolved fallback font instance is unavailable";
          receipt.status = text::FontRunResolutionReceipt::Status::Failed;
          receipt.failureStage =
              text::FontRunResolutionReceipt::FailureStage::ExplicitFallback;
          receipt.failedFallbackIndex =
              static_cast<std::uint32_t>(fallbackIndex);
          receipt.failureCode = "font-fallback-instance-missing";
          attemptReceipts.push_back(std::move(receipt));
          return {};
        }
        receipt.resolvedFallbackIdentities.push_back(
            resolved->second.identity);
      }
      attemptReceipts.push_back(std::move(receipt));
    }
  }
  context->resolutionReceipts = attemptReceipts;

  auto provider = sk_make_sp<skia::textlayout::TypefaceFontProvider>();
  for (const auto &[key, instance] : context->typefaces) {
    if (!instance.typeface) {
      error = "resolved font instance is unavailable";
      return {};
    }
    const auto alias = ExactFontFamilyAlias(key);
    provider->registerTypeface(instance.typeface, SkString(alias.c_str()));
  }
  context->assetManager = std::move(provider);
  context->collection = sk_make_sp<skia::textlayout::FontCollection>();
  context->collection->setAssetFontManager(context->assetManager);
  if (context->systemManager) {
    context->collection->setDynamicFontManager(context->systemManager);
    context->collection->setDefaultFontManager(context->systemManager);
    if (!config.allowSystemGlyphFallback)
      context->collection->disableFontFallback();
  } else {
    context->collection->disableFontFallback();
  }
  return context;
}

std::vector<text::FontRunResolutionReceipt> BuildFontResolutionReceipts(
    const text::ResolvedRichTextView &view, const FontContext &context) {
  std::vector<text::FontRunResolutionReceipt> receipts;
  for (const auto &paragraph : view.paragraphs) {
    receipts.reserve(receipts.size() + paragraph.runs.size());
    for (const auto &run : paragraph.runs) {
      auto receipt = AuthoredFontResolutionReceipt(paragraph, run);
      const auto primary = context.typefaces.find(
          FontInstanceResolver::ReferenceCacheKey(run.style.font.primary));
      if (primary == context.typefaces.end()) {
        receipt.status = text::FontRunResolutionReceipt::Status::Failed;
        receipt.failureStage =
            text::FontRunResolutionReceipt::FailureStage::Primary;
        receipt.failureCode = "font-primary-instance-missing";
        receipts.push_back(std::move(receipt));
        continue;
      }
      receipt.resolvedPrimaryIdentity = primary->second.identity;
      receipt.supportedAxes = primary->second.supportedAxes;
      receipt.primarySubstituted = primary->second.substituted;
      receipt.resolvedFallbackIdentities.reserve(
          run.style.font.fallbacks.size());
      for (std::size_t fallbackIndex = 0U;
           fallbackIndex < run.style.font.fallbacks.size(); ++fallbackIndex) {
        const auto fallback = context.typefaces.find(
            FontInstanceResolver::ReferenceCacheKey(
                run.style.font.fallbacks[fallbackIndex]));
        if (fallback == context.typefaces.end()) {
          receipt.status = text::FontRunResolutionReceipt::Status::Failed;
          receipt.failureStage = text::FontRunResolutionReceipt::FailureStage::
              ExplicitFallback;
          receipt.failedFallbackIndex =
              static_cast<std::uint32_t>(fallbackIndex);
          receipt.failureCode = "font-fallback-instance-missing";
          break;
        }
        receipt.resolvedFallbackIdentities.push_back(
            fallback->second.identity);
      }
      receipts.push_back(std::move(receipt));
    }
  }
  return receipts;
}

void FontContext::LayoutParagraph(skia::textlayout::Paragraph &paragraph,
                                  const float width) const {
  std::lock_guard<std::mutex> lock(paragraphMutex_);
  paragraph.layout(width);
}

  std::shared_ptr<FontContext>
  FontContextCache::Resolve(const std::string &key, const text::ResolvedRichTextView &view,
          const text::TextSourceCreationComponent sourceCreationComponent,
          const SkiaRuntimeConfig &config,
          const text::TextAssetResolver::Holder &assets,
          std::vector<text::FontRunResolutionReceipt> *attemptReceipts,
          std::string &error) {
    if (attemptReceipts)
      attemptReceipts->clear();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto found = entries_.find(key);
      if (found != entries_.end()) {
        if (auto context = found->second.lock()) {
          if (attemptReceipts)
            *attemptReceipts = BuildFontResolutionReceipts(view, *context);
          return context;
        }
        entries_.erase(found);
      }
    }
    std::vector<text::FontRunResolutionReceipt> attempted;
    auto built = BuildFontContext(view, sourceCreationComponent, config, assets,
                                  attempted, error);
    if (attemptReceipts)
      *attemptReceipts = attempted;
    if (!built)
      return {};
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto iterator = entries_.begin(); iterator != entries_.end();) {
      iterator = iterator->second.expired() ? entries_.erase(iterator)
                                            : std::next(iterator);
    }
    if (entries_.size() >= 256U)
      entries_.clear();
    entries_[key] = built;
    return built;
  }

} // namespace videocut::skia_runtime::internal
