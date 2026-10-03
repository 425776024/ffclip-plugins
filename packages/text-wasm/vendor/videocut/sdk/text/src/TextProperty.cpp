#include "videocut/text/TextProperty.h"

#include "videocut/text/TextAnimation.h"
#include "videocut/text/internal/RichTextValidation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace videocut::text {
namespace {

constexpr std::uint64_t kCompositionScope =
    TextPropertyScopeBit(TextPropertyScope::Composition);
constexpr std::uint64_t kSlotScope =
    TextPropertyScopeBit(TextPropertyScope::ContentSlot);
constexpr std::uint64_t kParagraphScope =
    TextPropertyScopeBit(TextPropertyScope::Paragraph);
constexpr std::uint64_t kRunScope =
    TextPropertyScopeBit(TextPropertyScope::Run);
constexpr std::uint64_t kRangeScope =
    TextPropertyScopeBit(TextPropertyScope::Utf8Range);
constexpr std::uint64_t kGlyphLayerScope =
    TextPropertyScopeBit(TextPropertyScope::GlyphMaterialLayer);
constexpr std::uint64_t kBackdropLayerScope =
    TextPropertyScopeBit(TextPropertyScope::BackdropLayer);

constexpr std::uint64_t kTextScopes =
    kCompositionScope | kSlotScope | kParagraphScope | kRunScope | kRangeScope;
constexpr std::uint64_t kRunScopes =
    kCompositionScope | kSlotScope | kRunScope | kRangeScope;
constexpr std::uint64_t kParagraphScopes =
    kCompositionScope | kSlotScope | kParagraphScope;
constexpr std::uint64_t kMaterialScopes = kRunScopes | kGlyphLayerScope;
constexpr std::uint64_t kBackdropScopes =
    kCompositionScope | kBackdropLayerScope;

constexpr std::uint32_t kReplace =
    TextPropertyCombineModeBit(TextPropertyCombineMode::Replace);
constexpr std::uint32_t kNumeric =
    kReplace | TextPropertyCombineModeBit(TextPropertyCombineMode::Add) |
    TextPropertyCombineModeBit(TextPropertyCombineMode::Multiply);

constexpr TextInvalidationImpact kLayout =
    TextInvalidationImpact::Layout | TextInvalidationImpact::Composite;
constexpr TextInvalidationImpact kResourceLayout =
    TextInvalidationImpact::Resources | TextInvalidationImpact::Layout |
    TextInvalidationImpact::GlyphMaterial | TextInvalidationImpact::Composite;
constexpr TextInvalidationImpact kGlyph =
    TextInvalidationImpact::GlyphMaterial | TextInvalidationImpact::Composite;
constexpr TextInvalidationImpact kBackdrop =
    TextInvalidationImpact::Resources | TextInvalidationImpact::Backdrop |
    TextInvalidationImpact::Composite;
constexpr TextInvalidationImpact kPost =
    TextInvalidationImpact::PostEffectState | TextInvalidationImpact::Composite;

constexpr TextPropertyDescriptor D(
    const TextPropertyId property, const char *name,
    const TextPropertyValueKind valueKind, const std::uint64_t legalScopeMask,
    const std::uint32_t legalCombineModeMask, const bool animatable,
    const TextInvalidationImpact invalidation) noexcept {
  return {property, name, valueKind, legalScopeMask, legalCombineModeMask,
          animatable, invalidation};
}

constexpr TextPropertyDescriptor kDescriptors[] = {
    D(TextPropertyId::FontReference, "font.reference",
      TextPropertyValueKind::Font, kTextScopes, kReplace, false,
      kResourceLayout),
    D(TextPropertyId::FontFamily, "font.family", TextPropertyValueKind::String,
      kTextScopes, kReplace, false, kResourceLayout),
    D(TextPropertyId::FontPostscriptName, "font.postscriptName",
      TextPropertyValueKind::String, kTextScopes, kReplace, false,
      kResourceLayout),
    D(TextPropertyId::FontWeight, "font.weight",
      TextPropertyValueKind::Integer, kTextScopes, kReplace, false,
      kResourceLayout),
    D(TextPropertyId::FontWidth, "font.width", TextPropertyValueKind::Integer,
      kTextScopes, kReplace, false, kResourceLayout),
    D(TextPropertyId::FontSlant, "font.slant",
      TextPropertyValueKind::Enumeration, kTextScopes, kReplace, false,
      kResourceLayout),
    D(TextPropertyId::FontVariationAxes, "font.variationAxes",
      TextPropertyValueKind::FontAxes, kTextScopes, kReplace, false,
      kResourceLayout),
    D(TextPropertyId::FontFeatures, "font.features",
      TextPropertyValueKind::FontFeatures, kTextScopes, kReplace, false,
      kResourceLayout),
    D(TextPropertyId::FontSize, "font.size", TextPropertyValueKind::Scalar,
      kTextScopes, kNumeric, true, kLayout),
    D(TextPropertyId::LetterSpacing, "text.letterSpacing",
      TextPropertyValueKind::Scalar, kTextScopes, kNumeric, true, kLayout),
    D(TextPropertyId::WordSpacing, "text.wordSpacing",
      TextPropertyValueKind::Scalar, kTextScopes, kNumeric, true, kLayout),
    D(TextPropertyId::BaselineShift, "text.baselineShift",
      TextPropertyValueKind::Scalar, kTextScopes, kNumeric, true, kLayout),
    D(TextPropertyId::UnderlineEnabled, "decoration.underline.enabled",
      TextPropertyValueKind::Boolean, kRunScopes, kReplace, false, kGlyph),
    D(TextPropertyId::UnderlineStyle, "decoration.underline.style",
      TextPropertyValueKind::Enumeration, kRunScopes, kReplace, false, kGlyph),
    D(TextPropertyId::UnderlineMaterial, "decoration.underline.material",
      TextPropertyValueKind::Material, kRunScopes, kReplace, false, kGlyph),
    D(TextPropertyId::UnderlineThickness, "decoration.underline.thickness",
      TextPropertyValueKind::Scalar, kRunScopes, kNumeric, true, kGlyph),
    D(TextPropertyId::UnderlineOffset, "decoration.underline.offset",
      TextPropertyValueKind::Scalar, kRunScopes, kNumeric, true, kGlyph),
    D(TextPropertyId::UnderlineSkipInk, "decoration.underline.skipInk",
      TextPropertyValueKind::Boolean, kRunScopes, kReplace, false, kGlyph),
    D(TextPropertyId::StrikeThroughEnabled,
      "decoration.strikeThrough.enabled", TextPropertyValueKind::Boolean,
      kRunScopes, kReplace, false, kGlyph),
    D(TextPropertyId::StrikeThroughStyle, "decoration.strikeThrough.style",
      TextPropertyValueKind::Enumeration, kRunScopes, kReplace, false, kGlyph),
    D(TextPropertyId::StrikeThroughMaterial,
      "decoration.strikeThrough.material", TextPropertyValueKind::Material,
      kRunScopes, kReplace, false, kGlyph),
    D(TextPropertyId::StrikeThroughThickness,
      "decoration.strikeThrough.thickness", TextPropertyValueKind::Scalar,
      kRunScopes, kNumeric, true, kGlyph),
    D(TextPropertyId::StrikeThroughOffset, "decoration.strikeThrough.offset",
      TextPropertyValueKind::Scalar, kRunScopes, kNumeric, true, kGlyph),
    D(TextPropertyId::GlyphFill, "glyph.fill",
      TextPropertyValueKind::Material, kMaterialScopes, kReplace, true, kGlyph),
    D(TextPropertyId::GlyphFillColor, "glyph.fill.color",
      TextPropertyValueKind::Color, kMaterialScopes, kReplace, true, kGlyph),
    D(TextPropertyId::GlyphFillOpacity, "glyph.fill.opacity",
      TextPropertyValueKind::Scalar, kMaterialScopes, kNumeric, true, kGlyph),
    D(TextPropertyId::GlyphFillGradientStartColor,
      "glyph.fill.gradient.startColor", TextPropertyValueKind::Color,
      kMaterialScopes, kReplace, true, kGlyph),
    D(TextPropertyId::GlyphFillGradientEndColor,
      "glyph.fill.gradient.endColor", TextPropertyValueKind::Color,
      kMaterialScopes, kReplace, true, kGlyph),
    D(TextPropertyId::GlyphFillGradientStartOffset,
      "glyph.fill.gradient.startOffset", TextPropertyValueKind::Scalar,
      kMaterialScopes, kReplace, true, kGlyph),
    D(TextPropertyId::GlyphFillGradientEndOffset,
      "glyph.fill.gradient.endOffset", TextPropertyValueKind::Scalar,
      kMaterialScopes, kReplace, true, kGlyph),
    D(TextPropertyId::GlyphFillGradientAngle, "glyph.fill.gradient.angle",
      TextPropertyValueKind::Scalar, kMaterialScopes, kNumeric, true, kGlyph),
    D(TextPropertyId::GlyphFillGradientCenterX,
      "glyph.fill.gradient.centerX", TextPropertyValueKind::Scalar,
      kMaterialScopes, kNumeric, true, kGlyph),
    D(TextPropertyId::GlyphFillGradientCenterY,
      "glyph.fill.gradient.centerY", TextPropertyValueKind::Scalar,
      kMaterialScopes, kNumeric, true, kGlyph),
    D(TextPropertyId::GlyphFillGradientRadius,
      "glyph.fill.gradient.radius", TextPropertyValueKind::Scalar,
      kMaterialScopes, kNumeric, true, kGlyph),
    D(TextPropertyId::GlyphMaterialStack, "glyph.materialStack",
      TextPropertyValueKind::MaterialStack, kRunScopes, kReplace, false,
      kGlyph),
    D(TextPropertyId::GlyphStrokeStack, "glyph.strokes",
      TextPropertyValueKind::StrokeStack, kRunScopes, kReplace, false, kGlyph),
    D(TextPropertyId::GlyphStrokeColor, "glyph.stroke.color",
      TextPropertyValueKind::Color, kGlyphLayerScope, kReplace, true, kGlyph),
    D(TextPropertyId::GlyphStrokeWidth, "glyph.stroke.width",
      TextPropertyValueKind::Scalar, kGlyphLayerScope, kNumeric, true, kGlyph),
    D(TextPropertyId::GlyphStrokeOpacity, "glyph.stroke.opacity",
      TextPropertyValueKind::Scalar, kGlyphLayerScope, kNumeric, true, kGlyph),
    D(TextPropertyId::GlyphShadowStack, "glyph.shadows",
      TextPropertyValueKind::ShadowStack, kRunScopes, kReplace, false, kGlyph),
    D(TextPropertyId::GlyphShadowColor, "glyph.shadow.color",
      TextPropertyValueKind::Color, kGlyphLayerScope, kReplace, true, kGlyph),
    D(TextPropertyId::GlyphShadowOpacity, "glyph.shadow.opacity",
      TextPropertyValueKind::Scalar, kGlyphLayerScope, kNumeric, true, kGlyph),
    D(TextPropertyId::GlyphShadowOffsetX, "glyph.shadow.offsetX",
      TextPropertyValueKind::Scalar, kGlyphLayerScope, kNumeric, true, kGlyph),
    D(TextPropertyId::GlyphShadowOffsetY, "glyph.shadow.offsetY",
      TextPropertyValueKind::Scalar, kGlyphLayerScope, kNumeric, true, kGlyph),
    D(TextPropertyId::GlyphShadowBlur, "glyph.shadow.blur",
      TextPropertyValueKind::Scalar, kGlyphLayerScope, kNumeric, true, kGlyph),
    D(TextPropertyId::GlyphShadowSpread, "glyph.shadow.spread",
      TextPropertyValueKind::Scalar, kGlyphLayerScope, kNumeric, true, kGlyph),
    D(TextPropertyId::GlyphShadowDistance, "glyph.shadow.distance",
      TextPropertyValueKind::Scalar, kGlyphLayerScope, kNumeric, true, kGlyph),
    D(TextPropertyId::GlyphShadowAngle, "glyph.shadow.angle",
      TextPropertyValueKind::Scalar, kGlyphLayerScope, kNumeric, true, kGlyph),
    D(TextPropertyId::GlyphGlowStack, "glyph.glows",
      TextPropertyValueKind::GlowStack, kRunScopes, kReplace, false, kGlyph),
    D(TextPropertyId::GlyphGlowColor, "glyph.glow.color",
      TextPropertyValueKind::Color, kGlyphLayerScope, kReplace, true, kGlyph),
    D(TextPropertyId::GlyphGlowOpacity, "glyph.glow.opacity",
      TextPropertyValueKind::Scalar, kGlyphLayerScope, kNumeric, true, kGlyph),
    D(TextPropertyId::GlyphGlowRadius, "glyph.glow.radius",
      TextPropertyValueKind::Scalar, kGlyphLayerScope, kNumeric, true, kGlyph),
    D(TextPropertyId::GlyphGlowSpread, "glyph.glow.spread",
      TextPropertyValueKind::Scalar, kGlyphLayerScope, kNumeric, true, kGlyph),
    D(TextPropertyId::GlyphGlowDirectionX, "glyph.glow.directionX",
      TextPropertyValueKind::Scalar, kGlyphLayerScope, kNumeric, true, kGlyph),
    D(TextPropertyId::GlyphGlowDirectionY, "glyph.glow.directionY",
      TextPropertyValueKind::Scalar, kGlyphLayerScope, kNumeric, true, kGlyph),
    D(TextPropertyId::RunBackground, "run.background",
      TextPropertyValueKind::BoxBackground, kRunScopes, kReplace, true,
      kGlyph),
    D(TextPropertyId::ParagraphBackground, "paragraph.background",
      TextPropertyValueKind::BoxBackground, kParagraphScopes, kReplace, false,
      kGlyph),
    D(TextPropertyId::ParagraphAlignment, "paragraph.alignment",
      TextPropertyValueKind::Enumeration, kParagraphScopes, kReplace, false,
      kLayout),
    D(TextPropertyId::ParagraphDirection, "paragraph.direction",
      TextPropertyValueKind::Enumeration, kParagraphScopes, kReplace, false,
      kLayout),
    D(TextPropertyId::ParagraphLocale, "paragraph.locale",
      TextPropertyValueKind::String, kParagraphScopes, kReplace, false,
      kLayout),
    D(TextPropertyId::ParagraphLineHeight, "paragraph.lineHeight",
      TextPropertyValueKind::Scalar, kParagraphScopes, kNumeric, true, kLayout),
    D(TextPropertyId::ParagraphMaximumLines, "paragraph.maximumLines",
      TextPropertyValueKind::Integer, kParagraphScopes, kReplace, false,
      kLayout),
    D(TextPropertyId::ParagraphOverflow, "paragraph.overflow",
      TextPropertyValueKind::Enumeration, kParagraphScopes, kReplace, false,
      kLayout),
    D(TextPropertyId::ParagraphWrap, "paragraph.wrap",
      TextPropertyValueKind::Enumeration, kParagraphScopes, kReplace, false,
      kLayout),
    D(TextPropertyId::ParagraphLineBreakPolicy, "paragraph.lineBreakPolicy",
      TextPropertyValueKind::Enumeration, kParagraphScopes, kReplace, false,
      kLayout),
    D(TextPropertyId::ParagraphHyphenation, "paragraph.hyphenation",
      TextPropertyValueKind::Enumeration, kParagraphScopes, kReplace, false,
      kLayout),
    D(TextPropertyId::ParagraphFirstLineIndent, "paragraph.firstLineIndent",
      TextPropertyValueKind::Scalar, kParagraphScopes, kNumeric, true, kLayout),
    D(TextPropertyId::ParagraphStartIndent, "paragraph.startIndent",
      TextPropertyValueKind::Scalar, kParagraphScopes, kNumeric, true, kLayout),
    D(TextPropertyId::ParagraphEndIndent, "paragraph.endIndent",
      TextPropertyValueKind::Scalar, kParagraphScopes, kNumeric, true, kLayout),
    D(TextPropertyId::ParagraphSpacingBefore, "paragraph.spacingBefore",
      TextPropertyValueKind::Scalar, kParagraphScopes, kNumeric, true, kLayout),
    D(TextPropertyId::ParagraphSpacingAfter, "paragraph.spacingAfter",
      TextPropertyValueKind::Scalar, kParagraphScopes, kNumeric, true, kLayout),
    D(TextPropertyId::ParagraphHangingPunctuation,
      "paragraph.hangingPunctuation", TextPropertyValueKind::Boolean,
      kParagraphScopes, kReplace, false, kLayout),
    D(TextPropertyId::ParagraphTabStops, "paragraph.tabStops",
      TextPropertyValueKind::TabStops, kParagraphScopes, kReplace, false,
      kLayout),
    D(TextPropertyId::LayoutFrame, "layout.frame",
      TextPropertyValueKind::LayoutBox, kCompositionScope, kReplace, false,
      kLayout),
    D(TextPropertyId::LayoutSizingMode, "layout.sizingMode",
      TextPropertyValueKind::Enumeration, kCompositionScope, kReplace, false,
      kLayout),
    D(TextPropertyId::LayoutVerticalAlignment, "layout.verticalAlignment",
      TextPropertyValueKind::Enumeration, kCompositionScope, kReplace, false,
      kLayout),
    D(TextPropertyId::LayoutPadding, "layout.padding",
      TextPropertyValueKind::Insets, kCompositionScope, kReplace, false,
      kLayout),
    D(TextPropertyId::LayoutPixelSnap, "layout.pixelSnap",
      TextPropertyValueKind::Boolean, kCompositionScope, kReplace, false,
      kLayout),
    D(TextPropertyId::WritingMode, "layout.writingMode",
      TextPropertyValueKind::WritingMode, kCompositionScope, kReplace, false,
      kLayout),
    D(TextPropertyId::BackdropStack, "backdrop.stack",
      TextPropertyValueKind::BackdropStack, kCompositionScope, kReplace, false,
      kBackdrop),
    D(TextPropertyId::BackdropMaterial, "backdrop.material",
      TextPropertyValueKind::Material, kBackdropLayerScope, kReplace, false,
      kBackdrop),
    D(TextPropertyId::BackdropFillColor, "backdrop.fill.color",
      TextPropertyValueKind::Color, kBackdropLayerScope, kReplace, true,
      kBackdrop),
    D(TextPropertyId::BackdropOpacity, "backdrop.opacity",
      TextPropertyValueKind::Scalar, kBackdropLayerScope, kNumeric, true,
      kBackdrop),
    D(TextPropertyId::BackdropPadding, "backdrop.padding",
      TextPropertyValueKind::Insets, kBackdropLayerScope, kReplace, true,
      kBackdrop),
    D(TextPropertyId::BackdropCornerRadius, "backdrop.cornerRadius",
      TextPropertyValueKind::Scalar, kBackdropLayerScope, kNumeric, true,
      kBackdrop),
    D(TextPropertyId::BackdropWidth, "backdrop.width",
      TextPropertyValueKind::Scalar, kBackdropLayerScope, kNumeric, true,
      kBackdrop),
    D(TextPropertyId::BackdropHeight, "backdrop.height",
      TextPropertyValueKind::Scalar, kBackdropLayerScope, kNumeric, true,
      kBackdrop),
    D(TextPropertyId::BackdropOffsetX, "backdrop.offsetX",
      TextPropertyValueKind::Scalar, kBackdropLayerScope, kNumeric, true,
      kBackdrop),
    D(TextPropertyId::BackdropOffsetY, "backdrop.offsetY",
      TextPropertyValueKind::Scalar, kBackdropLayerScope, kNumeric, true,
      kBackdrop),
    D(TextPropertyId::BackdropScaleX, "backdrop.scaleX",
      TextPropertyValueKind::Scalar, kBackdropLayerScope, kNumeric, true,
      kBackdrop),
    D(TextPropertyId::BackdropScaleY, "backdrop.scaleY",
      TextPropertyValueKind::Scalar, kBackdropLayerScope, kNumeric, true,
      kBackdrop),
    D(TextPropertyId::BackdropRotation, "backdrop.rotation",
      TextPropertyValueKind::Scalar, kBackdropLayerScope, kNumeric, true,
      kBackdrop),
    D(TextPropertyId::FrameBackdrop, "backdrop.frame",
      TextPropertyValueKind::BackdropLayer, kBackdropScopes, kReplace, true,
      kBackdrop),
    D(TextPropertyId::BubbleBackdrop, "backdrop.bubble",
      TextPropertyValueKind::BackdropLayer, kBackdropScopes, kReplace, true,
      kBackdrop),
    D(TextPropertyId::CustomBackdrop, "backdrop.custom",
      TextPropertyValueKind::BackdropLayer, kBackdropScopes, kReplace, true,
      kBackdrop),
    D(TextPropertyId::GlyphDecorativeMaterialStack, "glyph.decorativeMaterialStack",
      TextPropertyValueKind::MaterialStack, kRunScopes, kReplace, false,
      kGlyph),
    D(TextPropertyId::Bend, "layout.bend", TextPropertyValueKind::Bend,
      kCompositionScope, kReplace, true, kLayout),
    D(TextPropertyId::Path, "layout.path", TextPropertyValueKind::Path,
      kCompositionScope, kReplace, true, kLayout),
    D(TextPropertyId::SdfMaterial, "glyph.sdf",
      TextPropertyValueKind::SdfMaterial, kCompositionScope, kReplace, false,
      kPost),
    D(TextPropertyId::GlobalAlpha, "composition.alpha",
      TextPropertyValueKind::Scalar, kCompositionScope, kNumeric, true,
      TextInvalidationImpact::Composite),
    D(TextPropertyId::SemanticMaterialChannel, "glyph.semanticMaterialChannel",
      TextPropertyValueKind::Material, kGlyphLayerScope, kReplace, true, kGlyph),
    D(TextPropertyId::BackdropEnabled, "backdrop.enabled",
      TextPropertyValueKind::Boolean, kBackdropLayerScope, kReplace, false,
      kBackdrop),
};

void AddDiagnostic(TextPropertyPatchResult &result, std::string code,
                   std::string subject, std::string message) {
  result.diagnostics.push_back({std::move(code), DiagnosticSeverity::Error,
                                "text.property", std::move(subject),
                                std::move(message)});
}

bool Finite(const double value) noexcept { return std::isfinite(value); }

bool ValidColor(const Color &color) noexcept {
  return std::isfinite(color.red) && std::isfinite(color.green) &&
         std::isfinite(color.blue) && std::isfinite(color.alpha) &&
         color.red >= 0.0F && color.red <= 1.0F && color.green >= 0.0F &&
         color.green <= 1.0F && color.blue >= 0.0F && color.blue <= 1.0F &&
         color.alpha >= 0.0F && color.alpha <= 1.0F;
}

bool IsUtf8Boundary(const std::string_view value, const std::size_t offset) noexcept {
  return offset == 0U || offset == value.size() ||
         (offset < value.size() &&
          (static_cast<unsigned char>(value[offset]) & 0xC0U) != 0x80U);
}

bool ValueKindMatches(const TextPropertyId property,
                      const TextPropertyValueKind kind,
                      const TextPropertyValue &value) noexcept {
  switch (kind) {
  case TextPropertyValueKind::Boolean:
    return std::holds_alternative<bool>(value);
  case TextPropertyValueKind::Integer:
  case TextPropertyValueKind::Enumeration:
    return std::holds_alternative<std::int64_t>(value);
  case TextPropertyValueKind::Scalar:
    return std::holds_alternative<double>(value);
  case TextPropertyValueKind::String:
    return std::holds_alternative<std::string>(value);
  case TextPropertyValueKind::Color:
    return std::holds_alternative<Color>(value);
  case TextPropertyValueKind::Font:
    return property == TextPropertyId::FontReference
               ? std::holds_alternative<FontReference>(value)
               : std::holds_alternative<FontSpec>(value);
  case TextPropertyValueKind::FontAxes:
    return std::holds_alternative<std::vector<FontAxis>>(value);
  case TextPropertyValueKind::FontFeatures:
    return std::holds_alternative<std::vector<FontFeature>>(value);
  case TextPropertyValueKind::Material:
    return std::holds_alternative<TextMaterial>(value);
  case TextPropertyValueKind::MaterialStack:
    return std::holds_alternative<TextGlyphMaterialStack>(value);
  case TextPropertyValueKind::StrokeStack:
    return std::holds_alternative<std::vector<TextStrokeLayer>>(value);
  case TextPropertyValueKind::ShadowStack:
    return std::holds_alternative<std::vector<TextShadowLayer>>(value);
  case TextPropertyValueKind::GlowStack:
    return std::holds_alternative<std::vector<TextGlowLayer>>(value);
  case TextPropertyValueKind::InlineDecoration:
    return std::holds_alternative<InlineTextDecoration>(value);
  case TextPropertyValueKind::BoxBackground:
    return std::holds_alternative<TextBoxBackground>(value);
  case TextPropertyValueKind::ParagraphStyle:
    return std::holds_alternative<ParagraphStyle>(value);
  case TextPropertyValueKind::TabStops:
    return std::holds_alternative<std::vector<TextTabStop>>(value);
  case TextPropertyValueKind::LayoutBox:
    return std::holds_alternative<LayoutBox>(value);
  case TextPropertyValueKind::Insets:
    return std::holds_alternative<Insets>(value);
  case TextPropertyValueKind::WritingMode:
    return std::holds_alternative<TextWritingMode>(value);
  case TextPropertyValueKind::BackdropLayer:
    return std::holds_alternative<TextBackdropLayer>(value);
  case TextPropertyValueKind::BackdropStack:
    return std::holds_alternative<TextBackdropStack>(value);
  case TextPropertyValueKind::Bend:
    return std::holds_alternative<TextBend>(value);
  case TextPropertyValueKind::Path:
    return std::holds_alternative<TextPath>(value);
  case TextPropertyValueKind::SdfMaterial:
    return std::holds_alternative<TextSdfMaterial>(value);
  }
  return false;
}

bool ValidEnumeration(const TextPropertyId property,
                      const std::int64_t value) noexcept {
  switch (property) {
  case TextPropertyId::FontSlant:
    return value >= static_cast<std::int64_t>(FontSlant::Upright) &&
           value <= static_cast<std::int64_t>(FontSlant::Oblique);
  case TextPropertyId::UnderlineStyle:
  case TextPropertyId::StrikeThroughStyle:
    return value >= static_cast<std::int64_t>(TextDecorationLineStyle::Solid) &&
           value <= static_cast<std::int64_t>(TextDecorationLineStyle::Wavy);
  case TextPropertyId::ParagraphAlignment:
    return value >= static_cast<std::int64_t>(TextAlignment::Start) &&
           value <= static_cast<std::int64_t>(TextAlignment::Justify);
  case TextPropertyId::ParagraphDirection:
    return value >= static_cast<std::int64_t>(TextDirection::Auto) &&
           value <= static_cast<std::int64_t>(TextDirection::RightToLeft);
  case TextPropertyId::ParagraphOverflow:
    return value >= static_cast<std::int64_t>(TextOverflow::Clip) &&
           value <= static_cast<std::int64_t>(TextOverflow::Visible);
  case TextPropertyId::ParagraphWrap:
    return value >= static_cast<std::int64_t>(TextWrap::Word) &&
           value <= static_cast<std::int64_t>(TextWrap::None);
  case TextPropertyId::ParagraphLineBreakPolicy:
    return value >= static_cast<std::int64_t>(TextLineBreakPolicy::Unicode) &&
           value <= static_cast<std::int64_t>(TextLineBreakPolicy::Anywhere);
  case TextPropertyId::ParagraphHyphenation:
    return value >= static_cast<std::int64_t>(TextHyphenation::None) &&
           value <= static_cast<std::int64_t>(TextHyphenation::Automatic);
  case TextPropertyId::LayoutSizingMode:
    return value >= static_cast<std::int64_t>(TextLayoutSizingMode::AutoWidth) &&
           value <= static_cast<std::int64_t>(TextLayoutSizingMode::FitText);
  case TextPropertyId::LayoutVerticalAlignment:
    return value >= static_cast<std::int64_t>(VerticalAlignment::Top) &&
           value <= static_cast<std::int64_t>(VerticalAlignment::Bottom);
  default:
    return true;
  }
}

bool ValidScalar(const TextPropertyId property, const double value,
                 const RichTextLimits &limits) noexcept {
  if (!Finite(value))
    return false;
  switch (property) {
  case TextPropertyId::FontSize:
    return value > 0.0 && value <= limits.maximumFontSize;
  case TextPropertyId::ParagraphLineHeight:
    return value > 0.0 && value <= 16.0;
  case TextPropertyId::UnderlineThickness:
  case TextPropertyId::StrikeThroughThickness:
  case TextPropertyId::ParagraphSpacingBefore:
  case TextPropertyId::ParagraphSpacingAfter:
    return value >= 0.0 && value <= limits.maximumCanvasDimension;
  case TextPropertyId::LetterSpacing:
  case TextPropertyId::WordSpacing:
  case TextPropertyId::BaselineShift:
  case TextPropertyId::UnderlineOffset:
  case TextPropertyId::StrikeThroughOffset:
  case TextPropertyId::ParagraphFirstLineIndent:
  case TextPropertyId::ParagraphStartIndent:
  case TextPropertyId::ParagraphEndIndent:
    return std::fabs(value) <= limits.maximumCanvasDimension;
  case TextPropertyId::GlobalAlpha:
  case TextPropertyId::GlyphFillOpacity:
  case TextPropertyId::GlyphStrokeOpacity:
  case TextPropertyId::GlyphShadowOpacity:
  case TextPropertyId::GlyphGlowOpacity:
  case TextPropertyId::BackdropOpacity:
    return value >= 0.0 && value <= 1.0;
  case TextPropertyId::GlyphFillGradientStartOffset:
  case TextPropertyId::GlyphFillGradientEndOffset:
    return value >= 0.0 && value <= 1.0;
  case TextPropertyId::GlyphFillGradientCenterX:
  case TextPropertyId::GlyphFillGradientCenterY:
    return std::fabs(value) <= 8.0;
  case TextPropertyId::GlyphFillGradientRadius:
    return value > 0.0 && value <= 8.0;
  case TextPropertyId::GlyphStrokeWidth:
  case TextPropertyId::GlyphShadowBlur:
  case TextPropertyId::GlyphShadowSpread:
  case TextPropertyId::GlyphShadowDistance:
  case TextPropertyId::GlyphGlowRadius:
  case TextPropertyId::GlyphGlowSpread:
  case TextPropertyId::BackdropCornerRadius:
  case TextPropertyId::BackdropWidth:
  case TextPropertyId::BackdropHeight:
    return value >= 0.0 && value <= limits.maximumCanvasDimension;
  case TextPropertyId::GlyphGlowDirectionX:
  case TextPropertyId::GlyphGlowDirectionY:
    return value >= -1.0 && value <= 1.0;
  case TextPropertyId::BackdropScaleX:
  case TextPropertyId::BackdropScaleY:
    return value > 0.0 && value <= 64.0;
  case TextPropertyId::GlyphFillGradientAngle:
  case TextPropertyId::GlyphShadowAngle:
  case TextPropertyId::BackdropRotation:
    return std::fabs(value) <= 3'600.0;
  case TextPropertyId::GlyphShadowOffsetX:
  case TextPropertyId::GlyphShadowOffsetY:
  case TextPropertyId::BackdropOffsetX:
  case TextPropertyId::BackdropOffsetY:
    return std::fabs(value) <= limits.maximumCanvasDimension;
  default:
    return true;
  }
}

bool ValidInteger(const TextPropertyId property,
                  const std::int64_t value) noexcept {
  switch (property) {
  case TextPropertyId::FontWeight:
    return value >= 1 && value <= 1000;
  case TextPropertyId::FontWidth:
    return value >= 1 && value <= 9;
  case TextPropertyId::ParagraphMaximumLines:
    return value >= 0 && value <= 1'000'000;
  default:
    return true;
  }
}

bool ValidTargetShape(const TextPropertyTarget &target,
                      std::string &reason) {
  const auto validIds = [](const std::vector<std::string> &ids) {
    return std::all_of(ids.begin(), ids.end(), [](const std::string &id) {
      return !id.empty() && id.size() <= 512U && IsValidUtf8(id);
    });
  };
  if (!target.contentSlotId.empty() &&
      (target.contentSlotId.size() > 512U ||
       !IsValidUtf8(target.contentSlotId))) {
    reason = "content-slot identity is invalid";
    return false;
  }
  if (!validIds(target.paragraphIds) || !validIds(target.runIds) ||
      (!target.layerId.empty() &&
       (target.layerId.size() > 512U || !IsValidUtf8(target.layerId)))) {
    reason = "target identity is invalid";
    return false;
  }
  switch (target.scope) {
  case TextPropertyScope::Composition:
    if (!target.contentSlotId.empty() || !target.paragraphIds.empty() ||
        !target.runIds.empty() || target.range || !target.layerId.empty()) {
      reason = "composition target must not carry child identities";
      return false;
    }
    break;
  case TextPropertyScope::ContentSlot:
    if (target.contentSlotId.empty() || !target.paragraphIds.empty() ||
        !target.runIds.empty() || target.range || !target.layerId.empty()) {
      reason = "content-slot target is incomplete";
      return false;
    }
    break;
  case TextPropertyScope::Paragraph:
    if (target.paragraphIds.empty() || !target.contentSlotId.empty() ||
        !target.runIds.empty() || target.range || !target.layerId.empty()) {
      reason = "paragraph target is incomplete";
      return false;
    }
    break;
  case TextPropertyScope::Run:
    if (target.runIds.empty() || !target.contentSlotId.empty() ||
        !target.paragraphIds.empty() || target.range || !target.layerId.empty()) {
      reason = "run target is incomplete";
      return false;
    }
    break;
  case TextPropertyScope::Utf8Range:
    if (target.runIds.size() != 1U || !target.range ||
        target.range->begin >= target.range->end ||
        !target.contentSlotId.empty() || !target.paragraphIds.empty() ||
        !target.layerId.empty()) {
      reason = "UTF-8 range target requires one run and a non-empty range";
      return false;
    }
    break;
  case TextPropertyScope::GlyphMaterialLayer:
    if (target.layerId.empty() ||
        (target.range &&
         (target.runIds.size() != 1U ||
          target.range->begin >= target.range->end ||
          !target.contentSlotId.empty() || !target.paragraphIds.empty()))) {
      reason = "glyph-material target requires a layer identity";
      return false;
    }
    break;
  case TextPropertyScope::BackdropLayer:
    if (target.layerId.empty() || !target.contentSlotId.empty() ||
        !target.paragraphIds.empty() || !target.runIds.empty() || target.range) {
      reason = "backdrop target requires only a layer identity";
      return false;
    }
    break;
  }
  return true;
}

double ComposeScalar(const double current, const double authored,
                     const TextPropertyCombineMode mode) noexcept {
  switch (mode) {
  case TextPropertyCombineMode::Replace:
    return authored;
  case TextPropertyCombineMode::Add:
    return current + authored;
  case TextPropertyCombineMode::Multiply:
    return current * authored;
  case TextPropertyCombineMode::MatrixConcat:
  case TextPropertyCombineMode::ColorMix:
    return authored;
  }
  return authored;
}

template <typename Layer>
void ReplaceLayerKind(TextGlyphMaterialStack &stack,
                      const std::vector<Layer> &replacement) {
  auto next = replacement.begin();
  for (auto layer = stack.layers.begin(); layer != stack.layers.end();) {
    if (!std::holds_alternative<Layer>(*layer)) {
      ++layer;
    } else if (next != replacement.end()) {
      *layer++ = *next++;
    } else {
      layer = stack.layers.erase(layer);
    }
  }
  for (; next != replacement.end(); ++next)
    stack.layers.emplace_back(*next);
}

TextFillLayer *FindPrimaryFill(TextGlyphMaterialStack &stack,
                               const std::string &layerId = {}) {
  for (auto &layer : stack.layers) {
    if (auto *fill = std::get_if<TextFillLayer>(&layer);
        fill && (layerId.empty() || fill->layerId == layerId)) {
      return fill;
    }
  }
  return nullptr;
}

TextMaterialBinding *FindMaterialBinding(TextGlyphMaterialStack &stack,
                                         const std::string &identity) {
  for (auto &layer : stack.layers) {
    TextMaterialBinding *binding = std::visit(
        [&](auto &value) -> TextMaterialBinding * {
          if (value.layerId == identity)
            return &value.material;
          if (auto *slot = std::get_if<EditableTextStyleSlot>(&value.material);
              slot && slot->semanticRole == identity) {
            return &value.material;
          }
          return nullptr;
        },
        layer);
    if (binding)
      return binding;
  }
  return nullptr;
}

void ReplaceBindingMaterial(TextMaterialBinding &binding,
                            const TextMaterial &material) {
  if (auto *literal = std::get_if<LiteralTextMaterial>(&binding)) {
    literal->material = material;
  } else {
    std::get<EditableTextStyleSlot>(binding).fallback = material;
  }
}

TextMaterial &MutableBindingMaterial(TextMaterialBinding &binding) {
  if (auto *literal = std::get_if<LiteralTextMaterial>(&binding))
    return literal->material;
  return std::get<EditableTextStyleSlot>(binding).fallback;
}

Color RepresentativeColor(const TextMaterial &material) noexcept {
  return std::visit(
      [](const auto &source) -> Color {
        using Source = std::decay_t<decltype(source)>;
        if constexpr (std::is_same_v<Source, SolidTextMaterial>) {
          return source.color;
        } else if constexpr (std::is_same_v<Source,
                                            LinearGradientTextMaterial> ||
                             std::is_same_v<Source,
                                            RadialGradientTextMaterial>) {
          return source.stops.empty() ? Color{1.0F, 1.0F, 1.0F, 1.0F}
                                      : source.stops.front().color;
        } else {
          return source.underlayColor.value_or(
              Color{1.0F, 1.0F, 1.0F, source.opacity});
        }
      },
      material);
}

void SetMaterialColor(TextMaterial &material, const Color &color) {
  material = SolidTextMaterial{color};
}

void SetMaterialOpacity(TextMaterial &material, const float opacity) {
  std::visit(
      [opacity](auto &source) {
        using Source = std::decay_t<decltype(source)>;
        if constexpr (std::is_same_v<Source, SolidTextMaterial>) {
          source.color.alpha = opacity;
        } else if constexpr (std::is_same_v<Source,
                                            LinearGradientTextMaterial> ||
                             std::is_same_v<Source,
                                            RadialGradientTextMaterial>) {
          for (auto &stop : source.stops)
            stop.color.alpha = opacity;
        } else {
          source.opacity = opacity;
        }
      },
      material);
}

float MaterialOpacity(const TextMaterial &material) noexcept {
  return std::visit(
      [](const auto &source) -> float {
        using Source = std::decay_t<decltype(source)>;
        if constexpr (std::is_same_v<Source, SolidTextMaterial>) {
          return source.color.alpha;
        } else if constexpr (std::is_same_v<Source,
                                            LinearGradientTextMaterial> ||
                             std::is_same_v<Source,
                                            RadialGradientTextMaterial>) {
          return source.stops.empty() ? 1.0F : source.stops.front().color.alpha;
        } else {
          return source.opacity;
        }
      },
      material);
}

LinearGradientTextMaterial &EnsureLinearGradient(TextMaterial &material) {
  if (auto *linear = std::get_if<LinearGradientTextMaterial>(&material))
    return *linear;
  const auto color = RepresentativeColor(material);
  LinearGradientTextMaterial replacement;
  replacement.stops = {{0.0F, color}, {1.0F, color}};
  material = std::move(replacement);
  return std::get<LinearGradientTextMaterial>(material);
}

RadialGradientTextMaterial &EnsureRadialGradient(TextMaterial &material) {
  if (auto *radial = std::get_if<RadialGradientTextMaterial>(&material))
    return *radial;
  const auto color = RepresentativeColor(material);
  RadialGradientTextMaterial replacement;
  replacement.stops = {{0.0F, color}, {1.0F, color}};
  material = std::move(replacement);
  return std::get<RadialGradientTextMaterial>(material);
}

std::vector<GradientStop> &MutableGradientStops(TextMaterial &material) {
  if (auto *radial = std::get_if<RadialGradientTextMaterial>(&material))
    return radial->stops;
  return EnsureLinearGradient(material).stops;
}

template <typename Layer>
Layer *FindTypedLayer(TextGlyphMaterialStack &stack,
                      const std::string &layerId) {
  for (auto &candidate : stack.layers) {
    if (auto *layer = std::get_if<Layer>(&candidate);
        layer && (layerId.empty() || layer->layerId == layerId)) {
      return layer;
    }
  }
  return nullptr;
}

bool ApplyRunProperty(TextStyle &style, const TextPropertyAssignment &assignment,
                      const std::string &materialLayerId) {
  const auto property = assignment.address.property;
  const bool reset = assignment.disposition != TextPropertyDisposition::Set;
  static const TextStyle defaults{};
  const auto strikeProperty = [&] {
    switch (property) {
    case TextPropertyId::StrikeThroughEnabled:
    case TextPropertyId::StrikeThroughStyle:
    case TextPropertyId::StrikeThroughMaterial:
    case TextPropertyId::StrikeThroughThickness:
    case TextPropertyId::StrikeThroughOffset: return true;
    default: return false;
    }
  }();
  auto &decorationLine = strikeProperty ? style.decoration.strikeThrough
                                        : style.decoration.underline;
  const auto &defaultDecorationLine =
      strikeProperty ? defaults.decoration.strikeThrough
                     : defaults.decoration.underline;
  switch (property) {
  case TextPropertyId::FontReference:
    if (reset)
      return false;
    style.font.primary = std::get<FontReference>(assignment.value);
    style.font.family = style.font.primary.family;
    style.font.postscriptName = style.font.primary.postscriptName;
    style.font.weight = style.font.primary.weight;
    style.font.width = style.font.primary.width;
    style.font.slant = style.font.primary.slant;
    style.font.faceIndex = style.font.primary.faceIndex;
    style.font.variationAxes = style.font.primary.variationAxes;
    return true;
  case TextPropertyId::FontFamily:
    style.font.family = reset ? defaults.font.family
                              : std::get<std::string>(assignment.value);
    style.font.primary.family = style.font.family;
    return true;
  case TextPropertyId::FontPostscriptName:
    style.font.postscriptName =
        reset ? defaults.font.postscriptName
              : std::get<std::string>(assignment.value);
    style.font.primary.postscriptName = style.font.postscriptName;
    return true;
  case TextPropertyId::FontWeight:
    style.font.weight =
        reset ? defaults.font.weight
              : static_cast<std::int32_t>(
                    std::get<std::int64_t>(assignment.value));
    style.font.primary.weight = style.font.weight;
    return true;
  case TextPropertyId::FontWidth:
    style.font.width =
        reset ? defaults.font.width
              : static_cast<std::int32_t>(
                    std::get<std::int64_t>(assignment.value));
    style.font.primary.width = style.font.width;
    return true;
  case TextPropertyId::FontSlant:
    style.font.slant =
        reset ? defaults.font.slant
              : static_cast<FontSlant>(
                    std::get<std::int64_t>(assignment.value));
    style.font.primary.slant = style.font.slant;
    return true;
  case TextPropertyId::FontVariationAxes:
    style.font.variationAxes =
        reset ? defaults.font.variationAxes
              : std::get<std::vector<FontAxis>>(assignment.value);
    style.font.primary.variationAxes = style.font.variationAxes;
    return true;
  case TextPropertyId::FontFeatures:
    style.font.features =
        reset ? defaults.font.features
              : std::get<std::vector<FontFeature>>(assignment.value);
    return true;
  case TextPropertyId::FontSize:
    style.fontSize = static_cast<float>(
        reset ? defaults.fontSize
              : ComposeScalar(style.fontSize, std::get<double>(assignment.value),
                              assignment.combineMode));
    return true;
  case TextPropertyId::LetterSpacing:
    style.letterSpacing = static_cast<float>(
        reset ? defaults.letterSpacing
              : ComposeScalar(style.letterSpacing,
                              std::get<double>(assignment.value),
                              assignment.combineMode));
    return true;
  case TextPropertyId::WordSpacing:
    style.wordSpacing = static_cast<float>(
        reset ? defaults.wordSpacing
              : ComposeScalar(style.wordSpacing,
                              std::get<double>(assignment.value),
                              assignment.combineMode));
    return true;
  case TextPropertyId::BaselineShift:
    style.baselineShift = static_cast<float>(
        reset ? defaults.baselineShift
              : ComposeScalar(style.baselineShift,
                              std::get<double>(assignment.value),
                              assignment.combineMode));
    return true;
  case TextPropertyId::UnderlineEnabled:
  case TextPropertyId::StrikeThroughEnabled:
    decorationLine.enabled =
        reset ? defaultDecorationLine.enabled
              : std::get<bool>(assignment.value);
    return true;
  case TextPropertyId::UnderlineStyle:
  case TextPropertyId::StrikeThroughStyle:
    decorationLine.style =
        reset ? defaultDecorationLine.style
              : static_cast<TextDecorationLineStyle>(
                    std::get<std::int64_t>(assignment.value));
    return true;
  case TextPropertyId::UnderlineMaterial:
  case TextPropertyId::StrikeThroughMaterial: {
    if (reset) {
      decorationLine.material = defaultDecorationLine.material;
      return true;
    }
    ReplaceBindingMaterial(decorationLine.material,
                           std::get<TextMaterial>(assignment.value));
    return true;
  }
  case TextPropertyId::UnderlineThickness:
  case TextPropertyId::StrikeThroughThickness:
    decorationLine.thickness = static_cast<float>(
        reset ? defaultDecorationLine.thickness
              : ComposeScalar(decorationLine.thickness,
                              std::get<double>(assignment.value),
                              assignment.combineMode));
    return true;
  case TextPropertyId::UnderlineOffset:
  case TextPropertyId::StrikeThroughOffset:
    decorationLine.offset = static_cast<float>(
        reset ? defaultDecorationLine.offset
              : ComposeScalar(decorationLine.offset,
                              std::get<double>(assignment.value),
                              assignment.combineMode));
    return true;
  case TextPropertyId::UnderlineSkipInk:
    decorationLine.skipInk =
        reset ? defaultDecorationLine.skipInk
              : std::get<bool>(assignment.value);
    return true;
  case TextPropertyId::GlyphFill: {
    auto *fill = FindPrimaryFill(style.materials, materialLayerId);
    if (!fill && materialLayerId.empty()) {
      style.materials.layers.emplace_back(TextFillLayer{});
      fill = std::get_if<TextFillLayer>(&style.materials.layers.back());
    }
    if (!fill)
      return false;
    ReplaceBindingMaterial(
        fill->material,
        reset ? TextMaterial{SolidTextMaterial{}}
              : std::get<TextMaterial>(assignment.value));
    return true;
  }
  case TextPropertyId::GlyphFillColor:
  case TextPropertyId::GlyphFillOpacity:
  case TextPropertyId::GlyphFillGradientStartColor:
  case TextPropertyId::GlyphFillGradientEndColor:
  case TextPropertyId::GlyphFillGradientStartOffset:
  case TextPropertyId::GlyphFillGradientEndOffset:
  case TextPropertyId::GlyphFillGradientAngle:
  case TextPropertyId::GlyphFillGradientCenterX:
  case TextPropertyId::GlyphFillGradientCenterY:
  case TextPropertyId::GlyphFillGradientRadius: {
    auto *fill = FindPrimaryFill(style.materials, materialLayerId);
    if (!fill)
      return false;
    auto &material = MutableBindingMaterial(fill->material);
    switch (property) {
    case TextPropertyId::GlyphFillColor:
      SetMaterialColor(material,
                       reset ? Color{1.0F, 1.0F, 1.0F, 1.0F}
                             : std::get<Color>(assignment.value));
      break;
    case TextPropertyId::GlyphFillOpacity: {
      const auto value = reset
                             ? 1.0
                             : ComposeScalar(MaterialOpacity(material),
                                             std::get<double>(assignment.value),
                                             assignment.combineMode);
      SetMaterialOpacity(material, static_cast<float>(value));
      break;
    }
    case TextPropertyId::GlyphFillGradientStartColor: {
      auto &stops = MutableGradientStops(material);
      stops.front().color =
          reset ? Color{1.0F, 1.0F, 1.0F, 1.0F}
                : std::get<Color>(assignment.value);
      break;
    }
    case TextPropertyId::GlyphFillGradientEndColor: {
      auto &stops = MutableGradientStops(material);
      stops.back().color =
          reset ? Color{1.0F, 1.0F, 1.0F, 1.0F}
                : std::get<Color>(assignment.value);
      break;
    }
    case TextPropertyId::GlyphFillGradientStartOffset: {
      auto &stops = MutableGradientStops(material);
      stops.front().offset = static_cast<float>(
          reset ? 0.0 : std::get<double>(assignment.value));
      break;
    }
    case TextPropertyId::GlyphFillGradientEndOffset: {
      auto &stops = MutableGradientStops(material);
      stops.back().offset = static_cast<float>(
          reset ? 1.0 : std::get<double>(assignment.value));
      break;
    }
    case TextPropertyId::GlyphFillGradientAngle: {
      auto &gradient = EnsureLinearGradient(material);
      const double current =
          std::atan2(static_cast<double>(gradient.endY - gradient.startY),
                     static_cast<double>(gradient.endX - gradient.startX)) *
          180.0 / 3.14159265358979323846;
      const double degrees =
          reset ? 0.0
                : ComposeScalar(current, std::get<double>(assignment.value),
                                assignment.combineMode);
      const double radians = degrees * 3.14159265358979323846 / 180.0;
      const double centerX =
          (static_cast<double>(gradient.startX) + gradient.endX) * 0.5;
      const double centerY =
          (static_cast<double>(gradient.startY) + gradient.endY) * 0.5;
      const double halfLength =
          std::hypot(static_cast<double>(gradient.endX) - gradient.startX,
                     static_cast<double>(gradient.endY) - gradient.startY) *
          0.5;
      const double dx = std::cos(radians) * halfLength;
      const double dy = std::sin(radians) * halfLength;
      gradient.startX = static_cast<float>(centerX - dx);
      gradient.startY = static_cast<float>(centerY - dy);
      gradient.endX = static_cast<float>(centerX + dx);
      gradient.endY = static_cast<float>(centerY + dy);
      break;
    }
    case TextPropertyId::GlyphFillGradientCenterX: {
      auto &gradient = EnsureRadialGradient(material);
      gradient.centerX = static_cast<float>(
          reset ? 0.5
                : ComposeScalar(gradient.centerX,
                                std::get<double>(assignment.value),
                                assignment.combineMode));
      break;
    }
    case TextPropertyId::GlyphFillGradientCenterY: {
      auto &gradient = EnsureRadialGradient(material);
      gradient.centerY = static_cast<float>(
          reset ? 0.5
                : ComposeScalar(gradient.centerY,
                                std::get<double>(assignment.value),
                                assignment.combineMode));
      break;
    }
    case TextPropertyId::GlyphFillGradientRadius: {
      auto &gradient = EnsureRadialGradient(material);
      gradient.radius = static_cast<float>(
          reset ? 0.5
                : ComposeScalar(gradient.radius,
                                std::get<double>(assignment.value),
                                assignment.combineMode));
      break;
    }
    default:
      break;
    }
    return true;
  }
  case TextPropertyId::GlyphMaterialStack:
  case TextPropertyId::GlyphDecorativeMaterialStack:
    style.materials =
        reset ? defaults.materials
              : std::get<TextGlyphMaterialStack>(assignment.value);
    return true;
  case TextPropertyId::GlyphStrokeStack:
    ReplaceLayerKind(style.materials,
                     reset ? std::vector<TextStrokeLayer>{}
                           : std::get<std::vector<TextStrokeLayer>>(
                                 assignment.value));
    return true;
  case TextPropertyId::GlyphStrokeColor:
  case TextPropertyId::GlyphStrokeWidth:
  case TextPropertyId::GlyphStrokeOpacity: {
    auto *stroke =
        FindTypedLayer<TextStrokeLayer>(style.materials, materialLayerId);
    if (!stroke)
      return false;
    if (property == TextPropertyId::GlyphStrokeColor) {
      SetMaterialColor(MutableBindingMaterial(stroke->material),
                       reset ? Color{1.0F, 1.0F, 1.0F, 1.0F}
                             : std::get<Color>(assignment.value));
    } else if (property == TextPropertyId::GlyphStrokeOpacity) {
      auto &material = MutableBindingMaterial(stroke->material);
      const auto opacity =
          reset ? 1.0
                : ComposeScalar(MaterialOpacity(material),
                                std::get<double>(assignment.value),
                                assignment.combineMode);
      SetMaterialOpacity(material, static_cast<float>(opacity));
    } else {
      stroke->width = static_cast<float>(
          reset ? 0.0
                : ComposeScalar(stroke->width,
                                std::get<double>(assignment.value),
                                assignment.combineMode));
    }
    return true;
  }
  case TextPropertyId::GlyphShadowStack:
    ReplaceLayerKind(style.materials,
                     reset ? std::vector<TextShadowLayer>{}
                           : std::get<std::vector<TextShadowLayer>>(
                                 assignment.value));
    return true;
  case TextPropertyId::GlyphShadowColor:
  case TextPropertyId::GlyphShadowOpacity:
  case TextPropertyId::GlyphShadowOffsetX:
  case TextPropertyId::GlyphShadowOffsetY:
  case TextPropertyId::GlyphShadowBlur:
  case TextPropertyId::GlyphShadowSpread:
  case TextPropertyId::GlyphShadowDistance:
  case TextPropertyId::GlyphShadowAngle: {
    auto *shadow =
        FindTypedLayer<TextShadowLayer>(style.materials, materialLayerId);
    if (!shadow)
      return false;
    if (property == TextPropertyId::GlyphShadowColor) {
      SetMaterialColor(MutableBindingMaterial(shadow->material),
                       reset ? Color{0.0F, 0.0F, 0.0F, 1.0F}
                             : std::get<Color>(assignment.value));
      return true;
    }
    if (property == TextPropertyId::GlyphShadowOpacity) {
      auto &material = MutableBindingMaterial(shadow->material);
      const auto opacity =
          reset ? 1.0
                : ComposeScalar(MaterialOpacity(material),
                                std::get<double>(assignment.value),
                                assignment.combineMode);
      SetMaterialOpacity(material, static_cast<float>(opacity));
      return true;
    }
    const auto value = [&](const float current, const float fallback) {
      return static_cast<float>(
          reset ? fallback
                : ComposeScalar(current, std::get<double>(assignment.value),
                                assignment.combineMode));
    };
    if (property == TextPropertyId::GlyphShadowOffsetX) {
      shadow->offsetX = value(shadow->offsetX, 0.0F);
      shadow->normalizedPolarOffset.reset();
      shadow->normalizedUvOffset.reset();
    } else if (property == TextPropertyId::GlyphShadowOffsetY) {
      shadow->offsetY = value(shadow->offsetY, 0.0F);
      shadow->normalizedPolarOffset.reset();
      shadow->normalizedUvOffset.reset();
    } else if (property == TextPropertyId::GlyphShadowBlur)
      shadow->blurRadius = value(shadow->blurRadius, 0.0F);
    else if (property == TextPropertyId::GlyphShadowSpread)
      shadow->spread = value(shadow->spread, 0.0F);
    else if (property == TextPropertyId::GlyphShadowDistance)
      shadow->thicknessDistance = value(shadow->thicknessDistance, 0.0F);
    else
      shadow->thicknessAngleDegrees =
          value(shadow->thicknessAngleDegrees, 45.0F);
    return true;
  }
  case TextPropertyId::GlyphGlowStack:
    ReplaceLayerKind(style.materials,
                     reset ? std::vector<TextGlowLayer>{}
                           : std::get<std::vector<TextGlowLayer>>(
                                 assignment.value));
    return true;
  case TextPropertyId::GlyphGlowColor:
  case TextPropertyId::GlyphGlowOpacity:
  case TextPropertyId::GlyphGlowRadius:
  case TextPropertyId::GlyphGlowSpread:
  case TextPropertyId::GlyphGlowDirectionX:
  case TextPropertyId::GlyphGlowDirectionY: {
    auto *glow = FindTypedLayer<TextGlowLayer>(style.materials, materialLayerId);
    if (!glow)
      return false;
    if (property == TextPropertyId::GlyphGlowColor) {
      SetMaterialColor(MutableBindingMaterial(glow->material),
                       reset ? Color{1.0F, 1.0F, 1.0F, 1.0F}
                             : std::get<Color>(assignment.value));
      return true;
    }
    if (property == TextPropertyId::GlyphGlowOpacity) {
      auto &material = MutableBindingMaterial(glow->material);
      const auto opacity =
          reset ? 1.0
                : ComposeScalar(MaterialOpacity(material),
                                std::get<double>(assignment.value),
                                assignment.combineMode);
      SetMaterialOpacity(material, static_cast<float>(opacity));
      return true;
    }
    const auto value = [&](const float current, const float fallback) {
      return static_cast<float>(
          reset ? fallback
                : ComposeScalar(current, std::get<double>(assignment.value),
                                assignment.combineMode));
    };
    if (property == TextPropertyId::GlyphGlowRadius)
      glow->radius = value(glow->radius, 18.0F);
    else if (property == TextPropertyId::GlyphGlowSpread)
      glow->spread = value(glow->spread, 2.0F);
    else if (property == TextPropertyId::GlyphGlowDirectionX)
      glow->directionX = value(glow->directionX, 0.0F);
    else
      glow->directionY = value(glow->directionY, 0.0F);
    if (property == TextPropertyId::GlyphGlowRadius ||
        property == TextPropertyId::GlyphGlowDirectionX ||
        property == TextPropertyId::GlyphGlowDirectionY)
      glow->normalizedPolarOffset.reset();
    return true;
  }
  case TextPropertyId::RunBackground:
    style.background =
        reset ? defaults.background
              : std::get<TextBoxBackground>(assignment.value);
    return true;
  case TextPropertyId::SemanticMaterialChannel: {
    auto *binding = FindMaterialBinding(style.materials, materialLayerId);
    if (!binding)
      return false;
    ReplaceBindingMaterial(
        *binding, reset ? TextMaterial{SolidTextMaterial{}}
                        : std::get<TextMaterial>(assignment.value));
    return true;
  }
  default:
    return false;
  }
}

bool ApplyParagraphProperty(ParagraphStyle &style,
                            const TextPropertyAssignment &assignment) {
  const bool reset = assignment.disposition != TextPropertyDisposition::Set;
  const ParagraphStyle defaults{};
  switch (assignment.address.property) {
  case TextPropertyId::ParagraphBackground:
    style.background =
        reset ? defaults.background
              : std::get<TextBoxBackground>(assignment.value);
    return true;
  case TextPropertyId::ParagraphAlignment:
    style.alignment =
        reset ? defaults.alignment
              : static_cast<TextAlignment>(
                    std::get<std::int64_t>(assignment.value));
    return true;
  case TextPropertyId::ParagraphDirection:
    style.direction =
        reset ? defaults.direction
              : static_cast<TextDirection>(
                    std::get<std::int64_t>(assignment.value));
    return true;
  case TextPropertyId::ParagraphLocale:
    style.locale = reset ? defaults.locale
                         : std::get<std::string>(assignment.value);
    return true;
  case TextPropertyId::ParagraphLineHeight:
    style.lineHeight = static_cast<float>(
        reset ? defaults.lineHeight
              : ComposeScalar(style.lineHeight,
                              std::get<double>(assignment.value),
                              assignment.combineMode));
    return true;
  case TextPropertyId::ParagraphMaximumLines: {
    if (reset || std::get<std::int64_t>(assignment.value) == 0) {
      style.maximumLines.reset();
    } else {
      style.maximumLines = static_cast<std::uint32_t>(
          std::get<std::int64_t>(assignment.value));
    }
    return true;
  }
  case TextPropertyId::ParagraphOverflow:
    style.overflow =
        reset ? defaults.overflow
              : static_cast<TextOverflow>(
                    std::get<std::int64_t>(assignment.value));
    return true;
  case TextPropertyId::ParagraphWrap:
    style.wrap = reset ? defaults.wrap
                       : static_cast<TextWrap>(
                             std::get<std::int64_t>(assignment.value));
    return true;
  case TextPropertyId::ParagraphLineBreakPolicy:
    style.lineBreakPolicy =
        reset ? defaults.lineBreakPolicy
              : static_cast<TextLineBreakPolicy>(
                    std::get<std::int64_t>(assignment.value));
    return true;
  case TextPropertyId::ParagraphHyphenation:
    style.hyphenation =
        reset ? defaults.hyphenation
              : static_cast<TextHyphenation>(
                    std::get<std::int64_t>(assignment.value));
    return true;
  case TextPropertyId::ParagraphFirstLineIndent:
    style.firstLineIndent = static_cast<float>(
        reset ? defaults.firstLineIndent
              : ComposeScalar(style.firstLineIndent,
                              std::get<double>(assignment.value),
                              assignment.combineMode));
    return true;
  case TextPropertyId::ParagraphStartIndent:
    style.startIndent = static_cast<float>(
        reset ? defaults.startIndent
              : ComposeScalar(style.startIndent,
                              std::get<double>(assignment.value),
                              assignment.combineMode));
    return true;
  case TextPropertyId::ParagraphEndIndent:
    style.endIndent = static_cast<float>(
        reset ? defaults.endIndent
              : ComposeScalar(style.endIndent,
                              std::get<double>(assignment.value),
                              assignment.combineMode));
    return true;
  case TextPropertyId::ParagraphSpacingBefore:
    style.spacingBefore = static_cast<float>(
        reset ? defaults.spacingBefore
              : ComposeScalar(style.spacingBefore,
                              std::get<double>(assignment.value),
                              assignment.combineMode));
    return true;
  case TextPropertyId::ParagraphSpacingAfter:
    style.spacingAfter = static_cast<float>(
        reset ? defaults.spacingAfter
              : ComposeScalar(style.spacingAfter,
                              std::get<double>(assignment.value),
                              assignment.combineMode));
    return true;
  case TextPropertyId::ParagraphHangingPunctuation:
    style.hangingPunctuation =
        reset ? defaults.hangingPunctuation
              : std::get<bool>(assignment.value);
    return true;
  case TextPropertyId::ParagraphTabStops:
    style.tabStops = reset ? defaults.tabStops
                           : std::get<std::vector<TextTabStop>>(assignment.value);
    return true;
  default:
    return false;
  }
}

TextBackdropChannel ChannelForProperty(const TextPropertyId property) noexcept {
  if (property == TextPropertyId::BubbleBackdrop)
    return TextBackdropChannel::Bubble;
  if (property == TextPropertyId::CustomBackdrop)
    return TextBackdropChannel::Custom;
  return TextBackdropChannel::Frame;
}

TextBackdropLayer *FindBackdropLayer(TextBackdropStack &stack,
                                     const std::string &layerId) {
  const auto found = std::find_if(
      stack.layers.begin(), stack.layers.end(),
      [&](const TextBackdropLayer &layer) { return layer.layerId == layerId; });
  return found == stack.layers.end() ? nullptr : &*found;
}

void SetBackdropColor(TextBackdropSource &source, const Color &color) {
  std::visit(
      [&](auto &value) {
        using Source = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Source, RoundedRectBackdrop>) {
          SetMaterialColor(MutableBindingMaterial(value.fill), color);
        } else {
          value.fallbackColor = color;
        }
      },
      source);
}

bool ApplyCompositionProperty(LayoutBox &layout, TextWritingMode &writingMode,
                              TextLayerAppearance &appearance,
                              const TextPropertyAssignment &assignment) {
  const bool reset = assignment.disposition != TextPropertyDisposition::Set;
  const LayoutBox layoutDefaults{};
  const TextLayerAppearance appearanceDefaults{};
  switch (assignment.address.property) {
  case TextPropertyId::LayoutFrame:
    layout = reset ? layoutDefaults : std::get<LayoutBox>(assignment.value);
    return true;
  case TextPropertyId::LayoutSizingMode:
    layout.sizingMode =
        reset ? layoutDefaults.sizingMode
              : static_cast<TextLayoutSizingMode>(
                    std::get<std::int64_t>(assignment.value));
    return true;
  case TextPropertyId::LayoutVerticalAlignment:
    layout.verticalAlignment =
        reset ? layoutDefaults.verticalAlignment
              : static_cast<VerticalAlignment>(
                    std::get<std::int64_t>(assignment.value));
    return true;
  case TextPropertyId::LayoutPadding:
    layout.padding = reset ? layoutDefaults.padding
                           : std::get<Insets>(assignment.value);
    return true;
  case TextPropertyId::LayoutPixelSnap:
    layout.pixelSnap = reset ? layoutDefaults.pixelSnap
                             : std::get<bool>(assignment.value);
    return true;
  case TextPropertyId::WritingMode:
    writingMode = reset ? TextWritingMode::Horizontal
                        : std::get<TextWritingMode>(assignment.value);
    return true;
  case TextPropertyId::BackdropStack:
    appearance.backdrops =
        reset ? appearanceDefaults.backdrops
              : std::get<TextBackdropStack>(assignment.value);
    return true;
  case TextPropertyId::BackdropMaterial:
  case TextPropertyId::BackdropEnabled:
  case TextPropertyId::BackdropFillColor:
  case TextPropertyId::BackdropOpacity:
  case TextPropertyId::BackdropPadding:
  case TextPropertyId::BackdropCornerRadius:
  case TextPropertyId::BackdropWidth:
  case TextPropertyId::BackdropHeight:
  case TextPropertyId::BackdropOffsetX:
  case TextPropertyId::BackdropOffsetY:
  case TextPropertyId::BackdropScaleX:
  case TextPropertyId::BackdropScaleY:
  case TextPropertyId::BackdropRotation: {
    auto *layer = FindBackdropLayer(appearance.backdrops,
                                   assignment.address.target.layerId);
    if (!layer)
      return false;
    if (assignment.address.property == TextPropertyId::BackdropEnabled) {
      layer->enabled = !reset && std::get<bool>(assignment.value);
      return true;
    }
    if (assignment.address.property == TextPropertyId::BackdropMaterial) {
      if (auto *rounded = std::get_if<RoundedRectBackdrop>(&layer->source)) {
        if (reset)
          rounded->fill = RoundedRectBackdrop{}.fill;
        else
          ReplaceBindingMaterial(rounded->fill,
                                 std::get<TextMaterial>(assignment.value));
      } else if (reset) {
        layer->materialOverride.reset();
      } else if (layer->materialOverride) {
        ReplaceBindingMaterial(*layer->materialOverride,
                               std::get<TextMaterial>(assignment.value));
      } else {
        layer->materialOverride = LiteralTextMaterial{
            std::get<TextMaterial>(assignment.value)};
      }
      return true;
    }
    if (assignment.address.property == TextPropertyId::BackdropFillColor) {
      const auto color = reset ? Color{0.0F, 0.0F, 0.0F, 0.65F}
                               : std::get<Color>(assignment.value);
      if (std::holds_alternative<RoundedRectBackdrop>(layer->source)) {
        SetBackdropColor(layer->source, color);
      } else if (reset) {
        layer->materialOverride.reset();
      } else if (layer->materialOverride) {
        SetMaterialColor(MutableBindingMaterial(*layer->materialOverride),
                         color);
      } else {
        layer->materialOverride = LiteralTextMaterial{
            TextMaterial{SolidTextMaterial{color}}};
      }
      return true;
    }
    if (assignment.address.property == TextPropertyId::BackdropPadding) {
      layer->padding = reset ? Insets{24.0F, 12.0F, 24.0F, 12.0F}
                             : std::get<Insets>(assignment.value);
      return true;
    }
    if (assignment.address.property == TextPropertyId::BackdropCornerRadius ||
        assignment.address.property == TextPropertyId::BackdropWidth ||
        assignment.address.property == TextPropertyId::BackdropHeight) {
      auto *rounded = std::get_if<RoundedRectBackdrop>(&layer->source);
      if (!rounded)
        return false;
      if (assignment.address.property ==
          TextPropertyId::BackdropCornerRadius) {
        rounded->cornerRadius = static_cast<float>(
            reset ? 12.0
                  : ComposeScalar(rounded->cornerRadius,
                                  std::get<double>(assignment.value),
                                  assignment.combineMode));
      } else if (assignment.address.property == TextPropertyId::BackdropWidth) {
        if (reset) {
          rounded->authoredWidth.reset();
        } else {
          rounded->authoredWidth = static_cast<float>(ComposeScalar(
              rounded->authoredWidth.value_or(0.0F),
              std::get<double>(assignment.value), assignment.combineMode));
        }
      } else if (reset) {
        rounded->authoredHeight.reset();
      } else {
        rounded->authoredHeight = static_cast<float>(ComposeScalar(
            rounded->authoredHeight.value_or(0.0F),
            std::get<double>(assignment.value), assignment.combineMode));
      }
      return true;
    }
    const auto scalar = reset ? 0.0 : std::get<double>(assignment.value);
    if (assignment.address.property == TextPropertyId::BackdropOpacity) {
      layer->transform.opacity = static_cast<float>(
          reset ? 1.0
                : ComposeScalar(layer->transform.opacity, scalar,
                                assignment.combineMode));
    } else if (assignment.address.property == TextPropertyId::BackdropOffsetX) {
      layer->transform.offsetX = static_cast<float>(
          reset ? 0.0
                : ComposeScalar(layer->transform.offsetX, scalar,
                                assignment.combineMode));
    } else if (assignment.address.property == TextPropertyId::BackdropOffsetY) {
      layer->transform.offsetY = static_cast<float>(
          reset ? 0.0
                : ComposeScalar(layer->transform.offsetY, scalar,
                                assignment.combineMode));
    } else if (assignment.address.property == TextPropertyId::BackdropScaleX) {
      layer->transform.scaleX = static_cast<float>(
          reset ? 1.0
                : ComposeScalar(layer->transform.scaleX, scalar,
                                assignment.combineMode));
    } else if (assignment.address.property == TextPropertyId::BackdropScaleY) {
      layer->transform.scaleY = static_cast<float>(
          reset ? 1.0
                : ComposeScalar(layer->transform.scaleY, scalar,
                                assignment.combineMode));
    } else {
      layer->transform.rotationDegrees = static_cast<float>(
          reset ? 0.0
                : ComposeScalar(layer->transform.rotationDegrees, scalar,
                                assignment.combineMode));
    }
    return true;
  }
  case TextPropertyId::FrameBackdrop:
  case TextPropertyId::BubbleBackdrop:
  case TextPropertyId::CustomBackdrop: {
    const auto channel = ChannelForProperty(assignment.address.property);
    const auto layerId = assignment.address.target.layerId;
    auto matches = [&](const TextBackdropLayer &layer) {
      return !layerId.empty() ? layer.layerId == layerId
                              : layer.channel == channel;
    };
    const auto found = std::find_if(appearance.backdrops.layers.begin(),
                                    appearance.backdrops.layers.end(), matches);
    if (reset) {
      if (found != appearance.backdrops.layers.end())
        appearance.backdrops.layers.erase(found);
      return true;
    }
    auto replacement = std::get<TextBackdropLayer>(assignment.value);
    replacement.channel = channel;
    if (!layerId.empty())
      replacement.layerId = layerId;
    if (found == appearance.backdrops.layers.end())
      appearance.backdrops.layers.push_back(std::move(replacement));
    else
      *found = std::move(replacement);
    return true;
  }
  case TextPropertyId::Bend:
    appearance.bend = reset ? appearanceDefaults.bend
                            : std::get<TextBend>(assignment.value);
    return true;
  case TextPropertyId::Path:
    appearance.path = reset ? appearanceDefaults.path
                            : std::get<TextPath>(assignment.value);
    return true;
  case TextPropertyId::SdfMaterial:
    appearance.sdfMaterial =
        reset ? appearanceDefaults.sdfMaterial
              : std::get<TextSdfMaterial>(assignment.value);
    return true;
  case TextPropertyId::GlobalAlpha:
    appearance.globalAlpha = static_cast<float>(
        reset ? appearanceDefaults.globalAlpha
              : ComposeScalar(appearance.globalAlpha,
                              std::get<double>(assignment.value),
                              assignment.combineMode));
    return true;
  default:
    return false;
  }
}

bool IsRunProperty(const TextPropertyId property) noexcept {
  return property <= TextPropertyId::RunBackground ||
         property == TextPropertyId::GlyphDecorativeMaterialStack ||
         property == TextPropertyId::SemanticMaterialChannel;
}

bool IsParagraphProperty(const TextPropertyId property) noexcept {
  return property >= TextPropertyId::ParagraphBackground &&
         property <= TextPropertyId::ParagraphTabStops;
}

} // namespace

bool TextPropertyRangeMatches(const std::string_view value,
                             const TextUtf8Range &range) noexcept {
  return range.begin < range.end && range.end <= value.size() &&
         IsUtf8Boundary(value, static_cast<std::size_t>(range.begin)) &&
         IsUtf8Boundary(value, static_cast<std::size_t>(range.end));
}

std::unordered_set<std::string>
ResolveTextPropertyParagraphIds(const TextPropertyTarget &target,
                    const std::vector<TextContentSlot> &slots,
                    const std::vector<RichTextParagraph> &paragraphs) {
  std::unordered_set<std::string> result;
  if (target.scope == TextPropertyScope::Composition) {
    for (const auto &paragraph : paragraphs)
      result.insert(paragraph.paragraphId);
  } else if (target.scope == TextPropertyScope::ContentSlot) {
    const auto slot = std::find_if(
        slots.begin(), slots.end(), [&](const TextContentSlot &candidate) {
          return candidate.slotId == target.contentSlotId;
        });
    if (slot != slots.end())
      result.insert(slot->paragraphIds.begin(), slot->paragraphIds.end());
  } else {
    result.insert(target.paragraphIds.begin(), target.paragraphIds.end());
  }
  return result;
}

std::unordered_set<std::string>
ResolveTextPropertyRunIds(const TextPropertyTarget &target,
              const std::vector<TextContentSlot> &slots,
              const std::vector<RichTextParagraph> &paragraphs) {
  std::unordered_set<std::string> result;
  if (target.scope == TextPropertyScope::Composition) {
    for (const auto &paragraph : paragraphs)
      for (const auto &run : paragraph.runs)
        result.insert(run.runId);
  } else if (target.scope == TextPropertyScope::ContentSlot) {
    const auto slot = std::find_if(
        slots.begin(), slots.end(), [&](const TextContentSlot &candidate) {
          return candidate.slotId == target.contentSlotId;
        });
    if (slot != slots.end())
      result.insert(slot->runIds.begin(), slot->runIds.end());
  } else if (target.scope == TextPropertyScope::Paragraph) {
    const std::unordered_set<std::string> paragraphIds(
        target.paragraphIds.begin(), target.paragraphIds.end());
    for (const auto &paragraph : paragraphs) {
      if (paragraphIds.find(paragraph.paragraphId) == paragraphIds.end())
        continue;
      for (const auto &run : paragraph.runs)
        result.insert(run.runId);
    }
  } else if (target.scope == TextPropertyScope::GlyphMaterialLayer) {
    if (!target.runIds.empty()) {
      result.insert(target.runIds.begin(), target.runIds.end());
    } else if (!target.paragraphIds.empty()) {
      const std::unordered_set<std::string> paragraphIds(
          target.paragraphIds.begin(), target.paragraphIds.end());
      for (const auto &paragraph : paragraphs) {
        if (paragraphIds.find(paragraph.paragraphId) == paragraphIds.end())
          continue;
        for (const auto &run : paragraph.runs)
          result.insert(run.runId);
      }
    } else if (!target.contentSlotId.empty()) {
      const auto slot = std::find_if(
          slots.begin(), slots.end(), [&](const TextContentSlot &candidate) {
            return candidate.slotId == target.contentSlotId;
          });
      if (slot != slots.end())
        result.insert(slot->runIds.begin(), slot->runIds.end());
    } else {
      for (const auto &paragraph : paragraphs)
        for (const auto &run : paragraph.runs)
          result.insert(run.runId);
    }
  } else if (target.runIds.empty()) {
    for (const auto &paragraph : paragraphs)
      for (const auto &run : paragraph.runs)
        result.insert(run.runId);
  } else {
    result.insert(target.runIds.begin(), target.runIds.end());
  }
  return result;
}

namespace {

std::string UniqueSplitId(const std::string &patchId,
                          const std::string &runId, const char *side,
                          const std::unordered_set<std::string> &existing) {
  std::string base = patchId + ":" + runId + ":" + side;
  if (existing.find(base) == existing.end())
    return base;
  for (std::uint64_t suffix = 1U;; ++suffix) {
    auto candidate = base + ":" + std::to_string(suffix);
    if (existing.find(candidate) == existing.end())
      return candidate;
  }
}

bool SplitRangeTarget(std::vector<TextContentSlot> &slots,
                      std::vector<RichTextParagraph> &paragraphs,
                      const std::string &patchId,
                      const TextPropertyAssignment &assignment,
                      std::unordered_set<std::string> &allRunIds,
                      TextPropertyPatchResult &result) {
  const auto &target = assignment.address.target;
  if (!target.range)
    return true;
  if (target.runIds.size() != 1U) {
    AddDiagnostic(result, "text.property.range_invalid", patchId,
                  "UTF-8 range target requires exactly one run identity");
    return false;
  }
  const auto &runId = target.runIds.front();
  const auto begin = static_cast<std::size_t>(target.range->begin);
  const auto end = static_cast<std::size_t>(target.range->end);
  for (auto &paragraph : paragraphs) {
    const auto found = std::find_if(
        paragraph.runs.begin(), paragraph.runs.end(),
        [&](const RichTextRun &run) { return run.runId == runId; });
    if (found == paragraph.runs.end())
      continue;
    if (!TextPropertyRangeMatches(found->utf8Text, *target.range)) {
      AddDiagnostic(result, "text.property.range_invalid", runId,
                    "UTF-8 range does not match the accepted run text");
      return false;
    }
    if (begin == 0U && end == found->utf8Text.size())
      return true;

    RichTextRun original = *found;
    std::vector<RichTextRun> replacement;
    std::vector<std::string> replacementIds;
    if (begin > 0U) {
      auto prefix = original;
      prefix.runId =
          UniqueSplitId(patchId, original.runId, "before", allRunIds);
      prefix.utf8Text = original.utf8Text.substr(0U, begin);
      allRunIds.insert(prefix.runId);
      replacementIds.push_back(prefix.runId);
      replacement.push_back(std::move(prefix));
    }
    auto selected = original;
    selected.utf8Text = original.utf8Text.substr(begin, end - begin);
    replacementIds.push_back(selected.runId);
    replacement.push_back(std::move(selected));
    if (end < original.utf8Text.size()) {
      auto suffix = original;
      suffix.runId =
          UniqueSplitId(patchId, original.runId, "after", allRunIds);
      suffix.utf8Text = original.utf8Text.substr(end);
      allRunIds.insert(suffix.runId);
      replacementIds.push_back(suffix.runId);
      replacement.push_back(std::move(suffix));
    }
    const auto index = static_cast<std::size_t>(
        std::distance(paragraph.runs.begin(), found));
    paragraph.runs.erase(paragraph.runs.begin() +
                         static_cast<std::ptrdiff_t>(index));
    paragraph.runs.insert(paragraph.runs.begin() +
                              static_cast<std::ptrdiff_t>(index),
                          replacement.begin(), replacement.end());
    for (auto &slot : slots) {
      const auto runPosition =
          std::find(slot.runIds.begin(), slot.runIds.end(), runId);
      if (runPosition == slot.runIds.end())
        continue;
      const auto slotIndex = static_cast<std::size_t>(
          std::distance(slot.runIds.begin(), runPosition));
      slot.runIds.erase(slot.runIds.begin() +
                        static_cast<std::ptrdiff_t>(slotIndex));
      slot.runIds.insert(slot.runIds.begin() +
                             static_cast<std::ptrdiff_t>(slotIndex),
                         replacementIds.begin(), replacementIds.end());
    }
    result.changedParagraphIds.push_back(paragraph.paragraphId);
    result.changedRunIds.insert(result.changedRunIds.end(),
                                replacementIds.begin(), replacementIds.end());
    return true;
  }
  AddDiagnostic(result, "text.property.target_missing", runId,
                "UTF-8 range references a missing run");
  return false;
}

class PropertyValueValidationContext final {
public:
  ResolvedRichTextView &prepare(RichTextRun &&run) {
    if (view_) {
      auto &paragraph = view_->paragraphs.front();
      paragraph.runs.front() = std::move(run);
      paragraph.style = {};
      view_->layoutBox = {};
      view_->writingMode = ResolvedRichTextView{}.writingMode;
    } else {
      RichTextParagraph paragraph;
      paragraph.paragraphId = "validation.paragraph";
      paragraph.runs.push_back(std::move(run));
      TextContentSlot slot;
      slot.slotId = "validation.slot";
      slot.semanticRole = "content.primary";
      slot.paragraphIds.push_back(paragraph.paragraphId);
      slot.runIds.push_back(paragraph.runs.front().runId);
      view_.emplace();
      view_->slots.push_back(std::move(slot));
      view_->paragraphs.push_back(std::move(paragraph));
    }
    return *view_;
  }

private:
  // One patch owns the scratch view. Only its immutable identity containers
  // survive between assignments; every writable field starts at schema defaults.
  std::optional<ResolvedRichTextView> view_;
};

bool ValidateValueAgainstCurrentSchema(
    const TextPropertyAssignment &assignment, const RichTextLimits &limits,
    std::string &reason, PropertyValueValidationContext &context) {
  if (assignment.disposition != TextPropertyDisposition::Set)
    return true;

  FontReference font;
  font.kind = FontSourceKind::Builtin;
  font.family = "VideoCut Validation";
  font.assetId = "builtin.font.validation";
  RichTextRun run;
  run.runId = "validation.run";
  run.utf8Text = "Text";
  run.style.font.primary = font;
  run.style.font.family = font.family;
  run.style.font.weight = font.weight;
  run.style.font.width = font.width;
  run.style.font.slant = font.slant;
  const auto property = assignment.address.property;
  const bool targetsFillLayer =
      (property >= TextPropertyId::GlyphFill &&
       property <= TextPropertyId::GlyphFillGradientRadius) ||
      property == TextPropertyId::SemanticMaterialChannel;
  if (targetsFillLayer && !assignment.address.target.layerId.empty()) {
    auto &fill = std::get<TextFillLayer>(run.style.materials.layers.front());
    fill.layerId = assignment.address.target.layerId;
    EditableTextStyleSlot slot;
    slot.semanticRole = assignment.address.target.layerId;
    fill.material = std::move(slot);
  }
  if (property == TextPropertyId::GlyphStrokeColor ||
      property == TextPropertyId::GlyphStrokeWidth ||
      property == TextPropertyId::GlyphStrokeOpacity) {
    TextStrokeLayer layer;
    layer.layerId = assignment.address.target.layerId;
    run.style.materials.layers.emplace_back(std::move(layer));
  } else if (property == TextPropertyId::GlyphShadowColor ||
             property == TextPropertyId::GlyphShadowOpacity ||
             property == TextPropertyId::GlyphShadowOffsetX ||
             property == TextPropertyId::GlyphShadowOffsetY ||
             property == TextPropertyId::GlyphShadowBlur ||
             property == TextPropertyId::GlyphShadowSpread ||
             property == TextPropertyId::GlyphShadowDistance ||
             property == TextPropertyId::GlyphShadowAngle) {
    TextShadowLayer layer;
    layer.layerId = assignment.address.target.layerId;
    run.style.materials.layers.emplace_back(std::move(layer));
  } else if (property == TextPropertyId::GlyphGlowColor ||
             property == TextPropertyId::GlyphGlowOpacity ||
             property == TextPropertyId::GlyphGlowRadius ||
             property == TextPropertyId::GlyphGlowSpread ||
             property == TextPropertyId::GlyphGlowDirectionX ||
             property == TextPropertyId::GlyphGlowDirectionY) {
    TextGlowLayer layer;
    layer.layerId = assignment.address.target.layerId;
    run.style.materials.layers.emplace_back(std::move(layer));
  }
  auto &view = context.prepare(std::move(run));
  TextLayerAppearance appearance;
  if ((property >= TextPropertyId::BackdropMaterial &&
       property <= TextPropertyId::BackdropRotation) ||
      property == TextPropertyId::BackdropEnabled) {
    TextBackdropLayer layer;
    layer.layerId = assignment.address.target.layerId;
    layer.enabled = true;
    appearance.backdrops.layers.push_back(std::move(layer));
  }

  bool applied = false;
  if (IsRunProperty(assignment.address.property)) {
    applied = ApplyRunProperty(view.paragraphs.front().runs.front().style,
                               assignment,
                               assignment.address.target.layerId);
  } else if (IsParagraphProperty(assignment.address.property)) {
    applied = ApplyParagraphProperty(view.paragraphs.front().style, assignment);
  } else {
    applied = ApplyCompositionProperty(view.layoutBox, view.writingMode,
                                       appearance, assignment);
  }
  if (!applied) {
    reason = "property value cannot be materialized by the current schema";
    return false;
  }
  const auto validation = ValidateResolvedRichTextView(view, limits);
  if (!validation.valid) {
    reason = validation.diagnostics.empty()
                 ? "property value violates the current text schema"
                 : validation.diagnostics.front().message;
    return false;
  }
  std::string appearanceError;
  if (!ValidateTextLayerAppearance(appearance, &appearanceError)) {
    reason = std::move(appearanceError);
    return false;
  }
  return true;
}

bool ValidateAssignmentValue(const TextPropertyAssignment &assignment,
                             const TextPropertyDescriptor &descriptor,
                             const RichTextLimits &limits,
                             std::string &reason,
                             PropertyValueValidationContext &context) {
  if (assignment.disposition != TextPropertyDisposition::Set) {
    if (!std::holds_alternative<std::monostate>(assignment.value)) {
      reason = "clear and inherit assignments must not carry a value";
      return false;
    }
    if (assignment.combineMode != TextPropertyCombineMode::Replace) {
      reason = "clear and inherit assignments use replace composition";
      return false;
    }
    if (assignment.address.property == TextPropertyId::FontReference) {
      reason = "the required portable font identity cannot be cleared";
      return false;
    }
    return true;
  }
  if (!ValueKindMatches(assignment.address.property, descriptor.valueKind,
                        assignment.value)) {
    reason = "property value does not match its descriptor kind";
    return false;
  }
  if (const auto *scalar = std::get_if<double>(&assignment.value);
      scalar && !ValidScalar(assignment.address.property, *scalar, limits)) {
    reason = "scalar property value is outside its current-schema range";
    return false;
  }
  if (const auto *integer = std::get_if<std::int64_t>(&assignment.value)) {
    if (!ValidInteger(assignment.address.property, *integer) ||
        !ValidEnumeration(assignment.address.property, *integer)) {
      reason = "integer or enumeration property value is invalid";
      return false;
    }
  }
  if (const auto *string = std::get_if<std::string>(&assignment.value);
      string && (string->size() > 4096U || !IsValidUtf8(*string))) {
    reason = "string property value is not bounded valid UTF-8";
    return false;
  }
  if (const auto *color = std::get_if<Color>(&assignment.value);
      color && !ValidColor(*color)) {
    reason = "color property value is invalid";
    return false;
  }
  if ((assignment.address.property == TextPropertyId::UnderlineMaterial ||
       assignment.address.property ==
           TextPropertyId::StrikeThroughMaterial) &&
      !std::holds_alternative<SolidTextMaterial>(
          std::get<TextMaterial>(assignment.value))) {
    reason = "current inline-decoration storage accepts only a solid material";
    return false;
  }
  return ValidateValueAgainstCurrentSchema(assignment, limits, reason, context);
}

void Deduplicate(std::vector<std::string> &values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

} // namespace

const TextPropertyDescriptor *
DescribeTextProperty(const TextPropertyId property) noexcept {
  const auto found = std::find_if(
      std::begin(kDescriptors), std::end(kDescriptors),
      [property](const TextPropertyDescriptor &descriptor) {
        return descriptor.property == property;
      });
  return found == std::end(kDescriptors) ? nullptr : found;
}

std::optional<TextPropertyId>
ParseTextPropertyId(const std::string_view name) noexcept {
  const auto found = std::find_if(
      std::begin(kDescriptors), std::end(kDescriptors),
      [name](const TextPropertyDescriptor &descriptor) {
        return descriptor.name != nullptr && name == descriptor.name;
      });
  return found == std::end(kDescriptors)
             ? std::nullopt
             : std::optional<TextPropertyId>{found->property};
}

TextPropertyPatchResult
ValidateTextPropertyPatch(const TextPropertyPatch &patch,
                          const RichTextLimits &limits) {
  TextPropertyPatchResult result;
  if (patch.patchId.empty() || patch.patchId.size() > 512U ||
      !IsValidUtf8(patch.patchId)) {
    AddDiagnostic(result, "text.property.patch_id_invalid", patch.patchId,
                  "property patch identity must be bounded valid UTF-8");
  }
  if (patch.assignments.empty() || patch.assignments.size() > 4096U) {
    AddDiagnostic(result, "text.property.assignment_limit", patch.patchId,
                  "property patch assignment count is outside the limit");
  }

  std::unordered_map<std::string, TextUtf8Range> rangesByRun;
  PropertyValueValidationContext valueContext;
  for (const auto &assignment : patch.assignments) {
    const auto *descriptor = DescribeTextProperty(assignment.address.property);
    if (!descriptor) {
      AddDiagnostic(result, "text.property.unknown", patch.patchId,
                    "property ID has no current-schema descriptor");
      continue;
    }
    const auto scopeBit = TextPropertyScopeBit(assignment.address.target.scope);
    if ((descriptor->legalScopeMask & scopeBit) == 0U) {
      AddDiagnostic(result, "text.property.scope_invalid", descriptor->name,
                    "property target scope is not legal for this property");
    }
    const auto combineBit =
        TextPropertyCombineModeBit(assignment.combineMode);
    if ((descriptor->legalCombineModeMask & combineBit) == 0U) {
      AddDiagnostic(result, "text.property.combine_invalid", descriptor->name,
                    "property combine mode is not legal for this property");
    }
    std::string reason;
    if (!ValidTargetShape(assignment.address.target, reason)) {
      AddDiagnostic(result, "text.property.target_invalid", descriptor->name,
                    std::move(reason));
    }
    if (!ValidateAssignmentValue(assignment, *descriptor, limits, reason,
                                 valueContext)) {
      AddDiagnostic(result, "text.property.value_invalid", descriptor->name,
                    std::move(reason));
    }
    if (assignment.address.target.range &&
        assignment.address.target.runIds.size() == 1U) {
      const auto &runId = assignment.address.target.runIds.front();
      const auto range = *assignment.address.target.range;
      const auto [found, inserted] = rangesByRun.emplace(runId, range);
      if (!inserted &&
          (found->second.begin != range.begin || found->second.end != range.end)) {
        AddDiagnostic(result, "text.property.range_partition_ambiguous", runId,
                      "one atomic patch may use only one UTF-8 range per run");
      }
    }
  }
  result.valid = result.diagnostics.empty();
  return result;
}

TextPropertyPatchResult
ApplyTextPropertyPatch(TextPropertyDocumentView document,
                       const TextPropertyPatch &patch,
                       const RichTextLimits &limits) {
  auto result = ValidateTextPropertyPatch(patch, limits);
  if (!result.valid)
    return result;

  auto slots = document.contentSlots;
  auto paragraphs = document.paragraphs;
  auto layout = document.layout;
  auto writingMode = document.writingMode;
  auto appearance = document.appearance;
  std::unordered_set<std::string> allRunIds;
  for (const auto &paragraph : paragraphs)
    for (const auto &run : paragraph.runs)
      allRunIds.insert(run.runId);

  std::unordered_set<std::string> splitRanges;
  for (const auto &assignment : patch.assignments) {
    if (!assignment.address.target.range)
      continue;
    const auto &target = assignment.address.target;
    if (target.runIds.size() != 1U) {
      AddDiagnostic(result, "text.property.range_invalid", patch.patchId,
                    "UTF-8 range target requires exactly one run identity");
      result.valid = false;
      result.changed = false;
      return result;
    }
    const auto splitKey = target.runIds.front() + ":" +
                          std::to_string(target.range->begin) + ":" +
                          std::to_string(target.range->end);
    if (splitRanges.insert(splitKey).second &&
        !SplitRangeTarget(slots, paragraphs, patch.patchId, assignment,
                          allRunIds, result)) {
      result.valid = false;
      result.changed = false;
      return result;
    }
  }

  for (const auto &assignment : patch.assignments) {
    const auto property = assignment.address.property;
    bool assignmentChanged = false;
    if (IsRunProperty(property)) {
      const auto runIds = ResolveTextPropertyRunIds(assignment.address.target, slots,
                                        paragraphs);
      std::size_t appliedRuns = 0U;
      for (auto &paragraph : paragraphs) {
        for (auto &run : paragraph.runs) {
          if (runIds.find(run.runId) == runIds.end())
            continue;
          if (ApplyRunProperty(run.style, assignment,
                               assignment.address.target.layerId)) {
            ++appliedRuns;
            result.changedRunIds.push_back(run.runId);
            result.changedParagraphIds.push_back(paragraph.paragraphId);
          }
        }
      }
      assignmentChanged = !runIds.empty() && appliedRuns == runIds.size();
    } else if (IsParagraphProperty(property)) {
      const auto paragraphIds = ResolveTextPropertyParagraphIds(
          assignment.address.target, slots, paragraphs);
      std::size_t appliedParagraphs = 0U;
      for (auto &paragraph : paragraphs) {
        if (paragraphIds.find(paragraph.paragraphId) == paragraphIds.end())
          continue;
        if (ApplyParagraphProperty(paragraph.style, assignment)) {
          ++appliedParagraphs;
          result.changedParagraphIds.push_back(paragraph.paragraphId);
        }
      }
      assignmentChanged = !paragraphIds.empty() &&
                          appliedParagraphs == paragraphIds.size();
    } else {
      assignmentChanged = ApplyCompositionProperty(
          layout, writingMode, appearance, assignment);
      if (assignmentChanged && !assignment.address.target.layerId.empty())
        result.changedLayerIds.push_back(assignment.address.target.layerId);
    }
    if (!assignmentChanged) {
      AddDiagnostic(result, "text.property.target_unresolved",
                    DescribeTextProperty(property)->name,
                    "property target does not resolve to a mutable current-schema "
                    "value");
      result.valid = false;
      result.changed = false;
      return result;
    }
  }

  const ReferenceCanvas referenceCanvas;
  const auto textValidation = internal::ValidateRichText(
      {referenceCanvas, layout, writingMode, slots, paragraphs}, limits);
  if (!textValidation.valid) {
    result.diagnostics.insert(result.diagnostics.end(),
                              textValidation.diagnostics.begin(),
                              textValidation.diagnostics.end());
  }
  std::string appearanceError;
  if (!ValidateTextLayerAppearance(appearance, &appearanceError)) {
    AddDiagnostic(result, "text.property.appearance_invalid", patch.patchId,
                  std::move(appearanceError));
  }
  if (!result.diagnostics.empty()) {
    result.valid = false;
    result.changed = false;
    return result;
  }

  document.contentSlots = std::move(slots);
  document.paragraphs = std::move(paragraphs);
  document.layout = std::move(layout);
  document.writingMode = writingMode;
  document.appearance = std::move(appearance);
  Deduplicate(result.changedParagraphIds);
  Deduplicate(result.changedRunIds);
  Deduplicate(result.changedLayerIds);
  result.valid = true;
  result.changed = true;
  return result;
}

} // namespace videocut::text
