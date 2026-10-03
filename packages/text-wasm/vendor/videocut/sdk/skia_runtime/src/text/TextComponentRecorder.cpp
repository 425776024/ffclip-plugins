#include "text/TextRenderPipeline.h"

namespace videocut::skia_runtime::internal::text_lane {


bool RecordNativeSdfGlyphMaterialComponents(
    const text::ResolvedRichTextView &view,
    const text::TextLayerAppearance &appearance,
    const ResolvedLayout &layout,
    const TextRenderFramePlan &renderPlan, const FontContext &fonts,
    const std::int64_t localTimeUs, const SkRect &recordingBounds,
    const SkMatrix &presentationMatrix, const SkIRect &presentationCrop,
    SkiaGpuContext &gpuContext, TextVisualAssetStore &assets,
    std::vector<RenderComponent> &components,
    std::string &resourceIdentity, std::string &error) {
  const auto &framePlan = renderPlan.value();
  std::optional<GlyphPassLayoutState> layoutState;
  const auto passes = ResolveGlyphMaterialPasses(view, framePlan);
  const auto bridge = ResolveDocumentTextEffectBridge(view, appearance);
  const SkIRect componentBounds = presentationCrop;
  if (componentBounds.isEmpty()) {
    error = "text SDF component has empty recording bounds";
    return false;
  }
  SkMatrix inversePresentation;
  if (!presentationMatrix.invert(&inversePresentation)) {
    error = "text SDF presentation transform is not invertible";
    return false;
  }
  const SkM44 authoredToComponent =
      SkM44::Translate(-static_cast<float>(componentBounds.left()),
                       -static_cast<float>(componentBounds.top())) *
      SkM44(presentationMatrix);
  const SkM44 componentToAuthored =
      SkM44(inversePresentation) *
      SkM44::Translate(static_cast<float>(componentBounds.left()),
                       static_cast<float>(componentBounds.top()));
  const auto info = SkImageInfo::Make(
      componentBounds.width(), componentBounds.height(),
      kRGBA_8888_SkColorType, kPremul_SkAlphaType,
      SkColorSpace::MakeSRGB());
  IdentityBuilder resources;

  // Captured TextPro flower presets submit every Letter material layer to
  // one loaded RGBA8 presentation target and let fixed-function SourceOver
  // blend each draw in authored order.  Rendering each layer into a separate
  // transparent RGBA8 surface and compositing those snapshots later adds a
  // second quantization boundary per layer; the error compounds on deep
  // outline/glow stacks.  Preserve separately addressable components whenever
  // a post DAG, backdrop, or decoration can observe them.  Otherwise execute
  // the closed glyph stack directly in the shared target, exactly as the Qt
  // command stream does.  The fixed-30 base Letter contract remains on its
  // dedicated deferred native path below.
  constexpr float kNativeLetterRasterRange = 30.0F;
  constexpr float kNativeLetterRasterRangeTolerance = 0.0001F;
  const bool directGlyphStack =
      passes.size() > 1U && framePlan.postEffectNodes.empty() &&
      !ExecutionGraphRequiresExplicitRasterExecutor(
          framePlan.executionGraph.nodes) &&
      framePlan.decorationPasses.empty() &&
      std::none_of(appearance.backdrops.layers.begin(),
                   appearance.backdrops.layers.end(),
                   [](const auto &layer) { return layer.enabled; }) &&
      std::all_of(passes.begin(), passes.end(), [](const auto &pass) {
        return pass.blend == text::TextBlendMode::SourceOver;
      }) &&
      std::fabs(appearance.sdfMaterial.rasterDistanceRange -
                kNativeLetterRasterRange) >
          kNativeLetterRasterRangeTolerance;
  if (directGlyphStack) {
    SkRect authoredMaterialBounds = SkRect::MakeEmpty();
    auto surface = gpuContext.MakeSurface(info, error);
    if (!surface)
      return false;
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    IdentityBuilder identity;
    identity.AddString("qt-textpro-direct-glyph-stack-v1");
    identity.Add(appearance.sdfMaterial.sourceCreationComponent);
    identity.Add(appearance.sdfMaterial.distanceRange);
    identity.Add(appearance.sdfMaterial.rasterDistanceRange);
    identity.Add(appearance.sdfMaterial.smoothingScale);
    for (const auto &pass : passes) {
      auto renderPass = pass;
      const auto factoredLayerOpacity =
          FactorTextProRenderGroupLayerOpacity(renderPass.material);
      auto isolatedLayerSurface =
          factoredLayerOpacity ? gpuContext.MakeSurface(info, error) : nullptr;
      if (factoredLayerOpacity && !isolatedLayerSurface)
        return false;
      auto *passSurface =
          factoredLayerOpacity ? isolatedLayerSurface.get() : surface.get();
      if (isolatedLayerSurface)
        isolatedLayerSurface->getCanvas()->clear(SK_ColorTRANSPARENT);
      std::vector<std::string> assetSamples;
      for (std::size_t paragraphIndex = 0U;
           paragraphIndex < view.paragraphs.size(); ++paragraphIndex) {
        const auto &source = view.paragraphs[paragraphIndex];
        const auto &placement = layout.paragraphs[paragraphIndex];
        std::unique_ptr<Paragraph> paragraph;
        if (!BuildGlyphPassParagraph(
                source, renderPass, layout, framePlan, layoutState, fonts,
                localTimeUs, assets, paragraph, assetSamples, error, true) ||
            !RenderSdfParagraphMaterial(
                *paragraph, renderPass, layout, placement, renderPlan,
                appearance, bridge.sourceToReferenceScale,
                view.referenceCanvas.width, view.referenceCanvas.height,
                localTimeUs, authoredToComponent, gpuContext, *passSurface,
                assets, assetSamples, nullptr, authoredMaterialBounds, error)) {
          return false;
        }
      }
      if (isolatedLayerSurface) {
        auto layerImage = isolatedLayerSurface->makeImageSnapshot();
        if (!layerImage) {
          error = "text SDF isolated opacity layer snapshot failed";
          return false;
        }
        QtTextRenderGroupCompositeRequest opacityComposite;
        opacityComposite.presentationWidth = componentBounds.width();
        opacityComposite.presentationHeight = componentBounds.height();
        opacityComposite.mvp = {
            1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
            0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
        opacityComposite.customMatrix = opacityComposite.mvp;
        opacityComposite.alpha = *factoredLayerOpacity;
        // Qt writes the opaque Letter material into RGBA8 first, then its
        // RenderGroup fragment multiplies that stored sample by the authored
        // float alpha before the loaded RGBA8 target performs SourceOver.
        // SkPaint::setAlphaf is not that byte contract: its paint-color alpha
        // may be narrowed before the source RGB multiplication, changing a
        // valid 7/15 premultiplied channel into 6/15. Reuse the same captured
        // native RenderGroup pass used by Page publication so both paths keep
        // one operation order and one quantization boundary.
        if (!gpuContext.CompositeQtTextRenderGroup(
                layerImage, *surface, opacityComposite, error)) {
          return false;
        }
      }
      identity.AddString(pass.passId);
      identity.AddString(pass.layerId);
      identity.Add(pass.zOrder);
      identity.Add(static_cast<unsigned>(pass.blend));
      identity.Add(static_cast<unsigned>(pass.combineMode));
      for (const auto stableUnitId : pass.stableUnitIds)
        identity.Add(stableUnitId);
      for (const auto &sample : assetSamples) {
        identity.AddString(sample);
        resources.AddString(sample);
      }
    }
    auto image = surface->makeImageSnapshot();
    if (!image) {
      error = "text SDF direct glyph stack snapshot failed";
      return false;
    }
    SkPictureRecorder recorder;
    auto *canvas = recorder.beginRecording(recordingBounds);
    canvas->concat(componentToAuthored);
    canvas->drawImage(image, 0.0F, 0.0F);
    const auto &first = passes.front();
    components.push_back({"qt-textpro-direct-glyph-stack",
                          text::TextEffectCompositeItemKind::GlyphMaterial,
                          first.zOrder,
                          text::TextBlendMode::SourceOver,
                          recorder.finishRecordingAsPicture(),
                          identity.Finish(),
                          first.authoredOrder});
    components.back().authoredMaterialBounds = authoredMaterialBounds;
    resourceIdentity = resources.Finish();
    return true;
  }

  for (const auto &pass : passes) {
    SkRect authoredMaterialBounds = SkRect::MakeEmpty();
    auto renderPass = pass;
    const bool hasPageRenderGroup = std::any_of(
        framePlan.postEffectNodes.begin(), framePlan.postEffectNodes.end(),
        [](const auto &node) { return node.renderGroup.has_value(); });
    const auto factoredLayerOpacity =
        hasPageRenderGroup
            ? std::optional<float>{}
            : FactorTextProRenderGroupLayerOpacity(renderPass.material);
    auto nativePageSource =
        std::make_shared<QtTextNativePageSource>();
    auto surface = gpuContext.MakeSurface(info, error);
    if (!surface)
      return false;
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    std::vector<std::string> assetSamples;
    for (std::size_t paragraphIndex = 0U;
         paragraphIndex < view.paragraphs.size(); ++paragraphIndex) {
      const auto &source = view.paragraphs[paragraphIndex];
      const auto &placement = layout.paragraphs[paragraphIndex];
      // TextPro packs the complete paragraph/pass before applying each
      // Letter's animator matrix.  Keeping transformed units in one capture
      // preserves shared cell placement and the derivative/atlas phase; the
      // per-draw unit plan below still supplies independent transforms.
      std::unique_ptr<Paragraph> paragraph;
      if (!BuildGlyphPassParagraph(
              source, renderPass, layout, framePlan, layoutState, fonts,
              localTimeUs, assets, paragraph, assetSamples, error, true) ||
          !RenderSdfParagraphMaterial(
              *paragraph, renderPass, layout, placement, renderPlan,
              appearance, bridge.sourceToReferenceScale,
              view.referenceCanvas.width, view.referenceCanvas.height,
              localTimeUs,
              authoredToComponent, gpuContext, *surface, assets, assetSamples,
              nativePageSource.get(), authoredMaterialBounds, error)) {
        return false;
      }
    }
    auto image = surface->makeImageSnapshot();
    if (!image) {
      error = "text SDF component snapshot failed";
      return false;
    }
    SkPictureRecorder recorder;
    auto *canvas = recorder.beginRecording(recordingBounds);
    canvas->concat(componentToAuthored);
    if (factoredLayerOpacity) {
      SkPaint layerPaint;
      layerPaint.setAlphaf(*factoredLayerOpacity);
      canvas->drawImage(image, 0.0F, 0.0F, SkSamplingOptions(), &layerPaint);
    } else {
      canvas->drawImage(image, 0.0F, 0.0F);
    }
    auto picture = recorder.finishRecordingAsPicture();
    IdentityBuilder identity;
    identity.AddString(pass.passId);
    identity.AddString(pass.layerId);
    identity.Add(pass.zOrder);
    identity.Add(static_cast<unsigned>(pass.blend));
    identity.Add(static_cast<unsigned>(pass.combineMode));
    identity.Add(appearance.sdfMaterial.sourceCreationComponent);
    identity.Add(appearance.sdfMaterial.distanceRange);
    identity.Add(appearance.sdfMaterial.rasterDistanceRange);
    identity.Add(appearance.sdfMaterial.smoothingScale);
    for (const auto stableUnitId : pass.stableUnitIds)
      identity.Add(stableUnitId);
    for (const auto &sample : assetSamples) {
      identity.AddString(sample);
      resources.AddString(sample);
    }
    components.push_back({pass.passId,
                          text::TextEffectCompositeItemKind::GlyphMaterial,
                          pass.zOrder,
                          pass.blend,
                          std::move(picture),
                          identity.Finish(),
                          pass.authoredOrder,
                          (nativePageSource->batches.empty() ||
                           nativePageSource->containsRasterGlyphs)
                              ? nullptr
                              : std::move(nativePageSource)});
    components.back().authoredMaterialBounds = authoredMaterialBounds;
  }
  resourceIdentity = resources.Finish();
  return true;
}


bool RecordGlyphMaterialComponents(
    const text::ResolvedRichTextView &view,
    const text::TextLayerAppearance &appearance,
    const ResolvedLayout &layout,
    const TextRenderFramePlan &renderPlan, const FontContext &fonts,
    const std::int64_t localTimeUs, const SkRect &recordingBounds,
    const SkMatrix &presentationMatrix, const SkIRect &presentationCrop,
    SkiaGpuContext *gpuContext, TextVisualAssetStore &assets,
    std::vector<RenderComponent> &components,
    std::vector<Diagnostic> &diagnostics, std::string &resourceIdentity,
    std::string &error) {
  const auto &framePlan = renderPlan.value();
  if (appearance.sdfMaterial.enabled && gpuContext) {
    return RecordNativeSdfGlyphMaterialComponents(
        view, appearance, layout, renderPlan, fonts, localTimeUs,
        recordingBounds, presentationMatrix, presentationCrop, *gpuContext,
        assets, components, resourceIdentity, error);
  }
  const auto passes = ResolveGlyphMaterialPasses(view, framePlan);
  std::optional<GlyphPassLayoutState> layoutState;
  IdentityBuilder resources;
  for (const auto &pass : passes) {
    SkPictureRecorder recorder;
    auto *canvas = recorder.beginRecording(recordingBounds);
    std::vector<std::string> assetSamples;
    for (std::size_t paragraphIndex = 0U;
         paragraphIndex < view.paragraphs.size(); ++paragraphIndex) {
      const auto &source = view.paragraphs[paragraphIndex];
      const auto &placement = layout.paragraphs[paragraphIndex];
      const bool anyUnitEffect = std::any_of(
          placement.unitIndexes.begin(), placement.unitIndexes.end(),
          [&](const auto index) {
            const auto *unitPlan = renderPlan.FindUnit(
                layout.units[index].binding.stableUnitId);
            return unitPlan &&
                   (!unitPlan->transforms.empty() ||
                    unitPlan->opacity.has_value() ||
                    unitPlan->instanceColor.has_value() ||
                    (unitPlan->sdfBlurRadius &&
                     *unitPlan->sdfBlurRadius > 0.0F));
          });
      if (!anyUnitEffect) {
        // Animated units paint their independently shaped material paragraphs
        // below. Only build the whole-paragraph pass when it will be painted;
        // each unit path still resolves every glyph, mutation and resource.
        std::unique_ptr<Paragraph> paragraph;
        if (!BuildGlyphPassParagraph(
                source, pass, layout, framePlan, layoutState, fonts,
                localTimeUs, assets, paragraph, assetSamples, error)) {
          return false;
        }
        canvas->save();
        canvas->concat(layout.writingTransform);
        paragraph->paint(canvas, placement.x, placement.y);
        canvas->restore();
        continue;
      }
      for (const auto unitIndex : placement.unitIndexes) {
        const auto &unit = layout.units[unitIndex];
        if (!PassTargetsUnit(pass, unit))
          continue;
        auto unitPass = pass;
        unitPass.paragraphId.clear();
        unitPass.runId.clear();
        unitPass.stableUnitIds = {unit.binding.stableUnitId};
        std::unique_ptr<Paragraph> unitParagraph;
        if (!BuildGlyphPassParagraph(
                source, unitPass, layout, framePlan, layoutState, fonts,
                localTimeUs, assets, unitParagraph, assetSamples, error)) {
          return false;
        }
        const auto *unitPlan = renderPlan.FindUnit(unit.binding.stableUnitId);
        const int restoreCount =
            BeginUnitPaint(canvas);
        ApplyUnitTransforms(canvas, unitPlan);
        auto sliceBounds = unit.scriptBounds.isEmpty()
                               ? unit.authoredBounds
                               : unit.scriptBounds;
        // A per-unit paragraph contains only this unit's active material, so
        // its clip must include that material's complete visual support.
        // Clipping to the Letter rectangle drops legitimate stroke/shadow/
        // glow tails as soon as an otherwise-identity animator creates a unit
        // frame plan. Expanding every independently painted unit is safe:
        // neighbouring units remain transparent in this paragraph pass and
        // their own visual tails are composited exactly once.
        text::TextGlyphMaterialStack sliceMaterial;
        sliceMaterial.layers = {unitPass.material};
        const auto sliceOutsets = ResolveGlyphMaterialOutsets(sliceMaterial);
        sliceBounds.fLeft -= sliceOutsets.left;
        sliceBounds.fTop -= sliceOutsets.top;
        sliceBounds.fRight += sliceOutsets.right;
        sliceBounds.fBottom += sliceOutsets.bottom;
        canvas->clipRect(sliceBounds, SkClipOp::kIntersect, true);
        SkPaint layerPaint;
        const float opacity =
            unitPlan && unitPlan->opacity
                ? std::clamp(*unitPlan->opacity, 0.0F, 1.0F)
                : 1.0F;
        layerPaint.setAlphaf(opacity);
        if (unitPlan && unitPlan->instanceColor) {
          layerPaint.setColorFilter(SkColorFilters::Blend(
              ToSkColor(*unitPlan->instanceColor), SkBlendMode::kSrcIn));
        }
        const float blur =
            unitPlan && unitPlan->sdfBlurRadius
                ? std::clamp(*unitPlan->sdfBlurRadius, 0.0F, 512.0F)
                : 0.0F;
        if (blur > 0.001F) {
          layerPaint.setImageFilter(SkImageFilters::Blur(
              blur, blur, SkTileMode::kDecal, nullptr));
        }
        const bool needsLayer =
            opacity < 0.999999F ||
            (unitPlan && unitPlan->instanceColor.has_value()) ||
            blur > 0.001F;
        if (needsLayer)
          canvas->saveLayer(sliceBounds, &layerPaint);
        canvas->concat(layout.writingTransform);
        unitParagraph->paint(canvas, placement.x, placement.y);
        if (needsLayer)
          canvas->restore();
        canvas->restoreToCount(restoreCount);
      }
    }
    auto picture = recorder.finishRecordingAsPicture();
    IdentityBuilder identity;
    identity.AddString(pass.passId);
    identity.AddString(pass.layerId);
    identity.Add(pass.zOrder);
    identity.Add(static_cast<unsigned>(pass.blend));
    identity.Add(static_cast<unsigned>(pass.combineMode));
    for (const auto stableUnitId : pass.stableUnitIds)
      identity.Add(stableUnitId);
    for (const auto &sample : assetSamples) {
      identity.AddString(sample);
      resources.AddString(sample);
    }
    components.push_back({pass.passId,
                          text::TextEffectCompositeItemKind::GlyphMaterial,
                          pass.zOrder,
                          pass.blend,
                          std::move(picture),
                          identity.Finish(),
                          pass.authoredOrder});
  }
  resourceIdentity = resources.Finish();
  static_cast<void>(diagnostics);
  return true;
}


void DrawTextBoxBackground(SkCanvas *canvas,
                           const text::TextBoxBackground &background,
                           const SkRect &sourceBounds,
                           const float opacity) {
  if (!canvas || !background.enabled || background.color.alpha <= 0.0F ||
      sourceBounds.isEmpty() || opacity <= 0.0F)
    return;
  SkPaint paint;
  paint.setAntiAlias(true);
  auto color = background.color;
  color.alpha *= std::clamp(opacity, 0.0F, 1.0F);
  paint.setColor(ToSkColor(color));
  const auto bounds = ExpandByInsets(sourceBounds, background.padding);
  canvas->drawRoundRect(bounds, std::max(0.0F, background.cornerRadius),
                        std::max(0.0F, background.cornerRadius), paint);
}


sk_sp<SkPicture> RecordSemanticBackgrounds(
    const text::ResolvedRichTextView &view, const ResolvedLayout &layout,
    const TextRenderFramePlan &renderPlan,
    const SkRect &recordingBounds) {
  SkPictureRecorder recorder;
  auto *canvas = recorder.beginRecording(recordingBounds);
  bool hasBackground = false;
  const auto drawAnimatedBackground =
      [&](const text::TextBoxBackground &background,
          const SkRect &backgroundBounds,
          const auto &acceptUnit) {
        if (!background.enabled || background.color.alpha <= 0.0F ||
            backgroundBounds.isEmpty()) {
          return;
        }
        hasBackground = true;
        for (const auto &unit : layout.units) {
          if (!acceptUnit(unit))
            continue;
          const auto *unitPlan =
              renderPlan.FindUnit(unit.binding.stableUnitId);
          const float opacity =
              unitPlan && unitPlan->opacity
                  ? std::clamp(*unitPlan->opacity, 0.0F, 1.0F)
                  : 1.0F;
          if (opacity <= 0.0F)
            continue;
          auto clipBounds = unit.layoutBounds.isEmpty()
                                ? unit.authoredBounds
                                : unit.layoutBounds;
          if (clipBounds.isEmpty())
            continue;
          constexpr float kEdgeTolerance = 0.01F;
          if (clipBounds.left() <= backgroundBounds.left() +
                                       background.padding.left +
                                       kEdgeTolerance) {
            clipBounds.fLeft -= background.padding.left;
          }
          if (clipBounds.top() <= backgroundBounds.top() +
                                      background.padding.top +
                                      kEdgeTolerance) {
            clipBounds.fTop -= background.padding.top;
          }
          if (clipBounds.right() >= backgroundBounds.right() -
                                        background.padding.right -
                                        kEdgeTolerance) {
            clipBounds.fRight += background.padding.right;
          }
          if (clipBounds.bottom() >= backgroundBounds.bottom() -
                                         background.padding.bottom -
                                         kEdgeTolerance) {
            clipBounds.fBottom += background.padding.bottom;
          }
          const int restoreCount = BeginUnitPaint(canvas);
          ApplyUnitTransforms(canvas, unitPlan);
          canvas->clipRect(clipBounds, SkClipOp::kIntersect, true);
          if (opacity < 0.9999F) {
            SkPaint layerOpacity;
            layerOpacity.setAlphaf(opacity);
            canvas->saveLayer(&backgroundBounds, &layerOpacity);
          }
          DrawTextBoxBackground(canvas, background, backgroundBounds);
          if (opacity < 0.9999F)
            canvas->restore();
          canvas->restoreToCount(restoreCount);
        }
      };
  for (const auto &paragraphLayout : layout.paragraphs) {
    if (!paragraphLayout.source)
      continue;
    SkRect paragraphBounds = SkRect::MakeEmpty();
    for (const auto unitIndex : paragraphLayout.unitIndexes)
      paragraphBounds.join(layout.units[unitIndex].authoredBounds);
    drawAnimatedBackground(
        paragraphLayout.source->style.background, paragraphBounds,
        [&](const UnitGeometry &unit) {
          return unit.binding.paragraphId ==
                 paragraphLayout.source->paragraphId;
        });
    for (const auto &run : paragraphLayout.source->runs) {
      std::vector<SkRect> runFragments;
      for (const auto unitIndex : paragraphLayout.unitIndexes) {
        const auto &unit = layout.units[unitIndex];
        if (unit.binding.runId != run.runId)
          continue;
        if (unit.textBoxes.empty()) {
          runFragments.push_back(unit.authoredBounds);
          continue;
        }
        for (const auto &textBox : unit.textBoxes)
          runFragments.push_back(textBox.bounds);
      }
      std::stable_sort(runFragments.begin(), runFragments.end(),
                       [](const SkRect &left, const SkRect &right) {
                         if (left.top() != right.top())
                           return left.top() < right.top();
                         return left.left() < right.left();
                       });
      std::vector<SkRect> lineFragments;
      for (const auto &fragment : runFragments) {
        if (fragment.isEmpty())
          continue;
        auto line = std::find_if(
            lineFragments.begin(), lineFragments.end(),
            [&](const SkRect &candidate) {
              return std::min(candidate.bottom(), fragment.bottom()) >
                     std::max(candidate.top(), fragment.top());
            });
        if (line == lineFragments.end())
          lineFragments.push_back(fragment);
        else
          line->join(fragment);
      }
      for (const auto &runBounds : lineFragments) {
        drawAnimatedBackground(
            run.style.background, runBounds,
            [&](const UnitGeometry &unit) {
              return unit.binding.paragraphId ==
                         paragraphLayout.source->paragraphId &&
                     unit.binding.runId == run.runId &&
                     SkRect::Intersects(unit.authoredBounds, runBounds);
            });
      }
    }
  }
  static_cast<void>(view);
  auto picture = recorder.finishRecordingAsPicture();
  return hasBackground ? std::move(picture) : nullptr;
}


void DrawDecorationStroke(SkCanvas *canvas,
                          const text::TextDecorationLine &line,
                          const text::TextWritingMode writingMode,
                          const SkRect &unitBounds, SkPaint paint) {
  if (!canvas || !line.enabled || unitBounds.isEmpty())
    return;
  const float thickness = std::max(0.25F, line.thickness);
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(thickness);
  paint.setStrokeCap(line.style == text::TextDecorationLineStyle::Dotted
                         ? SkPaint::kRound_Cap
                         : SkPaint::kButt_Cap);
  const bool vertical = writingMode != text::TextWritingMode::Horizontal;
  const float start = vertical ? unitBounds.top() : unitBounds.left();
  const float end = vertical ? unitBounds.bottom() : unitBounds.right();
  const float cross = vertical ? unitBounds.centerX() + line.offset
                               : unitBounds.bottom() + line.offset;
  const auto point = [&](const float along, const float normal) {
    return vertical ? SkPoint::Make(normal, along)
                    : SkPoint::Make(along, normal);
  };
  const auto drawStraight = [&](const float normal) {
    canvas->drawLine(point(start, normal), point(end, normal), paint);
  };
  if (line.style == text::TextDecorationLineStyle::Solid) {
    drawStraight(cross);
    return;
  }
  if (line.style == text::TextDecorationLineStyle::Double) {
    drawStraight(cross - thickness);
    drawStraight(cross + thickness);
    return;
  }
  if (line.style == text::TextDecorationLineStyle::Dotted ||
      line.style == text::TextDecorationLineStyle::Dashed) {
    const float on = line.style == text::TextDecorationLineStyle::Dotted
                         ? thickness
                         : thickness * 3.0F;
    const float off = line.style == text::TextDecorationLineStyle::Dotted
                          ? thickness * 1.5F
                          : thickness * 2.0F;
    for (float cursor = start; cursor < end; cursor += on + off) {
      canvas->drawLine(point(cursor, cross),
                       point(std::min(end, cursor + on), cross), paint);
    }
    return;
  }
  SkPathBuilder path;
  const float wavelength = std::max(3.0F, thickness * 4.0F);
  const float amplitude = std::max(1.0F, thickness);
  path.moveTo(point(start, cross));
  for (float cursor = start + wavelength * 0.25F; cursor <= end;
       cursor += wavelength * 0.25F) {
    const float phase = (cursor - start) / wavelength *
                        2.0F * 3.14159265358979323846F;
    path.lineTo(point(std::min(cursor, end),
                      cross + std::sin(phase) * amplitude));
  }
  canvas->drawPath(path.detach(), paint);
}


bool RecordInlineDecorationComponents(
    const text::ResolvedRichTextView &view, const ResolvedLayout &layout,
    const TextRenderFramePlan &renderPlan,
    const std::int64_t localTimeUs, const SkRect &recordingBounds,
    TextVisualAssetStore &assets, std::vector<RenderComponent> &components,
    std::string &identity, std::string &error) {
  IdentityBuilder cacheIdentity;
  std::size_t authoredOrder = components.size();
  for (const auto &paragraph : view.paragraphs) {
    for (const auto &run : paragraph.runs) {
      const std::array<std::pair<const char *, const text::TextDecorationLine *>,
                       2>
          lines{{{"underline", &run.style.decoration.underline},
                 {"strike", &run.style.decoration.strikeThrough}}};
      for (const auto &[name, line] : lines) {
        if (!line || !line->enabled)
          continue;
        SkPictureRecorder recorder;
        auto *canvas = recorder.beginRecording(recordingBounds);
        std::string materialSample;
        for (const auto &unit : layout.units) {
          if (unit.binding.paragraphId != paragraph.paragraphId ||
              unit.binding.runId != run.runId)
            continue;
          SkPaint paint;
          std::string unitSample;
          if (!ConfigureTextMaterialPaint(
                  line->material, layout.logicalBounds, layout.inkBounds,
                  unit.authoredBounds, localTimeUs, assets, paint, unitSample,
                  error)) {
            return false;
          }
          const auto *unitPlan = renderPlan.FindUnit(unit.binding.stableUnitId);
          if (unitPlan && unitPlan->opacity) {
            paint.setAlphaf(paint.getAlphaf() *
                            std::clamp(*unitPlan->opacity, 0.0F, 1.0F));
          }
          const int restoreCount =
              BeginUnitPaint(canvas);
          ApplyUnitTransforms(canvas, unitPlan);
          if (line->skipInk) {
            auto ink = unit.authoredBounds;
            ink.inset(std::max(0.0F, line->thickness * 0.25F), 0.0F);
            canvas->clipRect(ink, SkClipOp::kDifference, true);
          }
          auto decorationBounds = unit.authoredBounds;
          if (std::string_view{name} == "strike" &&
              view.writingMode == text::TextWritingMode::Horizontal) {
            const float shift = decorationBounds.centerY() -
                                decorationBounds.bottom();
            decorationBounds.offset(0.0F, shift);
          } else if (std::string_view{name} == "underline" &&
                     view.writingMode != text::TextWritingMode::Horizontal) {
            const float side =
                view.writingMode == text::TextWritingMode::VerticalRightToLeft
                    ? decorationBounds.left()
                    : decorationBounds.right();
            decorationBounds.offset(side - decorationBounds.centerX(), 0.0F);
          }
          DrawDecorationStroke(canvas, *line, view.writingMode,
                               decorationBounds, std::move(paint));
          canvas->restoreToCount(restoreCount);
          if (!unitSample.empty())
            materialSample += "|" + unitSample;
        }
        const std::string componentId = "inline-decoration:" +
                                        paragraph.paragraphId + ":" +
                                        run.runId + ":" + name;
        IdentityBuilder componentIdentity;
        componentIdentity.AddString(componentId);
        componentIdentity.Add(line->thickness);
        componentIdentity.Add(line->offset);
        componentIdentity.Add(static_cast<unsigned>(line->style));
        componentIdentity.Add(line->skipInk);
        componentIdentity.AddString(materialSample);
        const auto resolvedIdentity = componentIdentity.Finish();
        cacheIdentity.AddString(resolvedIdentity);
        components.push_back({componentId,
                              text::TextEffectCompositeItemKind::GlyphMaterial,
                              1,
                              text::TextBlendMode::SourceOver,
                              recorder.finishRecordingAsPicture(),
                              resolvedIdentity,
                              authoredOrder++});
      }
    }
  }
  identity = cacheIdentity.Finish();
  return true;
}


SkRect BackdropFitBounds(const ResolvedLayout &layout,
                         const text::TextBackdropFitPolicy fit) {
  if (fit == text::TextBackdropFitPolicy::LayoutBounds)
    return layout.logicalBounds;
  if (fit == text::TextBackdropFitPolicy::LineUnion) {
    SkRect result = SkRect::MakeEmpty();
    for (const auto &line : layout.publicLayout.authoredLines)
      result.join(ToSkRect(line.bounds));
    if (!result.isEmpty())
      return result;
  }
  return layout.glyphInkBounds.isEmpty() ? layout.inkBounds
                                         : layout.glyphInkBounds;
}


SkRect ApplyAuthoredExtent(const SkRect &source,
                           const text::TextBackdropSource &backdrop,
                           const text::TextAlignment horizontalAlignment,
                           const text::TextDirection direction,
                           const bool preserveQtTypesettingHeadroom) {
  if (const auto *roundRect =
          std::get_if<text::RoundedRectBackdrop>(&backdrop)) {
    auto result = source;
    if (roundRect->authoredWidth)
      result = SkRect::MakeXYWH(result.centerX() - *roundRect->authoredWidth *
                                                      0.5F,
                                result.top(), *roundRect->authoredWidth,
                                result.height());
    if (roundRect->authoredHeight)
      result = SkRect::MakeXYWH(result.left(),
                                result.centerY() - *roundRect->authoredHeight *
                                                       0.5F,
                                result.width(), *roundRect->authoredHeight);
    return result;
  }
  if (const auto *nineSlice =
          std::get_if<text::NineSliceBackdrop>(&backdrop)) {
    auto result = source;
    // TextPro's typesetting feedback reserves five percent before accepting a
    // line fit (`available * 0.95 / measured`).  Keep that relative headroom
    // at the shape boundary as well: otherwise sub-pixel font corrections can
    // cross the minimum-content edge and make a nine-slice grow by a fraction
    // of a pixel even though Qt keeps the authored shape extent.  Once content
    // exceeds the headroom it remains free to grow, so editable text is not
    // frozen to the fixture size.
    constexpr float kQtTypesettingFitFraction = 0.95F;
    const auto fittedContentExtent = [&](const float measured,
                                         const float minimum) {
      if (!preserveQtTypesettingHeadroom || minimum <= 0.0F)
        return std::max(measured, minimum);
      const float growthThreshold = minimum / kQtTypesettingFitFraction;
      return measured <= growthThreshold ? minimum : measured;
    };
    const float widthAdjustment =
        fittedContentExtent(result.width(), nineSlice->minimumContentWidth) -
        result.width();
    const float heightAdjustment =
        fittedContentExtent(result.height(), nineSlice->minimumContentHeight) -
        result.height();
    const bool rightToLeft = direction == text::TextDirection::RightToLeft;
    switch (horizontalAlignment) {
    case text::TextAlignment::Start:
    case text::TextAlignment::Justify:
      if (rightToLeft)
        result.fLeft -= widthAdjustment;
      else
        result.fRight += widthAdjustment;
      break;
    case text::TextAlignment::End:
      if (rightToLeft)
        result.fRight += widthAdjustment;
      else
        result.fLeft -= widthAdjustment;
      break;
    case text::TextAlignment::Center:
      result.fLeft -= widthAdjustment * 0.5F;
      result.fRight += widthAdjustment * 0.5F;
      break;
    }
    result.fTop -= heightAdjustment * 0.5F;
    result.fBottom += heightAdjustment * 0.5F;
    result.fLeft -= nineSlice->contentInsets.left;
    result.fTop -= nineSlice->contentInsets.top;
    result.fRight += nineSlice->contentInsets.right;
    result.fBottom += nineSlice->contentInsets.bottom;
    return result;
  }
  if (const auto *animated =
          std::get_if<text::AnimatedBackdrop>(&backdrop)) {
    auto result = source;
    result.fLeft -= animated->contentInsets.left;
    result.fTop -= animated->contentInsets.top;
    result.fRight += animated->contentInsets.right;
    result.fBottom += animated->contentInsets.bottom;
    return result;
  }
  return source;
}


SkPath MakeRoundedBackdropPath(const text::RoundedRectBackdrop &source,
                               const SkRect &bounds) {
  SkPathBuilder builder;
  builder.addRRect(SkRRect::MakeRectXY(bounds,
                                      std::max(0.0F, source.cornerRadius),
                                      std::max(0.0F, source.cornerRadius)));
  const auto &tail = source.tail;
  const float position = std::clamp(tail.position, 0.0F, 1.0F);
  if (tail.edge == text::BubbleTailEdge::Top ||
      tail.edge == text::BubbleTailEdge::Bottom) {
    const float x = bounds.left() + position * bounds.width();
    const float baseY = tail.edge == text::BubbleTailEdge::Top
                            ? bounds.top()
                            : bounds.bottom();
    const float tipY = baseY +
                       (tail.edge == text::BubbleTailEdge::Top
                            ? -std::max(0.0F, tail.length)
                            : std::max(0.0F, tail.length));
    builder.moveTo(x - tail.width * 0.5F, baseY);
    builder.lineTo(x, tipY);
    builder.lineTo(x + tail.width * 0.5F, baseY);
    builder.close();
  } else if (tail.edge == text::BubbleTailEdge::Left ||
             tail.edge == text::BubbleTailEdge::Right) {
    const float y = bounds.top() + position * bounds.height();
    const float baseX = tail.edge == text::BubbleTailEdge::Left
                            ? bounds.left()
                            : bounds.right();
    const float tipX = baseX +
                       (tail.edge == text::BubbleTailEdge::Left
                            ? -std::max(0.0F, tail.length)
                            : std::max(0.0F, tail.length));
    builder.moveTo(baseX, y - tail.width * 0.5F);
    builder.lineTo(tipX, y);
    builder.lineTo(baseX, y + tail.width * 0.5F);
    builder.close();
  }
  return builder.detach();
}


namespace {

template <typename DrawPatch>
std::size_t DrawImageNinePatches(
    const SkImage *image, const SkIRect &sourceCenter,
    const SkRect &destination, const text::Insets &destinationInsets,
    const DrawPatch &drawPatch) {
  std::size_t draws = 0U;
  float left = std::max(0.0F, destinationInsets.left);
  float right = std::max(0.0F, destinationInsets.right);
  float top = std::max(0.0F, destinationInsets.top);
  float bottom = std::max(0.0F, destinationInsets.bottom);
  if (const float fixedWidth = left + right;
      fixedWidth > destination.width() && fixedWidth > 0.0F) {
    const float scale = destination.width() / fixedWidth;
    left *= scale;
    right *= scale;
  }
  if (const float fixedHeight = top + bottom;
      fixedHeight > destination.height() && fixedHeight > 0.0F) {
    const float scale = destination.height() / fixedHeight;
    top *= scale;
    bottom *= scale;
  }
  const std::array<float, 4> sourceXs{
      0.0F, static_cast<float>(sourceCenter.left()),
      static_cast<float>(sourceCenter.right()),
      static_cast<float>(image->width())};
  const std::array<float, 4> sourceYs{
      0.0F, static_cast<float>(sourceCenter.top()),
      static_cast<float>(sourceCenter.bottom()),
      static_cast<float>(image->height())};
  const std::array<float, 4> targetXs{destination.left(),
                                      destination.left() + left,
                                      destination.right() - right,
                                      destination.right()};
  const std::array<float, 4> targetYs{destination.top(),
                                      destination.top() + top,
                                      destination.bottom() - bottom,
                                      destination.bottom()};
  for (std::size_t row = 0U; row < 3U; ++row) {
    for (std::size_t column = 0U; column < 3U; ++column) {
      const auto source = SkRect::MakeLTRB(
          sourceXs[column], sourceYs[row], sourceXs[column + 1U],
          sourceYs[row + 1U]);
      const auto target = SkRect::MakeLTRB(
          targetXs[column], targetYs[row], targetXs[column + 1U],
          targetYs[row + 1U]);
      if (source.isEmpty() || target.isEmpty())
        continue;
      draws += drawPatch(source, target, row, column);
    }
  }
  return draws;
}

} // namespace

std::size_t DrawImageNineWithDestinationInsets(
    SkCanvas *canvas, const SkImage *image, const SkIRect &sourceCenter,
    const SkRect &destination, const text::Insets &destinationInsets,
    const SkSamplingOptions &sampling, const SkPaint &paint) {
  if (!canvas || !image || destination.isEmpty())
    return 0U;
  return DrawImageNinePatches(
      image, sourceCenter, destination, destinationInsets,
      [&](const SkRect &source, const SkRect &target, std::size_t, std::size_t) {
        canvas->drawImageRect(image, source, target, sampling, &paint,
                              SkCanvas::kStrict_SrcRectConstraint);
        return 1U;
      });
}


std::size_t DrawImageNineTiled(
    SkCanvas *canvas, const SkImage *image, const SkIRect &sourceCenter,
    const SkRect &destination, const text::Insets &destinationInsets,
    const float sourceScaleX, const float sourceScaleY,
    const SkSamplingOptions &sampling, const SkPaint &paint) {
  if (!canvas || !image || destination.isEmpty() || sourceScaleX <= 0.0F ||
      sourceScaleY <= 0.0F)
    return 0U;
  return DrawImageNinePatches(
      image, sourceCenter, destination, destinationInsets,
      [&](const SkRect &source, const SkRect &target, const std::size_t row,
          const std::size_t column) -> std::size_t {
        const bool repeatX = column == 1U;
        const bool repeatY = row == 1U;
        const float tileWidth =
            repeatX ? source.width() * sourceScaleX : target.width();
        const float tileHeight =
            repeatY ? source.height() * sourceScaleY : target.height();
        if (tileWidth <= 0.0F || tileHeight <= 0.0F)
          return 0U;
        std::size_t draws = 0U;
        float firstTileHeight = tileHeight;
        if (repeatY) {
          firstTileHeight = std::fmod(target.height(), tileHeight);
          if (firstTileHeight <= 0.0001F)
            firstTileHeight = tileHeight;
        }
        bool firstTileRow = true;
        for (float y = target.top(); y < target.bottom();) {
          const float drawnHeight = std::min(
              firstTileRow ? firstTileHeight : tileHeight,
              target.bottom() - y);
          for (float x = target.left(); x < target.right(); x += tileWidth) {
            const float drawnWidth = std::min(tileWidth, target.right() - x);
            auto clippedSource = source;
            if (repeatX)
              clippedSource.fRight =
                  source.left() + drawnWidth / sourceScaleX;
            if (repeatY) {
              if (firstTileRow && firstTileHeight < tileHeight) {
                clippedSource.fTop =
                    source.bottom() - drawnHeight / sourceScaleY;
              } else {
                clippedSource.fBottom =
                    source.top() + drawnHeight / sourceScaleY;
              }
            }
            const auto clippedTarget =
                SkRect::MakeXYWH(x, y, drawnWidth, drawnHeight);
            canvas->drawImageRect(image, clippedSource, clippedTarget, sampling,
                                  &paint, SkCanvas::kStrict_SrcRectConstraint);
            ++draws;
            if (!repeatX)
              break;
          }
          if (!repeatY)
            break;
          y += drawnHeight;
          firstTileRow = false;
        }
        return draws;
      });
}


std::int64_t PositiveModulo(const std::int64_t value,
                            const std::int64_t modulus) noexcept {
  if (modulus <= 0)
    return 0;
  const auto remainder = value % modulus;
  return remainder < 0 ? remainder + modulus : remainder;
}


std::int64_t ResolveAnimatedBackdropTime(
    const text::AnimatedBackdrop &source, const std::int64_t localTimeUs,
    const text::TextEffectFramePlan &framePlan,
    const std::string &layerId) noexcept {
  const auto begin = std::min(source.sourceInUs, source.sourceOutUs);
  const auto end = std::max(source.sourceInUs, source.sourceOutUs);
  const auto duration = std::max<std::int64_t>(1, end - begin);
  std::int64_t clock = localTimeUs + source.phaseUs;
  if (source.timeSource == text::TextBackdropTimeSource::Source) {
    const auto resource = std::find_if(
        framePlan.resources.begin(), framePlan.resources.end(),
        [&](const auto &sample) {
          return sample.resourceId == layerId ||
                 sample.assetId == source.asset.assetId;
        });
    if (resource != framePlan.resources.end())
      clock = resource->localTimeUs - begin;
  } else if (source.timeSource ==
             text::TextBackdropTimeSource::AnimationLayer) {
    const auto transition = std::find_if(
        framePlan.stateTransitions.begin(), framePlan.stateTransitions.end(),
        [&](const auto &candidate) { return candidate.stateId == layerId; });
    if (transition != framePlan.stateTransitions.end()) {
      clock = static_cast<std::int64_t>(std::llround(
          std::clamp(transition->progress, 0.0, 1.0) *
          static_cast<double>(duration)));
    }
  }
  switch (source.playback) {
  case text::TextBackdropPlaybackMode::Once:
  case text::TextBackdropPlaybackMode::Hold:
    // Source-out is an exclusive bound.  Sampling exactly at the end would
    // ask Skottie/animated-image providers for a frame past their admitted
    // range (and disagrees with ResolveDecorationAssetTime's duration-1
    // contract), so hold on the final valid tick.
    return begin + std::clamp<std::int64_t>(clock, 0, duration - 1);
  case text::TextBackdropPlaybackMode::Loop:
    return begin + PositiveModulo(clock, duration);
  case text::TextBackdropPlaybackMode::PingPong: {
    const auto cycle = PositiveModulo(clock, duration * 2);
    return begin + (cycle <= duration ? cycle : duration * 2 - cycle);
  }
  }
  return begin;
}


SkRect TransformBackdropBounds(const SkRect &bounds,
                               const text::TextBackdropTransform &transform,
                               const SkRect &anchorBounds) {
  SkMatrix matrix;
  matrix.setTranslate(transform.offsetX, transform.offsetY);
  matrix.preTranslate(anchorBounds.centerX(), anchorBounds.centerY());
  matrix.preRotate(transform.rotationDegrees);
  matrix.preScale(transform.scaleX, transform.scaleY);
  matrix.preTranslate(-anchorBounds.centerX(), -anchorBounds.centerY());
  return MapRect(matrix, bounds);
}


SkRect TransformBackdropBounds(
    const SkRect &bounds, const text::TextBackdropTransform &transform) {
  return TransformBackdropBounds(bounds, transform, bounds);
}


void ApplyBackdropTransform(SkCanvas *canvas, const SkRect &bounds,
                            const text::TextBackdropTransform &transform) {
  canvas->translate(transform.offsetX, transform.offsetY);
  canvas->translate(bounds.centerX(), bounds.centerY());
  if (transform.rotationDegrees != 0.0F)
    canvas->rotate(transform.rotationDegrees);
  canvas->scale(transform.scaleX, transform.scaleY);
  canvas->translate(-bounds.centerX(), -bounds.centerY());
}


bool DrawResolvedVisualAsset(SkCanvas *canvas, ResolvedVisualAsset &asset,
                             const SkRect &bounds,
                             const std::int64_t sourceTimeUs,
                             TextVisualAssetStore &assets,
                             std::string &sampleIdentity,
                             std::string &error) {
  const auto orientation = asset.reference.orientation;
  const bool quarterTurn = orientation == text::TextureOrientation::Right ||
                           orientation == text::TextureOrientation::Left;
  const float orientationDegrees =
      orientation == text::TextureOrientation::Right
          ? 90.0F
      : orientation == text::TextureOrientation::Down
          ? 180.0F
      : orientation == text::TextureOrientation::Left
          ? -90.0F
          : 0.0F;
  const auto orientedBounds =
      quarterTurn
          ? SkRect::MakeXYWH(bounds.centerX() - bounds.height() * 0.5F,
                             bounds.centerY() - bounds.width() * 0.5F,
                             bounds.height(), bounds.width())
          : bounds;
  canvas->save();
  if (orientationDegrees != 0.0F) {
    canvas->rotate(orientationDegrees, bounds.centerX(), bounds.centerY());
  }
  if (asset.svg) {
    canvas->clipRect(orientedBounds);
    canvas->translate(orientedBounds.left(), orientedBounds.top());
    canvas->scale(orientedBounds.width() /
                      std::max(1.0F, asset.intrinsicWidth),
                  orientedBounds.height() /
                      std::max(1.0F, asset.intrinsicHeight));
    asset.svg->render(canvas);
    canvas->restore();
    sampleIdentity = TextureKey(asset.reference);
    return true;
  }
  if (asset.immutableLottieResources &&
      !asset.qtFollowerResourceId.empty()) {
    auto image = assets.SampleQtFollowerDirectRaster(
        asset, sourceTimeUs, sampleIdentity, error);
    if (!image) {
      canvas->restore();
      return false;
    }
    SkMatrix imageToBounds;
    imageToBounds.setRectToRect(
        SkRect::MakeIWH(image->width(), image->height()), orientedBounds,
        SkMatrix::kFill_ScaleToFit);
    SkPaint rawAssociated;
    rawAssociated.setShader(image->makeRawShader(
        SkTileMode::kClamp, SkTileMode::kClamp,
        SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone),
        &imageToBounds));
    canvas->drawRect(orientedBounds, rawAssociated);
    canvas->restore();
    return true;
  }
  if (asset.lottie) {
    asset.lottie->seekFrameTime(static_cast<double>(sourceTimeUs) /
                                1'000'000.0);
    asset.lottie->render(canvas, &orientedBounds);
    canvas->restore();
    sampleIdentity = TextureKey(asset.reference) + "|lottie:" +
                     std::to_string(sourceTimeUs);
    return true;
  }
  auto image = assets.SampleRaster(asset, sourceTimeUs, sampleIdentity, error);
  if (!image)
  {
    canvas->restore();
    return false;
  }
  canvas->drawImageRect(image.get(), orientedBounds,
                        SkSamplingOptions(SkFilterMode::kLinear,
                                          SkMipmapMode::kLinear),
                        nullptr);
  canvas->restore();
  return true;
}


bool DrawBackdropSource(
    SkCanvas *canvas, const text::TextBackdropSource &source,
    const SkRect &bounds, const std::int64_t sourceTimeUs,
    const text::TextBackdropChannel channel,
    const text::TextEffectSize &sourceIntrinsicSize,
    TextVisualAssetStore &assets,
    std::string &sampleIdentity,
    std::vector<Diagnostic> &diagnostics, const std::string &subject,
    std::string &error,
    std::vector<text::TextMaterialExecutionEvidence> *recordedDraws) {
  bool success = true;
  std::visit(
      [&](const auto &value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, text::RoundedRectBackdrop>) {
          const auto path = MakeRoundedBackdropPath(value, bounds);
          SkPaint fill;
          if (!ConfigureTextMaterialPaint(value.fill, bounds, bounds, bounds,
                                          sourceTimeUs, assets, fill,
                                          sampleIdentity, error)) {
            success = false;
            return;
          }
          canvas->drawPath(path, fill);
          for (const auto &stroke : value.strokes) {
            SkPaint paint;
            std::string strokeIdentity;
            if (!ConfigureTextMaterialPaint(stroke.material, bounds, bounds,
                                            bounds, sourceTimeUs, assets, paint,
                                            strokeIdentity, error)) {
              success = false;
              return;
            }
            ApplyLayerGeometry(text::TextGlyphMaterialLayer{stroke}, paint);
            canvas->drawPath(path, paint);
            if (!strokeIdentity.empty())
              sampleIdentity += "|" + strokeIdentity;
          }
        } else if constexpr (std::is_same_v<Value,
                                            text::NineSliceBackdrop>) {
          const auto decodeUsage =
              channel == text::TextBackdropChannel::Bubble
                  ? RasterDecodeUsage::QtStretchableBubble
                  : RasterDecodeUsage::Default;
          auto *asset = assets.Resolve(value.asset, error, decodeUsage);
          std::string frameIdentity;
          auto image = asset ? assets.SampleRaster(*asset, sourceTimeUs,
                                                   frameIdentity, error)
                             : sk_sp<SkImage>{};
          if (!image) {
            SkPaint fallback;
            fallback.setColor(ToSkColor(value.fallbackColor));
            canvas->drawRect(bounds, fallback);
            diagnostics.push_back(MakeDiagnostic(
                "text.backdrop.asset_fallback", DiagnosticSeverity::Warning,
                "resource", subject,
                error.empty() ? "nine-slice asset resolution failed" : error));
            error.clear();
            sampleIdentity = "fallback:" + TextureKey(value.asset);
            return;
          }
          sampleIdentity = frameIdentity;
          const auto orientation = value.asset.orientation;
          const bool quarterTurn =
              orientation == text::TextureOrientation::Right ||
              orientation == text::TextureOrientation::Left;
          const float orientationDegrees =
              orientation == text::TextureOrientation::Right
                  ? 90.0F
              : orientation == text::TextureOrientation::Down
                  ? 180.0F
              : orientation == text::TextureOrientation::Left
                  ? -90.0F
                  : 0.0F;
          const auto destination =
              quarterTurn
                  ? SkRect::MakeXYWH(bounds.centerX() - bounds.height() * 0.5F,
                                     bounds.centerY() - bounds.width() * 0.5F,
                                     bounds.height(), bounds.width())
                  : bounds;
          SkPaint imagePaint;
          std::size_t draws = 0U;
          std::string_view stretchCapability;
          // Qt's InfoSticker quad is coverage-free: the PNG alpha and the
          // linear texel sample alone define its edge.  Coverage AA adds a
          // phase-dependent fringe when the authored destination is
          // fractional (the common bubble case).
          imagePaint.setAntiAlias(false);
          canvas->save();
          if (orientationDegrees != 0.0F)
            canvas->rotate(orientationDegrees, bounds.centerX(),
                           bounds.centerY());
          if (value.stretchMode == text::TextBackdropStretchMode::Stretch) {
            canvas->drawImageRect(
                image.get(), destination,
                SkSamplingOptions(SkFilterMode::kLinear,
                                  SkMipmapMode::kNone),
                &imagePaint);
            draws = destination.isEmpty() ? 0U : 1U;
            stretchCapability = "backdrop-stretch:stretch";
          } else {
            const int width = image->width();
            const int height = image->height();
            const int left = std::clamp(
                static_cast<int>(std::lround(value.capInsets.left)), 0,
                std::max(0, width - 1));
            const int top = std::clamp(
                static_cast<int>(std::lround(value.capInsets.top)), 0,
                std::max(0, height - 1));
            const int right = std::clamp(
                width -
                    static_cast<int>(std::lround(value.capInsets.right)),
                left + 1, width);
            const int bottom = std::clamp(
                height -
                    static_cast<int>(std::lround(value.capInsets.bottom)),
                top + 1, height);
            const SkIRect center =
                SkIRect::MakeLTRB(left, top, right, bottom);
            const SkSamplingOptions sampling(SkFilterMode::kLinear,
                                             SkMipmapMode::kNone);
            const float authoredNativeWidth =
                value.contentInsets.left + value.minimumContentWidth +
                value.contentInsets.right;
            const float authoredNativeHeight =
                value.contentInsets.top + value.minimumContentHeight +
                value.contentInsets.bottom;
            const bool hasAuthoredNativeExtent =
                value.minimumContentWidth > 0.0F &&
                value.minimumContentHeight > 0.0F &&
                authoredNativeWidth > 0.0F && authoredNativeHeight > 0.0F;
            if (std::getenv("VIDEOCUT_TRACE_R3P_BACKDROP") != nullptr) {
              std::fprintf(stderr,
                           "[VIDEOCUT_R3P_BACKDROP] destination=[%.9g %.9g "
                           "%.9g %.9g] native=[%.9g %.9g] image=%dx%d\n",
                           destination.left(), destination.top(),
                           destination.width(), destination.height(),
                           authoredNativeWidth, authoredNativeHeight, width,
                           height);
            }
            if (hasAuthoredNativeExtent &&
                value.stretchMode ==
                    text::TextBackdropStretchMode::NineSlice) {
              draws = DrawImageNineWithDestinationInsets(
                  canvas, image.get(), center, destination,
                  value.contentInsets, sampling, imagePaint);
              stretchCapability = "backdrop-stretch:nine_slice";
            } else {
              const float authoredScaleX =
                  sourceIntrinsicSize.width > 0.0F
                      ? sourceIntrinsicSize.width / static_cast<float>(width)
                      : hasAuthoredNativeExtent
                            ? authoredNativeWidth / static_cast<float>(width)
                            : 1.0F;
              const float authoredScaleY =
                  sourceIntrinsicSize.height > 0.0F
                      ? sourceIntrinsicSize.height / static_cast<float>(height)
                      : hasAuthoredNativeExtent
                            ? authoredNativeHeight / static_cast<float>(height)
                            : 1.0F;
              draws = DrawImageNineTiled(canvas, image.get(), center, destination,
                                 value.contentInsets, authoredScaleX,
                                 authoredScaleY, sampling, imagePaint);
              stretchCapability = "backdrop-stretch:tile_center";
            }
          }
          canvas->restore();
          if (recordedDraws && draws > 0U) {
            recordedDraws->push_back(
                {subject, std::string(stretchCapability),
                 "skia-metal-backdrop-picture", draws});
            recordedDraws->push_back(
                {value.asset.assetId, "media-type:" + asset->mediaType,
                 "skia-metal-backdrop-picture", draws});
          }
        } else {
          auto *asset = assets.Resolve(value.asset, error);
          if (!asset || !DrawResolvedVisualAsset(canvas, *asset, bounds,
                                                 sourceTimeUs, assets,
                                                 sampleIdentity, error)) {
            SkPaint fallback;
            fallback.setColor(ToSkColor(value.fallbackColor));
            canvas->drawRect(bounds, fallback);
            diagnostics.push_back(MakeDiagnostic(
                "text.backdrop.asset_fallback", DiagnosticSeverity::Warning,
                "resource", subject,
                error.empty() ? "visual backdrop asset resolution failed"
                              : error));
            error.clear();
            sampleIdentity = "fallback:" + TextureKey(value.asset) + "|" +
                             std::to_string(sourceTimeUs);
          }
        }
      },
      source);
  return success;
}


std::optional<SharedUnitFrameState> ResolveSharedUnitFrameState(
    const ResolvedLayout &layout,
    const TextRenderFramePlan &renderPlan) {
  if (layout.units.empty())
    return std::nullopt;
  std::optional<SharedUnitFrameState> result;
  bool hasAnimatedState = false;
  for (const auto &unit : layout.units) {
    const auto *plan =
        renderPlan.FindUnit(unit.binding.stableUnitId);
    SharedUnitFrameState sampled;
    if (plan) {
      if (!plan->transforms.empty()) {
        const auto composed = text::ComposeTextEffectTransforms(
            plan->transforms);
        if (!composed)
          return std::nullopt;
        sampled.localToText = *composed;
        hasAnimatedState = true;
      }
      if (plan->opacity) {
        sampled.opacity = std::clamp(*plan->opacity, 0.0F, 1.0F);
        hasAnimatedState = hasAnimatedState || sampled.opacity < 0.999999F;
      }
    }
    if (!result) {
      result = sampled;
      continue;
    }
    if (std::fabs(result->opacity - sampled.opacity) > 0.000001F ||
        !std::equal(result->localToText.columnMajor.begin(),
                    result->localToText.columnMajor.end(),
                    sampled.localToText.columnMajor.begin(),
                    [](const float left, const float right) {
                      return std::fabs(left - right) <= 0.000001F;
                    })) {
      return std::nullopt;
    }
  }
  return hasAnimatedState ? result : std::nullopt;
}


TextVisualOutsets ToVisualOutsets(
    const text::TextEffectOutsets &value) noexcept {
  return {value.left, value.top, value.right, value.bottom};
}


SkRect ResolveFrameBackdropBounds(const text::TextEffectBackdropPass &pass,
                                  const ResolvedLayout &layout) {
  auto based = ToSkRect(pass.basedRect);
  if (based.isEmpty() && pass.basedRange.begin < pass.basedRange.end &&
      pass.basedRange.end <= layout.units.size()) {
    for (auto index = pass.basedRange.begin; index < pass.basedRange.end;
         ++index) {
      based.join(layout.units[index].layoutBounds);
    }
  }
  if (based.isEmpty())
    based = ToSkRect(pass.bounds);
  if (based.isEmpty())
    return based;

  SkRect resolved = based;
  const auto intrinsic = pass.sourceIntrinsicSize;
  if (intrinsic.width > 0.0F && intrinsic.height > 0.0F &&
      pass.fitMode != text::TextBackdropFitMode::Stretch) {
    const float widthScale = based.width() / intrinsic.width;
    const float heightScale = based.height() / intrinsic.height;
    float scale = 1.0F;
    switch (pass.fitMode) {
    case text::TextBackdropFitMode::Width:
      scale = widthScale;
      break;
    case text::TextBackdropFitMode::Height:
      scale = heightScale;
      break;
    case text::TextBackdropFitMode::LongSide:
      scale = std::max(based.width(), based.height()) /
              std::max(intrinsic.width, intrinsic.height);
      break;
    case text::TextBackdropFitMode::ShortSide:
      scale = std::min(based.width(), based.height()) /
              std::min(intrinsic.width, intrinsic.height);
      break;
    case text::TextBackdropFitMode::Stretch:
      break;
    }
    const float width = intrinsic.width * scale;
    const float height = intrinsic.height * scale;
    resolved = SkRect::MakeXYWH(
        based.centerX() - pass.sourcePivotX * width,
        based.centerY() - pass.sourcePivotY * height, width, height);
  }
  resolved.fLeft -= pass.expand.left;
  resolved.fTop -= pass.expand.top;
  resolved.fRight += pass.expand.right;
  resolved.fBottom += pass.expand.bottom;
  return resolved;
}


TextVisualOutsets ResolvedBackdropOutsets(
    const ResolvedBackdropPass &pass) noexcept {
  auto result = ResolveBackdropSourceOutsets(pass.source);
  result.Include(pass.sourceOutsets);
  return result;
}


void AddResolvedBackdropGeometryIdentity(IdentityBuilder &identity,
                                         const ResolvedBackdropPass &pass) {
  identity.Add(pass.basedRange.begin);
  identity.Add(pass.basedRange.end);
  identity.Add(pass.basedRect.left());
  identity.Add(pass.basedRect.top());
  identity.Add(pass.basedRect.right());
  identity.Add(pass.basedRect.bottom());
  identity.Add(pass.sourceIntrinsicSize.width);
  identity.Add(pass.sourceIntrinsicSize.height);
  identity.Add(pass.fitMode);
  identity.Add(pass.sourcePivotX);
  identity.Add(pass.sourcePivotY);
  identity.Add(pass.expand.left);
  identity.Add(pass.expand.top);
  identity.Add(pass.expand.right);
  identity.Add(pass.expand.bottom);
  identity.Add(pass.sourceOutsets.left);
  identity.Add(pass.sourceOutsets.top);
  identity.Add(pass.sourceOutsets.right);
  identity.Add(pass.sourceOutsets.bottom);
  identity.Add(pass.bounds.left());
  identity.Add(pass.bounds.top());
  identity.Add(pass.bounds.right());
  identity.Add(pass.bounds.bottom());
}


std::vector<ResolvedBackdropPass> ResolveBackdropPasses(
    const text::TextRenderDocument &document, const ResolvedLayout &layout,
    const text::TextRenderRequest &request,
    const text::TextEffectFramePlan &framePlan) {
  std::vector<ResolvedBackdropPass> result;
  std::size_t order = 0U;
  for (const auto &layer : document.appearance.backdrops.layers) {
    if (!layer.enabled)
      continue;
    const auto alignment = document.text.paragraphs.empty()
                               ? text::TextAlignment::Start
                               : document.text.paragraphs.front()
                                     .style.alignment;
    const auto direction = document.text.paragraphs.empty()
                               ? text::TextDirection::LeftToRight
                               : document.text.paragraphs.front()
                                     .style.direction;
    auto fitBounds = BackdropFitBounds(layout, layer.fit);
    if (layer.fit == text::TextBackdropFitPolicy::InkBounds &&
        !layout.glyphInkBounds.isEmpty() && !layout.inkBounds.isEmpty()) {
      // Qt fits the bubble height to tight glyph contours, but start/end
      // alignment retains the shaped line's optical side bearing. The old
      // verified bubble kernel anchored the tight width to textInk before
      // applying the source safe-area insets.
      const bool rightToLeft =
          direction == text::TextDirection::RightToLeft;
      const bool alignLeft =
          (alignment == text::TextAlignment::Start && !rightToLeft) ||
          (alignment == text::TextAlignment::End && rightToLeft) ||
          (alignment == text::TextAlignment::Justify && !rightToLeft);
      const bool alignRight =
          (alignment == text::TextAlignment::End && !rightToLeft) ||
          (alignment == text::TextAlignment::Start && rightToLeft) ||
          (alignment == text::TextAlignment::Justify && rightToLeft);
      if (alignLeft)
        fitBounds.offset(layout.inkBounds.left() - fitBounds.left(), 0.0F);
      else if (alignRight)
        fitBounds.offset(layout.inkBounds.right() - fitBounds.right(), 0.0F);
    }
    auto basedBounds = ExpandByInsets(fitBounds, layer.padding);
    const bool preserveQtTypesettingHeadroom =
        layer.channel == text::TextBackdropChannel::Bubble &&
        document.appearance.sdfMaterial.enabled;
    basedBounds = ApplyAuthoredExtent(
        basedBounds, layer.source, alignment, direction,
        preserveQtTypesettingHeadroom);
    const auto *nineSlice =
        std::get_if<text::NineSliceBackdrop>(&layer.source);
    if (preserveQtTypesettingHeadroom && nineSlice &&
        nineSlice->stretchMode != text::TextBackdropStretchMode::Stretch) {
      SkRect lineUnion = SkRect::MakeEmpty();
      for (const auto &line : layout.publicLayout.authoredLines)
        lineUnion.join(ToSkRect(line.bounds));
      if (!lineUnion.isEmpty()) {
        // AMGInfoSticker grows the image from tight ink dimensions but keeps
        // the nine-grid entity anchored to the shaped line origin. Keeping
        // those two concerns separate preserves the font side bearings
        // instead of visually centering the glyph ink inside the bubble.
        basedBounds.offset(lineUnion.centerX() - fitBounds.centerX(),
                           lineUnion.centerY() - fitBounds.centerY());
      }
    }
    text::TextEffectBackdropPass geometry;
    geometry.basedRect = ReferenceEffectRect(basedBounds);
    geometry.bounds = geometry.basedRect;
    if (layer.sourceIntrinsicWidth && layer.sourceIntrinsicHeight) {
      geometry.sourceIntrinsicSize = {*layer.sourceIntrinsicWidth,
                                      *layer.sourceIntrinsicHeight};
    }
    geometry.fitMode = layer.fitMode;
    geometry.sourcePivotX = layer.sourcePivotX;
    geometry.sourcePivotY = layer.sourcePivotY;
    geometry.expand = {layer.expand.left, layer.expand.top,
                       layer.expand.right, layer.expand.bottom};
    geometry.sourceOutsets = {
        layer.sourceOutsets.left, layer.sourceOutsets.top,
        layer.sourceOutsets.right, layer.sourceOutsets.bottom};
    auto bounds = ResolveFrameBackdropBounds(geometry, layout);
    auto transform = layer.transform;
    if (!request.suppressAnimation && !layer.animationClips.empty() &&
        request.durationUs > 0) {
      const auto sample = text::SampleTextLayerAnimationClips(
          layer.animationClips, request.localTimeUs, request.durationUs);
      const auto &canvas = document.text.referenceCanvas;
      transform.offsetX +=
          sample.positionX * (canvas.width + bounds.width()) * 0.5F;
      transform.offsetY +=
          sample.positionY * (canvas.height + bounds.height()) * 0.5F;
      transform.scaleX *= sample.scaleX;
      transform.scaleY *= sample.scaleY;
      transform.rotationDegrees += sample.rotationDegrees;
      transform.opacity *= sample.opacity;
    }
    auto sourceTime = request.localTimeUs;
    if (const auto *animated =
            std::get_if<text::AnimatedBackdrop>(&layer.source)) {
      sourceTime = request.suppressAnimation
                       ? animated->sourceInUs
                       : ResolveAnimatedBackdropTime(*animated,
                                                     request.localTimeUs,
                                                     framePlan,
                                                     layer.layerId);
    }
    ResolvedBackdropPass pass;
    pass.passId = "authored:" + layer.layerId;
    pass.layerId = layer.layerId;
    pass.zOrder = layer.zOrder;
    pass.source = layer.source;
    pass.channel = layer.channel;
    pass.materialOverride = layer.materialOverride;
    pass.transform = transform;
    pass.bounds = bounds;
    pass.basedRect = basedBounds;
    pass.sourceIntrinsicSize = geometry.sourceIntrinsicSize;
    pass.fitMode = layer.fitMode;
    pass.sourcePivotX = layer.sourcePivotX;
    pass.sourcePivotY = layer.sourcePivotY;
    pass.expand = ToVisualOutsets(geometry.expand);
    pass.sourceOutsets = ToVisualOutsets(geometry.sourceOutsets);
    pass.sourceTimeUs = sourceTime;
    pass.authoredOrder = order++;
    result.push_back(std::move(pass));
  }
  for (const auto &sampled : framePlan.backdropPasses) {
    auto channel = text::TextBackdropChannel::Frame;
    if (!sampled.layerId.empty()) {
      const auto authored =
          std::find_if(result.begin(), result.end(), [&](const auto &pass) {
            return pass.layerId == sampled.layerId;
          });
      if (authored != result.end())
        channel = authored->channel;
      result.erase(
          std::remove_if(result.begin(), result.end(), [&](const auto &pass) {
            return pass.layerId == sampled.layerId;
          }),
          result.end());
    }
    ResolvedBackdropPass pass;
    pass.passId = sampled.passId;
    pass.layerId = sampled.layerId;
    pass.zOrder = sampled.zOrder;
    pass.source = sampled.source;
    pass.channel = channel;
    pass.transform = sampled.transform;
    pass.basedRange = sampled.basedRange;
    pass.basedRect = ToSkRect(sampled.basedRect);
    pass.sourceIntrinsicSize = sampled.sourceIntrinsicSize;
    pass.fitMode = sampled.fitMode;
    pass.sourcePivotX = sampled.sourcePivotX;
    pass.sourcePivotY = sampled.sourcePivotY;
    pass.expand = ToVisualOutsets(sampled.expand);
    pass.sourceOutsets = ToVisualOutsets(sampled.sourceOutsets);
    pass.bounds = ResolveFrameBackdropBounds(sampled, layout);
    pass.sourceTimeUs = sampled.localTimeUs;
    pass.authoredOrder = order++;
    result.push_back(std::move(pass));
  }
  std::stable_sort(result.begin(), result.end(), [](const auto &left,
                                                     const auto &right) {
    if (left.zOrder != right.zOrder)
      return left.zOrder < right.zOrder;
    return left.authoredOrder < right.authoredOrder;
  });
  return result;
}


bool RecordBackdropComponents(
    const text::TextRenderDocument &document, const ResolvedLayout &layout,
    const text::TextRenderRequest &request,
    const TextRenderFramePlan &renderPlan,
    const SkRect &recordingBounds, TextVisualAssetStore &assets,
    std::vector<RenderComponent> &components,
    std::vector<Diagnostic> &diagnostics, std::string &identity,
    std::string &error) {
  const auto &framePlan = renderPlan.value();
  const auto sameTextureReference = [](const text::TextureReference &left,
                                       const text::TextureReference &right) {
    return left.sourceKind == right.sourceKind &&
           left.assetId == right.assetId && left.digest == right.digest &&
           left.mediaType == right.mediaType &&
           left.colorSpace == right.colorSpace &&
           left.orientation == right.orientation;
  };
  const auto sourceTexture = [](const text::TextBackdropSource &source)
      -> const text::TextureReference * {
    return std::visit(
        [](const auto &value) -> const text::TextureReference * {
          using Value = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Value, text::NineSliceBackdrop> ||
                        std::is_same_v<Value, text::VectorBackdrop> ||
                        std::is_same_v<Value, text::AnimatedBackdrop>) {
            return &value.asset;
          }
          return nullptr;
        },
        source);
  };
  const auto isIdentityFallback = [&](const ResolvedBackdropPass &pass) {
    if (!pass.materialOverride)
      return false;
    const auto *slot =
        std::get_if<text::EditableTextStyleSlot>(&*pass.materialOverride);
    const auto *source = sourceTexture(pass.source);
    const auto *fallback =
        slot ? std::get_if<text::TextureTextMaterial>(&slot->fallback)
             : nullptr;
    if (!source || !slot || !slot->replacementMask || !fallback ||
        !sameTextureReference(*source, fallback->texture) ||
        !sameTextureReference(*source, *slot->replacementMask)) {
      return false;
    }
    const auto &coordinates = fallback->coordinates;
    return fallback->fit == text::TextureFit::Stretch &&
           fallback->mapping == text::TextureMapping::ScopeBounds &&
           coordinates.coordinateSpace ==
               text::PaintCoordinateSpace::LayoutBox &&
           coordinates.coordinateOutset == 0.0F &&
           coordinates.coordinateScale == 1.0F &&
           fallback->scale == 1.0F && fallback->rotationDegrees == 0.0F &&
           fallback->offsetX == 0.0F && fallback->offsetY == 0.0F &&
           !fallback->flipX && !fallback->flipY &&
           fallback->atlasColumns == 1U && fallback->atlasRows == 1U &&
           fallback->opacity == 1.0F && !fallback->sourceAlpha &&
           !fallback->underlayColor && fallback->underlayGradient.empty();
  };
  IdentityBuilder cacheIdentity;
  const auto passes =
      ResolveBackdropPasses(document, layout, request, framePlan);
  const auto sharedUnitState =
      ResolveSharedUnitFrameState(layout, renderPlan);
  for (const auto &pass : passes) {
    SkPictureRecorder recorder;
    auto *canvas = recorder.beginRecording(recordingBounds);
    canvas->save();
    if (pass.inheritsSharedUnitState && sharedUnitState) {
      canvas->concat(SkM44::ColMajor(
          sharedUnitState->localToText.columnMajor.data()));
    }
    ApplyBackdropTransform(canvas, pass.bounds, pass.transform);
    const auto sourceVisualBounds = ExpandByOutsets(
        pass.bounds, ResolvedBackdropOutsets(pass));
    const float inheritedOpacity =
        pass.inheritsSharedUnitState && sharedUnitState
            ? sharedUnitState->opacity
            : 1.0F;
    const float resolvedOpacity = std::clamp(
        pass.transform.opacity * inheritedOpacity, 0.0F, 1.0F);
    bool opacityLayer = resolvedOpacity < 0.9999F;
    if (opacityLayer) {
      SkPaint opacity;
      opacity.setAlphaf(resolvedOpacity);
      canvas->saveLayer(&sourceVisualBounds, &opacity);
    }
    const bool identityFallback = isIdentityFallback(pass);
    const bool materialMask =
        pass.materialOverride.has_value() &&
        !std::holds_alternative<text::RoundedRectBackdrop>(pass.source) &&
        !identityFallback;
    if (materialMask)
      canvas->saveLayer(&pass.bounds, nullptr);
    std::string assetSample;
    std::vector<text::TextMaterialExecutionEvidence> recordedDraws;
    if (!DrawBackdropSource(canvas, pass.source, pass.bounds,
                            pass.sourceTimeUs, pass.channel,
                            pass.sourceIntrinsicSize, assets, assetSample,
                            diagnostics, pass.layerId, error,
                            TextMaterialExecutionScope::Enabled()
                                ? &recordedDraws : nullptr)) {
      return false;
    }
    if (materialMask) {
      SkPaint replacement;
      std::string replacementSample;
      if (!ConfigureTextMaterialPaint(
              *pass.materialOverride, pass.bounds, pass.bounds, pass.bounds,
              pass.sourceTimeUs, assets, replacement, replacementSample,
              error)) {
        return false;
      }
      const auto *slot = std::get_if<text::EditableTextStyleSlot>(
          &*pass.materialOverride);
      replacement.setBlendMode(slot && slot->replacementMask
                                   ? SkBlendMode::kSrc
                                   : SkBlendMode::kSrcIn);
      canvas->drawRect(pass.bounds, replacement);
      canvas->restore();
      if (!replacementSample.empty())
        assetSample += "|material:" + replacementSample;
    }
    if (opacityLayer)
      canvas->restore();
    canvas->restore();
    IdentityBuilder passIdentity;
    passIdentity.AddString(pass.passId);
    passIdentity.AddString(pass.layerId);
    passIdentity.Add(pass.zOrder);
    passIdentity.Add(pass.sourceTimeUs);
    AddResolvedBackdropGeometryIdentity(passIdentity, pass);
    passIdentity.Add(pass.transform.offsetX);
    passIdentity.Add(pass.transform.offsetY);
    passIdentity.Add(pass.transform.scaleX);
    passIdentity.Add(pass.transform.scaleY);
    passIdentity.Add(pass.transform.rotationDegrees);
    passIdentity.Add(pass.transform.opacity);
    passIdentity.AddString(assetSample);
    const auto resolvedIdentity = passIdentity.Finish();
    cacheIdentity.AddString(resolvedIdentity);
    components.push_back({pass.passId,
                          text::TextEffectCompositeItemKind::Backdrop,
                          pass.zOrder,
                          text::TextBlendMode::SourceOver,
                          recorder.finishRecordingAsPicture(),
                          resolvedIdentity,
                          pass.authoredOrder});
    components.back().recordedResourceDraws = std::move(recordedDraws);
  }
  for (const auto &transition : framePlan.stateTransitions) {
    cacheIdentity.AddString(transition.stateId);
    cacheIdentity.Add(transition.active);
    cacheIdentity.Add(transition.progress);
  }
  identity = cacheIdentity.Finish();
  return true;
}


void AddDecorationPassGeometryIdentity(
    IdentityBuilder &identity, const text::TextEffectDecorationPass &pass) {
  identity.Add(pass.bounds.x);
  identity.Add(pass.bounds.y);
  identity.Add(pass.bounds.width);
  identity.Add(pass.bounds.height);
  for (const auto component : pass.transform.localToText.columnMajor)
    identity.Add(component);
  identity.Add(pass.opacity);
  identity.Add(pass.basedRange.begin);
  identity.Add(pass.basedRange.end);
  identity.Add(pass.basedRect.x);
  identity.Add(pass.basedRect.y);
  identity.Add(pass.basedRect.width);
  identity.Add(pass.basedRect.height);
  identity.Add(pass.sourceIntrinsicSize.width);
  identity.Add(pass.sourceIntrinsicSize.height);
  identity.Add(pass.fitMode);
  identity.Add(pass.sourcePivotX);
  identity.Add(pass.sourcePivotY);
  identity.Add(pass.expand.left);
  identity.Add(pass.expand.top);
  identity.Add(pass.expand.right);
  identity.Add(pass.expand.bottom);
  identity.Add(pass.sourceOutsets.left);
  identity.Add(pass.sourceOutsets.top);
  identity.Add(pass.sourceOutsets.right);
  identity.Add(pass.sourceOutsets.bottom);
}


bool RecordDecorationComponents(
    const text::TextEffectFramePlan &framePlan, const SkRect &recordingBounds,
    TextVisualAssetStore &assets, std::vector<RenderComponent> &components,
    std::vector<Diagnostic> &diagnostics, std::string &identity,
    std::string &error) {
  IdentityBuilder cacheIdentity;
  std::size_t authoredOrder = components.size();
  for (const auto &pass : framePlan.decorationPasses) {
    if (std::getenv("VIDEOCUT_TRACE_TEXT_EFFECT_FRAMEPLAN") != nullptr) {
      const auto &m = pass.transform.localToText.columnMajor;
      std::fprintf(
          stderr,
          "[VIDEOCUT_TEXT_EFFECT_DECORATION] pass=%s layer=%s asset=%s "
          "scope=%u time_us=%lld bounds=[%g %g %g %g] based=[%g %g %g "
          "%g] intrinsic=[%g %g] fit=%u pivot=[%g %g] opacity=%g "
          "matrix=[%g %g %g %g;%g %g %g %g;%g %g %g %g;%g %g %g "
          "%g]\n",
          pass.passId.c_str(), pass.layerId.c_str(), pass.assetId.c_str(),
          static_cast<unsigned>(pass.effectScope),
          static_cast<long long>(pass.assetTimeUs), pass.bounds.x,
          pass.bounds.y, pass.bounds.width, pass.bounds.height,
          pass.basedRect.x, pass.basedRect.y, pass.basedRect.width,
          pass.basedRect.height, pass.sourceIntrinsicSize.width,
          pass.sourceIntrinsicSize.height, static_cast<unsigned>(pass.fitMode),
          pass.sourcePivotX, pass.sourcePivotY, pass.opacity, m[0], m[4],
          m[8], m[12], m[1], m[5], m[9], m[13], m[2], m[6], m[10],
          m[14], m[3], m[7], m[11], m[15]);
    }
    const auto resource = std::find_if(
        framePlan.resources.begin(), framePlan.resources.end(),
        [&](const auto &sample) {
          return !sample.assetId.empty() && sample.assetId == pass.assetId;
        });
    const auto digest = resource == framePlan.resources.end()
                            ? std::string{}
                            : resource->digest;
    // The decoration pass is the sampled frame contract. Resource entries
    // admit bytes/digest and may be shared by multiple decorations with
    // different clocks; they must not overwrite the pass-local asset time.
    const auto sourceTime = pass.assetTimeUs;
    auto *asset = assets.ResolveUnqualified(pass.assetId, digest, error);
    if (!asset) {
      diagnostics.push_back(MakeDiagnostic(
          "text.decoration.asset_missing", DiagnosticSeverity::Error,
          "resource", pass.decorationId,
          error.empty() ? "decoration asset resolution failed" : error));
      return false;
    }
    const auto bounds = ToSkRect(pass.bounds);
    const auto sourceVisualBounds =
        ExpandByOutsets(bounds, ToVisualOutsets(pass.sourceOutsets));
    if (std::getenv("VIDEOCUT_TRACE_TEXT_EFFECT_FRAMEPLAN") != nullptr) {
      const auto mapped =
          MapEffectBounds(sourceVisualBounds, pass.transform.localToText)
              .value_or(sourceVisualBounds);
      std::fprintf(stderr,
                   "[VIDEOCUT_TEXT_EFFECT_DECORATION_BOUNDS] pass=%s "
                   "recording=[%g %g %g %g] mapped=[%g %g %g %g]\n",
                   pass.passId.c_str(), recordingBounds.left(),
                   recordingBounds.top(), recordingBounds.right(),
                   recordingBounds.bottom(), mapped.left(), mapped.top(),
                   mapped.right(), mapped.bottom());
    }
    std::string sampleIdentity;
    sk_sp<SkImage> rasterSample;
    std::optional<QtTextFollowerComponentDraw> nativeFollower;
    const bool directFollower = asset->immutableLottieResources &&
                                !asset->qtFollowerResourceId.empty();
    if (asset->reference.orientation == text::TextureOrientation::Up &&
        (directFollower || asset->raster || asset->stream)) {
      rasterSample = directFollower
                         ? assets.SampleQtFollowerDirectRaster(
                               *asset, sourceTime, sampleIdentity, error)
                         : assets.SampleRaster(*asset, sourceTime,
                                               sampleIdentity, error);
      if (!rasterSample)
        return false;
    }
    SkPictureRecorder recorder;
    auto *canvas = recorder.beginRecording(recordingBounds);
    if (rasterSample) {
      const float logicalWidth =
          pass.sourceIntrinsicSize.width > 0.0F
              ? pass.sourceIntrinsicSize.width
              : asset->intrinsicWidth;
      const float logicalHeight =
          pass.sourceIntrinsicSize.height > 0.0F
              ? pass.sourceIntrinsicSize.height
              : asset->intrinsicHeight;
      const auto &m = pass.transform.localToText.columnMajor;
      if (!std::isfinite(logicalWidth) || logicalWidth <= 0.0F ||
          !std::isfinite(logicalHeight) || logicalHeight <= 0.0F ||
          rasterSample->width() <= 0 || rasterSample->height() <= 0 ||
          m[2] != 0.0F || m[3] != 0.0F || m[6] != 0.0F ||
          m[7] != 0.0F || m[8] != 0.0F || m[9] != 0.0F ||
          m[11] != 0.0F || m[14] != 0.0F || m[15] != 1.0F) {
        error = "text decoration raster placement is invalid";
        return false;
      }
      const float fitScaleX = bounds.width() / logicalWidth;
      const float fitScaleY = bounds.height() / logicalHeight;
      const float xx = m[0] * fitScaleX;
      const float xy = m[4] * fitScaleY;
      const float yx = m[1] * fitScaleX;
      const float yy = m[5] * fitScaleY;
      const float translationX =
          m[0] * bounds.centerX() + m[4] * bounds.centerY() + m[12];
      const float translationY =
          m[1] * bounds.centerX() + m[5] * bounds.centerY() + m[13];
      const float rasterToOutputScaleX =
          logicalWidth / static_cast<float>(rasterSample->width());
      const float rasterToOutputScaleY =
          logicalHeight / static_cast<float>(rasterSample->height());
      const float placeX = 0.5F * rasterToOutputScaleX;
      const float placeY = 0.5F * rasterToOutputScaleY;
      SkMatrix placement;
      placement.setAll(
          xx * rasterToOutputScaleX, xy * rasterToOutputScaleY,
          translationX - xx * logicalWidth * 0.5F -
              xy * logicalHeight * 0.5F + xx * placeX + xy * placeY - 0.5F,
          yx * rasterToOutputScaleX, yy * rasterToOutputScaleY,
          translationY - yx * logicalWidth * 0.5F -
              yy * logicalHeight * 0.5F + yx * placeX + yy * placeY - 0.5F,
          0.0F, 0.0F, 1.0F);
      if (directFollower) {
        nativeFollower = QtTextFollowerComponentDraw{
            rasterSample, placement, std::clamp(pass.opacity, 0.0F, 1.0F)};
      }
      canvas->save();
      canvas->concat(placement);
      SkPaint opacity;
      opacity.setAlphaf(std::clamp(pass.opacity, 0.0F, 1.0F));
      if (directFollower) {
        // The recovered VideoAnimSeq texture is a raw RGBA transport whose RGB
        // may intentionally exceed A on emissive low-coverage pixels. Qt's
        // Sprite shader samples those bytes and its Metal attachment applies
        // source-one/one-minus-source-alpha. drawImage() treats the sample as
        // an ordinary color image and associates/clamps it; a raw shader keeps
        // the captured byte semantics while retaining the same linear/clamp
        // sampling and source-over blend.
        opacity.setShader(rasterSample->makeRawShader(
            SkTileMode::kClamp, SkTileMode::kClamp,
            SkSamplingOptions(SkFilterMode::kLinear,
                              SkMipmapMode::kNone)));
        canvas->drawRect(
            SkRect::MakeIWH(rasterSample->width(), rasterSample->height()),
            opacity);
      } else {
        canvas->drawImage(rasterSample, 0.0F, 0.0F,
                          SkSamplingOptions(SkFilterMode::kLinear,
                                            SkMipmapMode::kNone),
                          &opacity);
      }
      canvas->restore();
    } else {
      canvas->save();
      canvas->concat(
          SkM44::ColMajor(pass.transform.localToText.columnMajor.data()));
      if (pass.opacity < 0.9999F) {
        SkPaint opacity;
        opacity.setAlphaf(std::clamp(pass.opacity, 0.0F, 1.0F));
        canvas->saveLayer(&sourceVisualBounds, &opacity);
      }
      if (!DrawResolvedVisualAsset(canvas, *asset, bounds, sourceTime, assets,
                                   sampleIdentity, error)) {
        return false;
      }
      if (pass.opacity < 0.9999F)
        canvas->restore();
      canvas->restore();
    }
    IdentityBuilder componentIdentity;
    componentIdentity.AddString(pass.passId);
    componentIdentity.AddString(pass.decorationId);
    componentIdentity.AddString(pass.assetId);
    componentIdentity.AddString(digest);
    componentIdentity.Add(sourceTime);
    componentIdentity.AddString(sampleIdentity);
    AddDecorationPassGeometryIdentity(componentIdentity, pass);
    const auto resolvedIdentity = componentIdentity.Finish();
    cacheIdentity.AddString(resolvedIdentity);
    RenderComponent component{
        pass.passId,
        text::TextEffectCompositeItemKind::Decoration,
        pass.zOrder,
        text::TextBlendMode::SourceOver,
        recorder.finishRecordingAsPicture(),
        resolvedIdentity,
        authoredOrder++};
    component.nativeFollower = std::move(nativeFollower);
    components.push_back(std::move(component));
  }
  identity = cacheIdentity.Finish();
  return true;
}

} // namespace videocut::skia_runtime::internal::text_lane
