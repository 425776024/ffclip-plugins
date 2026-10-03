#include "videocut/text_composition/TextCompositionDocument.h"

#include "CanonicalTextIdentity.h"
#include "videocut/base/Sha256.h"
#include "videocut/text/internal/RichTextValidation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace videocut::text_composition {
namespace {

void Add(std::vector<Diagnostic> &diagnostics, std::string code,
         std::string subject, std::string message,
         const DiagnosticSeverity severity = DiagnosticSeverity::Error) {
  diagnostics.push_back({std::move(code), severity, "validate",
                         std::move(subject), std::move(message)});
}

bool Finite(const float value) noexcept { return std::isfinite(value); }
bool Finite(const double value) noexcept { return std::isfinite(value); }

bool ValidIdentity(const std::string &value, const std::size_t maximum,
                   const bool allowEmpty = false) noexcept {
  return (allowEmpty || !value.empty()) && value.size() <= maximum &&
         text::IsValidUtf8(value);
}

bool PortableAssetIdentity(const std::string &value,
                           const std::size_t maximum) noexcept {
  if (!ValidIdentity(value, maximum) || value.find('\\') != std::string::npos)
    return false;
  std::string_view path{value};
  constexpr std::string_view prefix{"asset://"};
  if (path.rfind(prefix, 0U) == 0U) {
    path.remove_prefix(prefix.size());
  } else if (path.find(':') != std::string_view::npos) {
    return false;
  }
  if (path.empty() || path.front() == '/')
    return false;
  for (std::size_t begin = 0U; begin <= path.size();) {
    const auto end = path.find('/', begin);
    const auto count = end == std::string_view::npos ? path.size() - begin
                                                      : end - begin;
    const auto segment = path.substr(begin, count);
    if (segment.empty() || segment == "." || segment == "..")
      return false;
    if (end == std::string_view::npos)
      break;
    begin = end + 1U;
  }
  return true;
}

bool ValidResourceDigest(const std::string &value,
                         const TextResourceOwnership ownership) noexcept {
  if (value.empty())
    return ownership != TextResourceOwnership::ProjectManaged;
  return base::Sha256::IsCanonicalDigest(value);
}

bool ValidMediaType(const std::string &value,
                    const std::size_t maximum) noexcept {
  return !value.empty() && value.size() <= maximum &&
         value.find('/') != std::string::npos &&
         std::all_of(value.begin(), value.end(), [](const unsigned char byte) {
           return byte >= 0x20U && byte != 0x7fU;
         });
}

bool ValidResourceMediaKind(const TextResourceKind kind,
                            const std::string_view mediaType) noexcept {
  if (mediaType == "application/vnd.videocut.mesh")
    return kind == TextResourceKind::Mesh;
  if (mediaType == "application/vnd.videocut.float-texture")
    return kind == TextResourceKind::FloatTexture;
  if (mediaType == "video/mp4;profile=videocut-packed-alpha")
    return kind == TextResourceKind::Animated;
  if (mediaType == "application/json")
    return kind == TextResourceKind::Vector;
  if (mediaType == "image/png" || mediaType == "image/jpeg")
    return kind == TextResourceKind::Texture;
  return true;
}

bool Utf8Boundary(const std::string &value, const std::uint64_t offset) {
  if (offset > value.size())
    return false;
  return offset == value.size() || offset == 0U ||
         (static_cast<unsigned char>(value[static_cast<std::size_t>(offset)]) &
          0xc0U) != 0x80U;
}

bool ValidInsets(const text::Insets &insets,
                 const TextCompositionLimits &limits) noexcept {
  const float maximum = limits.maximumLocalMagnitude;
  return Finite(insets.left) && Finite(insets.top) && Finite(insets.right) &&
         Finite(insets.bottom) && insets.left >= 0.0F && insets.top >= 0.0F &&
         insets.right >= 0.0F && insets.bottom >= 0.0F &&
         insets.left <= maximum && insets.top <= maximum &&
         insets.right <= maximum && insets.bottom <= maximum;
}

bool ValidOverride(const vector::VectorFieldOverride &field,
                   const TextCompositionLimits &limits) noexcept {
  if (!ValidIdentity(field.fieldId, limits.maximumIdentityBytes))
    return false;
  switch (field.value.kind) {
  case vector::VectorFieldKind::Color:
    return Finite(field.value.color.red) &&
           Finite(field.value.color.green) &&
           Finite(field.value.color.blue) &&
           Finite(field.value.color.alpha) && field.value.color.red >= 0.0F &&
           field.value.color.red <= 1.0F &&
           field.value.color.green >= 0.0F &&
           field.value.color.green <= 1.0F &&
           field.value.color.blue >= 0.0F &&
           field.value.color.blue <= 1.0F &&
           field.value.color.alpha >= 0.0F &&
           field.value.color.alpha <= 1.0F;
  case vector::VectorFieldKind::Text:
    return field.value.text.size() <= limits.richText.maximumUtf8Bytes &&
           text::IsValidUtf8(field.value.text);
  case vector::VectorFieldKind::Number:
    return Finite(field.value.number);
  }
  return false;
}

bool ValidDecorationAsset(const DecorationAssetReference &asset,
                          const TextCompositionLimits &limits,
                          const std::uint64_t maximumBytes) noexcept {
  return PortableAssetIdentity(asset.assetId, limits.maximumIdentityBytes) &&
         (asset.digest.empty() || base::Sha256::IsCanonicalDigest(asset.digest)) &&
         ValidMediaType(asset.mediaType, limits.maximumIdentityBytes) &&
         Finite(asset.intrinsicWidth) && Finite(asset.intrinsicHeight) &&
         asset.intrinsicWidth > 0.0F && asset.intrinsicHeight > 0.0F &&
         asset.intrinsicWidth <= limits.maximumLocalMagnitude &&
         asset.intrinsicHeight <= limits.maximumLocalMagnitude &&
         asset.estimatedResourceBytes > 0U &&
         asset.estimatedResourceBytes <= maximumBytes;
}

TextResourceKind DecorationResourceKind(
    const std::string &mediaType) noexcept {
  const bool animated = mediaType.rfind("video/", 0U) == 0U ||
                        mediaType == "image/gif" ||
                        mediaType == "image/apng" ||
                        mediaType == "image/webp+animated";
  const bool vector = mediaType == "application/json" ||
                      mediaType == "application/lottie+json" ||
                      mediaType == "image/svg+xml";
  return vector ? TextResourceKind::Vector
                : animated ? TextResourceKind::Animated
                           : TextResourceKind::Texture;
}

bool HasDecorationResource(const TextCompositionDocument &document,
                           const DecorationAssetReference &asset) noexcept {
  const auto expected = DecorationResourceKind(asset.mediaType);
  return std::any_of(
      document.resources.begin(), document.resources.end(),
      [&](const auto &resource) {
        return resource.assetId == asset.assetId &&
               resource.digest == asset.digest &&
               resource.mediaType == asset.mediaType &&
               resource.kind == expected;
      });
}

// Borrows the unfiltered closure for this validation call. Invalid and duplicate
// records still participate in existence checks, independently of ID validation.
class AuthoredResourceLookup final {
public:
  explicit AuthoredResourceLookup(
      const std::vector<TextResourceReference> &resources)
      : resources_(resources), scanBudget_(resources.size()) {
    // Delay sorting until linear queries have visited roughly R * log2(R)
    // records. Large closures with few references need no index allocation.
    for (auto count = resources.size(); count > 1U; count /= 2U)
      scanBudget_ += resources.size();
  }

  bool contains(const text::TextureReference &reference,
                const TextResourceKind expected) const {
    const auto ownership =
        reference.sourceKind == text::TextureSourceKind::Builtin
            ? TextResourceOwnership::Builtin
            : TextResourceOwnership::ProjectManaged;
    return any(reference.assetId, [&](const auto &resource) {
      return resource.assetId == reference.assetId &&
             resource.digest == reference.digest &&
             resource.mediaType == reference.mediaType &&
             resource.ownership == ownership &&
             (resource.kind == expected ||
              (expected == TextResourceKind::Texture &&
               resource.kind == TextResourceKind::Animated));
    });
  }

  bool contains(const text::FontReference &font) const {
    return any(font.assetId, [&](const auto &resource) {
      return resource.kind == TextResourceKind::Font &&
             resource.ownership == TextResourceOwnership::ProjectManaged &&
             resource.assetId == font.assetId && resource.digest == font.digest;
    });
  }

private:
  void buildAssetIndex() const {
    byAsset_.reserve(resources_.size());
    for (const auto &resource : resources_)
      byAsset_.push_back(&resource);
    std::sort(byAsset_.begin(), byAsset_.end(),
              [](const auto *left, const auto *right) {
                return left->assetId < right->assetId;
              });
  }

  template <typename Predicate>
  bool any(const std::string_view identity, Predicate &&matches) const {
    if (lastMatch_ && matches(*lastMatch_))
      return true;
    if (resources_.size() <= 1U || (byAsset_.empty() && scanBudget_ > 0U)) {
      const auto found =
          std::find_if(resources_.begin(), resources_.end(), matches);
      const auto visited = static_cast<std::size_t>(found - resources_.begin()) +
                           (found != resources_.end() ? 1U : 0U);
      scanBudget_ -= std::min(scanBudget_, visited);
      lastMatch_ = found == resources_.end() ? nullptr : &*found;
      return lastMatch_ != nullptr;
    }
    if (byAsset_.empty())
      buildAssetIndex();
    auto entry = std::lower_bound(
        byAsset_.begin(), byAsset_.end(), identity,
        [](const auto *candidate, const auto key) {
          return candidate->assetId < key;
        });
    for (; entry != byAsset_.end() && (*entry)->assetId == identity; ++entry)
      if (matches(**entry)) {
        lastMatch_ = *entry;
        return true;
      }
    return false;
  }

  const std::vector<TextResourceReference> &resources_;
  mutable const TextResourceReference *lastMatch_{nullptr};
  mutable std::size_t scanBudget_;
  mutable std::vector<const TextResourceReference *> byAsset_;
};

const text::RichTextParagraph *
FindParagraph(const TextCompositionDocument &document,
              const std::string_view paragraphId) noexcept {
  const auto found = std::find_if(
      document.content.begin(), document.content.end(),
      [&](const auto &paragraph) { return paragraph.paragraphId == paragraphId; });
  return found == document.content.end() ? nullptr : &*found;
}

struct RunLocation final {
  const text::RichTextParagraph *paragraph;
  const text::RichTextRun *run;
};

using RunLocations = std::unordered_map<std::string, RunLocation>;

const text::RichTextRun *FindRun(const TextCompositionDocument &document,
                                const std::string &paragraphId,
                                const std::string &runId,
                                const RunLocations *locations = nullptr) noexcept {
  if (locations) {
    const auto found = locations->find(runId);
    return found != locations->end() &&
                   found->second.paragraph->paragraphId == paragraphId
               ? found->second.run
               : nullptr;
  }
  const auto *paragraph = FindParagraph(document, paragraphId);
  if (!paragraph)
    return nullptr;
  const auto found = std::find_if(
      paragraph->runs.begin(), paragraph->runs.end(),
      [&](const auto &run) { return run.runId == runId; });
  return found == paragraph->runs.end() ? nullptr : &*found;
}

template <typename Callback>
void VisitTextureReferences(const TextCompositionDocument &document,
                            Callback &&callback) {
  const auto visitMaterial = [&](const text::TextMaterial &material) {
    if (const auto *texture = std::get_if<text::TextureTextMaterial>(&material))
      callback(texture->texture, TextResourceKind::Texture);
  };
  const auto visitBinding = [&](const text::TextMaterialBinding &binding) {
    if (const auto *literal = std::get_if<text::LiteralTextMaterial>(&binding)) {
      visitMaterial(literal->material);
    } else if (const auto *slot =
                   std::get_if<text::EditableTextStyleSlot>(&binding)) {
      visitMaterial(slot->fallback);
      if (slot->replacementMask)
        callback(*slot->replacementMask, TextResourceKind::Texture);
    }
  };
  for (const auto &paragraph : document.content) {
    for (const auto &run : paragraph.runs) {
      for (const auto &layer : run.style.materials.layers) {
        std::visit(
            [&](const auto &typed) {
              visitBinding(typed.material);
              using Layer = std::decay_t<decltype(typed)>;
              if constexpr (std::is_same_v<Layer, text::TextShadowLayer>) {
                for (const auto &stroke : typed.strokes)
                  visitBinding(stroke.material);
              }
            },
            layer);
      }
      visitBinding(run.style.decoration.underline.material);
      visitBinding(run.style.decoration.strikeThrough.material);
    }
  }
  for (const auto &layer : document.presentation.appearance.backdrops.layers) {
    if (layer.materialOverride)
      visitBinding(*layer.materialOverride);
    std::visit(
        [&](const auto &source) {
          using Source = std::decay_t<decltype(source)>;
          if constexpr (std::is_same_v<Source, text::RoundedRectBackdrop>) {
            visitBinding(source.fill);
            for (const auto &stroke : source.strokes)
              visitBinding(stroke.material);
          } else if constexpr (std::is_same_v<Source,
                                              text::AnimatedBackdrop>) {
            callback(source.asset, TextResourceKind::Animated);
          } else {
            callback(source.asset, TextResourceKind::Texture);
          }
        },
        layer.source);
  }
}

template <typename Callback>
void VisitFontReferences(const TextCompositionDocument &document,
                         Callback &&callback) {
  for (const auto &paragraph : document.content) {
    for (const auto &run : paragraph.runs) {
      callback(run.style.font.primary);
      for (const auto &fallback : run.style.font.fallbacks)
        callback(fallback);
    }
  }
}

bool ContainsLineSeparator(const std::string &value) noexcept {
  return value.find('\n') != std::string::npos ||
         value.find('\r') != std::string::npos;
}

std::size_t SaturatingAdd(const std::size_t left,
                          const std::size_t right) noexcept {
  const auto maximum = std::numeric_limits<std::size_t>::max();
  return right > maximum - left ? maximum : left + right;
}

std::uint64_t SaturatingAddResourceBytes(const std::uint64_t left,
                                         const std::uint64_t right) noexcept {
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  return right > maximum - left ? maximum : left + right;
}

bool MagnitudeExceeds(const std::int64_t value,
                      const std::int64_t maximum) noexcept {
  if (maximum < 0)
    return true;
  if (value >= 0)
    return value > maximum;
  return value == std::numeric_limits<std::int64_t>::min() ||
         -value > maximum;
}

DecorationBindingValidationResult ValidateVectorDecorationBindingWithRuns(
    const TextCompositionDocument &document,
    const VectorDecorationBinding &binding,
    const TextCompositionLimits &limits, const RunLocations *runs) {
  DecorationBindingValidationResult result;
  const auto fail = [&](std::string code, std::string message) {
    Add(result.diagnostics, std::move(code), binding.decorationId,
        std::move(message));
  };

  if (!binding.enabled) {
    Add(result.diagnostics, "text_composition.decoration.authored_disabled",
        binding.decorationId, "decoration is disabled by authored state",
        DiagnosticSeverity::Information);
    return result;
  }
  if (!ValidIdentity(binding.decorationId, limits.maximumIdentityBytes))
    fail("text_composition.decoration.id_invalid",
         "decoration identity must be bounded valid UTF-8");
  const auto perDecorationLimit = std::min(
      limits.maximumSingleDecorationResourceBytes,
      document.decorationBudget.maximumResourceBytesPerDecoration);
  if (!ValidDecorationAsset(binding.asset, limits, perDecorationLimit))
    fail("text_composition.decoration.asset_invalid",
         "decoration asset must be a portable, digest-closed resource");
  else if (!HasDecorationResource(document, binding.asset))
    fail("text_composition.decoration.resource_missing",
         "decoration asset is absent from the portable resource closure");
  if (binding.asset.staticFieldOverrides.size() >
      limits.maximumOverridesPerDecoration)
    fail("text_composition.decoration.override_budget",
         "field override count exceeds the configured budget");
  std::unordered_set<std::string> overrideIds;
  for (const auto &field : binding.asset.staticFieldOverrides) {
    if (!ValidOverride(field, limits) ||
        !overrideIds.insert(field.fieldId).second) {
      fail("text_composition.decoration.override_invalid",
           "field overrides require unique IDs and valid values");
      break;
    }
  }

  if (binding.instancePattern) {
    const auto &pattern = *binding.instancePattern;
    if (pattern.selectionPolicy <
            DecorationVariantSelectionPolicy::RepeatOne ||
        pattern.selectionPolicy >
            DecorationVariantSelectionPolicy::ExplicitPattern ||
        pattern.phasePolicy < DecorationPerIndexPhasePolicy::Shared ||
        pattern.phasePolicy >
            DecorationPerIndexPhasePolicy::DeterministicSeed)
      fail("text_composition.decoration.pattern_enum_invalid",
           "instance pattern uses an unknown current policy");
    if (binding.mode != VectorDecorationMode::PerGraphemeBackground)
      fail("text_composition.decoration.pattern_mode_invalid",
           "instance patterns require per-grapheme background mode");
    if (pattern.assetVariants.empty() ||
        pattern.assetVariants.size() > limits.maximumVariantsPerDecoration)
      fail("text_composition.decoration.variant_count_invalid",
           "instance pattern variant count exceeds the configured bound");
    std::unordered_set<std::string> variantIds;
    std::unordered_set<std::string> assetDigests{binding.asset.digest};
    std::uint64_t variantBytes = binding.asset.estimatedResourceBytes;
    for (const auto &variant : pattern.assetVariants) {
      if (!ValidIdentity(variant.variantId, limits.maximumIdentityBytes) ||
          !variantIds.insert(variant.variantId).second ||
          !ValidDecorationAsset(variant.asset, limits, perDecorationLimit) ||
          !HasDecorationResource(document, variant.asset)) {
        fail("text_composition.decoration.variant_invalid",
             "each variant requires a unique ID and portable resource");
        break;
      }
      std::unordered_set<std::string> fieldIds;
      if (variant.asset.staticFieldOverrides.size() >
          limits.maximumOverridesPerDecoration ||
          std::any_of(variant.asset.staticFieldOverrides.begin(),
                      variant.asset.staticFieldOverrides.end(),
                      [&](const auto &field) {
                        return !ValidOverride(field, limits) ||
                               !fieldIds.insert(field.fieldId).second;
                      })) {
        fail("text_composition.decoration.variant_override_invalid",
             "variant overrides require unique IDs and valid values");
        break;
      }
      if (assetDigests.insert(variant.asset.digest).second) {
        variantBytes = SaturatingAddResourceBytes(
            variantBytes, variant.asset.estimatedResourceBytes);
      }
    }
    if (variantBytes > perDecorationLimit)
      fail("text_composition.decoration.variant_resource_budget",
           "decoration variants exceed the per-binding resource budget");
    if (pattern.selectionPolicy ==
        DecorationVariantSelectionPolicy::ExplicitPattern) {
      if (pattern.explicitPattern.empty() ||
          pattern.explicitPattern.size() >
              limits.maximumExplicitPatternLength ||
          std::any_of(pattern.explicitPattern.begin(),
                      pattern.explicitPattern.end(), [&](const auto index) {
                        return index >= pattern.assetVariants.size();
                      }))
        fail("text_composition.decoration.explicit_pattern_invalid",
             "explicit variant pattern is empty, oversized or out of range");
    } else if (!pattern.explicitPattern.empty()) {
      fail("text_composition.decoration.explicit_pattern_unexpected",
           "explicit indices require explicit-pattern selection");
    }
    const auto span = static_cast<float>(
        pattern.assetVariants.empty() ? 0U
                                      : pattern.assetVariants.size() - 1U);
    if (!Finite(pattern.offsetXStep) || !Finite(pattern.offsetYStep) ||
        !Finite(pattern.scaleStep) ||
        !Finite(pattern.rotationStepDegrees) ||
        std::fabs(pattern.offsetXStep) * span > limits.maximumLocalMagnitude ||
        std::fabs(pattern.offsetYStep) * span > limits.maximumLocalMagnitude ||
        1.0F - std::fabs(pattern.scaleStep) * span * 0.5F <= 0.0F ||
        1.0F + std::fabs(pattern.scaleStep) * span * 0.5F >
            limits.maximumLocalMagnitude ||
        std::fabs(pattern.rotationStepDegrees) * span > 3600.0F ||
        MagnitudeExceeds(pattern.phaseStepUs,
                         limits.maximumAssetDurationUs))
      fail("text_composition.decoration.instance_pattern_invalid",
           "per-index transform or phase exceeds native bounds");
  }

  if (binding.mode < VectorDecorationMode::TrackedGrapheme ||
      binding.mode > VectorDecorationMode::CompositionOverlay ||
      binding.target.scope < DecorationTargetScope::AllText ||
      binding.target.scope > DecorationTargetScope::Utf8Range ||
      binding.placementDriver <
          DecorationPlacementDriver::SentenceUniformProgress ||
      binding.placementDriver > DecorationPlacementDriver::GeometryOnly ||
      binding.anchor < DecorationAnchor::Above ||
      binding.anchor > DecorationAnchor::CaretPath ||
      binding.fit < DecorationFit::Contain ||
      binding.fit > DecorationFit::Native ||
      binding.assetPlayback.clock < DecorationAssetClock::Composition ||
      binding.assetPlayback.clock > DecorationAssetClock::Target ||
      binding.assetPlayback.mode < DecorationPlaybackMode::Loop ||
      binding.assetPlayback.mode > DecorationPlaybackMode::Hold ||
      binding.transformInherit < DecorationTransformInherit::Full ||
      binding.transformInherit > DecorationTransformInherit::TranslateOnly ||
      binding.sampling < DecorationSamplingPolicy::Automatic ||
      binding.sampling > DecorationSamplingPolicy::LinearClamp ||
      binding.effectScope <
          DecorationEffectScope::InsideGroupBehindText ||
      binding.effectScope > DecorationEffectScope::AfterGroupEffect ||
      binding.fallback < DecorationFallbackPolicy::DisableDecoration ||
      binding.fallback > DecorationFallbackPolicy::ClipVisibleInstances)
    fail("text_composition.decoration.enum_invalid",
         "decoration uses a value outside the current closed enum sets");
  if (!ValidInsets(binding.padding, limits) ||
      !Finite(binding.localTransform.offsetX) ||
      !Finite(binding.localTransform.offsetY) ||
      !Finite(binding.localTransform.scaleX) ||
      !Finite(binding.localTransform.scaleY) ||
      binding.localTransform.scaleX <= 0.0F ||
      binding.localTransform.scaleY <= 0.0F ||
      binding.localTransform.scaleX > limits.maximumLocalMagnitude ||
      binding.localTransform.scaleY > limits.maximumLocalMagnitude ||
      !Finite(binding.localTransform.rotationDegrees) ||
      std::fabs(binding.localTransform.offsetX) >
          limits.maximumLocalMagnitude ||
      std::fabs(binding.localTransform.offsetY) >
          limits.maximumLocalMagnitude ||
      std::fabs(binding.localTransform.rotationDegrees) > 3'600.0F ||
      !Finite(binding.localTransform.opacity) ||
      binding.localTransform.opacity < 0.0F ||
      binding.localTransform.opacity > 1.0F)
    fail("text_composition.decoration.transform_invalid",
         "padding and local transform must be finite and bounded");
  const auto &playback = binding.assetPlayback;
  if (playback.sourceInUs < 0 || playback.sourceOutUs <= playback.sourceInUs ||
      playback.sourceOutUs - playback.sourceInUs >
          limits.maximumAssetDurationUs ||
      !Finite(playback.speed) || playback.speed <= 0.0 ||
      playback.speed > 1024.0 ||
      MagnitudeExceeds(playback.phaseUs, limits.maximumAssetDurationUs))
    fail("text_composition.decoration.playback_invalid",
         "asset playback range, speed or phase is invalid");
  if (!binding.fallbackAssetId.empty() &&
      !PortableAssetIdentity(binding.fallbackAssetId,
                             limits.maximumIdentityBytes))
    fail("text_composition.decoration.fallback_invalid",
         "fallback must be empty or a portable resource identity");
  else if (!binding.fallbackAssetId.empty() &&
           std::none_of(document.resources.begin(), document.resources.end(),
                        [&](const auto &resource) {
                          return (resource.resourceId == binding.fallbackAssetId ||
                                  resource.assetId == binding.fallbackAssetId) &&
                                 resource.kind != TextResourceKind::Font;
                        }))
    fail("text_composition.decoration.fallback_resource_missing",
         "decoration fallback is absent from the portable resource closure");

  if (binding.target.scope == DecorationTargetScope::Utf8Range) {
    const auto *run =
        FindRun(document, binding.target.paragraphId, binding.target.runId,
                runs);
    if (!run || binding.target.range.begin >= binding.target.range.end ||
        binding.target.range.end > run->utf8Text.size() ||
        !Utf8Boundary(run->utf8Text, binding.target.range.begin) ||
        !Utf8Boundary(run->utf8Text, binding.target.range.end))
      fail("text_composition.decoration.target_range_invalid",
           "decoration target must be a current run-local UTF-8 range");
  } else if (!binding.target.paragraphId.empty() ||
             !binding.target.runId.empty() || binding.target.range.begin != 0U ||
             binding.target.range.end != 0U) {
    fail("text_composition.decoration.target_all_text_payload",
         "all-text targets cannot retain a stale range owner");
  }
  result.usable = std::none_of(
      result.diagnostics.begin(), result.diagnostics.end(),
      [](const auto &diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
      });
  return result;
}

} // namespace

DecorationBindingValidationResult ValidateVectorDecorationBinding(
    const TextCompositionDocument &document,
    const VectorDecorationBinding &binding,
    const TextCompositionLimits &limits) {
  return ValidateVectorDecorationBindingWithRuns(document, binding, limits,
                                                 nullptr);
}

TextCompositionValidationResult ValidateTextCompositionDocument(
    const TextCompositionDocument &document,
    const TextCompositionLimits &limits) {
  TextCompositionValidationResult result;
  result.valid = true;
  result.textOnlyFastPath = document.decorations.empty();
  const auto fail = [&](std::string code, std::string subject,
                        std::string message) {
    result.valid = false;
    Add(result.diagnostics, std::move(code), std::move(subject),
        std::move(message));
  };

  if (document.schemaRevision != text::kTextSchemaRevision)
    fail("text_composition.schema_revision_invalid", {},
         "document must use the sole current text schema revision");
  if (document.durationTicks <= 0)
    fail("text_composition.duration_invalid", {},
         "composition duration must be positive");
  if (document.role > TextRole::Title)
    fail("text_composition.role_invalid", {},
         "text role is outside the closed role set");
  if (document.effectPolicy >
      CompositionEffectPolicy::TextAndInsideGroupDecorations)
    fail("text_composition.effect_policy_invalid", {},
         "composition effect routing is outside the closed policy set");

  const auto richValidation = text::internal::ValidateRichText(
      {document.presentation.referenceCanvas,
       document.presentation.authoredLayoutFrame,
       document.presentation.writingMode, document.contentSlots,
       document.content},
      limits.richText);
  if (!richValidation.valid)
    result.valid = false;
  for (const auto &diagnostic : richValidation.diagnostics) {
    Add(result.diagnostics, "text_composition.text." + diagnostic.code,
        diagnostic.subjectId, diagnostic.message,
        static_cast<DiagnosticSeverity>(diagnostic.severity));
  }

  RunLocations runOwner;
  bool runIdentitiesValid = true;
  std::unordered_set<std::string> paragraphIds;
  std::unordered_set<std::string> runIds;
  for (const auto &paragraph : document.content) {
    if (!ValidIdentity(paragraph.paragraphId, limits.maximumIdentityBytes) ||
        !paragraphIds.insert(paragraph.paragraphId).second) {
      runIdentitiesValid = false;
      fail("text_composition.paragraph_identity_invalid",
           paragraph.paragraphId,
           "paragraph identities must be unique bounded UTF-8");
      continue;
    }
    if (paragraph.runs.empty())
      fail("text_composition.paragraph_empty", paragraph.paragraphId,
           "each paragraph must retain one editable run");
    for (const auto &run : paragraph.runs) {
      if (!ValidIdentity(run.runId, limits.maximumIdentityBytes) ||
          !runIds.insert(run.runId).second) {
        runIdentitiesValid = false;
        fail("text_composition.run_identity_invalid", run.runId,
             "run identities must be unique bounded UTF-8");
      } else {
        runOwner.emplace(run.runId, RunLocation{&paragraph, &run});
      }
      if (!text::IsValidUtf8(run.utf8Text) ||
          ContainsLineSeparator(run.utf8Text))
        fail("text_composition.run_text_invalid", run.runId,
             "run text must be valid UTF-8 without paragraph separators");
    }
  }

  // Invalid identities retain the original first-paragraph/first-run lookup.
  const auto *runLookup = runIdentitiesValid ? &runOwner : nullptr;
  std::unordered_set<std::string> slotIds;
  std::unordered_map<std::string, std::size_t> paragraphSlotOwners;
  std::unordered_map<std::string, std::size_t> runSlotOwners;
  for (const auto &slot : document.contentSlots) {
    if (!ValidIdentity(slot.slotId, limits.maximumIdentityBytes) ||
        !ValidIdentity(slot.semanticRole, limits.maximumIdentityBytes) ||
        !slotIds.insert(slot.slotId).second) {
      fail("text_composition.content_slot_invalid", slot.slotId,
           "content slots require unique IDs and a semantic role");
      continue;
    }
    std::unordered_set<std::string> localParagraphs;
    for (const auto &paragraphId : slot.paragraphIds) {
      if (paragraphIds.count(paragraphId) == 0U ||
          !localParagraphs.insert(paragraphId).second) {
        fail("text_composition.content_slot_paragraph_invalid", slot.slotId,
             "content slot paragraph references must be unique and current");
      } else {
        ++paragraphSlotOwners[paragraphId];
      }
    }
    std::unordered_set<std::string> localRuns;
    for (const auto &runId : slot.runIds) {
      const auto owner = runOwner.find(runId);
      if (owner == runOwner.end() || !localRuns.insert(runId).second ||
          localParagraphs.count(owner->second.paragraph->paragraphId) == 0U) {
        fail("text_composition.content_slot_run_invalid", slot.slotId,
             "content slot runs must be unique children of its paragraphs");
      } else {
        ++runSlotOwners[runId];
      }
    }
  }
  for (const auto &paragraphId : paragraphIds) {
    if (paragraphSlotOwners[paragraphId] != 1U)
      fail("text_composition.paragraph_slot_ownership", paragraphId,
           "each paragraph must belong to exactly one content slot");
  }
  for (const auto &runId : runIds) {
    if (runSlotOwners[runId] != 1U)
      fail("text_composition.run_slot_ownership", runId,
           "each run must belong to exactly one content slot");
  }

  std::unordered_set<std::string> timedSpanIds;
  if (document.timedText) {
    if (document.timedText->clock != TimedTextClock::ClipSourceLocal)
      fail("text_composition.timed_text_clock_invalid", {},
           "timed text uses the clip-source-local clock only");
    if (document.timedText->spans.empty() ||
        document.timedText->spans.size() > limits.richText.maximumTimedSpans)
      fail("text_composition.timed_text_count_invalid", {},
           "timed text must be absent or contain a bounded non-empty span set");
    for (const auto &span : document.timedText->spans) {
      const auto *run =
          FindRun(document, span.paragraphId, span.runId, runLookup);
      if (!ValidIdentity(span.spanId, limits.maximumIdentityBytes) ||
          !timedSpanIds.insert(span.spanId).second || !run ||
          span.range.begin >= span.range.end ||
          (run && span.range.end > run->utf8Text.size()) ||
          (run && (!Utf8Boundary(run->utf8Text, span.range.begin) ||
                   !Utf8Boundary(run->utf8Text, span.range.end))) ||
          span.startOffsetUs < 0 || span.endOffsetUs <= span.startOffsetUs ||
          span.transitionEndOffsetUs < span.startOffsetUs ||
          span.transitionEndOffsetUs > span.endOffsetUs ||
          span.progressMode > text::TimedTextProgressMode::GraphemeSweep ||
          !ValidIdentity(span.semantic, limits.maximumIdentityBytes)) {
        fail("text_composition.timed_span_invalid", span.spanId,
             "timed spans require current run-local ranges and valid clocks");
      }
    }
  }

  std::string appearanceError;
  if (!text::ValidateTextLayerAppearance(document.presentation.appearance,
                                         &appearanceError))
    fail("text_composition.appearance_invalid", {}, appearanceError);
  std::string animationError;
  if (!text::ValidateTextAnimationStack(
          document.animations,
          document.timedText && !document.timedText->spans.empty(),
          &animationError))
    fail("text_composition.animation_invalid", {}, animationError);

  std::unordered_set<std::string> animationIds;
  for (const auto &layer : document.animations.layers) {
    if (!ValidIdentity(layer.layerId, limits.maximumIdentityBytes) ||
        !animationIds.insert(layer.layerId).second)
      fail("text_composition.animation_layer_identity_invalid", layer.layerId,
           "animation layer identities must be unique bounded UTF-8");
    for (const auto &paragraphId : layer.target.paragraphIds) {
      if (paragraphIds.count(paragraphId) == 0U)
        fail("text_composition.animation_target_paragraph_missing",
             layer.layerId, "animation target references a missing paragraph");
    }
    for (const auto &runId : layer.target.runIds) {
      if (runIds.count(runId) == 0U)
        fail("text_composition.animation_target_run_missing", layer.layerId,
             "animation target references a missing run");
    }
    if (!layer.target.contentSlotId.empty() &&
        slotIds.count(layer.target.contentSlotId) == 0U)
      fail("text_composition.animation_target_slot_missing", layer.layerId,
           "animation target references a missing content slot");
    if (layer.target.scope == text::TextPropertyScope::Utf8Range &&
        layer.target.runIds.size() == 1U && layer.target.range) {
      const auto owner = runOwner.find(layer.target.runIds.front());
      const auto *run = owner == runOwner.end()
                            ? nullptr
                            : FindRun(document,
                                      owner->second.paragraph->paragraphId,
                                      owner->first, runLookup);
      if (!run || layer.target.range->begin >= layer.target.range->end ||
          layer.target.range->end > run->utf8Text.size() ||
          !Utf8Boundary(run->utf8Text, layer.target.range->begin) ||
          !Utf8Boundary(run->utf8Text, layer.target.range->end))
        fail("text_composition.animation_target_range_invalid", layer.layerId,
             "animation target must resolve to a current UTF-8 boundary range");
    }
    if (layer.target.scope == text::TextPropertyScope::GlyphMaterialLayer) {
      const auto found = std::any_of(
          document.content.begin(), document.content.end(),
          [&](const auto &paragraph) {
            return std::any_of(
                paragraph.runs.begin(), paragraph.runs.end(),
                [&](const auto &run) {
                  return std::any_of(
                      run.style.materials.layers.begin(),
                      run.style.materials.layers.end(), [&](const auto &value) {
                        return std::visit(
                            [&](const auto &typed) {
                              return typed.layerId == layer.target.layerId;
                            },
                            value);
                      });
                });
          });
      if (!found)
        fail("text_composition.animation_glyph_layer_missing", layer.layerId,
             "animation target references a missing glyph material layer");
    }
    if (layer.target.scope == text::TextPropertyScope::BackdropLayer &&
        std::none_of(
            document.presentation.appearance.backdrops.layers.begin(),
            document.presentation.appearance.backdrops.layers.end(),
            [&](const auto &backdrop) {
              return backdrop.layerId == layer.target.layerId;
            }))
      fail("text_composition.animation_backdrop_layer_missing", layer.layerId,
           "animation target references a missing backdrop layer");
    for (const auto &animator : layer.animators) {
      for (const auto &paragraphId : animator.paragraphIds)
        if (paragraphIds.count(paragraphId) == 0U)
          fail("text_composition.animator_paragraph_missing",
               animator.animatorId,
               "animator references a missing paragraph");
      for (const auto &runId : animator.runIds)
        if (runIds.count(runId) == 0U)
          fail("text_composition.animator_run_missing", animator.animatorId,
               "animator references a missing run");
    }
    if (layer.timeDriver.timedRanges)
      for (const auto &spanId : layer.timeDriver.timedRanges->spanIds)
        if (timedSpanIds.count(spanId) == 0U)
          fail("text_composition.animation_timed_span_missing", layer.layerId,
               "animation clock references a missing timed-text span");
  }

  std::unordered_map<std::string, const TextResourceReference *> resources;
  if (document.resources.size() > limits.maximumResources)
    fail("text_composition.resource_count_invalid", {},
         "portable resource count exceeds the configured bound");
  for (const auto &resource : document.resources) {
    if (resource.kind < TextResourceKind::Font ||
        resource.kind > TextResourceKind::FloatTexture ||
        resource.ownership < TextResourceOwnership::Builtin ||
        resource.ownership > TextResourceOwnership::System ||
        !ValidIdentity(resource.resourceId, limits.maximumIdentityBytes) ||
        !PortableAssetIdentity(resource.assetId,
                               limits.maximumIdentityBytes) ||
        !ValidResourceDigest(resource.digest, resource.ownership) ||
        !ValidMediaType(resource.mediaType, limits.maximumIdentityBytes) ||
        !ValidResourceMediaKind(resource.kind, resource.mediaType) ||
        !resources.emplace(resource.resourceId, &resource).second) {
      fail("text_composition.resource_invalid", resource.resourceId,
           "resources require unique portable identities and closed digests");
    }
  }
  const auto validateExecutionResources =
      [&](const auto &self,
          const std::vector<text::TextEffectExecutionNode> &nodes,
          const std::size_t depth) -> void {
    if (depth > 32U)
      return;
    for (const auto &node : nodes) {
      if (node.kind == text::TextEffectExecutionNodeKind::MediaInput &&
          node.resourceIds.empty()) {
        fail("text_composition.execution_media_resource_missing", node.nodeId,
             "media input nodes require an explicit portable resource");
      }
      for (const auto &resourceId : node.resourceIds) {
        const auto resource = resources.find(resourceId);
        if (resource == resources.end()) {
          fail("text_composition.execution_resource_missing", node.nodeId,
               "execution node references a missing portable resource");
        }
      }
      if (node.capability == text::TextEffectExecutionCapability::MediaMesh) {
        for (const auto &resourceId : node.resourceIds) {
          const auto resource = resources.find(resourceId);
          if (resource != resources.end() &&
              resource->second->kind != TextResourceKind::Mesh &&
              resource->second->kind != TextResourceKind::Vector) {
            fail("text_composition.execution_mesh_resource_invalid",
                 node.nodeId,
                 "mesh media nodes require canonical mesh or vector "
                 "resources");
          }
        }
      }
      if (node.capability ==
          text::TextEffectExecutionCapability::MaterialVatRbdMesh) {
        constexpr std::array<TextResourceKind, 3U> requiredKinds{
            TextResourceKind::Mesh, TextResourceKind::FloatTexture,
            TextResourceKind::FloatTexture};
        if (node.resourceIds.size() != requiredKinds.size()) {
          fail("text_composition.execution_vat_resource_invalid", node.nodeId,
               "VAT material requires one mesh and two float textures");
        } else {
          for (std::size_t index = 0U; index < requiredKinds.size(); ++index) {
            const auto resource = resources.find(node.resourceIds[index]);
            if (resource != resources.end() &&
                resource->second->kind != requiredKinds[index]) {
              fail("text_composition.execution_vat_resource_invalid",
                   node.nodeId,
                   "VAT material resource order or kind is invalid");
            }
          }
        }
      }
      self(self, node.children, depth + 1U);
    }
  };
  validateExecutionResources(validateExecutionResources,
                             document.animations.executionGraph.nodes, 0U);
  const AuthoredResourceLookup resourceLookup(document.resources);
  const auto resolvePortable = [&](const text::TextureReference &reference,
                                   const TextResourceKind expected) {
    const bool digestValid =
        reference.digest.empty()
            ? reference.sourceKind != text::TextureSourceKind::ProjectManaged
            : base::Sha256::IsCanonicalDigest(reference.digest);
    if (!PortableAssetIdentity(reference.assetId,
                               limits.maximumIdentityBytes) ||
        !digestValid ||
        !ValidMediaType(reference.mediaType, limits.maximumIdentityBytes)) {
      fail("text_composition.texture_reference_invalid", reference.assetId,
           "texture references must be portable and digest closed");
      return;
    }
    if (!resourceLookup.contains(reference, expected))
      fail("text_composition.texture_resource_missing", reference.assetId,
           "authored texture is absent from the portable resource closure");
  };
  VisitTextureReferences(document, resolvePortable);
  VisitFontReferences(document, [&](const text::FontReference &font) {
    if (font.kind != text::FontSourceKind::ProjectManaged)
      return;
    if (!resourceLookup.contains(font))
      fail("text_composition.font_resource_missing", font.assetId,
           "project-managed font is absent from the resource closure");
  });
  if (document.decorations.size() >
      std::min<std::size_t>(limits.maximumDecorations,
                            document.decorationBudget.maximumDecorations))
    Add(result.diagnostics, "text_composition.decoration.count_budget", {},
        "decorations beyond the authored budget are disabled",
        DiagnosticSeverity::Warning);
  std::unordered_set<std::string> decorationIds;
  std::unordered_map<std::string, const VectorDecorationBinding *>
      decorationBindings;
  std::unordered_set<std::string> decorationDigests;
  std::uint64_t decorationBytes = 0U;
  for (std::size_t index = 0U; index < document.decorations.size(); ++index) {
    const auto &binding = document.decorations[index];
    const auto validation =
        ValidateVectorDecorationBindingWithRuns(document, binding, limits,
                                                 runLookup);
    if (!decorationIds.insert(binding.decorationId).second) {
      result.disabledDecorationIds.push_back(binding.decorationId);
      result.disabledDecorationIndexes.push_back(index);
      Add(result.diagnostics, "text_composition.decoration.duplicate_id",
          binding.decorationId,
          "duplicate decoration identity disables this binding");
      continue;
    }
    decorationBindings.emplace(binding.decorationId, &binding);
    result.diagnostics.insert(result.diagnostics.end(),
                              validation.diagnostics.begin(),
                              validation.diagnostics.end());
    if (!validation.usable ||
        index >= document.decorationBudget.maximumDecorations ||
        index >= limits.maximumDecorations) {
      result.disabledDecorationIds.push_back(binding.decorationId);
      result.disabledDecorationIndexes.push_back(index);
      continue;
    }
    std::unordered_set<std::string> bindingDigests;
    std::uint64_t bindingBytes = 0U;
    const auto account = [&](const DecorationAssetReference &asset) {
      if (decorationDigests.count(asset.digest) != 0U ||
          !bindingDigests.insert(asset.digest).second)
        return;
      bindingBytes = SaturatingAddResourceBytes(
          bindingBytes, asset.estimatedResourceBytes);
    };
    account(binding.asset);
    if (binding.instancePattern)
      for (const auto &variant : binding.instancePattern->assetVariants)
        account(variant.asset);
    const auto maximum = std::min(
        limits.maximumDecorationResourceBytes,
        document.decorationBudget.maximumResourceBytes);
    if (bindingBytes > maximum - std::min(decorationBytes, maximum)) {
      result.disabledDecorationIds.push_back(binding.decorationId);
      result.disabledDecorationIndexes.push_back(index);
      Add(result.diagnostics,
          "text_composition.decoration.resource_budget",
          binding.decorationId,
          "decoration exceeds the remaining authored resource budget");
      continue;
    }
    decorationDigests.insert(bindingDigests.begin(), bindingDigests.end());
    decorationBytes += bindingBytes;
  }

  for (const auto &layer : document.animations.layers) {
    for (const auto &animation : layer.decorations) {
      const auto binding = decorationBindings.find(animation.decorationId);
      if (binding == decorationBindings.end()) {
        fail("text_composition.animation_decoration_missing", layer.layerId,
             "animation decoration must target a current decoration binding");
        continue;
      }
      if (animation.assetId.empty())
        continue;
      const auto assetMatches = [&](const DecorationAssetReference &asset) {
        return asset.assetId == animation.assetId;
      };
      bool matched = assetMatches(binding->second->asset);
      if (!matched && binding->second->instancePattern) {
        matched = std::any_of(
            binding->second->instancePattern->assetVariants.begin(),
            binding->second->instancePattern->assetVariants.end(),
            [&](const auto &variant) { return assetMatches(variant.asset); });
      }
      if (!matched)
        fail("text_composition.animation_decoration_asset_missing",
             layer.layerId,
             "animation decoration asset selector is outside its binding");
    }
  }

  const auto validOrigin = [&](const std::optional<TextTemplateOrigin> &origin) {
    return !origin ||
           (ValidIdentity(origin->templateId, limits.maximumIdentityBytes) &&
            base::Sha256::IsCanonicalDigest(origin->packageDigest));
  };
  if (!validOrigin(document.templateOrigin) ||
      !validOrigin(document.bubbleTemplateOrigin) ||
      !validOrigin(document.flowerTemplateOrigin) ||
      !validOrigin(document.animationTemplateOrigin))
    fail("text_composition.template_origin_invalid", {},
         "template origin must be informational portable identity metadata");
  return result;
}

std::string ComputeTextCompositionDocumentIdentity(
    const TextCompositionDocument &document) {
  return detail::MakeIdentity(
      "videocut.text-composition.document",
      [&](auto &writer) { detail::EncodeDocumentIdentity(writer, document); });
}

std::size_t EstimateTextCompositionDocumentBytes(
    const TextCompositionDocument &document) noexcept {
  detail::CanonicalTextIdentityWriter writer(
      "videocut.text-composition.authored-memory");
  detail::EncodeDocumentIdentity(writer, document);
  return SaturatingAdd(sizeof(TextCompositionDocument), writer.EncodedBytes());
}

void RebaseTimedTextForUtf8Edit(TimedTextTrack &track,
                                const std::string &paragraphId,
                                const std::string &runId,
                                const text::TextUtf8Range replacedRange,
                                const std::uint64_t replacementByteCount) {
  if (replacedRange.begin > replacedRange.end)
    return;
  const auto removed = replacedRange.end - replacedRange.begin;
  track.spans.erase(
      std::remove_if(track.spans.begin(), track.spans.end(),
                     [&](TimedTextSpan &span) {
                       if (span.paragraphId != paragraphId ||
                           span.runId != runId)
                         return false;
                       if (span.range.end <= replacedRange.begin)
                         return false;
                       if (span.range.begin < replacedRange.end)
                         return true;
                       if (replacementByteCount >= removed) {
                         const auto delta = replacementByteCount - removed;
                         span.range.begin += delta;
                         span.range.end += delta;
                       } else {
                         const auto delta = removed - replacementByteCount;
                         span.range.begin -= delta;
                         span.range.end -= delta;
                       }
                       return false;
                     }),
      track.spans.end());
}

TimedTextTrack SliceTimedTextTrack(const TimedTextTrack &track,
                                   const std::int64_t beginUs,
                                   const std::int64_t endUs) {
  TimedTextTrack result;
  result.clock = track.clock;
  if (endUs <= beginUs)
    return result;
  for (const auto &span : track.spans) {
    const auto intersectionBegin = std::max(span.startOffsetUs, beginUs);
    const auto intersectionEnd = std::min(span.endOffsetUs, endUs);
    if (intersectionBegin >= intersectionEnd)
      continue;
    auto sliced = span;
    sliced.startOffsetUs = intersectionBegin - beginUs;
    sliced.endOffsetUs = intersectionEnd - beginUs;
    sliced.transitionEndOffsetUs =
        std::clamp(span.transitionEndOffsetUs, intersectionBegin,
                   intersectionEnd) -
        beginUs;
    result.spans.push_back(std::move(sliced));
  }
  return result;
}

} // namespace videocut::text_composition
