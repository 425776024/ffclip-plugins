#include "text/TextRenderPipeline.h"

namespace videocut::skia_runtime::internal {
namespace {
using namespace text_lane;

class FrameResultCache final {
public:
  explicit FrameResultCache(const text::TextRenderOptions &options)
      : maximumBytes_(options.maximumCacheBytes),
        maximumFrames_(options.maximumCachedFrames) {}

  std::optional<text::TextRenderResult> Find(const std::string &key,
                                             const std::int64_t pts) {
    const auto found = std::find_if(
        entries_.begin(), entries_.end(),
        [&](const auto &entry) { return entry.key == key; });
    if (found == entries_.end())
      return std::nullopt;
    entries_.splice(entries_.begin(), entries_, found);
    auto result = entries_.front().result;
    if (result.image && result.image.desc().timing.pts != pts) {
      auto timing = result.image.desc().timing;
      timing.pts = pts;
      const auto retimed = result.image.withTiming(timing);
      if (retimed)
        result.image = retimed.value();
    }
    return result;
  }

  void Store(std::string key, const text::TextRenderResult &result,
             const std::size_t bytes) {
    if (maximumFrames_ == 0U || maximumBytes_ == 0U || bytes > maximumBytes_)
      return;
    const auto existing = std::find_if(
        entries_.begin(), entries_.end(),
        [&](const auto &entry) { return entry.key == key; });
    if (existing != entries_.end()) {
      residentBytes_ -= existing->bytes;
      entries_.erase(existing);
    }
    entries_.push_front({std::move(key), result, bytes});
    residentBytes_ += bytes;
    while (!entries_.empty() &&
           (entries_.size() > maximumFrames_ ||
            residentBytes_ > maximumBytes_)) {
      residentBytes_ -= entries_.back().bytes;
      entries_.pop_back();
    }
  }

  void Clear() noexcept {
    entries_.clear();
    residentBytes_ = 0U;
  }

  [[nodiscard]] std::size_t residentBytes() const noexcept {
    return residentBytes_;
  }

private:
  struct Entry final {
    std::string key;
    text::TextRenderResult result;
    std::size_t bytes{0U};
  };
  std::size_t maximumBytes_{0U};
  std::size_t maximumFrames_{0U};
  std::list<Entry> entries_;
  std::size_t residentBytes_{0U};
};

class TextLane final : public text::TextRenderLane {
public:
  TextLane(text::TextRenderOptions options, SkiaRuntimeConfig config,
           std::shared_ptr<FontContextCache> fontCache)
      : options_(std::move(options)), config_(std::move(config)),
        fontCache_(std::move(fontCache)), frameCache_(options_),
        visualAssets_(config_, options_.assets) {}

  ~TextLane() override {
    effectRuntimeClockStore_.Clear();
    ReleaseExactQtTextPostEffectState(renderGraphInstanceId_);
  }

  bool ReplaceDocument(text::TextRenderDocument document,
                       text::TextDocumentInstallReceipt &receipt,
                       std::string &error) override {
    std::lock_guard<std::mutex> lock(mutex_);
    receipt = {};
    error.clear();
    const auto validation = text::ValidateResolvedRichTextView(document.text);
    if (!validation.valid) {
      error = validation.diagnostics.empty()
                  ? "resolved rich-text view is invalid"
                  : validation.diagnostics.front().message;
      return false;
    }
    if (!text::ValidateTextLayerAppearance(document.appearance, &error))
      return false;
    if (!text::ValidateTextAnimationStack(document.animations,
                                          !document.timedSpans.empty(),
                                          &error))
      return false;
    const auto fontIdentity = FontResourceIdentity(
        document.text, document.appearance.sdfMaterial.sourceCreationComponent);
    auto fonts = fontCache_->Resolve(fontIdentity, document.text,
                                     document.appearance.sdfMaterial.sourceCreationComponent,
                                     config_,
                                     options_.assets,
                                     &receipt.fontResolutions, error);
    if (!fonts)
      return false;
    document_ = std::move(document);
    fonts_ = std::move(fonts);
    fontIdentity_ = fontIdentity;
    installedFontResolutions_ = receipt.fontResolutions;
    ++generation_;
    if (generation_ == 0U)
      generation_ = 1U;
    frameCache_.Clear();
    resolvedCacheAliases_.clear();
    visualAssets_.Reset();
    effectRuntimeClockStore_.Clear();
    executionGraphHistories_.clear();
    return true;
  }

  std::uint64_t generation() const noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
  }

  std::size_t cachedBytes() const noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    return frameCache_.residentBytes() + visualAssets_.residentBytes();
  }

  void PurgeCache() noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    frameCache_.Clear();
    resolvedCacheAliases_.clear();
    visualAssets_.Reset();
    effectRuntimeClockStore_.Clear();
    executionGraphHistories_.clear();
  }

  text::TextRenderResult Render(const text::TextRenderRequest &request) override {
    std::lock_guard<std::mutex> lock(mutex_);
    text::TextRenderResult result;
    if (request.captureExecutionEvidence)
      result.executionEvidence.emplace();
    TextMaterialExecutionScope materialEvidenceScope(
        result.executionEvidence ? &*result.executionEvidence : nullptr);
    result.documentGeneration = generation_;
    if (!document_ || !fonts_) {
      result.status = text::TextStatusCode::NotReady;
      result.diagnostics.push_back(MakeDiagnostic(
          "text.document.not_ready", DiagnosticSeverity::Error, "render", {},
          "no text render document has been installed"));
      return result;
    }
    if (IsCanceled(request.cancel)) {
      result.status = text::TextStatusCode::Canceled;
      return result;
    }
    if (!IsFinitePresentation(request)) {
      return Failure(std::move(result), text::TextStatusCode::InvalidDocument,
                     "text.presentation.invalid", "render", {},
                     "sampled text presentation or timing is invalid");
    }
    if (request.outputWidth == 0U || request.outputHeight == 0U ||
        request.outputWidth > options_.maximumWidth ||
        request.outputHeight > options_.maximumHeight) {
      return Failure(std::move(result), text::TextStatusCode::BudgetExceeded,
                     "text.surface.dimension_budget", "raster", {},
                     "requested text surface exceeds the lane budget");
    }

    try {
      // The installed document stays immutable under the lane lock. Ordinary
      // playback borrows it; only property sampling needs an authored copy.
      std::optional<text::TextRenderDocument> sampledDocument;
      const auto *effective = &*document_;
      std::string error;
      if (!request.framePlan.sampledProperties.empty()) {
        auto &sampled = sampledDocument.emplace(*document_);
        if (!ApplySampledProperties(request.framePlan, sampled,
                                    result.diagnostics, error)) {
          return Failure(std::move(result),
                         text::TextStatusCode::InvalidDocument,
                         "text.sampled_property.invalid", "property", {},
                         std::move(error));
        }
        effective = &sampled;
      }
      const auto &externalPlan = request.framePlan;

      auto effectiveFontIdentity = sampledDocument
          ? FontResourceIdentity(
                effective->text,
                effective->appearance.sdfMaterial.sourceCreationComponent)
          : fontIdentity_;
      auto effectiveFonts = effectiveFontIdentity == fontIdentity_
          ? fonts_
          : fontCache_->Resolve(
                effectiveFontIdentity, effective->text,
                effective->appearance.sdfMaterial.sourceCreationComponent,
                config_, options_.assets, nullptr, error);
      if (!effectiveFonts) {
        return Failure(std::move(result), text::TextStatusCode::MissingResource,
                       "text.font.resolve_failed", "resource", {},
                       std::move(error));
      }
      result.diagnostics.insert(result.diagnostics.end(),
                                effectiveFonts->diagnostics.begin(),
                                effectiveFonts->diagnostics.end());
      result.fontResolutions = sampledDocument
          ? BuildFontResolutionReceipts(effective->text, *effectiveFonts)
          : installedFontResolutions_;

      // Layout consumes unit bindings and layout mutations, not resources,
      // material programs or execution-graph payloads.
      text::TextEffectFramePlan topologyPlan;
      topologyPlan.units = externalPlan.units;
      topologyPlan.sourceCreationComponent =
          effective->appearance.sdfMaterial.sourceCreationComponent;
      ResolvedLayout baseLayout;
      if (!BuildLayout(effective->text, topologyPlan, *effectiveFonts,
                       baseLayout, error)) {
        return Failure(std::move(result),
                       text::TextStatusCode::InvalidDocument,
                       "text.layout.base_failed", "layout", {},
                       std::move(error));
      }

      const auto effectBridge = ResolveDocumentTextEffectBridge(
          effective->text, effective->appearance);
      text::TextEffectEvaluationRequest evaluationRequest;
      evaluationRequest.executionEvidence = result.executionEvidence
          ? &*result.executionEvidence : nullptr;
      evaluationRequest.animations = &effective->animations;
      evaluationRequest.timedSpans = &effective->timedSpans;
      evaluationRequest.frame = BuildTextEffectEvaluationFrame(
          baseLayout, 0.0, request.localTimeUs, effective->text,
          effective->appearance, *effectiveFonts->unicode);
      evaluationRequest.frame.outputSize = std::array<float, 2>{
          static_cast<float>(request.outputWidth),
          static_cast<float>(request.outputHeight)};
      const auto sourcePresentation = ResolvePresentationMatrix(
          effective->text, request, baseLayout.logicalBounds);
      const auto sourceAxisX = sourcePresentation.mapVector(
          SkVector::Make(effectBridge.sourceToReferenceScale, 0.0F));
      const auto sourceAxisY = sourcePresentation.mapVector(
          SkVector::Make(0.0F, effectBridge.sourceToReferenceScale));
      evaluationRequest.frame.sourceToOutputScale = std::array<float, 2>{
          std::copysign(sourceAxisX.length(), request.presentation.scaleX *
              (request.presentation.flipHorizontal ? -1.0F : 1.0F)),
          std::copysign(sourceAxisY.length(), request.presentation.scaleY *
              (request.presentation.flipVertical ? -1.0F : 1.0F))};
      const auto sourceOrigin = sourcePresentation.mapPoint(SkPoint::Make(
          effective->text.referenceCanvas.width * 0.5F,
          effective->text.referenceCanvas.height * 0.5F));
      evaluationRequest.frame.sourceOriginOutput =
          std::array<float, 2>{sourceOrigin.x(), sourceOrigin.y()};
      evaluationRequest.referenceCanvas = effective->text.referenceCanvas;
      evaluationRequest.bounds.layoutBounds =
          ReferenceEffectRect(baseLayout.logicalBounds);
      evaluationRequest.bounds.controlBounds =
          ReferenceEffectRect(baseLayout.controlBounds);
      evaluationRequest.bounds.inkBounds =
          ReferenceEffectRect(baseLayout.inkBounds);
      evaluationRequest.bounds.visualBounds =
          ReferenceEffectRect(baseLayout.visualBounds);
      evaluationRequest.bounds.expandedRenderTargetBounds =
          evaluationRequest.bounds.visualBounds;
      evaluationRequest.cacheIdentities = externalPlan.cacheIdentities;
      evaluationRequest.clipLocalTimeUs = request.localTimeUs;
      evaluationRequest.clipDurationUs = request.durationUs;
      evaluationRequest.sourceToReferenceScale =
          effectBridge.sourceToReferenceScale;
      evaluationRequest.sourceCreationComponent =
          effective->appearance.sdfMaterial.sourceCreationComponent;
      evaluationRequest.suppressAnimation = request.suppressAnimation;
      if (!BuildTextEffectEvaluationResources(
              effective->animations, externalPlan, request.suppressAnimation,
              visualAssets_, evaluationRequest.resources, error)) {
        return Failure(std::move(result),
                       text::TextStatusCode::MissingResource,
                       "text.animation.resource_failed", "resource", {},
                       std::move(error));
      }
      if (!request.suppressAnimation &&
          !effective->animations.executionGraph.nodes.empty() &&
          !BuildTextEffectExecutionEvaluationInputs(
              effective->animations.executionGraph, request,
              renderGraphInstanceId_, effectRuntimeClockStore_,
              executionGraphHistories_, evaluationRequest.stateInputs,
              evaluationRequest.historyInputs, error)) {
        return Failure(std::move(result), text::TextStatusCode::Failed,
                       "text.execution_graph.state_failed", "animation", {},
                       std::move(error));
      }
      auto evaluated = text::EvaluateTextEffects(evaluationRequest);
      result.diagnostics.insert(result.diagnostics.end(),
                                evaluated.diagnostics.begin(),
                                evaluated.diagnostics.end());
      if (!evaluated.valid) {
        error = evaluated.diagnostics.empty()
                    ? "text effect evaluation failed"
                    : evaluated.diagnostics.front().message;
        return Failure(std::move(result),
                       text::TextStatusCode::InvalidDocument,
                       "text.animation.sample_failed", "animation", {},
                       std::move(error));
      }
      text::TextEffectFramePlan installedPlan =
          std::move(evaluated.framePlan);

      const bool animationChangesDocument =
          !installedPlan.sampledProperties.empty();
      // Keep the base layout's paragraph source pointers alive when both
      // external and animation property sampling create temporary documents.
      std::optional<text::TextRenderDocument> animatedDocument;
      if (animationChangesDocument) {
        auto &sampled = animatedDocument.emplace(*document_);
        effective = &sampled;
        if (!ApplySampledProperties(installedPlan, sampled,
                                    result.diagnostics, error) ||
            !ApplySampledProperties(request.framePlan, sampled,
                                    result.diagnostics, error)) {
          return Failure(std::move(result),
                         text::TextStatusCode::InvalidDocument,
                         "text.animation.property_failed", "property", {},
                         std::move(error));
        }
        effectiveFontIdentity = FontResourceIdentity(
            effective->text, effective->appearance.sdfMaterial.sourceCreationComponent);
        effectiveFonts = effectiveFontIdentity == fontIdentity_
            ? fonts_
            : fontCache_->Resolve(
                  effectiveFontIdentity, effective->text,
                  effective->appearance.sdfMaterial.sourceCreationComponent,
                  config_, options_.assets, nullptr, error);
        if (!effectiveFonts) {
          return Failure(std::move(result),
                         text::TextStatusCode::MissingResource,
                         "text.animation.font_failed", "resource", {},
                         std::move(error));
        }
        result.fontResolutions =
            BuildFontResolutionReceipts(effective->text, *effectiveFonts);
      }
      installedPlan.sampledProperties.clear();

      text::TextEffectFramePlan mutableFramePlan = std::move(installedPlan);
      MergeFramePlan(mutableFramePlan, externalPlan);
      mutableFramePlan.sourceCreationComponent =
          effective->appearance.sdfMaterial.sourceCreationComponent;
      mutableFramePlan.sampledProperties.clear();
      ResolvedLayout layout;
      if (!animationChangesDocument &&
          CanReuseBaseLayout(baseLayout, topologyPlan, mutableFramePlan)) {
        layout = baseLayout;
      } else if (!BuildLayout(effective->text, mutableFramePlan,
                              *effectiveFonts, layout, error)) {
        return Failure(std::move(result),
                       text::TextStatusCode::InvalidDocument,
                       "text.layout.final_failed", "layout", {},
                       std::move(error));
      }
      if (!AttachGlyphContributorIdentities(
              layout, *effectiveFonts, result.fontResolutions, error)) {
        return Failure(std::move(result),
                       text::TextStatusCode::InvalidDocument,
                       "text.font.glyph_receipt_failed", "layout", {},
                       std::move(error));
      }
      if (!ApplyTextEffectVisualLineReflow(
              effective->text, mutableFramePlan, baseLayout, layout, error)) {
        return Failure(
            std::move(result), text::TextStatusCode::InvalidDocument,
            "text.layout.font_size_reflow_failed", "layout", {},
            std::move(error));
      }
      const auto finalEffectFrame = BuildTextEffectEvaluationFrame(
          layout, 0.0, request.localTimeUs, effective->text,
          effective->appearance, *effectiveFonts->unicode);
      if (!text::RetargetTextEffectProgramTransforms(
              mutableFramePlan, finalEffectFrame,
              effective->text.referenceCanvas,
              effectBridge.sourceToReferenceScale, error)) {
        return Failure(
            std::move(result), text::TextStatusCode::InvalidDocument,
            "text.animation.transform_retarget_failed", "animation", {},
            std::move(error));
      }
      const TextRenderFramePlan renderPlan(std::move(mutableFramePlan));
      const auto &framePlan = renderPlan.value();
      if (std::getenv("VIDEOCUT_TRACE_TEXT_EFFECT_FRAMEPLAN") != nullptr) {
        for (const auto &unit : layout.units) {
          const auto *sample =
              renderPlan.FindUnit(unit.binding.stableUnitId);
          if (!sample || sample->transforms.empty())
            continue;
          const auto composed =
              text::ComposeTextEffectTransforms(sample->transforms);
          if (!composed)
            continue;
          const auto &m = composed->columnMajor;
          for (std::size_t transformIndex = 0U;
               transformIndex < sample->transforms.size(); ++transformIndex) {
            const auto &transform = sample->transforms[transformIndex];
            if (!transform.programRetarget)
              continue;
            const auto &components =
                transform.programRetarget->sourceComponents;
            std::fprintf(
                stderr,
                "[VIDEOCUT_TEXT_EFFECT_FRAMEPLAN_TRANSFORM] time_us=%lld "
                "unit=%llu transform=%zu offset=[%.9g %.9g %.9g] "
                "rotation=[%.9g %.9g %.9g] scale=[%.9g %.9g %.9g] "
                "anchor_components=[%.9g %.9g %.9g] "
                "anchor_range=[%zu %zu] tight=[%.9g %.9g %.9g %.9g] "
                "layout=[%.9g %.9g %.9g %.9g]\n",
                static_cast<long long>(request.localTimeUs),
                static_cast<unsigned long long>(unit.binding.stableUnitId),
                transformIndex, components.offsetX, components.offsetY,
                components.offsetZ, components.rotationX,
                components.rotationY, components.rotationZ,
                components.scaleX, components.scaleY, components.scaleZ,
                components.anchorX, components.anchorY, components.anchorZ,
                components.anchorRangeBegin, components.anchorRangeEnd,
                transform.tightAnchorBounds.x, transform.tightAnchorBounds.y,
                transform.tightAnchorBounds.width,
                transform.tightAnchorBounds.height,
                transform.layoutAnchorBounds.x,
                transform.layoutAnchorBounds.y,
                transform.layoutAnchorBounds.width,
                transform.layoutAnchorBounds.height);
          }
          std::fprintf(
              stderr,
              "[VIDEOCUT_TEXT_EFFECT_FRAMEPLAN_MATRIX] time_us=%lld "
              "unit=%llu bounds=[%g %g %g %g] reflow_y=%g "
              "matrix=[%g %g %g %g;%g %g %g %g;%g %g %g %g;%g %g "
              "%g %g]\n",
              static_cast<long long>(request.localTimeUs),
              static_cast<unsigned long long>(unit.binding.stableUnitId),
              unit.authoredBounds.x(), unit.authoredBounds.y(),
              unit.authoredBounds.width(), unit.authoredBounds.height(),
              unit.visualReflowOffsetY, m[0], m[4], m[8], m[12], m[1], m[5],
              m[9], m[13], m[2], m[6], m[10], m[14], m[3], m[7], m[11],
              m[15]);
        }
      }

      SkRect liveLetterBounds = SkRect::MakeEmpty();
      for (const auto &unit : layout.units) {
        if (unit.scriptBounds.isEmpty())
          continue;
        const auto *unitPlan =
            renderPlan.FindUnit(unit.binding.stableUnitId);
        if (!unitPlan || unitPlan->transforms.empty()) {
          liveLetterBounds.join(unit.scriptBounds);
          continue;
        }
        const auto transform =
            text::ComposeTextEffectTransforms(unitPlan->transforms);
        liveLetterBounds.join(
            transform ? MapEffectBounds(unit.scriptBounds, *transform)
                            .value_or(unit.scriptBounds)
                      : unit.scriptBounds);
      }

      const auto authoredVisual =
          ResolveRecordingBounds(*effective, layout, request, renderPlan);
      auto presentationMatrix = ResolvePresentationMatrix(
          effective->text, request, layout.logicalBounds);
      for (const auto &pass :
           ResolveBackdropPasses(*effective, layout, request, framePlan)) {
        if (pass.channel != text::TextBackdropChannel::Bubble)
          continue;
        const auto *nineSlice =
            std::get_if<text::NineSliceBackdrop>(&pass.source);
        if (!nineSlice)
          continue;
        const float authoredWidth =
            nineSlice->contentInsets.left +
            nineSlice->minimumContentWidth +
            nineSlice->contentInsets.right;
        const float authoredHeight =
            nineSlice->contentInsets.top +
            nineSlice->minimumContentHeight +
            nineSlice->contentInsets.bottom;
        const bool usesAuthoredNativeExtent =
            authoredWidth > 0.0F && authoredHeight > 0.0F &&
            std::fabs(pass.bounds.width() - authoredWidth) <= 1.0e-3F &&
            std::fabs(pass.bounds.height() - authoredHeight) <= 1.0e-3F;
        if (!usesAuthoredNativeExtent)
          continue;
        // Qt's AMGInfoSticker shape and its editable glyphs share one entity
        // origin. textRect places the text aperture inside the PNG; neither
        // element follows the platform font's fractional ink center. Apply
        // the relative entity-origin correction once to the presentation
        // matrix so native glyphs, the bubble and later user transforms stay
        // in the same coordinate system.
        const auto &canvas = effective->text.referenceCanvas;
        presentationMatrix.preTranslate(
            canvas.width * 0.5F - pass.bounds.centerX(),
            canvas.height * 0.5F - pass.bounds.centerY());
        break;
      }
      const SkRect deformationInk =
          layout.glyphInkBounds.isEmpty() ? layout.inkBounds
                                          : layout.glyphInkBounds;
      const float pathBaseline =
          layout.publicLayout.authoredLines.empty()
              ? layout.logicalBounds.centerY()
              : layout.publicLayout.authoredLines.front().baseline;
      const BendGeometry bend{
          effective->appearance.bend.enabled,
          effective->appearance.bend.amount,
          effective->text.layoutBox.x,
          std::max(1.0F, effective->text.layoutBox.width),
          std::max(1.0F, effective->text.layoutBox.height)};
      const auto path = BuildPathGeometry(
          effective->appearance.path, effective->text.layoutBox,
          deformationInk.left(), deformationInk.right(), pathBaseline);
      const auto outputVisual = MapDeformedRect(
          presentationMatrix, authoredVisual, bend, path);
      SkIRect crop;
      outputVisual.roundOut(&crop);
      if (!crop.intersect(SkIRect::MakeWH(
              static_cast<int>(request.outputWidth),
              static_cast<int>(request.outputHeight)))) {
        result.status = text::TextStatusCode::Empty;
        result.diagnostics.push_back(MakeDiagnostic(
            "text.raster.outside_output", DiagnosticSeverity::Warning,
            "raster", {}, "text visual extent lies outside the output"));
        return result;
      }
      const auto surfacePixels = static_cast<std::uint64_t>(crop.width()) *
                                 static_cast<std::uint64_t>(crop.height());
      const bool useHighPrecisionPublication =
          options_.publicationFormat == text::TextPublicationFormat::Rgba16Float;
      auto surfaceBytes = surfacePixels *
                          (useHighPrecisionPublication ? 8U : 4U);
      if (surfaceBytes > options_.maximumSurfaceBytes) {
        return Failure(std::move(result), text::TextStatusCode::BudgetExceeded,
                       "text.surface.byte_budget", "raster", {},
                       "text surface exceeds the byte budget");
      }

      GranularFrameIdentities identities;
      identities.layout = CombineIdentity(
          framePlan.cacheIdentities.layout,
          LayoutIdentity(*effective, framePlan, generation_,
                         effectiveFontIdentity));
      identities.backdrops = CombineIdentity(
          framePlan.cacheIdentities.backdrops,
          BackdropFrameIdentity(*effective, layout, request, framePlan));
      IdentityBuilder resources;
      resources.AddString(effectiveFontIdentity);
      resources.AddString(framePlan.programId);
      for (const auto &sample : framePlan.resources) {
        resources.AddString(sample.resourceId);
        resources.AddString(sample.assetId);
        resources.AddString(sample.digest);
        // Catalog callers timestamp every resource sample, including static
        // font/texture bytes. Only temporal assets change when that clock
        // advances; immutable content is already fenced by asset and digest.
        const bool immutableSample =
            sample.kind == text::TextEffectResourceKind::Font ||
            (sample.kind == text::TextEffectResourceKind::Texture &&
             !sample.mediaType.empty() &&
             !IsStreamingMediaType(sample.mediaType) &&
             !IsLottieMediaType(sample.mediaType));
        if (!immutableSample)
          resources.Add(sample.localTimeUs);
        resources.Add(sample.kind);
        resources.AddString(sample.mediaType);
      }
      identities.resources = CombineIdentity(
          framePlan.cacheIdentities.resources, resources.Finish());
      identities.glyphs = CombineIdentity(
          framePlan.cacheIdentities.glyphs,
          GlyphFrameIdentity(*effective, framePlan, identities.layout,
                             identities.resources, request.localTimeUs));
      identities.postEffects = CombineIdentity(
          framePlan.cacheIdentities.postEffects,
          PostEffectFrameIdentity(framePlan, request));
      identities.executionGraph = CombineIdentity(
          framePlan.cacheIdentities.executionGraph,
          ExecutionGraphFrameIdentity(framePlan));
      const bool useExplicitExecutionGraph =
          !framePlan.executionGraph.nodes.empty() &&
          !IsLinearTextPostEffectExecutionGraph(framePlan) &&
          ExecutionGraphRequiresExplicitRasterExecutor(
              framePlan.executionGraph.nodes,
              &request.externallyCompositedResourceIds);
      const auto decorationFrameIdentity =
          DecorationFrameIdentity(framePlan);
      const auto compositeOrderIdentity =
          CompositeOrderIdentity(framePlan);
      const auto appearanceFrameIdentity =
          AppearanceFrameIdentity(effective->appearance);
      IdentityBuilder complete;
      complete.AddString(identities.resources);
      complete.AddString(identities.layout);
      complete.AddString(identities.glyphs);
      complete.AddString(identities.backdrops);
      complete.AddString(identities.postEffects);
      complete.AddString(identities.executionGraph);
      complete.AddString(decorationFrameIdentity);
      complete.AddString(compositeOrderIdentity);
      complete.AddString(appearanceFrameIdentity);
      complete.AddString(PresentationIdentity(request));
      complete.Add(request.gpuConsumer);
      complete.Add(request.gpuDeviceIndex);
      complete.Add(request.gpuDeviceGeneration);
      identities.composite = CombineIdentity(
          framePlan.cacheIdentities.composite, complete.Finish());
      const auto preliminaryCompositeIdentity = identities.composite;
      const auto alias =
          resolvedCacheAliases_.find(preliminaryCompositeIdentity);
      const auto &lookupIdentity = alias == resolvedCacheAliases_.end()
                                       ? preliminaryCompositeIdentity
                                       : alias->second;
      // Playback supplies a clock even to static text. Only stateful post
      // nodes or execution graphs can make returning a cached frame skip
      // runtime work; resolved glyph/material identities already cover all
      // stateless animation samples.
      const bool reusableFrame =
          (!request.effectRuntimeClock || framePlan.postEffectNodes.empty()) &&
          framePlan.executionGraph.nodes.empty();
      if (reusableFrame) {
        if (auto cached = frameCache_.Find(lookupIdentity,
                                           request.localTimeUs)) {
          if (!request.captureExecutionEvidence) {
            cached->executionEvidence.reset();
            return std::move(*cached);
          }
          if (cached->executionEvidence && cached->executionEvidence->completed) {
            cached->executionEvidence->frameCacheReused = true;
            return std::move(*cached);
          }
        }
      }
      if (std::getenv("VIDEOCUT_TRACE_TEXT_FRAME_CACHE") != nullptr) {
        std::fprintf(stderr,
            "[TEXT_FRAME_CACHE] miss time_us=%lld reusable=%d key=%s "
            "layout=%s glyphs=%s resources=%s bytes=%llu budget=%llu\n",
            static_cast<long long>(request.localTimeUs), reusableFrame,
            lookupIdentity.c_str(), identities.layout.c_str(),
            identities.glyphs.c_str(), identities.resources.c_str(),
            static_cast<unsigned long long>(frameCache_.residentBytes()),
            static_cast<unsigned long long>(options_.maximumCacheBytes));
      }

      if (IsCanceled(request.cancel)) {
        result.status = text::TextStatusCode::Canceled;
        return result;
      }
      if (gpuDeviceIndex_ != request.gpuDeviceIndex ||
          gpuDeviceGeneration_ != request.gpuDeviceGeneration) {
        gpuContext_.reset();
        gpuContextAttempted_ = false;
        gpuContextError_.clear();
        frameCache_.Clear();
        resolvedCacheAliases_.clear();
        visualAssets_.Reset();
        effectRuntimeClockStore_.Clear();
        executionGraphHistories_.clear();
        gpuDeviceIndex_ = request.gpuDeviceIndex;
        gpuDeviceGeneration_ = request.gpuDeviceGeneration;
      }
      if ((request.gpuConsumer || !framePlan.postEffectNodes.empty() ||
           !framePlan.executionGraph.nodes.empty() ||
           effective->appearance.sdfMaterial.enabled ||
           NativeTextGpuRequired()) &&
          !gpuContextAttempted_) {
        gpuContextAttempted_ = true;
        gpuContext_ = CreateSkiaGpuContext(gpuContextError_, gpuDeviceIndex_,
                                          gpuDeviceGeneration_);
      }
      if ((request.gpuConsumer || NativeTextGpuRequired()) && !gpuContext_) {
        return Failure(std::move(result),
                       text::TextStatusCode::UnsupportedFeature,
                       "text.native_gpu_unavailable", "raster", {},
                       gpuContextError_.empty()
                           ? "native Metal text rendering is unavailable"
                           : gpuContextError_);
      }

      std::vector<RenderComponent> components;
      const auto semanticBackground = RecordSemanticBackgrounds(
          effective->text, layout, renderPlan, authoredVisual);
      if (semanticBackground) {
        components.push_back({"semantic-backgrounds",
                              text::TextEffectCompositeItemKind::GlyphMaterial,
                              -1000,
                              text::TextBlendMode::SourceOver,
                              semanticBackground,
                              identities.layout,
                              0U});
      }
      std::string backdropIdentity;
      if (!RecordBackdropComponents(
              *effective, layout, request, renderPlan, authoredVisual,
              visualAssets_, components, result.diagnostics,
              backdropIdentity, error)) {
        return Failure(std::move(result), text::TextStatusCode::MissingResource,
                       "text.backdrop.render_failed", "backdrop", {},
                       std::move(error));
      }
      std::string glyphResourceIdentity;
      // The verified non-Page Letter path evaluates derivatives in the full
      // presentation target, then crops the finished RGBA result.  A tight
      // component target changes both the captured MVP decomposition and
      // hardware fwidth before the image is replayed.  Page RenderGroups use
      // the deferred native source above and therefore keep their own exact
      // target allocation.
      const auto directLetterExecution =
          useExplicitExecutionGraph
              ? ResolveDirectLetterExecutionPlan(framePlan.executionGraph.nodes,
                                                 layout.logicalBounds)
              : DirectLetterExecutionPlan{true, SkMatrix::I(), 1.0F};
      const bool directLetterExecutionTopology =
          directLetterExecution.supported;
      const SkIRect glyphComponentCrop =
          framePlan.postEffectNodes.empty() &&
                  directLetterExecutionTopology
              ? SkIRect::MakeWH(static_cast<int>(request.outputWidth),
                                static_cast<int>(request.outputHeight))
              : crop;
      const auto glyphComponentBegin = components.size();
      if (!RecordGlyphMaterialComponents(
              effective->text, effective->appearance, layout, renderPlan,
              *effectiveFonts, request.localTimeUs, authoredVisual,
              presentationMatrix, glyphComponentCrop, gpuContext_.get(),
              visualAssets_, components, result.diagnostics,
              glyphResourceIdentity, error)) {
        return Failure(std::move(result), text::TextStatusCode::MissingResource,
                       "text.glyph.material_failed", "material", {},
                       std::move(error));
      }
      const std::vector<RenderComponent> materialComponents(
          components.begin() +
              static_cast<std::ptrdiff_t>(glyphComponentBegin),
          components.end());
      std::string inlineDecorationIdentity;
      if (!RecordInlineDecorationComponents(
              effective->text, layout, renderPlan, request.localTimeUs,
              authoredVisual, visualAssets_, components,
              inlineDecorationIdentity, error)) {
        return Failure(std::move(result), text::TextStatusCode::MissingResource,
                       "text.decoration.material_failed", "material", {},
                       std::move(error));
      }
      std::string decorationIdentity;
      if (!RecordDecorationComponents(
              framePlan, authoredVisual, visualAssets_, components,
              result.diagnostics, decorationIdentity, error)) {
        return Failure(std::move(result), text::TextStatusCode::MissingResource,
                       "text.decoration.render_failed", "decoration", {},
                       std::move(error));
      }
      identities.resources = CombineIdentity(
          identities.resources,
          glyphResourceIdentity + backdropIdentity + decorationIdentity +
              inlineDecorationIdentity);

      std::unordered_map<std::string, sk_sp<SkPicture>> postOutputs;
      sk_sp<SkPicture> postDefault;
      std::vector<QtTextRenderGroupCompositeDraw>
          nativeRenderGroupComposites;
      std::string postIdentity;
      SkRect fixedPageLetterBounds = SkRect::MakeEmpty();
      SkRect authoredMaterialBounds = SkRect::MakeEmpty();
      // TextPro resolves absoluteFontSize/reflow before getLettersRect, but
      // freezes the reference-writer Letter rect union before animator
      // transforms/post. Preserve those exact binary32 rects when the native
      // Page source is available; scriptBounds is only a non-native fallback.
      for (const auto &component : components) {
        authoredMaterialBounds.join(component.authoredMaterialBounds);
        if (!component.nativePageSource)
          continue;
        for (const auto &batch : component.nativePageSource->batches)
          fixedPageLetterBounds.join(batch.fixedLetterBounds);
      }
      if (fixedPageLetterBounds.isEmpty()) {
        for (const auto &unit : layout.units)
          fixedPageLetterBounds.join(unit.scriptBounds);
      }
      if (fixedPageLetterBounds.isEmpty()) {
        for (const auto &unit : baseLayout.units)
          fixedPageLetterBounds.join(unit.scriptBounds);
      }
      SkRect stablePageLetterBounds = SkRect::MakeEmpty();
      for (const auto &unit : baseLayout.units)
        stablePageLetterBounds.join(unit.scriptBounds);
      SkRect captionCanvasPageBounds = SkRect::MakeEmpty();
      const bool usesCaptionCanvas =
          useExplicitExecutionGraph &&
          framePlan.sourceCreationComponent ==
              text::TextSourceCreationComponent::SdfText &&
          ExecutionGraphUsesCaptionCanvas(framePlan.executionGraph.nodes);
      if (usesCaptionCanvas) {
        if (!finalEffectFrame.canvasRect) {
          return Failure(
              std::move(result), text::TextStatusCode::InvalidDocument,
              "text.execution_graph.canvas_missing", "render_graph", {},
              "caption layout requires the unpadded source canvas");
        }
        const auto canvas = ToSkRect(*finalEffectFrame.canvasRect);
        float fontSize = 0.0F;
        for (const auto &unit : layout.units)
          fontSize = std::max(fontSize, unit.canvasCellFontSize);
        // CaptionModule's SDFText canvas is centred at the source origin.
        // getRectExpanded adds the canonical 30/150-em guard at each edge;
        // the Letter quad's padded ink union is a different geometry class.
        const float guard = fontSize * (30.0F / 150.0F);
        captionCanvasPageBounds = SkRect::MakeXYWH(
            effective->text.referenceCanvas.width * 0.5F -
                canvas.width() * 0.5F - guard,
            effective->text.referenceCanvas.height * 0.5F -
                canvas.height() * 0.5F - guard,
            canvas.width() + guard * 2.0F,
            canvas.height() + guard * 2.0F);
        if (std::getenv("VIDEOCUT_TRACE_TEXT_RENDER_GROUP_TOPOLOGY") != nullptr) {
          std::fprintf(stderr,
                       "[VIDEOCUT_TEXT_CAPTION_CANVAS] width=%.9g height=%.9g "
                       "guard=%.9g source_scale=%.9g\n",
                       canvas.width(), canvas.height(), guard,
                       effectBridge.sourceToReferenceScale);
        }
      }
      const SkRect pageDomainLetterBounds =
          usesCaptionCanvas
              ? captionCanvasPageBounds
              : useExplicitExecutionGraph &&
                  FramePlanRequiresStablePageDomain(framePlan) &&
                  !stablePageLetterBounds.isEmpty() &&
                  stablePageLetterBounds.isFinite()
              ? stablePageLetterBounds
              : fixedPageLetterBounds;
      const auto *presentationSceneMaterial =
          useExplicitExecutionGraph
              ? ResolvePresentationSceneMaterial(
                    framePlan.executionGraph.nodes)
              : nullptr;
      const bool usePresentationSceneExecution =
          presentationSceneMaterial &&
          framePlan.sourceCreationComponent ==
              text::TextSourceCreationComponent::SdfText &&
          !effective->appearance.path.enabled &&
          (!effective->appearance.bend.enabled ||
           std::fabs(effective->appearance.bend.amount) < 0.0001F);
      bool usePresentationParticleExecution = false;
      std::string compositeIdentity;
      sk_sp<SkPicture> finalPicture;
      if (useExplicitExecutionGraph) {
        const auto *sourceCanvasMaterial = FindSdfSourceCanvasMaterial(
            framePlan.executionGraph.nodes, error);
        if (!error.empty()) {
          return Failure(
              std::move(result), text::TextStatusCode::InvalidDocument,
              "text.execution_graph.source_canvas_conflict", "render_graph", {}, error);
        }
        if (!sourceCanvasMaterial) sourceCanvasMaterial = presentationSceneMaterial;
        float sourceAttachmentGuard = 0.0F;
        if (sourceCanvasMaterial) {
          for (const auto &unit : layout.units)
            sourceAttachmentGuard = std::max(
                sourceAttachmentGuard, unit.canvasCellFontSize * (30.0F / 150.0F));
        }
        std::optional<text::TextExecutionNodeEvidence> sourceAttachmentEvidence;
        std::optional<PageRenderGroupDomain> executionPageDomain;
        if (sourceCanvasMaterial) {
          TextExecutionParameterConsumptionScope sourceParameters(
              *sourceCanvasMaterial, result.executionEvidence.has_value());
          executionPageDomain = ResolveSdfSourceAttachmentDomain(
              *sourceCanvasMaterial, effective->text.referenceCanvas.width,
              effective->text.referenceCanvas.height, sourceAttachmentGuard,
              framePlan, effective->animations, presentationMatrix);
          if (result.executionEvidence) {
            sourceAttachmentEvidence.emplace(
                sourceCanvasMaterial->nodeId, sourceCanvasMaterial->active,
                executionPageDomain.has_value(), sourceCanvasMaterial->parameters.size());
            sourceParameters.Finish(*sourceAttachmentEvidence);
          }
        } else {
          executionPageDomain = ResolveExecutionGraphPageRenderGroupDomain(
              framePlan, effective->animations, presentationMatrix, pageDomainLetterBounds);
        }
        if (sourceCanvasMaterial && !executionPageDomain) {
          return Failure(
              std::move(result), text::TextStatusCode::InvalidDocument,
              "text.execution_graph.scene_source_invalid", "render_graph",
              {}, "text presentation scene has no closed SDF source target");
        }
        std::vector<RenderComponent> pageExecutionComponents;
        std::vector<RenderComponent> pageExecutionMaterialComponents;
        const std::vector<RenderComponent> *executionComponents = &components;
        const std::vector<RenderComponent> *executionMaterialComponents =
            &materialComponents;
        SkRect executionBounds = authoredVisual;
        if (executionPageDomain) {
          const auto scoped = ResolvePageRenderGroupComponents(
              *executionPageDomain, framePlan, components);
          pageExecutionComponents = MakePageLocalComponents(
              scoped, *executionPageDomain, gpuContext_.get(), error);
          if (pageExecutionComponents.empty() && !scoped.empty()) {
            return Failure(
                std::move(result), text::TextStatusCode::Failed,
                "text.execution_graph.page_source_failed", "render_graph", {},
                error.empty()
                    ? "text execution graph Page source could not be materialized"
                    : std::move(error));
          }
          for (const auto &component : pageExecutionComponents) {
            if (component.kind ==
                text::TextEffectCompositeItemKind::GlyphMaterial) {
              pageExecutionMaterialComponents.push_back(component);
            }
          }
          executionComponents = &pageExecutionComponents;
          executionMaterialComponents = &pageExecutionMaterialComponents;
          executionBounds = usePresentationSceneExecution
                                ? SkRect::MakeWH(
                                      static_cast<float>(request.outputWidth),
                                      static_cast<float>(request.outputHeight))
                                : executionPageDomain->localBounds;
        }
        SkPoint executionScenePivot = SkPoint::Make(
            effective->text.layoutBox.x +
                effective->text.layoutBox.width * 0.5F,
            effective->text.layoutBox.y +
                effective->text.layoutBox.height * 0.5F);
        if (executionPageDomain) {
          executionScenePivot =
              executionPageDomain->authoredToDevice.mapPoint(
                  executionScenePivot);
          executionScenePivot.offset(
              -executionPageDomain->deviceTargetBounds.left(),
              -executionPageDomain->deviceTargetBounds.top());
          executionScenePivot.set(
              executionScenePivot.x() * executionPageDomain->rasterScaleX,
              executionScenePivot.y() * executionPageDomain->rasterScaleY);
        }
        if (!ExecuteTextEffectExecutionGraph(
                renderPlan, request, *executionComponents,
                *executionMaterialComponents, layout,
                executionPageDomain ? &*executionPageDomain : nullptr,
                executionBounds, executionScenePivot,
                usePresentationSceneExecution
                    ? static_cast<float>(request.outputWidth) /
                          effective->text.referenceCanvas.width
                    : 1.0F,
                usePresentationSceneExecution
                    ? static_cast<float>(request.outputHeight) /
                          effective->text.referenceCanvas.height
                    : 1.0F,
                effective->text.referenceCanvas.width,
                effective->text.referenceCanvas.height,
                effectBridge.sourceToReferenceScale,
                visualAssets_, renderGraphInstanceId_, gpuContext_.get(),
                executionGraphHistories_, postOutputs, postDefault,
                usePresentationParticleExecution,
                result.executionEvidence ? &*result.executionEvidence : nullptr,
                sourceAttachmentEvidence ? &*sourceAttachmentEvidence : nullptr,
                postIdentity, error)) {
          return Failure(std::move(result), text::TextStatusCode::Failed,
                         "text.execution_graph.failed", "render_graph", {},
                         std::move(error));
        }
        if (executionPageDomain && postDefault &&
            !usePresentationSceneExecution &&
            !usePresentationParticleExecution) {
          if (!gpuContext_) {
            return Failure(std::move(result), text::TextStatusCode::Failed,
                           "text.execution_graph.page_gpu_missing",
                           "render_graph", {},
                           "text execution graph Page requires a GPU context");
          }
          auto materialized = MaterializePageRenderGroup(
              postDefault, *executionPageDomain, *gpuContext_, error);
          if (!materialized.authoredPicture || !materialized.pageImage) {
            return Failure(
                std::move(result), text::TextStatusCode::Failed,
                "text.execution_graph.page_materialize_failed", "render_graph",
                {},
                error.empty()
                    ? "text execution graph Page output could not be materialized"
                    : std::move(error));
          }
          if (std::fabs(executionPageDomain->rasterScaleX - 1.0F) <
                  0.000001F &&
              std::fabs(executionPageDomain->rasterScaleY - 1.0F) <
                  0.000001F) {
            auto compositeRequest = BuildQtTextRenderGroupCompositeRequest(
                *executionPageDomain, static_cast<int>(request.outputWidth),
                static_cast<int>(request.outputHeight),
                effective->text.referenceCanvas.width,
                effective->text.referenceCanvas.height);
            if (!compositeRequest) {
              return Failure(
                  std::move(result), text::TextStatusCode::Failed,
                  "text.execution_graph.page_composite_invalid",
                  "render_graph", {},
                  "text execution graph Page has no closed native composite");
            }
            nativeRenderGroupComposites.push_back(
                QtTextRenderGroupCompositeDraw{
                    materialized.pageImage, std::move(*compositeRequest)});
          }
          postDefault = std::move(materialized.authoredPicture);
        }
        compositeIdentity = postIdentity;
        finalPicture = postDefault;
      } else {
        if (!ExecutePostEffectGraph(
                renderPlan, request, effective->animations, layout, components,
                authoredVisual, presentationMatrix, pageDomainLetterBounds,
                effective->text.referenceCanvas.width,
                effective->text.referenceCanvas.height, renderGraphInstanceId_,
                generation_, effectRuntimeClockStore_, gpuContext_.get(),
                postOutputs, postDefault, nativeRenderGroupComposites,
                result.diagnostics,
                result.executionEvidence ? &*result.executionEvidence : nullptr,
                postIdentity, error)) {
          return Failure(std::move(result), text::TextStatusCode::Failed,
                         "text.post_effect.graph_failed", "post_effect", {},
                         std::move(error));
        }
        finalPicture = ResolveFinalComposite(
            framePlan, components, postOutputs, postDefault, authoredVisual,
            compositeIdentity);
      }
      if (!finalPicture) {
        result.status = text::TextStatusCode::Empty;
        result.diagnostics.push_back(MakeDiagnostic(
            "text.raster.empty", DiagnosticSeverity::Warning, "raster", {},
            "text document produced no drawable component"));
        return result;
      }
      identities.backdrops =
          CombineIdentity(identities.backdrops, backdropIdentity);
      identities.glyphs = CombineIdentity(
          identities.glyphs,
          glyphResourceIdentity + inlineDecorationIdentity);
      if (!useExplicitExecutionGraph) {
        identities.postEffects =
            CombineIdentity(identities.postEffects, postIdentity);
      } else {
        identities.executionGraph =
            CombineIdentity(identities.executionGraph, postIdentity);
      }
      IdentityBuilder exactComposite;
      exactComposite.AddString(identities.resources);
      exactComposite.AddString(identities.layout);
      exactComposite.AddString(identities.glyphs);
      exactComposite.AddString(identities.backdrops);
      exactComposite.AddString(identities.postEffects);
      exactComposite.AddString(identities.executionGraph);
      exactComposite.AddString(decorationFrameIdentity);
      exactComposite.AddString(compositeOrderIdentity);
      exactComposite.AddString(appearanceFrameIdentity);
      exactComposite.AddString(compositeIdentity);
      exactComposite.AddString(PresentationIdentity(request));
      identities.composite = CombineIdentity(
          framePlan.cacheIdentities.composite, exactComposite.Finish());
      if (resolvedCacheAliases_.size() >= 256U)
        resolvedCacheAliases_.clear();
      resolvedCacheAliases_[preliminaryCompositeIdentity] =
          identities.composite;

      const float effectiveAlpha = std::clamp(
          effective->appearance.globalAlpha * request.presentation.opacity,
          0.0F, 1.0F);
      bool hasOutsidePageComponent = false;
      sk_sp<SkPicture> nativeBeforeRenderGroupPicture;
      sk_sp<SkPicture> nativeAfterRenderGroupPicture;
      std::vector<QtTextFollowerComponentDraw>
          nativeBeforeRenderGroupFollowers;
      std::vector<QtTextFollowerComponentDraw>
          nativeAfterRenderGroupFollowers;
      if (!nativeRenderGroupComposites.empty()) {
        const text::TextEffectRenderGroupExecutionPlan *execution = nullptr;
        for (const auto &node : framePlan.postEffectNodes) {
          if (node.renderGroup &&
              node.renderGroup->spec.mode ==
                  text::TextRenderGroupMode::Page) {
            execution = &*node.renderGroup;
            break;
          }
        }
        std::unordered_set<std::string> afterEffectDecorationIds;
        for (const auto &pass : framePlan.decorationPasses) {
          if (pass.effectScope ==
              text::TextEffectDecorationEffectScope::AfterRenderGroupEffect) {
            afterEffectDecorationIds.insert(pass.passId);
          }
        }
        if (execution) {
          std::vector<RenderComponent> before;
          std::vector<RenderComponent> after;
          for (const auto &component : components) {
            if (!component.picture || PageRenderGroupIncludesComponent(
                                          *execution,
                                          afterEffectDecorationIds,
                                          component)) {
              continue;
            }
            hasOutsidePageComponent = true;
            if (afterEffectDecorationIds.contains(component.id))
              after.push_back(component);
            else
              before.push_back(component);
          }
          const auto resolveNativeFollowers = [](const auto &side,
                                                 auto &nativeFollowers) {
            if (side.empty() ||
                !std::all_of(side.begin(), side.end(), [](const auto &item) {
                  return item.nativeFollower.has_value();
                })) {
              return false;
            }
            std::vector<const RenderComponent *> ordered;
            ordered.reserve(side.size());
            for (const auto &component : side)
              ordered.push_back(&component);
            std::stable_sort(
                ordered.begin(), ordered.end(),
                [](const auto *left, const auto *right) {
                  if (left->zOrder != right->zOrder)
                    return left->zOrder < right->zOrder;
                  return left->authoredOrder < right->authoredOrder;
                });
            nativeFollowers.reserve(ordered.size());
            for (const auto *component : ordered)
              nativeFollowers.push_back(*component->nativeFollower);
            return true;
          };
          if (!resolveNativeFollowers(before,
                                      nativeBeforeRenderGroupFollowers)) {
            nativeBeforeRenderGroupPicture =
                ComposePictures(before, authoredVisual);
          }
          if (!resolveNativeFollowers(after,
                                      nativeAfterRenderGroupFollowers)) {
            nativeAfterRenderGroupPicture =
                ComposePictures(after, authoredVisual);
          }
        }
      }
      const bool useNativeRenderGroupComposite =
          !nativeRenderGroupComposites.empty() &&
          !hasOutsidePageComponent &&
          !effective->appearance.path.enabled &&
          (!effective->appearance.bend.enabled ||
           std::fabs(effective->appearance.bend.amount) < 0.0001F);

      // The pre-refactor Letter path has a separate no-post fast path.  It
      // submits the immutable typed Letter packet directly to a
      // presentation-sized target; replaying the component's already
      // materialized picture would quantize it once more through the
      // authored/presentation matrix (and, for Bubble, drops the native
      // nine-slice/text anchor relationship).  Keep this decision at the
      // final FramePlan boundary: no template-id special case and no second
      // selector/evaluator path.
      std::unordered_map<std::string, sk_sp<SkPicture>>
          nativeDirectLetterPictures;
      bool nativeDirectLetterSupported =
          framePlan.postEffectNodes.empty() &&
          directLetterExecutionTopology && gpuContext_ &&
          !effective->appearance.path.enabled &&
          (!effective->appearance.bend.enabled ||
           std::fabs(effective->appearance.bend.amount) < 0.0001F) &&
          effectiveAlpha >= 0.9999F;
      if (nativeDirectLetterSupported) {
        PageRenderGroupDomain directDomain;
        directDomain.authoredToDevice.setConcat(
            presentationMatrix, directLetterExecution.authoredTransform);
        if (!presentationMatrix.invert(&directDomain.deviceToAuthored)) {
          nativeDirectLetterSupported = false;
        } else {
          directDomain.deviceTargetBounds = SkRect::MakeWH(
              static_cast<float>(request.outputWidth),
              static_cast<float>(request.outputHeight));
          directDomain.localBounds = directDomain.deviceTargetBounds;
          directDomain.targetWidth = static_cast<int>(request.outputWidth);
          directDomain.targetHeight = static_cast<int>(request.outputHeight);
          directDomain.rasterScaleX = 1.0F;
          directDomain.rasterScaleY = 1.0F;
          bool foundNative = false;
          for (const auto &component : components) {
            if (!component.nativePageSource)
              continue;
            std::string nativeError;
            auto picture = RenderQtTextNativePageSource(
                *component.nativePageSource, directDomain, nullptr,
                gpuContext_.get(), nativeError);
            if (!picture) {
              nativeDirectLetterSupported = false;
              if (std::getenv("VIDEOCUT_TRACE_QT_TEXT_LETTER_NATIVE") !=
                  nullptr) {
                std::fprintf(
                    stderr,
                    "[VIDEOCUT_QT_TEXT_LETTER_NATIVE] "
                    "stage=direct_render_failed id=%s reason=%s\n",
                    component.id.c_str(), nativeError.c_str());
              }
              break;
            }
            nativeDirectLetterPictures.emplace(component.id,
                                               std::move(picture));
            foundNative = true;
          }
          nativeDirectLetterSupported =
              nativeDirectLetterSupported && foundNative &&
              nativeDirectLetterPictures.size() ==
                  std::count_if(components.begin(), components.end(),
                                [](const auto &component) {
                                  return component.nativePageSource != nullptr;
                                });
        }
      }
      if (std::getenv("VIDEOCUT_TRACE_TEXT_RENDER_GROUP_TOPOLOGY") != nullptr) {
        std::fprintf(stderr,
                     "[VIDEOCUT_TEXT_RENDER_GROUP_FINAL] native=%d "
                     "direct_letter=%d outside=%d components=%zu\n",
                     useNativeRenderGroupComposite ? 1 : 0,
                     nativeDirectLetterSupported ? 1 : 0,
                     hasOutsidePageComponent ? 1 : 0, components.size());
      }
      if (useNativeRenderGroupComposite || usePresentationSceneExecution ||
          usePresentationParticleExecution) {
        crop = SkIRect::MakeWH(static_cast<int>(request.outputWidth),
                               static_cast<int>(request.outputHeight));
        const auto presentationPixels =
            static_cast<std::uint64_t>(crop.width()) *
            static_cast<std::uint64_t>(crop.height());
        surfaceBytes = presentationPixels *
                       (useHighPrecisionPublication ? 8U : 4U);
        if (surfaceBytes > options_.maximumSurfaceBytes) {
          return Failure(
              std::move(result), text::TextStatusCode::BudgetExceeded,
              "text.surface.byte_budget", "raster", {},
              "text presentation surface exceeds the lane byte budget");
        }
      }

      const auto info = SkImageInfo::Make(
          crop.width(), crop.height(),
          useHighPrecisionPublication ? kRGBA_F16_SkColorType
                                      : kRGBA_8888_SkColorType,
          kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
      std::string surfaceError;
      auto surface = gpuContext_ ? gpuContext_->MakeSurface(info, surfaceError)
                                 : SkSurfaces::Raster(info);
      if (!surface) {
        return Failure(std::move(result), text::TextStatusCode::Failed,
                       "text.surface.allocation_failed", "raster", {},
                       surfaceError.empty()
                           ? "Skia text surface allocation failed"
                           : std::move(surfaceError));
      }
      auto *canvas = surface->getCanvas();
      canvas->clear(SK_ColorTRANSPARENT);
      if (useNativeRenderGroupComposite) {
        const auto drawNativeOutside = [&](const sk_sp<SkPicture> &picture) {
          if (!picture)
            return;
          canvas->save();
          canvas->concat(presentationMatrix);
          canvas->drawPicture(picture);
          canvas->restore();
        };
        const auto compositeNativeFollowers =
            [&](const std::vector<QtTextFollowerComponentDraw> &followers) {
              for (const auto &follower : followers) {
                SkMatrix imageToPresentation;
                imageToPresentation.setConcat(presentationMatrix,
                                              follower.imageToAuthored);
                std::array<float, 9> values{};
                imageToPresentation.get9(values.data());
                QtTextFollowerCompositeRequest followerRequest;
                followerRequest.presentationWidth =
                    static_cast<int>(request.outputWidth);
                followerRequest.presentationHeight =
                    static_cast<int>(request.outputHeight);
                followerRequest.imageToPresentation = values;
                followerRequest.opacity = follower.opacity;
                if (!gpuContext_->CompositeQtTextFollower(
                        follower.image, *surface, followerRequest,
                        surfaceError)) {
                  return false;
                }
              }
              return true;
            };
        if (!compositeNativeFollowers(nativeBeforeRenderGroupFollowers)) {
          return Failure(
              std::move(result), text::TextStatusCode::Failed,
              "text.follower.native_composite_failed", "paint", {},
              surfaceError.empty() ? "native Qt follower composite failed"
                                   : std::move(surfaceError));
        }
        drawNativeOutside(nativeBeforeRenderGroupPicture);
        for (const auto &nativeRenderGroupComposite :
             nativeRenderGroupComposites) {
          auto renderGroupRequest = nativeRenderGroupComposite.request;
          renderGroupRequest.alpha *= effectiveAlpha;
          if (!gpuContext_->CompositeQtTextRenderGroup(
                  nativeRenderGroupComposite.pageImage, *surface,
                  renderGroupRequest, surfaceError)) {
            return Failure(
                std::move(result), text::TextStatusCode::Failed,
                "text.render_group.native_composite_failed", "paint", {},
                surfaceError.empty()
                    ? "native Qt RenderGroup composite failed"
                    : std::move(surfaceError));
          }
        }
        drawNativeOutside(nativeAfterRenderGroupPicture);
        if (!compositeNativeFollowers(nativeAfterRenderGroupFollowers)) {
          return Failure(
              std::move(result), text::TextStatusCode::Failed,
              "text.follower.native_composite_failed", "paint", {},
              surfaceError.empty() ? "native Qt follower composite failed"
                                   : std::move(surfaceError));
        }
      } else if (usePresentationSceneExecution ||
                 usePresentationParticleExecution) {
        canvas->translate(-static_cast<float>(crop.x()),
                          -static_cast<float>(crop.y()));
        if (effectiveAlpha < 0.9999F) {
          SkPaint opacity;
          opacity.setAlphaf(effectiveAlpha);
          canvas->drawPicture(finalPicture, nullptr, &opacity);
        } else {
          canvas->drawPicture(finalPicture);
        }
      } else if (nativeDirectLetterSupported) {
        // Native Letter pictures are already in top-left presentation pixel
        // coordinates.  Other components (backdrops, inline decorations,
        // semantic backgrounds) remain authored-space pictures and therefore
        // receive the same presentation transform as the old direct path.
        std::vector<const RenderComponent *> ordered;
        ordered.reserve(components.size());
        for (const auto &component : components) {
          if (component.picture || component.nativePageSource)
            ordered.push_back(&component);
        }
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const auto *left, const auto *right) {
                           if (left->zOrder != right->zOrder)
                             return left->zOrder < right->zOrder;
                           return left->authoredOrder < right->authoredOrder;
                         });
        for (const auto *component : ordered) {
          if (const auto found = nativeDirectLetterPictures.find(component->id);
              found != nativeDirectLetterPictures.end()) {
            canvas->save();
            canvas->translate(-static_cast<float>(crop.x()),
                              -static_cast<float>(crop.y()));
            if (directLetterExecution.opacity < 0.9999F) {
              SkPaint opacity;
              opacity.setAlphaf(directLetterExecution.opacity);
              canvas->drawPicture(found->second, nullptr, &opacity);
            } else {
              canvas->drawPicture(found->second);
            }
            canvas->restore();
            continue;
          }
          if (!component->picture)
            continue;
          canvas->save();
          canvas->translate(-static_cast<float>(crop.x()),
                            -static_cast<float>(crop.y()));
          // `presentationMatrix` is the single matrix sampled for this
          // FramePlan.  Concatenating it directly preserves the old
          // applyContentTransform order (including binary32 translation and
          // pivot) and avoids recomputing a center from a post-reflow bounds.
          SkMatrix directPresentationMatrix;
          directPresentationMatrix.setConcat(
              presentationMatrix, directLetterExecution.authoredTransform);
          canvas->concat(directPresentationMatrix);
          if (directLetterExecution.opacity < 0.9999F) {
            SkPaint opacity;
            opacity.setAlphaf(directLetterExecution.opacity);
            canvas->drawPicture(component->picture, nullptr, &opacity);
          } else {
            canvas->drawPicture(component->picture);
          }
          canvas->restore();
          for (const auto &draw : component->recordedResourceDraws)
            TextMaterialExecutionScope::Record(
                draw.layerId, draw.capability, draw.executor,
                draw.submittedDraws);
        }
      } else {
        canvas->translate(-static_cast<float>(crop.x()),
                          -static_cast<float>(crop.y()));
        ApplyPresentationTransform(canvas, effective->text, request,
                                   layout.logicalBounds);
        if (effectiveAlpha < 0.9999F) {
          SkPaint opacity;
          opacity.setAlphaf(effectiveAlpha);
          canvas->saveLayer(&authoredVisual, &opacity);
        }
        if (path.enabled) {
          if (!DrawPictureOnPath(canvas, *finalPicture, deformationInk,
                                 authoredVisual, path)) {
            return Failure(
                std::move(result), text::TextStatusCode::Failed,
                "text.path.mesh_allocation_failed", "paint", {},
                "Skia text path mesh allocation failed");
          }
        } else if (bend.enabled && std::fabs(bend.amount) >= 0.0001F) {
          const float left = authoredVisual.left();
          const float width = std::max(1.0F, authoredVisual.width());
          const int slices =
              std::clamp(static_cast<int>(std::ceil(width / 48.0F)), 12, 64);
          const float sliceWidth = width / static_cast<float>(slices);
          const float verticalOutset =
              std::fabs(bend.amount) * bend.height * 0.25F + 2.0F;
          for (int index = 0; index < slices; ++index) {
            const float sliceLeft =
                left + sliceWidth * static_cast<float>(index);
            const float sliceRight = index + 1 == slices
                                         ? authoredVisual.right()
                                         : sliceLeft + sliceWidth;
            canvas->save();
            canvas->clipRect(
                SkRect::MakeLTRB(
                    sliceLeft, authoredVisual.top() - verticalOutset,
                    sliceRight, authoredVisual.bottom() + verticalOutset),
                SkClipOp::kIntersect, false);
            canvas->translate(
                0.0F, BendOffsetY(bend, (sliceLeft + sliceRight) * 0.5F));
            canvas->drawPicture(finalPicture);
            canvas->restore();
          }
        } else {
          canvas->drawPicture(finalPicture);
        }
        if (effectiveAlpha < 0.9999F)
          canvas->restore();
      }
      if (!request.gpuConsumer && gpuContext_ &&
          !gpuContext_->WaitForSubmittedWork(error)) {
        return Failure(std::move(result), text::TextStatusCode::Failed,
                       "text.frame.producer_failed", "publication", {},
                       std::move(error));
      }
      auto published =
          request.gpuConsumer
              ? gpuContext_->PublishFrame(*surface, request.localTimeUs,
                    generation_, request.delivery ==
                        text::TextRasterDelivery::PremultipliedComposite,
                    request.cancel)
          : request.delivery == text::TextRasterDelivery::PremultipliedComposite
              ? PublishPremultipliedRgba(
                    *surface, static_cast<std::uint32_t>(crop.width()),
                    static_cast<std::uint32_t>(crop.height()),
                    request.localTimeUs, generation_, request.cancel)
              : PublishStraightRgba(
                    *surface, static_cast<std::uint32_t>(crop.width()),
                    static_cast<std::uint32_t>(crop.height()),
                    request.localTimeUs, generation_, request.cancel);
      if (!published.frame) {
        if (published.error == "canceled") {
          result.status = text::TextStatusCode::Canceled;
          return result;
        }
        return Failure(std::move(result), text::TextStatusCode::Failed,
                       "text.frame.publish_failed", "publication", {},
                       std::move(published.error));
      }

      result.image = std::move(published.frame);
      result.status = text::TextStatusCode::Ok;
      if (result.executionEvidence) {
        result.executionEvidence->completed = true;
        result.executionEvidence->executionGraphParameterConsumptionVerified =
            std::all_of(result.executionEvidence->nodes.begin(),
                        result.executionEvidence->nodes.end(),
                        [](const auto &node) {
                          return node.completed &&
                                 (!node.active || node.parameterConsumptionVerified);
                        });
      }
      result.originX = crop.x();
      result.originY = crop.y();
      result.layout = layout.publicLayout;
      // Authoring bounds include the neutral template appearance. Sampled
      // per-letter motion and post-effect render padding stay render-only.
      result.layout.authoredControlBounds = ResolveAuthoredControlBounds(
          *document_, baseLayout, authoredMaterialBounds);
      MapLayoutToPresentation(result.layout, presentationMatrix,
                              authoredVisual, liveLetterBounds, bend, path,
                              request.outputWidth, request.outputHeight);
      result.logicalBounds = result.layout.logicalBounds;
      result.inkBounds = result.layout.inkBounds;
      result.documentGeneration = generation_;
      if (result.diagnostics.size() > config_.maximumDiagnostics)
        result.diagnostics.resize(config_.maximumDiagnostics);
      if (reusableFrame) {
        frameCache_.Store(identities.composite, result,
                          result.image.residentBytes());
      }
      return result;
    } catch (const std::exception &exception) {
      return Failure(std::move(result), text::TextStatusCode::Failed,
                     "text.render.exception", "render", {},
                     exception.what());
    } catch (...) {
      return Failure(std::move(result), text::TextStatusCode::Failed,
                     "text.render.unknown_failure", "render", {},
                     "unknown Skia text rendering failure");
    }
  }

private:
  static text::TextRenderResult Failure(text::TextRenderResult result,
                                        const text::TextStatusCode status,
                                        std::string code, std::string stage,
                                        std::string subject,
                                        std::string message) {
    result.status = status;
    result.diagnostics.push_back(MakeDiagnostic(
        std::move(code), DiagnosticSeverity::Error, std::move(stage),
        std::move(subject), std::move(message)));
    return result;
  }

  text::TextRenderOptions options_;
  SkiaRuntimeConfig config_;
  std::shared_ptr<FontContextCache> fontCache_;
  const std::uint64_t renderGraphInstanceId_{
      NextTextPostEffectGraphInstanceId()};
  mutable std::mutex mutex_;
  std::optional<text::TextRenderDocument> document_;
  std::shared_ptr<FontContext> fonts_;
  std::string fontIdentity_;
  std::vector<text::FontRunResolutionReceipt> installedFontResolutions_;
  std::uint64_t generation_{0U};
  std::int32_t gpuDeviceIndex_{-1};
  std::uint64_t gpuDeviceGeneration_{1};
  FrameResultCache frameCache_;
  std::unordered_map<std::string, std::string> resolvedCacheAliases_;
  TextVisualAssetStore visualAssets_;
  text::EffectRuntimeClockStore effectRuntimeClockStore_;
  std::unordered_map<std::string, TextExecutionGraphHistoryState>
      executionGraphHistories_;
  bool gpuContextAttempted_{false};
  std::string gpuContextError_;
  std::unique_ptr<SkiaGpuContext> gpuContext_;
};

class TextFactory final : public text::TextRendererFactory {
public:
  explicit TextFactory(SkiaRuntimeConfig config)
      : config_(std::move(config)),
        fontCache_(std::make_shared<FontContextCache>()) {}

  text::TextRenderLaneHolder
  CreateLane(const text::TextRenderOptions &options,
             std::string &error) const override {
    error.clear();
    if (options.maximumWidth == 0U || options.maximumHeight == 0U ||
        options.maximumSurfaceBytes == 0U) {
      error = "text render lane budget is invalid";
      return {};
    }
    return std::make_shared<TextLane>(options, config_, fontCache_);
  }

private:
  SkiaRuntimeConfig config_;
  std::shared_ptr<FontContextCache> fontCache_;
};

} // namespace

text::TextRendererFactoryHolder
MakeTextRendererFactory(const SkiaRuntimeConfig &config, std::string &error) {
  error.clear();
  if (!SkUnicodes::ICU::Make()) {
    error = "SkUnicode ICU implementation is unavailable";
    return {};
  }
  return std::make_shared<TextFactory>(config);
}

} // namespace videocut::skia_runtime::internal
