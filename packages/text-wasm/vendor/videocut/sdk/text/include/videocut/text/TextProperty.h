#pragma once

#include "videocut/text/TextLayerAppearance.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <variant>
#include <vector>

namespace videocut::text {

enum class TextPropertyScope : std::uint8_t {
  Composition = 0,
  ContentSlot,
  Paragraph,
  Run,
  Utf8Range,
  GlyphMaterialLayer,
  BackdropLayer,
};

enum class TextPropertyId : std::uint16_t {
  FontReference = 0,
  FontFamily,
  FontPostscriptName,
  FontWeight,
  FontWidth,
  FontSlant,
  FontVariationAxes,
  FontFeatures,
  FontSize,
  LetterSpacing,
  WordSpacing,
  BaselineShift,
  UnderlineEnabled,
  UnderlineStyle,
  UnderlineMaterial,
  UnderlineThickness,
  UnderlineOffset,
  UnderlineSkipInk,
  StrikeThroughEnabled,
  StrikeThroughStyle,
  StrikeThroughMaterial,
  StrikeThroughThickness,
  StrikeThroughOffset,
  GlyphFill,
  GlyphFillColor,
  GlyphFillOpacity,
  GlyphFillGradientStartColor,
  GlyphFillGradientEndColor,
  GlyphFillGradientStartOffset,
  GlyphFillGradientEndOffset,
  GlyphFillGradientAngle,
  GlyphFillGradientCenterX,
  GlyphFillGradientCenterY,
  GlyphFillGradientRadius,
  GlyphMaterialStack,
  GlyphStrokeStack,
  GlyphStrokeColor,
  GlyphStrokeWidth,
  GlyphStrokeOpacity,
  GlyphShadowStack,
  GlyphShadowColor,
  GlyphShadowOpacity,
  GlyphShadowOffsetX,
  GlyphShadowOffsetY,
  GlyphShadowBlur,
  GlyphShadowSpread,
  GlyphShadowDistance,
  GlyphShadowAngle,
  GlyphGlowStack,
  GlyphGlowColor,
  GlyphGlowOpacity,
  GlyphGlowRadius,
  GlyphGlowSpread,
  GlyphGlowDirectionX,
  GlyphGlowDirectionY,
  RunBackground,
  ParagraphBackground,
  ParagraphAlignment,
  ParagraphDirection,
  ParagraphLocale,
  ParagraphLineHeight,
  ParagraphMaximumLines,
  ParagraphOverflow,
  ParagraphWrap,
  ParagraphLineBreakPolicy,
  ParagraphHyphenation,
  ParagraphFirstLineIndent,
  ParagraphStartIndent,
  ParagraphEndIndent,
  ParagraphSpacingBefore,
  ParagraphSpacingAfter,
  ParagraphHangingPunctuation,
  ParagraphTabStops,
  LayoutFrame,
  LayoutSizingMode,
  LayoutVerticalAlignment,
  LayoutPadding,
  LayoutPixelSnap,
  WritingMode,
  BackdropStack,
  BackdropMaterial,
  BackdropFillColor,
  BackdropOpacity,
  BackdropPadding,
  BackdropCornerRadius,
  BackdropWidth,
  BackdropHeight,
  BackdropOffsetX,
  BackdropOffsetY,
  BackdropScaleX,
  BackdropScaleY,
  BackdropRotation,
  FrameBackdrop,
  BubbleBackdrop,
  CustomBackdrop,
  GlyphDecorativeMaterialStack,
  Bend,
  Path,
  SdfMaterial,
  GlobalAlpha,
  SemanticMaterialChannel,
  BackdropEnabled,
};

enum class TextPropertyValueKind : std::uint8_t {
  Boolean = 0,
  Integer,
  Scalar,
  String,
  Color,
  Font,
  FontAxes,
  FontFeatures,
  Material,
  MaterialStack,
  StrokeStack,
  ShadowStack,
  GlowStack,
  InlineDecoration,
  BoxBackground,
  ParagraphStyle,
  TabStops,
  LayoutBox,
  Insets,
  WritingMode,
  BackdropLayer,
  BackdropStack,
  Bend,
  Path,
  SdfMaterial,
  Enumeration,
};

enum class TextAffinity : std::uint8_t {
  Upstream = 0,
  Downstream,
};

struct TextUtf8Range final {
  std::uint64_t begin{0};
  std::uint64_t end{0};

  friend bool operator==(const TextUtf8Range &left,
                         const TextUtf8Range &right) noexcept {
    return left.begin == right.begin && left.end == right.end;
  }
};

struct TextPropertyTarget final {
  TextPropertyScope scope{TextPropertyScope::Composition};
  std::string contentSlotId;
  std::vector<std::string> paragraphIds;
  std::vector<std::string> runIds;
  std::optional<TextUtf8Range> range;
  std::string layerId;

  friend bool operator==(const TextPropertyTarget &left,
                         const TextPropertyTarget &right) noexcept {
    return left.scope == right.scope &&
           left.contentSlotId == right.contentSlotId &&
           left.paragraphIds == right.paragraphIds &&
           left.runIds == right.runIds && left.range == right.range &&
           left.layerId == right.layerId;
  }
};

struct TextPropertyAddress final {
  TextPropertyId property{TextPropertyId::GlyphFill};
  TextPropertyTarget target{};

  friend bool operator==(const TextPropertyAddress &left,
                         const TextPropertyAddress &right) noexcept {
    return left.property == right.property && left.target == right.target;
  }

  friend bool operator!=(const TextPropertyAddress &left,
                         const TextPropertyAddress &right) noexcept {
    return !(left == right);
  }
};

using TextPropertyValue =
    std::variant<std::monostate, bool, std::int64_t, double, std::string, Color,
                 FontReference, FontSpec, std::vector<FontAxis>,
                 std::vector<FontFeature>, TextMaterial,
                 TextGlyphMaterialStack, std::vector<TextStrokeLayer>,
                 std::vector<TextShadowLayer>, std::vector<TextGlowLayer>,
                 InlineTextDecoration, TextBoxBackground, ParagraphStyle,
                 std::vector<TextTabStop>, LayoutBox, Insets, TextWritingMode,
                 TextBackdropLayer, TextBackdropStack, TextBend, TextPath,
                 TextSdfMaterial>;

enum class TextPropertyDisposition : std::uint8_t {
  Set = 0,
  Clear,
  Inherit,
};

struct TextPropertyAssignment final {
  TextPropertyAddress address{};
  TextPropertyDisposition disposition{TextPropertyDisposition::Set};
  TextPropertyCombineMode combineMode{TextPropertyCombineMode::Replace};
  TextPropertyValue value{};
};

struct TextPropertyPatch final {
  std::string patchId;
  std::uint64_t baseRevision{0};
  std::vector<TextPropertyAssignment> assignments;
};

struct TextPropertyDescriptor final {
  TextPropertyId property{TextPropertyId::GlyphFill};
  const char *name{nullptr};
  TextPropertyValueKind valueKind{TextPropertyValueKind::Scalar};
  std::uint64_t legalScopeMask{0};
  std::uint32_t legalCombineModeMask{0};
  bool animatable{false};
  TextInvalidationImpact invalidation{TextInvalidationImpact::None};
};

struct TextPropertyPatchResult final {
  bool valid{false};
  bool changed{false};
  std::vector<std::string> changedParagraphIds;
  std::vector<std::string> changedRunIds;
  std::vector<std::string> changedLayerIds;
  std::vector<Diagnostic> diagnostics;
};

enum class TextPropertyStateKind : std::uint8_t {
  Uniform = 0,
  Mixed,
  Inherited,
  Unavailable,
};

struct TextPropertyState final {
  TextPropertyAddress address{};
  TextPropertyStateKind state{TextPropertyStateKind::Unavailable};
  TextPropertyValue value{};
};

/// Non-owning mutable projection of the sole canonical authored document.
/// The property engine never stores this view or creates a second document.
struct TextPropertyDocumentView final {
  std::vector<TextContentSlot> &contentSlots;
  std::vector<RichTextParagraph> &paragraphs;
  LayoutBox &layout;
  TextWritingMode &writingMode;
  TextLayerAppearance &appearance;
};

[[nodiscard]] constexpr std::uint64_t
TextPropertyScopeBit(const TextPropertyScope scope) noexcept {
  return std::uint64_t{1U} << static_cast<std::uint8_t>(scope);
}

[[nodiscard]] constexpr std::uint32_t
TextPropertyCombineModeBit(const TextPropertyCombineMode mode) noexcept {
  return std::uint32_t{1U} << static_cast<std::uint8_t>(mode);
}

const TextPropertyDescriptor *
DescribeTextProperty(TextPropertyId property) noexcept;

std::optional<TextPropertyId>
ParseTextPropertyId(std::string_view name) noexcept;

/// Encodes one complete property value using the sole current canonical
/// binary representation. The operation fails closed for non-finite values,
/// invalid closed enums, out-of-range fields, or values outside [limits].
bool EncodeCanonicalTextPropertyValue(
    const TextPropertyValue &value, std::vector<std::uint8_t> &output,
    const RichTextLimits &limits = {});

/// Decodes one complete current canonical property value. Trailing bytes and
/// non-canonical encodings are rejected; [output] is unchanged on failure.
bool DecodeCanonicalTextPropertyValue(
    const std::vector<std::uint8_t> &bytes, TextPropertyValue &output,
    const RichTextLimits &limits = {});

TextPropertyPatchResult
ValidateTextPropertyPatch(const TextPropertyPatch &patch,
                          const RichTextLimits &limits = {});

std::unordered_set<std::string> ResolveTextPropertyParagraphIds(
    const TextPropertyTarget &target, const std::vector<TextContentSlot> &slots,
    const std::vector<RichTextParagraph> &paragraphs);

std::unordered_set<std::string> ResolveTextPropertyRunIds(
    const TextPropertyTarget &target, const std::vector<TextContentSlot> &slots,
    const std::vector<RichTextParagraph> &paragraphs);

bool TextPropertyRangeMatches(std::string_view text,
                             const TextUtf8Range &range) noexcept;

TextPropertyPatchResult
ApplyTextPropertyPatch(TextPropertyDocumentView document,
                       const TextPropertyPatch &patch,
                       const RichTextLimits &limits = {});

} // namespace videocut::text
