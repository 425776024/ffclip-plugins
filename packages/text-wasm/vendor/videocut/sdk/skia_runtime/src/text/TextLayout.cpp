#include "text/TextRenderPipeline.h"

namespace videocut::skia_runtime::internal::text_lane {


std::vector<std::size_t> GraphemeOffsets(SkUnicode &unicode,
                                         const std::string &utf8,
                                         const std::string &locale) {
  std::vector<std::size_t> result{0U};
  if (!utf8.empty()) {
    auto iterator = unicode.makeBreakIterator(
        locale.empty() ? "und" : locale.c_str(),
        SkUnicode::BreakType::kGraphemes);
    if (iterator && iterator->setText(utf8.data(),
                                      static_cast<int>(utf8.size()))) {
      for (auto position = iterator->first(); !iterator->isDone();
           position = iterator->next()) {
        if (position >= 0)
          result.push_back(static_cast<std::size_t>(position));
      }
    }
    result.push_back(utf8.size());
  }
  std::sort(result.begin(), result.end());
  result.erase(std::remove_if(result.begin(), result.end(),
                              [&](const auto offset) {
                                return offset > utf8.size();
                              }),
               result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  if (result.empty() || result.front() != 0U)
    result.insert(result.begin(), 0U);
  if (result.back() != utf8.size())
    result.push_back(utf8.size());
  return result;
}


std::size_t Utf16UnitsForUtf8Prefix(const std::string &utf8,
                                    const std::size_t offset) noexcept {
  const auto bounded = std::min(offset, utf8.size());
  const int units = SkUTF::UTF8ToUTF16(nullptr, 0, utf8.data(), bounded);
  return units >= 0 ? static_cast<std::size_t>(units) : bounded;
}


std::uint64_t StableUnitFallbackIdentity(
    const TextEffectLayoutUnitBinding &unit) noexcept {
  IdentityBuilder identity;
  identity.AddString(unit.paragraphId);
  identity.AddString(unit.runId);
  identity.Add(unit.utf8Begin);
  identity.Add(unit.utf8End);
  auto value = identity.value();
  return value == 0U ? 1U : value;
}


bool BuildLayoutUnitBindings(
    const text::ResolvedRichTextView &view, SkUnicode &unicode,
    const text::TextEffectFramePlan &framePlan,
    std::vector<TextEffectLayoutUnitBinding> &units, std::string &error) {
  units.clear();
  for (const auto &paragraph : view.paragraphs) {
    for (const auto &run : paragraph.runs) {
      const auto offsets = GraphemeOffsets(
          unicode, run.utf8Text,
          run.locale.empty() ? paragraph.style.locale : run.locale);
      for (std::size_t index = 1U; index < offsets.size(); ++index) {
        if (offsets[index] <= offsets[index - 1U])
          continue;
        TextEffectLayoutUnitBinding unit;
        unit.paragraphId = paragraph.paragraphId;
        unit.runId = run.runId;
        unit.utf8Begin = offsets[index - 1U];
        unit.utf8End = offsets[index];
        units.push_back(std::move(unit));
      }
    }
  }

  if (!framePlan.units.empty() && framePlan.units.size() != units.size()) {
    error = "text frame-plan unit topology does not match the resolved text";
    return false;
  }
  std::unordered_set<std::uint64_t> identities;
  for (std::size_t index = 0U; index < units.size(); ++index) {
    units[index].stableUnitId = framePlan.units.empty()
                                    ? StableUnitFallbackIdentity(units[index])
                                    : framePlan.units[index].stableUnitId;
    if (units[index].stableUnitId == 0U ||
        !identities.insert(units[index].stableUnitId).second) {
      error = "text frame-plan unit identities are zero or duplicated";
      return false;
    }
  }
  return true;
}


const text::RichTextRun *FindRun(const text::ResolvedRichTextView &view,
                                 const std::string &paragraphId,
                                 const std::string &runId) noexcept {
  for (const auto &paragraph : view.paragraphs) {
    if (paragraph.paragraphId != paragraphId)
      continue;
    const auto run = std::find_if(
        paragraph.runs.begin(), paragraph.runs.end(),
        [&](const auto &candidate) { return candidate.runId == runId; });
    return run == paragraph.runs.end() ? nullptr : &*run;
  }
  return nullptr;
}


bool ApplySampledProperties(const text::TextEffectFramePlan &framePlan,
                            text::TextRenderDocument &document,
                            std::vector<Diagnostic> &diagnostics,
                            std::string &error) {
  if (framePlan.sampledProperties.empty())
    return true;
  text::TextPropertyPatch patch;
  patch.patchId = "renderer.sampled-properties";
  patch.assignments = framePlan.sampledProperties;
  auto result = text::ApplyTextPropertyPatch(
      {document.text.slots, document.text.paragraphs,
       document.text.layoutBox, document.text.writingMode,
       document.appearance},
      patch);
  diagnostics.insert(diagnostics.end(), result.diagnostics.begin(),
                     result.diagnostics.end());
  if (!result.valid) {
    error = result.diagnostics.empty()
                ? "sampled text property assignments are invalid"
                : result.diagnostics.front().message;
    return false;
  }
  return true;
}


SkParagraphStyle MakeParagraphStyle(const text::ParagraphStyle &source,
                                   const bool applyRoundingHack) {
  using namespace skia::textlayout;
  SkParagraphStyle result;
  // Source-canvas geometry also drives shader uniforms and random seeds.
  // Preserve the shaped binary32 advances for SDFText instead of rounding
  // its line and selection-box widths to hundredths of a pixel.
  result.setApplyRoundingHack(applyRoundingHack);
  switch (source.alignment) {
  case text::TextAlignment::Start:
    result.setTextAlign(TextAlign::kStart);
    break;
  case text::TextAlignment::Center:
    result.setTextAlign(TextAlign::kCenter);
    break;
  case text::TextAlignment::End:
    result.setTextAlign(TextAlign::kEnd);
    break;
  case text::TextAlignment::Justify:
    result.setTextAlign(TextAlign::kJustify);
    break;
  }
  result.setTextDirection(source.direction == text::TextDirection::RightToLeft
                              ? TextDirection::kRtl
                              : TextDirection::kLtr);
  if (source.maximumLines)
    result.setMaxLines(*source.maximumLines);
  if (source.overflow == text::TextOverflow::Ellipsis)
    result.setEllipsis(SkString("\xE2\x80\xA6"));
  result.setHeight(source.lineHeight);
  result.setRenderSoftHyphens(source.hyphenation ==
                              text::TextHyphenation::Automatic);
  return result;
}


SkTextStyle MakeBaseTextStyle(
    const text::TextStyle &source, const FontContext &fonts,
    const ResolvedTextEffectLayoutMutation *mutation,
    const float globalScale,
    const std::string_view locale, const float paragraphLineHeight) {
  using namespace skia::textlayout;
  SkTextStyle result;
  const auto apply = [](const float authored,
                        const std::vector<TextEffectLayoutScalarOperation>
                            *operations) {
    if (!operations)
      return authored;
    return ApplyTextEffectLayoutScalarOperations(authored, *operations)
        .value_or(authored);
  };
  result.setFontSize(std::max(
      0.0F, apply(source.fontSize,
                  mutation ? &mutation->absoluteFontSize : nullptr) *
                globalScale));
  result.setLetterSpacing(
      apply(source.letterSpacing,
            mutation ? &mutation->letterSpacing : nullptr) *
      globalScale);
  result.setWordSpacing(
      apply(source.wordSpacing,
            mutation ? &mutation->wordSpacing : nullptr) *
      globalScale);
  result.setBaselineShift(
      apply(source.baselineShift,
            mutation ? &mutation->baselineShift : nullptr) *
      globalScale);
  // ParagraphStyle's height does not override explicitly pushed run styles.
  // Bind the authored value to every measurement/material run so edits and
  // animator operations affect the same baselines and control geometry.
  if (paragraphLineHeight != 1.0F ||
      (mutation && !mutation->paragraphLineHeight.empty())) {
    result.setHeight(apply(paragraphLineHeight,
        mutation ? &mutation->paragraphLineHeight : nullptr));
    result.setHeightOverride(true);
  }
  result.setFontStyle(ToSkFontStyle(source.font));
  result.setFontEdging(kQtTextGlyphEdging);
  result.setFontHinting(kQtTextGlyphHinting);
  result.setSubpixel(kQtTextGlyphSubpixelPositioning);
  result.setLocale(SkString(locale.empty() ? "und"
                                           : std::string(locale).c_str()));

  std::vector<SkString> families;
  families.emplace_back(
      ExactFontFamilyAlias(source.font.primary).c_str());
  for (const auto &fallback : source.font.fallbacks)
    families.emplace_back(ExactFontFamilyAlias(fallback).c_str());
  result.setFontFamilies(std::move(families));
  for (const auto &feature : source.font.features)
    result.addFontFeature(SkString(feature.tag.c_str()),
                          static_cast<int>(feature.value));
  result.setColor(SK_ColorWHITE);

  // Decoration lines are independent typed-material passes. SkParagraph's
  // shared decoration paint cannot represent separate underline and
  // strike-through gradients/textures, so shaping deliberately disables it.
  result.setDecoration(TextDecoration::kNoDecoration);
  static_cast<void>(fonts);
  return result;
}


void RecomputeUniformTightAnchorBounds(
    const text::ResolvedRichTextView &view, ResolvedLayout &layout) {
  for (auto &unit : layout.units)
    unit.uniformTightAnchorBounds = SkRect::MakeEmpty();

  auto &published = layout.publicLayout;
  // getLettersRect(..., true, true) preserves each Letter's tight width but
  // reflects the furthest tight edge on a row around the row origin after the
  // complete logical row set has been centred in the TextPro design canvas.
  std::vector<SkRect> scriptLineBounds(published.lines.size(),
                                       SkRect::MakeEmpty());
  const auto publishedLineRect = [&](const std::size_t index) {
    const auto &bounds = published.lines[index].bounds;
    return SkRect::MakeXYWH(bounds.x, bounds.y, bounds.width, bounds.height);
  };
  const auto lineForBounds = [&](const SkRect &bounds) {
    std::size_t best = 0U;
    float maximumOverlap = -1.0F;
    for (std::size_t index = 0U; index < published.lines.size(); ++index) {
      const auto line = publishedLineRect(index);
      const float overlap =
          std::max(0.0F, std::min(bounds.bottom(), line.bottom()) -
                             std::max(bounds.top(), line.top()));
      if (overlap > maximumOverlap) {
        maximumOverlap = overlap;
        best = index;
      }
    }
    return best;
  };
  SkRect logicalRows = SkRect::MakeEmpty();
  for (const auto &unit : layout.units) {
    if (unit.scriptBounds.isEmpty() || scriptLineBounds.empty())
      continue;
    scriptLineBounds[lineForBounds(unit.scriptBounds)].join(unit.scriptBounds);
  }
  for (std::size_t index = 0U; index < scriptLineBounds.size(); ++index) {
    if (!scriptLineBounds[index].isEmpty())
      logicalRows.join(publishedLineRect(index));
  }
  const float originShiftY =
      logicalRows.isEmpty()
          ? 0.0F
          : view.referenceCanvas.height * 0.5F - logicalRows.centerY();
  std::vector<float> uniformHalfHeights(published.lines.size(), 0.0F);
  for (const auto &unit : layout.units) {
    if (unit.scriptBounds.isEmpty() || unit.tightInkBounds.isEmpty() ||
        uniformHalfHeights.empty()) {
      continue;
    }
    const auto lineIndex = lineForBounds(unit.scriptBounds);
    const auto line = publishedLineRect(lineIndex);
    const float rowCenterY = line.centerY() + originShiftY;
    uniformHalfHeights[lineIndex] = std::max(
        uniformHalfHeights[lineIndex],
        std::max(std::fabs(unit.tightInkBounds.top() - rowCenterY),
                 std::fabs(unit.tightInkBounds.bottom() - rowCenterY)));
  }
  for (auto &unit : layout.units) {
    if (unit.scriptBounds.isEmpty() || unit.tightInkBounds.isEmpty())
      continue;
    const auto lineIndex =
        scriptLineBounds.empty() ? 0U : lineForBounds(unit.scriptBounds);
    const float halfHeight =
        lineIndex < uniformHalfHeights.size() &&
                uniformHalfHeights[lineIndex] > 0.0F
            ? uniformHalfHeights[lineIndex]
            : unit.tightInkBounds.height() * 0.5F;
    const float anchorCenterY =
        lineIndex < published.lines.size()
            ? publishedLineRect(lineIndex).centerY() + originShiftY
            : unit.tightInkBounds.centerY();
    unit.uniformTightAnchorBounds = SkRect::MakeXYWH(
        unit.letterInitialPosition.x() - unit.tightInkBounds.width() * 0.5F,
        anchorCenterY - halfHeight, unit.tightInkBounds.width(),
        halfHeight * 2.0F);
  }
}


std::unique_ptr<Paragraph> BuildMeasurementParagraph(
    const text::RichTextParagraph &paragraph, const FontContext &fonts,
    const std::vector<TextEffectLayoutUnitBinding> &units,
    const ResolvedTextEffectLayoutMutations &mutations,
    const float shapingScale, const bool applyRoundingHack,
    std::string &error) {
  auto builder = ParagraphBuilder::make(
      MakeParagraphStyle(paragraph.style, applyRoundingHack),
                                        fonts.collection, fonts.unicode);
  if (!builder) {
    error = "SkParagraph measurement builder creation failed";
    return {};
  }
  for (const auto &run : paragraph.runs) {
    std::vector<const TextEffectLayoutUnitBinding *> runUnits;
    for (const auto &unit : units) {
      if (unit.paragraphId == paragraph.paragraphId &&
          unit.runId == run.runId)
        runUnits.push_back(&unit);
    }
    if (runUnits.empty() && !run.utf8Text.empty()) {
      builder->pushStyle(MakeBaseTextStyle(
          run.style, fonts, nullptr, shapingScale,
          run.locale.empty() ? paragraph.style.locale : run.locale,
          paragraph.style.lineHeight));
      builder->addText(run.utf8Text.data(), run.utf8Text.size());
      builder->pop();
      continue;
    }
    // Keep SkParagraph's shaping run intact whenever no layout mutation is
    // active.  Rebuilding one substring per grapheme changes kerning,
    // ligatures, bidi resolution and line breaking, which was the source of
    // the widespread small/offset glyph drift after the composition rewrite.
    const bool hasMutation = std::any_of(
        runUnits.begin(), runUnits.end(), [&](const auto *unit) {
          const auto *mutation = mutations.Find(
              unit->paragraphId, unit->runId, unit->utf8Begin,
              unit->utf8End);
          return mutation &&
                 ((mutation->replacementCodepoint &&
                   *mutation->replacementCodepoint != 0U) ||
                  !mutation->absoluteFontSize.empty() ||
                  !mutation->letterSpacing.empty() ||
                  !mutation->wordSpacing.empty() ||
                  !mutation->baselineShift.empty() ||
                  !mutation->paragraphLineHeight.empty());
        });
    if (!hasMutation && !run.utf8Text.empty()) {
      builder->pushStyle(MakeBaseTextStyle(
          run.style, fonts, nullptr, shapingScale,
          run.locale.empty() ? paragraph.style.locale : run.locale,
          paragraph.style.lineHeight));
      builder->addText(run.utf8Text.data(), run.utf8Text.size());
      builder->pop();
      continue;
    }
    for (const auto *unit : runUnits) {
      const auto *mutation = mutations.Find(
          unit->paragraphId, unit->runId, unit->utf8Begin,
          unit->utf8End);
      builder->pushStyle(
          MakeBaseTextStyle(
              run.style, fonts, mutation, shapingScale,
              run.locale.empty() ? paragraph.style.locale : run.locale,
              paragraph.style.lineHeight));
      if (mutation && mutation->replacementCodepoint &&
          *mutation->replacementCodepoint != 0U) {
        char replacement[SkUTF::kMaxBytesInUTF8Sequence]{};
        const auto byteCount = SkUTF::ToUTF8(
            static_cast<SkUnichar>(*mutation->replacementCodepoint),
            replacement);
        if (byteCount == 0U) {
          error = "text effect replacement codepoint is not encodable";
          return {};
        }
        builder->addText(replacement, byteCount);
      } else {
        builder->addText(run.utf8Text.data() + unit->utf8Begin,
                         unit->utf8End - unit->utf8Begin);
      }
      builder->pop();
    }
  }
  auto result = builder->Build();
  if (!result)
    error = "SkParagraph measurement construction failed";
  return result;
}


SkMatrix ResolveWritingTransform(const text::ResolvedRichTextView &view,
                                 const SkRect &horizontalBounds) {
  SkMatrix result = SkMatrix::I();
  if (view.writingMode == text::TextWritingMode::Horizontal)
    return result;
  const auto &box = view.layoutBox;
  if (view.writingMode == text::TextWritingMode::VerticalRightToLeft) {
    result.setAll(0.0F, -1.0F, box.x + box.width + horizontalBounds.top(),
                  1.0F, 0.0F, box.y - horizontalBounds.left(), 0.0F, 0.0F,
                  1.0F);
  } else {
    result.setAll(0.0F, 1.0F, box.x - horizontalBounds.top(), -1.0F, 0.0F,
                  box.y + box.height + horizontalBounds.left(), 0.0F, 0.0F,
                  1.0F);
  }
  return result;
}


SkRect MapRect(const SkMatrix &matrix, const SkRect &rect) {
  SkRect result = rect;
  matrix.mapRect(&result);
  return result;
}


std::string FlattenDocumentText(const text::ResolvedRichTextView &view) {
  std::string result;
  for (std::size_t paragraphIndex = 0U;
       paragraphIndex < view.paragraphs.size(); ++paragraphIndex) {
    if (paragraphIndex != 0U)
      result.push_back('\n');
    for (const auto &run : view.paragraphs[paragraphIndex].runs)
      result += run.utf8Text;
  }
  return result;
}


bool CanReuseBaseLayout(const ResolvedLayout &base,
                       const text::TextEffectFramePlan &basePlan,
                       const text::TextEffectFramePlan &finalPlan) {
  if (basePlan.sourceCreationComponent != finalPlan.sourceCreationComponent ||
      !basePlan.layoutMutations.empty() || !finalPlan.layoutMutations.empty())
    return false;
  const auto changesShaping = [](const auto &unit) {
    return unit.absoluteFontSize.has_value() ||
           unit.replacementCodepoint.has_value();
  };
  if (std::any_of(basePlan.units.begin(), basePlan.units.end(), changesShaping) ||
      std::any_of(finalPlan.units.begin(), finalPlan.units.end(), changesShaping))
    return false;
  if (finalPlan.units.empty())
    return basePlan.units.empty();
  if (finalPlan.units.size() != base.units.size())
    return false;
  for (std::size_t index = 0; index < base.units.size(); ++index)
    if (finalPlan.units[index].stableUnitId != base.units[index].binding.stableUnitId)
      return false;
  return true;
}

bool BuildLayout(const text::ResolvedRichTextView &view,
                 const text::TextEffectFramePlan &framePlan,
                 const FontContext &fonts, ResolvedLayout &output,
                 std::string &error) {
  output = {};
  std::vector<TextEffectLayoutUnitBinding> bindings;
  if (!BuildLayoutUnitBindings(view, *fonts.unicode, framePlan, bindings,
                               error))
    return false;
  ResolvedTextEffectLayoutMutations mutations;
  if (!mutations.Resolve(framePlan, bindings, error))
    return false;

  float fitReferenceFontSize = 0.0F;
  for (const auto &binding : bindings) {
    const auto *run = FindRun(view, binding.paragraphId, binding.runId);
    if (!run)
      continue;
    const auto *mutation = mutations.Find(
        binding.paragraphId, binding.runId, binding.utf8Begin,
        binding.utf8End);
    const float resolvedFontSize =
        mutation
            ? ApplyTextEffectLayoutScalarOperations(
                  run->style.fontSize, mutation->absoluteFontSize)
                  .value_or(run->style.fontSize)
            : run->style.fontSize;
    fitReferenceFontSize = std::max(fitReferenceFontSize, resolvedFontSize);
  }
  fitReferenceFontSize = std::max(1.0F, fitReferenceFontSize);

  const auto &box = view.layoutBox;
  // SkParagraph is always shaped in its native horizontal coordinate space.
  // The layout box's width is therefore the wrapping constraint even for a
  // vertical writing mode; the writing transform maps that shaped line onto
  // the vertical axis afterwards.  FitText compares the resulting line
  // extent against the final writing axis separately below.
  const float horizontalConstraint =
      box.width - box.padding.left - box.padding.right;
  const float writingAxisConstraint =
      view.writingMode == text::TextWritingMode::Horizontal
          ? horizontalConstraint
          : box.height - box.padding.top - box.padding.bottom;
  if (!std::isfinite(horizontalConstraint) || horizontalConstraint <= 0.0F) {
    error = "resolved text layout has no positive writing-axis constraint";
    return false;
  }

  auto buildAtScale = [&](const float shapingScale) -> bool {
    output.paragraphs.clear();
    output.units.clear();
    float y = box.y + box.padding.top;
    float maximumWidth = 0.0F;
    std::size_t documentUtf8Offset = 0U;
    std::size_t nextUnitIndex = 0U;
    for (std::size_t paragraphIndex = 0U;
         paragraphIndex < view.paragraphs.size(); ++paragraphIndex) {
      const auto &source = view.paragraphs[paragraphIndex];
      y += source.style.spacingBefore;
      auto paragraph = BuildMeasurementParagraph(
          source, fonts, bindings, mutations, shapingScale,
          framePlan.sourceCreationComponent !=
              text::TextSourceCreationComponent::SdfText,
          error);
      if (!paragraph)
        return false;
      const float paragraphWidth = std::max(
          1.0F, horizontalConstraint - source.style.startIndent -
                    source.style.endIndent);
      fonts.LayoutParagraph(*paragraph, paragraphWidth);
      ParagraphLayout resolved;
      resolved.source = &source;
      resolved.x = box.x + box.padding.left + source.style.startIndent;
      resolved.y = y;
      resolved.width = paragraphWidth;
      resolved.height = static_cast<float>(paragraph->getHeight());
      resolved.documentUtf8Offset = documentUtf8Offset;
      resolved.measurement = std::move(paragraph);
      const float materialTextWidth = std::clamp(
          static_cast<float>(resolved.measurement->getLongestLine()), 1.0F,
          paragraphWidth);
      float materialTextX = resolved.x;
      if (source.style.alignment == text::TextAlignment::Center) {
        materialTextX += (paragraphWidth - materialTextWidth) * 0.5F;
      } else if (source.style.alignment == text::TextAlignment::End) {
        materialTextX += paragraphWidth - materialTextWidth;
      }
      resolved.materialLayoutBounds = SkRect::MakeXYWH(
          resolved.x, resolved.y, paragraphWidth,
          std::max(1.0F, box.height));
      resolved.materialTextBounds = SkRect::MakeXYWH(
          materialTextX, resolved.y, materialTextWidth,
          std::max(1.0F, resolved.height));

      std::string paragraphText;
      std::size_t paragraphBytes = 0U;
      std::string paragraphLocale = source.style.locale;
      for (const auto &run : source.runs) {
        paragraphBytes += run.utf8Text.size();
        if (paragraphLocale.empty() && !run.locale.empty())
          paragraphLocale = run.locale;
      }
      paragraphText.reserve(paragraphBytes);
      for (const auto &run : source.runs)
        paragraphText.append(run.utf8Text);
      std::vector<SkUnicode::Position> wordBreaks;
      fonts.unicode->getUtf8Words(
          paragraphText.data(), static_cast<int>(paragraphText.size()),
          paragraphLocale.empty() ? "und" : paragraphLocale.c_str(),
          &wordBreaks);
      wordBreaks.push_back(0U);
      wordBreaks.push_back(paragraphText.size());
      std::sort(wordBreaks.begin(), wordBreaks.end());
      wordBreaks.erase(std::unique(wordBreaks.begin(), wordBreaks.end()),
                       wordBreaks.end());

      std::size_t paragraphUtf16Offset = 0U;
      std::size_t paragraphUtf8Offset = 0U;
      for (const auto &run : source.runs) {
        while (nextUnitIndex < bindings.size() &&
               bindings[nextUnitIndex].paragraphId == source.paragraphId &&
               bindings[nextUnitIndex].runId == run.runId) {
          const auto &binding = bindings[nextUnitIndex];
          UnitGeometry unit;
          unit.binding = binding;
          const auto *unitMutation = mutations.Find(
              binding.paragraphId, binding.runId,
              binding.utf8Begin, binding.utf8End);
          unit.canvasCellFontSize =
              (unitMutation
                   ? ApplyTextEffectLayoutScalarOperations(
                         run.style.fontSize, unitMutation->absoluteFontSize)
                         .value_or(run.style.fontSize)
                   : run.style.fontSize) * shapingScale;
          unit.paragraphIndex = paragraphIndex;
          unit.paragraphUtf16Begin =
              paragraphUtf16Offset +
              Utf16UnitsForUtf8Prefix(run.utf8Text, binding.utf8Begin);
          unit.paragraphUtf16End =
              paragraphUtf16Offset +
              Utf16UnitsForUtf8Prefix(run.utf8Text, binding.utf8End);
          unit.paragraphUtf8Begin =
              paragraphUtf8Offset + binding.utf8Begin;
          unit.paragraphUtf8End = paragraphUtf8Offset + binding.utf8End;
          const auto boxes = resolved.measurement->getRectsForRange(
              static_cast<unsigned>(unit.paragraphUtf16Begin),
              static_cast<unsigned>(unit.paragraphUtf16End),
              skia::textlayout::RectHeightStyle::kTight,
              skia::textlayout::RectWidthStyle::kTight);
          for (const auto &textBox : boxes) {
            unit.localBounds.join(textBox.rect);
            unit.textBoxes.push_back(
                {textBox.rect,
                 textBox.direction ==
                     skia::textlayout::TextDirection::kRtl});
          }
          const auto layoutBoxes = resolved.measurement->getRectsForRange(
              static_cast<unsigned>(unit.paragraphUtf16Begin),
              static_cast<unsigned>(unit.paragraphUtf16End),
              skia::textlayout::RectHeightStyle::kMax,
              skia::textlayout::RectWidthStyle::kTight);
          for (const auto &textBox : layoutBoxes)
            unit.localLayoutBounds.join(textBox.rect);
          if (unit.localBounds.isEmpty()) {
            unit.localBounds = SkRect::MakeXYWH(
                0.0F, 0.0F, 1.0F,
                std::max(1.0F, run.style.fontSize * shapingScale));
          }
          if (unit.localLayoutBounds.isEmpty())
            unit.localLayoutBounds = unit.localBounds;
          unit.authoredBounds = unit.localBounds;
          unit.authoredBounds.offset(resolved.x, resolved.y);
          for (auto &textBox : unit.textBoxes)
            textBox.bounds.offset(resolved.x, resolved.y);
          unit.layoutBounds = unit.localLayoutBounds;
          unit.layoutBounds.offset(resolved.x, resolved.y);
          unit.documentUtf8Begin = documentUtf8Offset + paragraphUtf8Offset +
                                   binding.utf8Begin;
          unit.documentUtf8End = documentUtf8Offset + paragraphUtf8Offset +
                                 binding.utf8End;
          const auto paragraphClusterBegin =
              paragraphUtf8Offset + binding.utf8Begin;
          const auto paragraphClusterEnd =
              paragraphUtf8Offset + binding.utf8End;
          const auto upper = std::upper_bound(
              wordBreaks.begin(), wordBreaks.end(), paragraphClusterBegin);
          const auto wordBegin =
              upper == wordBreaks.begin() ? wordBreaks.front()
                                          : *std::prev(upper);
          const auto wordEnd = upper == wordBreaks.end()
                                   ? wordBreaks.back()
                                   : *upper;
          if (wordBegin <= paragraphClusterBegin &&
              paragraphClusterEnd <= wordEnd) {
            unit.documentWordUtf8Begin = documentUtf8Offset + wordBegin;
            unit.documentWordUtf8End = documentUtf8Offset + wordEnd;
          } else {
            unit.documentWordUtf8Begin = unit.documentUtf8Begin;
            unit.documentWordUtf8End = unit.documentUtf8End;
          }
          resolved.unitIndexes.push_back(output.units.size());
          output.units.push_back(std::move(unit));
          ++nextUnitIndex;
        }
        paragraphUtf16Offset +=
            Utf16UnitsForUtf8Prefix(run.utf8Text, run.utf8Text.size());
        paragraphUtf8Offset += run.utf8Text.size();
      }
      documentUtf8Offset += paragraphUtf8Offset;
      if (paragraphIndex + 1U < view.paragraphs.size())
        ++documentUtf8Offset;
      maximumWidth = std::max(maximumWidth, resolved.width);
      y += resolved.height + source.style.spacingAfter;
      output.paragraphs.push_back(std::move(resolved));
    }
    output.logicalBounds = SkRect::MakeXYWH(
        box.x + box.padding.left, box.y + box.padding.top,
        std::max(1.0F, maximumWidth),
        std::max(1.0F, y - box.y - box.padding.top));
    output.shapingScale = shapingScale;
    return true;
  };

  float shapingScale = 1.0F;
  if (box.sizingMode == text::TextLayoutSizingMode::FitText) {
    const float availableHeight =
        std::max(1.0F, box.height - box.padding.top - box.padding.bottom);
    const float availableWidth = std::max(1.0F, writingAxisConstraint);
    const float minimumScale = box.minimumFitFontSize / fitReferenceFontSize;
    const float maximumScale = box.maximumFitFontSize / fitReferenceFontSize;
    const bool traceQtFit =
        std::getenv("VIDEOCUT_TRACE_QT_TEXT_FIT") != nullptr;

    // AmazingEngine 22 / TextPro 11.3 evidence:
    // FUN_0087c184 performs at most 50 multiplicative iterations. Its
    // FUN_0087ccf8 kernel first resolves a writing-axis overflow with
    // 0.95*available/measured. Once width fits, it uses the iteration progress
    // and a square-root height factor to approach the shape box, stopping when
    // the height fill crosses 1-progress. Reproducing that finite float loop is
    // intentional: replacing it with an exact min(width,height) solution moves
    // animated glyph edges away from the Qt sample positions.
    auto measureUnwrapped = [&](const float scale, float &measuredWidth,
                                float &measuredHeight) -> bool {
      measuredWidth = 0.0F;
      measuredHeight = 0.0F;
      for (const auto &paragraph : view.paragraphs) {
        auto measurement = BuildMeasurementParagraph(
            paragraph, fonts, bindings, mutations, scale,
            framePlan.sourceCreationComponent !=
                text::TextSourceCreationComponent::SdfText,
            error);
        if (!measurement)
          return false;
        const float paragraphWidth = std::max(
            1.0F, horizontalConstraint - paragraph.style.startIndent -
                      paragraph.style.endIndent);
        fonts.LayoutParagraph(*measurement, paragraphWidth);
        measuredWidth = std::max(
            measuredWidth,
            paragraph.style.startIndent + paragraph.style.endIndent +
                static_cast<float>(measurement->getLongestLine()));
        TextGlyphOutlineCaptureRequest capture;
        capture.paragraph = measurement.get();
        capture.writingTransform = SkMatrix::I();
        std::size_t paragraphUtf8Offset = 0U;
        for (const auto &run : paragraph.runs) {
          for (const auto &binding : bindings) {
            if (binding.paragraphId != paragraph.paragraphId ||
                binding.runId != run.runId)
              continue;
            capture.units.push_back(
                {binding.stableUnitId, binding.paragraphId, binding.runId,
                 paragraphUtf8Offset + binding.utf8Begin,
                 paragraphUtf8Offset + binding.utf8End, binding.utf8Begin,
                 binding.utf8End});
          }
          paragraphUtf8Offset += run.utf8Text.size();
        }
        std::vector<CapturedTextGlyphOutline> outlines;
        if (!CaptureTextGlyphOutlines(capture, outlines, error))
          return false;
        struct QtFitLineMetric final {
          bool hasGlyph{false};
          float firstAscent{0.0F};
          float lastDescent{0.0F};
          float nominalAdvance{0.0F};
        };
        std::map<int, QtFitLineMetric> lineMetrics;
        for (const auto &outline : outlines) {
          auto &line = lineMetrics[outline.lineIndex];
          const float ascent =
              outline.baselineOrigin.y() - outline.tightBounds.top();
          const float descent =
              outline.tightBounds.bottom() - outline.baselineOrigin.y();
          if (!line.hasGlyph) {
            line.firstAscent = ascent;
            line.hasGlyph = true;
          }
          line.lastDescent = descent;
          line.nominalAdvance =
              std::max(line.nominalAdvance, outline.shapedFontSize);
        }
        // FUN_0087ccf8 materializes one 0x58-byte record per line.  The
        // FUN_0087d758 height consumer uses the first record's ascent, the
        // last record's descent and the intervening nominal line advances;
        // it does not sum each glyph row's tight height and it does not use
        // SkParagraph's font-leading baseline distance.  The LLDB receipt for
        // Qt bubble 04 at scale 1 is 126.879173 + 154.166672 + 9.095834 =
        // 290.141663 in Qt's native domain.  Derive the same quantities from
        // the shaped outlines so mixed fonts and externally supplied sizes
        // remain data-driven.
        float glyphHeight = 0.0F;
        if (!lineMetrics.empty()) {
          glyphHeight = lineMetrics.begin()->second.firstAscent +
                        lineMetrics.rbegin()->second.lastDescent;
          for (auto line = lineMetrics.begin();
               std::next(line) != lineMetrics.end(); ++line) {
            glyphHeight += line->second.nominalAdvance *
                           std::max(0.0F, paragraph.style.lineHeight);
          }
        }
        if (glyphHeight <= 0.0F)
          glyphHeight = static_cast<float>(measurement->getHeight());
        measuredHeight += paragraph.style.spacingBefore + glyphHeight +
                          paragraph.style.spacingAfter;
      }
      measuredWidth +=
          (box.fitMeasurementOutsets.left +
           box.fitMeasurementOutsets.right) * scale;
      measuredHeight +=
          (box.fitMeasurementOutsets.top +
           box.fitMeasurementOutsets.bottom) * scale;
      measuredWidth = std::max(1.0F, measuredWidth);
      measuredHeight = std::max(1.0F, measuredHeight);
      return true;
    };

    for (std::uint32_t iteration = 0U; iteration < 50U; ++iteration) {
      float measuredWidth = 0.0F;
      float measuredHeight = 0.0F;
      if (!measureUnwrapped(shapingScale, measuredWidth, measuredHeight))
        return false;
      float factor = 1.0F;
      const char *decision = "height-fill";
      if (measuredWidth > availableWidth) {
        factor = (availableWidth * 0.95F) / measuredWidth;
        decision = "width-overflow";
      } else if (measuredHeight > availableHeight) {
        factor =
            std::sqrt((availableHeight * 0.95F) / measuredHeight);
        decision = "height-overflow";
      } else {
        const float progress = static_cast<float>(iteration) / 50.0F;
        const float heightFill = measuredHeight / availableHeight;
        if (heightFill > 1.0F - progress) {
          if (traceQtFit) {
            std::fprintf(stderr,
                         "[VIDEOCUT_QT_TEXT_FIT] iteration=%u scale=%.9g "
                         "width=%.9g/%.9g height=%.9g/%.9g decision=stop\n",
                         iteration, shapingScale, measuredWidth,
                         availableWidth, measuredHeight, availableHeight);
          }
          break;
        }
        factor = std::sqrt(((1.0F - progress) * availableHeight) /
                           measuredHeight);
      }
      if (traceQtFit) {
        std::fprintf(stderr,
                     "[VIDEOCUT_QT_TEXT_FIT] iteration=%u scale=%.9g "
                     "width=%.9g/%.9g height=%.9g/%.9g factor=%.9g "
                     "decision=%s\n",
                     iteration, shapingScale, measuredWidth, availableWidth,
                     measuredHeight, availableHeight, factor, decision);
      }
      if (!std::isfinite(factor) || factor <= 0.0F)
        break;
      const float next =
          std::clamp(shapingScale * factor, minimumScale, maximumScale);
      if (std::fabs(next - shapingScale) < 1.0e-6F)
        break;
      shapingScale = next;
    }
    if (traceQtFit) {
      std::fprintf(stderr,
                   "[VIDEOCUT_QT_TEXT_FIT] final_scale=%.9g "
                   "box=(%.9g,%.9g,%.9g,%.9g)\n",
                   shapingScale, box.x, box.y, box.width, box.height);
    }
  }
  if (!buildAtScale(shapingScale))
    return false;
  if (box.fitOriginX &&
      box.sizingMode == text::TextLayoutSizingMode::FitText) {
    const float alignedOriginX =
        box.x + box.padding.left + horizontalConstraint * 0.5F;
    output.fitWriterOffsetX =
        (1.0F - shapingScale) * (*box.fitOriginX - alignedOriginX);
  }

  const float availableHeight =
      std::max(0.0F, box.height - box.padding.top - box.padding.bottom);
  float verticalOffset = 0.0F;
  if (box.verticalAlignment == text::VerticalAlignment::Center)
    verticalOffset =
        std::max(0.0F, availableHeight - output.logicalBounds.height()) *
        0.5F;
  else if (box.verticalAlignment == text::VerticalAlignment::Bottom)
    verticalOffset =
        std::max(0.0F, availableHeight - output.logicalBounds.height());
  if (std::isfinite(verticalOffset) && verticalOffset != 0.0F) {
    for (auto &paragraph : output.paragraphs)
      paragraph.y += verticalOffset;
    for (auto &unit : output.units) {
      unit.authoredBounds.offset(0.0F, verticalOffset);
      unit.layoutBounds.offset(0.0F, verticalOffset);
    }
    output.logicalBounds.offset(0.0F, verticalOffset);
  }

  output.inkBounds = SkRect::MakeEmpty();
  for (const auto &unit : output.units)
    output.inkBounds.join(unit.authoredBounds);
  if (output.inkBounds.isEmpty())
    output.inkBounds = output.logicalBounds;
  output.controlBounds = output.inkBounds;
  output.controlBounds.outset(2.0F, 2.0F);
  output.writingTransform = ResolveWritingTransform(view, output.logicalBounds);
  const auto isDrawableTextEffectUnit = [&](const UnitGeometry &unit) {
    const auto *run = FindRun(view, unit.binding.paragraphId,
                              unit.binding.runId);
    if (!run)
      return true;
    const auto *mutation = mutations.Find(
        unit.binding.paragraphId, unit.binding.runId,
        unit.binding.utf8Begin, unit.binding.utf8End);
    if (!mutation || mutation->absoluteFontSize.empty())
      return true;
    const auto resolved = ApplyTextEffectLayoutScalarOperations(
        run->style.fontSize, mutation->absoluteFontSize);
    return resolved && std::isfinite(*resolved) && *resolved > 0.0F;
  };
  for (const auto &paragraph : output.paragraphs) {
    if (!paragraph.measurement)
      continue;
    TextGlyphOutlineCaptureRequest capture;
    capture.paragraph = paragraph.measurement.get();
    capture.paragraphX = paragraph.x;
    capture.paragraphY = paragraph.y;
    // Capture the paragraph in its native (horizontal, top-left) space.  The
    // capture helper already applies the requested matrix to paths and tight
    // bounds; applying the writing transform here and then mapping the layout
    // below would rotate/flip vertical text twice.  Keep the single bridge at
    // the published-layout boundary so glyph origins, script bounds and line
    // geometry share one coordinate space.
    capture.writingTransform = SkMatrix::I();
    for (const auto unitIndex : paragraph.unitIndexes) {
      const auto &unit = output.units[unitIndex];
      capture.units.push_back(
          {unit.binding.stableUnitId, unit.binding.paragraphId,
           unit.binding.runId, unit.paragraphUtf8Begin, unit.paragraphUtf8End,
           unit.binding.utf8Begin, unit.binding.utf8End});
    }
    std::vector<CapturedTextGlyphOutline> outlines;
    std::string outlineError;
    if (!CaptureTextGlyphOutlines(capture, outlines, outlineError))
      continue;
    for (const auto unitIndex : paragraph.unitIndexes) {
      auto &unit = output.units[unitIndex];
      // Qt omits an absoluteFontSize==0 Letter before getLettersRect. The
      // one-pixel shaping placeholder must not participate in script/tight
      // geometry or inflate the row-wide ExtraMatrix anchor height.
      if (!isDrawableTextEffectUnit(unit))
        continue;
      SkRect combinedTightBounds = SkRect::MakeEmpty();
      float captureOrderMetricHeight = 0.0F;
      for (const auto &outline : outlines) {
        if (unit.binding.stableUnitId != outline.stableUnitId ||
            unit.binding.paragraphId != outline.paragraphId ||
            unit.binding.runId != outline.runId ||
            outline.runUtf8Cluster < unit.binding.utf8Begin ||
            outline.runUtf8Cluster >= unit.binding.utf8End) {
          continue;
        }
        combinedTightBounds.join(outline.tightBounds);
        captureOrderMetricHeight = outline.shapedFontSize;
      }
      if (combinedTightBounds.isEmpty() ||
          !std::isfinite(captureOrderMetricHeight) ||
          captureOrderMetricHeight <= 0.0F) {
        continue;
      }
      output.glyphInkBounds.join(combinedTightBounds);
      unit.tightInkBounds.join(combinedTightBounds);
      constexpr float kTextProSdfCanonicalEm = 150.0F;
      constexpr float kTextProSdfMinimumPadding = 30.0F;
      const float authoredAtlasScale =
          kTextProSdfCanonicalEm /
          std::max(1.0F, captureOrderMetricHeight);
      const float letterPadding =
          kTextProSdfMinimumPadding / authoredAtlasScale;
      auto letterBounds = combinedTightBounds;
      letterBounds.outset(letterPadding, letterPadding);
      unit.scriptBounds.join(letterBounds);
    }
  }
  for (auto &unit : output.units) {
    if (!unit.scriptBounds.isEmpty()) {
      unit.letterInitialPosition = SkPoint::Make(unit.scriptBounds.centerX(),
                                                 unit.scriptBounds.centerY());
    }
  }
  if (view.writingMode != text::TextWritingMode::Horizontal) {
    output.logicalBounds = MapRect(output.writingTransform,
                                   output.logicalBounds);
    output.inkBounds = MapRect(output.writingTransform, output.inkBounds);
    output.glyphInkBounds = MapRect(output.writingTransform,
                                    output.glyphInkBounds);
    output.controlBounds = MapRect(output.writingTransform,
                                   output.controlBounds);
    for (auto &unit : output.units) {
      unit.authoredBounds =
          MapRect(output.writingTransform, unit.authoredBounds);
      unit.layoutBounds = MapRect(output.writingTransform, unit.layoutBounds);
      for (auto &textBox : unit.textBoxes)
        textBox.bounds = MapRect(output.writingTransform, textBox.bounds);
      unit.tightInkBounds =
          MapRect(output.writingTransform, unit.tightInkBounds);
      unit.scriptBounds = MapRect(output.writingTransform, unit.scriptBounds);
      unit.letterInitialPosition =
          SkPoint::Make(unit.scriptBounds.centerX(), unit.scriptBounds.centerY());
    }
  }

  SkRect authoredLetterBounds = SkRect::MakeEmpty();
  for (const auto &unit : output.units)
    authoredLetterBounds.join(unit.scriptBounds);

  auto &published = output.publicLayout;
  published.authoredLogicalBounds = {output.logicalBounds.x(),
                                     output.logicalBounds.y(),
                                     output.logicalBounds.width(),
                                     output.logicalBounds.height()};
  published.authoredInkBounds = {output.inkBounds.x(), output.inkBounds.y(),
                                 output.inkBounds.width(),
                                 output.inkBounds.height()};
  published.authoredLetterBounds = {
      authoredLetterBounds.x(), authoredLetterBounds.y(),
      authoredLetterBounds.width(), authoredLetterBounds.height()};
  published.authoredControlBounds = {
      output.controlBounds.x(), output.controlBounds.y(),
      output.controlBounds.width(), output.controlBounds.height()};
  published.logicalBounds = published.authoredLogicalBounds;
  published.inkBounds = published.authoredInkBounds;
  published.letterBounds = published.authoredLetterBounds;
  published.controlBounds = published.authoredControlBounds;

  for (const auto &paragraph : output.paragraphs) {
    std::vector<skia::textlayout::LineMetrics> metrics;
    paragraph.measurement->getLineMetrics(metrics);
    for (const auto &line : metrics) {
      SkRect bounds = SkRect::MakeXYWH(
          paragraph.x + static_cast<float>(line.fLeft),
          paragraph.y + static_cast<float>(line.fBaseline - line.fAscent),
          static_cast<float>(line.fWidth),
          static_cast<float>(line.fAscent + line.fDescent));
      float baseline = paragraph.y + static_cast<float>(line.fBaseline);
      if (view.writingMode != text::TextWritingMode::Horizontal) {
        bounds = MapRect(output.writingTransform, bounds);
        const SkPoint source = SkPoint::Make(paragraph.x, baseline);
        SkPoint mapped;
        output.writingTransform.mapPoints({&mapped, 1U}, {&source, 1U});
        baseline = mapped.y();
      }
      published.lines.push_back(
          {paragraph.source->paragraphId, line.fStartIndex, line.fEndIndex,
           {bounds.x(), bounds.y(), bounds.width(), bounds.height()}, baseline,
           line.fHardBreak});
      published.baselines.push_back(baseline);
    }
  }

  RecomputeUniformTightAnchorBounds(view, output);
  for (std::size_t unitIndex = 0U; unitIndex < output.units.size();
       ++unitIndex) {
    const auto &unit = output.units[unitIndex];
    text::TextClusterMetrics cluster;
    cluster.paragraphId = unit.binding.paragraphId;
    cluster.runId = unit.binding.runId;
    cluster.utf8Begin = unit.binding.utf8Begin;
    cluster.utf8End = unit.binding.utf8End;
    cluster.documentUtf8Begin = unit.documentUtf8Begin;
    cluster.documentUtf8End = unit.documentUtf8End;
    cluster.documentWordUtf8Begin = unit.documentWordUtf8Begin;
    cluster.documentWordUtf8End = unit.documentWordUtf8End;
    cluster.graphemeIndex = unitIndex;
    const auto appendBox = [&](const SkRect &bounds,
                               const bool rightToLeft) {
      constexpr float kCaretWidth = 1.0F;
      const float beginX =
          rightToLeft ? bounds.right() : bounds.left();
      const float endX =
          rightToLeft ? bounds.left() : bounds.right();
      cluster.boxes.push_back(
          {{bounds.x(), bounds.y(), bounds.width(), bounds.height()},
           {beginX - kCaretWidth * 0.5F, bounds.top(), kCaretWidth,
            bounds.height()},
           {endX - kCaretWidth * 0.5F, bounds.top(), kCaretWidth,
            bounds.height()}});
    };
    if (unit.textBoxes.empty()) {
      appendBox(unit.authoredBounds, false);
    } else {
      for (const auto &textBox : unit.textBoxes)
        appendBox(textBox.bounds, textBox.rightToLeft);
    }
    published.clusters.push_back(std::move(cluster));
  }
  published.authoredLines = published.lines;
  published.authoredClusters = published.clusters;
  published.overflow = output.logicalBounds.height() > box.height ||
                       output.logicalBounds.width() > box.width ||
                       std::any_of(output.paragraphs.begin(),
                                   output.paragraphs.end(), [](const auto &p) {
                                     return p.measurement->didExceedMaxLines();
                                   });
  const auto flattened = FlattenDocumentText(view);
  published.textDigest = videocut::vector::Sha256Digest(
      reinterpret_cast<const std::uint8_t *>(flattened.data()),
      flattened.size());
  IdentityBuilder semantic;
  semantic.AddString(published.textDigest);
  semantic.Add(output.logicalBounds.x());
  semantic.Add(output.logicalBounds.y());
  semantic.Add(output.logicalBounds.width());
  semantic.Add(output.logicalBounds.height());
  semantic.Add(static_cast<unsigned>(view.writingMode));
  semantic.Add(shapingScale);
  published.semanticDigest = semantic.Finish();
  return true;
}


std::string GlyphContributorIdentity(const SkTypeface *typeface,
                                     const FontContext &fonts) {
  if (!typeface)
    return {};
  const auto uniqueId = typeface->uniqueID();
  for (const auto &[key, instance] : fonts.typefaces) {
    static_cast<void>(key);
    if (instance.typeface && instance.typeface->uniqueID() == uniqueId)
      return instance.identity;
  }

  SkString postscriptName;
  if (typeface->getPostScriptName(&postscriptName) &&
      !postscriptName.isEmpty()) {
    return "system-glyph:" + std::string(postscriptName.c_str());
  }
  SkString familyName;
  typeface->getFamilyName(&familyName);
  if (familyName.isEmpty())
    return {};
  const auto style = typeface->fontStyle();
  return "system-glyph-family:" + std::string(familyName.c_str()) +
         "|weight=" + std::to_string(style.weight()) +
         "|width=" + std::to_string(style.width()) +
         "|slant=" +
         std::to_string(static_cast<unsigned>(style.slant()));
}


bool AttachGlyphContributorIdentities(
    const ResolvedLayout &layout, const FontContext &fonts,
    std::vector<text::FontRunResolutionReceipt> &receipts,
    std::string &error) {
  using ReceiptKey = std::pair<std::string, std::string>;
  std::map<ReceiptKey, std::size_t> receiptByRun;
  for (std::size_t index = 0U; index < receipts.size(); ++index) {
    const auto &receipt = receipts[index];
    if (!receiptByRun
             .emplace(ReceiptKey{receipt.paragraphId, receipt.runId}, index)
             .second) {
      error = "font receipt contains a duplicate paragraph/run identity";
      return false;
    }
  }

  constexpr std::size_t kMaximumGlyphContributorsPerRun = 64U;
  bool failed = false;
  for (const auto &paragraph : layout.paragraphs) {
    if (failed || !paragraph.measurement || !paragraph.source)
      continue;
    struct RunRange final {
      std::size_t begin{0U};
      std::size_t end{0U};
      std::size_t receiptIndex{0U};
    };
    std::vector<RunRange> ranges;
    ranges.reserve(paragraph.source->runs.size());
    std::size_t offset = 0U;
    for (const auto &run : paragraph.source->runs) {
      const auto receipt = receiptByRun.find(
          ReceiptKey{paragraph.source->paragraphId, run.runId});
      if (receipt == receiptByRun.end()) {
        error = "shaped text run has no font resolution receipt";
        failed = true;
        break;
      }
      const auto next = offset + run.utf8Text.size();
      ranges.push_back({offset, next, receipt->second});
      offset = next;
    }
    if (failed)
      break;

    paragraph.measurement->visit(
        [&](const int, const Paragraph::VisitorInfo *info) {
          if (failed || !info || info->count == 0)
            return;
          if (info->count < 0 || !info->glyphs || !info->utf8Starts ||
              !info->font.getTypeface()) {
            error = "shaped glyph run has no exact font identity";
            failed = true;
            return;
          }
          const auto identity =
              GlyphContributorIdentity(info->font.getTypeface(), fonts);
          if (identity.empty()) {
            error = "shaped glyph contributor identity is unavailable";
            failed = true;
            return;
          }
          for (int glyphIndex = 0; glyphIndex < info->count; ++glyphIndex) {
            const auto cluster =
                static_cast<std::size_t>(info->utf8Starts[glyphIndex]);
            const auto range = std::find_if(
                ranges.begin(), ranges.end(), [&](const RunRange &candidate) {
                  return cluster >= candidate.begin && cluster < candidate.end;
                });
            if (range == ranges.end())
              continue;
            auto &runReceipt = receipts[range->receiptIndex];
            if (info->glyphs[glyphIndex] == 0U) {
              if (runReceipt.missingGlyphCount ==
                  std::numeric_limits<std::uint32_t>::max()) {
                error = "missing glyph count exceeds the receipt budget";
                failed = true;
                return;
              }
              ++runReceipt.missingGlyphCount;
            }
            auto &contributors =
                runReceipt.glyphContributorIdentities;
            if (std::find(contributors.begin(), contributors.end(), identity) !=
                contributors.end()) {
              continue;
            }
            if (contributors.size() >= kMaximumGlyphContributorsPerRun) {
              error = "shaped glyph contributor count exceeds the receipt "
                      "budget";
              failed = true;
              return;
            }
            contributors.push_back(identity);
          }
        });
  }
  return !failed;
}


void MergeFramePlan(text::TextEffectFramePlan &destination,
                    const text::TextEffectFramePlan &source) {
  destination.sampledProperties.insert(destination.sampledProperties.end(),
                                       source.sampledProperties.begin(),
                                       source.sampledProperties.end());
  if (!source.units.empty()) {
    // Index only incoming IDs. A single sample needs no batch preparation;
    // ordinals remain valid when appending units grows the destination.
    std::unordered_map<std::uint64_t, std::optional<std::size_t>> targets;
    if (source.units.size() > 1U) {
      targets.reserve(source.units.size());
      for (const auto &unit : source.units)
        targets.try_emplace(unit.stableUnitId);
      auto unresolved = targets.size();
      for (std::size_t index = 0U;
           index < destination.units.size() && unresolved > 0U; ++index) {
        const auto found = targets.find(destination.units[index].stableUnitId);
        if (found != targets.end() && !found->second) {
          found->second = index;
          --unresolved;
        }
      }
    }
    for (const auto &unit : source.units) {
      std::size_t targetIndex;
      if (source.units.size() == 1U) {
        const auto found = std::find_if(
            destination.units.begin(), destination.units.end(),
            [&](const auto &candidate) {
              return candidate.stableUnitId == unit.stableUnitId;
            });
        targetIndex = static_cast<std::size_t>(found - destination.units.begin());
      } else {
        auto &indexed = targets.find(unit.stableUnitId)->second;
        if (!indexed)
          indexed = destination.units.size();
        targetIndex = *indexed;
      }
      if (targetIndex == destination.units.size())
        destination.units.push_back({unit.stableUnitId});
      auto &target = destination.units[targetIndex];
      // External samples replace scalar values; transform order is retained.
      if (unit.opacity)
        target.opacity = unit.opacity;
      if (unit.instanceColor)
        target.instanceColor = unit.instanceColor;
      if (unit.absoluteFontSize)
        target.absoluteFontSize = unit.absoluteFontSize;
      if (unit.sdfBlurRadius)
        target.sdfBlurRadius = unit.sdfBlurRadius;
      target.transforms.insert(target.transforms.end(),
                                unit.transforms.begin(), unit.transforms.end());
    }
  }
  destination.layoutMutations.insert(destination.layoutMutations.end(),
                                     source.layoutMutations.begin(),
                                     source.layoutMutations.end());
  destination.glyphMaterialPasses.insert(
      destination.glyphMaterialPasses.end(),
      source.glyphMaterialPasses.begin(), source.glyphMaterialPasses.end());
  destination.backdropPasses.insert(destination.backdropPasses.end(),
                                    source.backdropPasses.begin(),
                                    source.backdropPasses.end());
  destination.decorationPasses.insert(destination.decorationPasses.end(),
                                      source.decorationPasses.begin(),
                                      source.decorationPasses.end());
  destination.postEffectNodes.insert(destination.postEffectNodes.end(),
                                     source.postEffectNodes.begin(),
                                     source.postEffectNodes.end());
  destination.compositeOrder.insert(destination.compositeOrder.end(),
                                    source.compositeOrder.begin(),
                                    source.compositeOrder.end());
  for (const auto &resource : source.resources) {
    const auto existing = std::find_if(
        destination.resources.begin(), destination.resources.end(),
        [&](const auto &candidate) {
          return candidate.resourceId == resource.resourceId &&
                 candidate.assetId == resource.assetId &&
                 candidate.digest == resource.digest;
        });
    if (existing == destination.resources.end()) {
      destination.resources.push_back(resource);
      continue;
    }
    existing->localTimeUs = resource.localTimeUs;
    if (resource.kind != text::TextEffectResourceKind::Unspecified) {
      existing->kind = resource.kind;
      existing->mediaType = resource.mediaType;
    }
  }
  destination.stateTransitions.insert(destination.stateTransitions.end(),
                                      source.stateTransitions.begin(),
                                      source.stateTransitions.end());
  if (!source.executionGraph.nodes.empty())
    destination.executionGraph = source.executionGraph;
}


std::size_t VisualLineForUnit(const ResolvedLayout &layout,
                              const UnitGeometry &unit) {
  std::size_t resolved = unit.paragraphIndex;
  float maximumIntersection = 0.0F;
  for (std::size_t index = 0U;
       index < layout.publicLayout.authoredLines.size(); ++index) {
    const auto &lineBounds = layout.publicLayout.authoredLines[index].bounds;
    auto intersection = SkRect::MakeXYWH(
        lineBounds.x, lineBounds.y, lineBounds.width, lineBounds.height);
    if (!intersection.intersect(unit.authoredBounds))
      continue;
    const float area = intersection.width() * intersection.height();
    if (area > maximumIntersection) {
      maximumIntersection = area;
      resolved = index;
    }
  }
  return resolved;
}


bool ApplyTextEffectVisualLineReflow(
    const text::ResolvedRichTextView &view,
    const text::TextEffectFramePlan &framePlan,
    const ResolvedLayout &baseLayout, ResolvedLayout &rebuiltLayout,
    std::string &error) {
  error.clear();
  if (framePlan.layoutMutations.empty() &&
      std::none_of(framePlan.units.begin(), framePlan.units.end(),
                   [](const auto &unit) {
                     return unit.absoluteFontSize.has_value();
                   })) {
    return true;
  }

  std::vector<TextEffectLayoutUnitBinding> bindings;
  bindings.reserve(rebuiltLayout.units.size());
  for (const auto &unit : rebuiltLayout.units)
    bindings.push_back(unit.binding);
  ResolvedTextEffectLayoutMutations mutations;
  if (!mutations.Resolve(framePlan, bindings, error)) {
    return false;
  }

  struct LineAccumulator final {
    SkRect baseLetterBounds{SkRect::MakeEmpty()};
    SkRect rebuiltLetterBounds{SkRect::MakeEmpty()};
  };
  std::vector<LineAccumulator> accumulators(
      rebuiltLayout.publicLayout.authoredLines.size());
  std::vector<TextEffectVisualLineFontSizeSample> samples;
  samples.reserve(rebuiltLayout.units.size());
  std::unordered_map<std::uint64_t, const UnitGeometry *> baseUnits;
  baseUnits.reserve(baseLayout.units.size());
  for (const auto &unit : baseLayout.units)
    baseUnits.try_emplace(unit.binding.stableUnitId, &unit);
  // Capture line membership before the geometry offsets below are applied.
  std::vector<std::size_t> visualLines;
  visualLines.reserve(rebuiltLayout.units.size());
  for (const auto &unit : rebuiltLayout.units) {
    const auto line = VisualLineForUnit(rebuiltLayout, unit);
    visualLines.push_back(line);
    if (line >= accumulators.size())
      continue;
    const auto base = baseUnits.find(unit.binding.stableUnitId);
    const auto *run = FindRun(view, unit.binding.paragraphId,
                              unit.binding.runId);
    if (base == baseUnits.end() || !run ||
        !std::isfinite(run->style.fontSize) || run->style.fontSize <= 0.0F) {
      continue;
    }
    const auto *mutation = mutations.Find(
        unit.binding.paragraphId, unit.binding.runId,
        unit.binding.utf8Begin, unit.binding.utf8End);
    std::optional<float> target;
    if (mutation && !mutation->absoluteFontSize.empty()) {
      target = ApplyTextEffectLayoutScalarOperations(
          run->style.fontSize, mutation->absoluteFontSize);
      if (!target) {
        error = "text effect visual-line font-size target is invalid";
        return false;
      }
    }
    samples.push_back({line, run->style.fontSize, target});
    accumulators[line].baseLetterBounds.join(base->second->scriptBounds);
    accumulators[line].rebuiltLetterBounds.join(unit.scriptBounds);
  }

  const auto scales = ResolveTextEffectVisualLineFontSizeScales(samples);
  if (!scales) {
    error = "text effect visual-line font-size scale resolution failed";
    return false;
  }
  std::vector<float> offsets(accumulators.size(), 0.0F);
  for (const auto &lineScale : *scales) {
    if (lineScale.visualLine >= accumulators.size())
      continue;
    const auto &line = accumulators[lineScale.visualLine];
    if (line.baseLetterBounds.isEmpty() ||
        line.rebuiltLetterBounds.isEmpty()) {
      continue;
    }
    const auto origin = ResolveTextEffectVisualLineOrigin(
        view.referenceCanvas.height * 0.5F,
        line.baseLetterBounds.centerY(), line.rebuiltLetterBounds.centerY(),
        lineScale.effectiveMaximumScale);
    if (!origin) {
      error = "text effect visual-line origin resolution failed";
      return false;
    }
    offsets[lineScale.visualLine] = origin->offsetY;
    if (std::getenv("VIDEOCUT_TRACE_TEXT_EFFECT") != nullptr) {
      std::fprintf(
          stderr,
          "[VIDEOCUT_TEXT_EFFECT_FONT_SIZE_LINE_ORIGIN] line=%zu "
          "scale=%.9g base=[%.9g %.9g %.9g %.9g] rebuilt=[%.9g %.9g "
          "%.9g %.9g] design_center_y=%.9g desired_center_y=%.9g "
          "offset_y=%.9g\n",
          lineScale.visualLine, lineScale.effectiveMaximumScale,
          line.baseLetterBounds.left(), line.baseLetterBounds.top(),
          line.baseLetterBounds.right(), line.baseLetterBounds.bottom(),
          line.rebuiltLetterBounds.left(), line.rebuiltLetterBounds.top(),
          line.rebuiltLetterBounds.right(), line.rebuiltLetterBounds.bottom(),
          view.referenceCanvas.height * 0.5F, origin->desiredCenterY,
          origin->offsetY);
    }
  }

  for (std::size_t index = 0U; index < rebuiltLayout.units.size(); ++index) {
    auto &unit = rebuiltLayout.units[index];
    const auto line = visualLines[index];
    if (line >= offsets.size() || offsets[line] == 0.0F)
      continue;
    const float offsetY = offsets[line];
    unit.visualReflowOffsetY = offsetY;
    unit.localBounds.offset(0.0F, offsetY);
    unit.localLayoutBounds.offset(0.0F, offsetY);
    unit.authoredBounds.offset(0.0F, offsetY);
    unit.layoutBounds.offset(0.0F, offsetY);
    for (auto &textBox : unit.textBoxes)
      textBox.bounds.offset(0.0F, offsetY);
    unit.tightInkBounds.offset(0.0F, offsetY);
    unit.scriptBounds.offset(0.0F, offsetY);
    unit.uniformTightAnchorBounds.offset(0.0F, offsetY);
    unit.letterInitialPosition.offset(0.0F, offsetY);
  }
  for (std::size_t index = 0U;
       index < rebuiltLayout.publicLayout.lines.size() &&
       index < offsets.size();
       ++index) {
    auto &line = rebuiltLayout.publicLayout.lines[index];
    line.bounds.y += offsets[index];
    line.baseline += offsets[index];
    if (index < rebuiltLayout.publicLayout.baselines.size())
      rebuiltLayout.publicLayout.baselines[index] += offsets[index];
  }
  using UnitRange =
      std::tuple<std::string_view, std::string_view, std::size_t, std::size_t>;
  // Bindings stay unchanged; retain the first unit for duplicate ranges.
  std::map<UnitRange, const UnitGeometry *> rebuiltUnits;
  for (const auto &unit : rebuiltLayout.units) {
    const auto &binding = unit.binding;
    rebuiltUnits.try_emplace(
        UnitRange{binding.paragraphId, binding.runId, binding.utf8Begin,
                  binding.utf8End},
        &unit);
  }
  for (auto &cluster : rebuiltLayout.publicLayout.clusters) {
    const auto unit = rebuiltUnits.find(
        {cluster.paragraphId, cluster.runId, cluster.utf8Begin, cluster.utf8End});
    if (unit == rebuiltUnits.end() ||
        unit->second->visualReflowOffsetY == 0.0F) {
      continue;
    }
    for (auto &box : cluster.boxes) {
      box.bounds.y += unit->second->visualReflowOffsetY;
      box.utf8BeginCaret.y += unit->second->visualReflowOffsetY;
      box.utf8EndCaret.y += unit->second->visualReflowOffsetY;
    }
  }
  rebuiltLayout.publicLayout.authoredLines =
      rebuiltLayout.publicLayout.lines;
  rebuiltLayout.publicLayout.authoredClusters =
      rebuiltLayout.publicLayout.clusters;
  RecomputeUniformTightAnchorBounds(view, rebuiltLayout);

  rebuiltLayout.logicalBounds = SkRect::MakeEmpty();
  for (const auto &line : rebuiltLayout.publicLayout.lines) {
    rebuiltLayout.logicalBounds.join(SkRect::MakeXYWH(
        line.bounds.x, line.bounds.y, line.bounds.width,
        line.bounds.height));
  }
  rebuiltLayout.inkBounds = SkRect::MakeEmpty();
  rebuiltLayout.glyphInkBounds = SkRect::MakeEmpty();
  SkRect rebuiltLetterBounds = SkRect::MakeEmpty();
  for (const auto &unit : rebuiltLayout.units) {
    rebuiltLayout.inkBounds.join(unit.authoredBounds);
    rebuiltLayout.glyphInkBounds.join(unit.tightInkBounds);
    rebuiltLetterBounds.join(unit.scriptBounds);
  }
  if (rebuiltLayout.logicalBounds.isEmpty())
    rebuiltLayout.logicalBounds = rebuiltLayout.inkBounds;
  if (rebuiltLayout.inkBounds.isEmpty())
    rebuiltLayout.inkBounds = rebuiltLayout.logicalBounds;
  rebuiltLayout.controlBounds = rebuiltLayout.inkBounds;
  rebuiltLayout.controlBounds.outset(2.0F, 2.0F);
  auto &published = rebuiltLayout.publicLayout;
  published.authoredLogicalBounds = {
      rebuiltLayout.logicalBounds.x(), rebuiltLayout.logicalBounds.y(),
      rebuiltLayout.logicalBounds.width(), rebuiltLayout.logicalBounds.height()};
  published.authoredInkBounds = {
      rebuiltLayout.inkBounds.x(), rebuiltLayout.inkBounds.y(),
      rebuiltLayout.inkBounds.width(), rebuiltLayout.inkBounds.height()};
  published.authoredLetterBounds = {
      rebuiltLetterBounds.x(), rebuiltLetterBounds.y(),
      rebuiltLetterBounds.width(), rebuiltLetterBounds.height()};
  published.authoredControlBounds = {
      rebuiltLayout.controlBounds.x(), rebuiltLayout.controlBounds.y(),
      rebuiltLayout.controlBounds.width(), rebuiltLayout.controlBounds.height()};
  published.logicalBounds = published.authoredLogicalBounds;
  published.inkBounds = published.authoredInkBounds;
  published.letterBounds = published.authoredLetterBounds;
  published.controlBounds = published.authoredControlBounds;
  return true;
}

} // namespace videocut::skia_runtime::internal::text_lane
