#include "text/TextRenderPipeline.h"

namespace videocut::skia_runtime::internal::text_lane {


bool ExecutePostEffectGraph(
    const TextRenderFramePlan &renderPlan,
    const text::TextRenderRequest &request,
    const text::TextAnimationStack &animations,
    const ResolvedLayout &layout,
    const std::vector<RenderComponent> &components,
    const SkRect &recordingBounds, const SkMatrix &presentationMatrix,
    const SkRect &fixedPageLetterBounds,
    const float referenceWidth, const float referenceHeight,
    const std::uint64_t graphInstanceId, const std::uint64_t lifecycleEpoch,
    text::EffectRuntimeClockStore &runtimeClockStore,
    SkiaGpuContext *gpuContext,
    std::unordered_map<std::string, sk_sp<SkPicture>> &outputs,
    sk_sp<SkPicture> &current,
    std::vector<QtTextRenderGroupCompositeDraw> &nativeComposites,
    std::vector<Diagnostic> &diagnostics,
    text::TextExecutionEvidence *executionEvidence,
    std::string &identity, std::string &error) {
  const auto &framePlan = renderPlan.value();
  nativeComposites.clear();
  const auto pageDomain = ResolvePageRenderGroupDomain(
      framePlan, presentationMatrix, fixedPageLetterBounds);
  std::vector<RenderComponent> pageComponents;
  const auto globalSource = ComposePictures(components, recordingBounds);
  sk_sp<SkPicture> pageSource;
  if (pageDomain) {
    if (!gpuContext) {
      error = "text RenderGroup Page requires the native GPU context";
      return false;
    }
    const auto scoped = ResolvePageRenderGroupComponents(
        *pageDomain, framePlan, components);
    pageComponents = MakePageLocalComponents(scoped, *pageDomain, gpuContext, error);
    if (pageComponents.empty() && !scoped.empty()) {
      if (error.empty())
        error = "text RenderGroup Page source could not be materialized";
      return false;
    }
    pageSource = ComposePictures(pageComponents, pageDomain->localBounds);
    if (std::getenv("VIDEOCUT_TRACE_TEXT_RENDER_GROUP_TOPOLOGY") != nullptr ||
        std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
      std::fprintf(
          stderr,
          "[VIDEOCUT_TEXT_RENDER_GROUP] layer=%s fixed=[%.9g %.9g %.9g "
          "%.9g] visual=[%.9g %.9g %.9g %.9g] "
          "source_bounds=[%a %a %a %a] "
          "target_bounds=[%.9g %.9g %.9g %.9g] target=%dx%d "
          "raster_scale=[%.9g %.9g] inputs=%zu\n",
          pageDomain->execution.layerId.c_str(),
          pageDomain->execution.fixedGeometryBounds.x,
          pageDomain->execution.fixedGeometryBounds.y,
          pageDomain->execution.fixedGeometryBounds.width,
          pageDomain->execution.fixedGeometryBounds.height,
          pageDomain->execution.visualSourceBounds.x,
          pageDomain->execution.visualSourceBounds.y,
          pageDomain->execution.visualSourceBounds.width,
          pageDomain->execution.visualSourceBounds.height,
          static_cast<double>(fixedPageLetterBounds.left()),
          static_cast<double>(fixedPageLetterBounds.top()),
          static_cast<double>(fixedPageLetterBounds.right()),
          static_cast<double>(fixedPageLetterBounds.bottom()),
          pageDomain->deviceTargetBounds.left(),
          pageDomain->deviceTargetBounds.top(),
          pageDomain->deviceTargetBounds.right(),
          pageDomain->deviceTargetBounds.bottom(), pageDomain->targetWidth,
          pageDomain->targetHeight, pageDomain->rasterScaleX,
          pageDomain->rasterScaleY, pageComponents.size());
      for (const auto &component : pageComponents) {
        std::fprintf(stderr,
                     "[VIDEOCUT_TEXT_RENDER_GROUP_INPUT] id=%s kind=%u\n",
                     component.id.c_str(),
                     static_cast<unsigned>(component.kind));
      }
    }
  }
  current = globalSource;
  IdentityBuilder cacheIdentity;
  outputs.clear();
  std::unordered_set<std::string> pageOutputIds;
  std::unordered_map<const SkPicture *, MaterializedPageRenderGroup>
      materializedPagePictures;
  const auto materializePage = [&](const sk_sp<SkPicture> &picture)
      -> const MaterializedPageRenderGroup * {
    if (!pageDomain || !gpuContext || !picture)
      return nullptr;
    const auto found = materializedPagePictures.find(picture.get());
    if (found != materializedPagePictures.end())
      return &found->second;
    auto result = MaterializePageRenderGroup(
        picture, *pageDomain, *gpuContext, error);
    if (!result.authoredPicture || !result.pageImage)
      return nullptr;
    const auto [inserted, _] = materializedPagePictures.emplace(
        picture.get(), std::move(result));
    return &inserted->second;
  };
  std::unordered_set<std::string> declaredNodeIds;
  for (const auto &node : framePlan.postEffectNodes) {
    if (node.nodeId.empty() || !declaredNodeIds.insert(node.nodeId).second) {
      error = "text post-effect graph contains an empty node identity";
      return false;
    }
  }

  const auto nodeRunsInPage = [&](const text::TextEffectPostEffectNode &node) {
    return pageDomain && node.renderGroup &&
           node.renderGroup->spec.mode == text::TextRenderGroupMode::Page &&
           node.renderGroup->renderGroupInstanceId ==
               pageDomain->execution.renderGroupInstanceId;
  };
  std::unordered_set<std::string> executedNodeIds;
  std::string lastExecutedNodeId;
  while (executedNodeIds.size() < framePlan.postEffectNodes.size()) {
    bool madeProgress = false;
    for (const auto &node : framePlan.postEffectNodes) {
      if (executedNodeIds.contains(node.nodeId))
        continue;
      bool waitingForDependency = false;
      for (const auto &inputId : node.inputIds) {
        if (outputs.contains(inputId) ||
            std::any_of(components.begin(), components.end(),
                        [&](const auto &component) {
                          return component.id == inputId;
                        })) {
          continue;
        }
        if (declaredNodeIds.contains(inputId)) {
          waitingForDependency = true;
          continue;
        }
        error = "text post-effect node references an unknown input: " +
                node.nodeId + " <- " + inputId;
        return false;
      }
      if (waitingForDependency)
        continue;

      // A non-Page RenderGroup owns one target per resolved topology range.
      // Do this before the global DAG path; feeding its source through
      // recordingBounds would merge all letters and is the source of the
      // historical per-letter/line/word regressions.
      const bool independent =
          node.renderGroup &&
          node.renderGroup->spec.mode != text::TextRenderGroupMode::Page;
      if (independent && node.inputIds.empty()) {
        const auto topology =
            BuildIndependentRenderGroupTopology(layout, *node.renderGroup);
        const auto unitSources = BuildIndependentUnitSources(
            renderPlan, *node.renderGroup, layout, components, recordingBounds);
        std::unordered_set<std::string> afterEffectDecorations;
        for (const auto &pass : framePlan.decorationPasses) {
          if (pass.effectScope ==
              text::TextEffectDecorationEffectScope::AfterRenderGroupEffect)
            afterEffectDecorations.insert(pass.passId);
        }
        bool independentNativeCompositeEligible =
            gpuContext && framePlan.postEffectNodes.size() == 1U &&
            std::all_of(components.begin(), components.end(),
                        [&](const auto &component) {
                          return PageRenderGroupIncludesComponent(
                              *node.renderGroup, afterEffectDecorations,
                              component);
                        });
        std::vector<QtTextRenderGroupCompositeDraw>
            independentNativeComposites;
        const QtTextNativePageSource *nativeSource = nullptr;
        for (const auto &component : components) {
          if (!component.nativePageSource ||
              !PageRenderGroupIncludesComponent(*node.renderGroup,
                                                afterEffectDecorations,
                                                component))
            continue;
          nativeSource = component.nativePageSource.get();
          break;
        }
        if (std::getenv("VIDEOCUT_TRACE_TEXT_RENDER_GROUP_TOPOLOGY") !=
            nullptr) {
          std::fprintf(
              stderr,
              "[VIDEOCUT_TEXT_INDEPENDENT_GROUP] node=%s mode=%u units=%zu "
              "ranges=%zu sources=%zu fixed=[%.9g %.9g %.9g %.9g]\n",
              node.nodeId.c_str(), static_cast<unsigned>(
                                        node.renderGroup->spec.mode),
              topology.orderedUnitIndexes.size(), topology.ranges.size(),
              unitSources.size(), node.renderGroup->fixedGeometryBounds.x,
              node.renderGroup->fixedGeometryBounds.y,
              node.renderGroup->fixedGeometryBounds.width,
              node.renderGroup->fixedGeometryBounds.height);
        }
        struct RenderedRange final {
          std::size_t start{0U};
          std::size_t end{0U};
          sk_sp<SkPicture> picture;
        };
        std::vector<RenderedRange> renderedRanges;
        std::vector<bool> covered(topology.orderedUnitIndexes.size(), false);
        const double progress = ResolvePostEffectProgress(
            framePlan, node.nodeId, request.localTimeUs, request.durationUs);
        std::int64_t wholeTimeUs = request.durationUs;
        std::int64_t authoredTimeUs = 0;
        for (const auto &layer : animations.layers) {
          if (layer.layerId != node.renderGroup->layerId)
            continue;
          if (layer.timeDriver.durationUs > 0)
            wholeTimeUs = layer.timeDriver.durationUs;
          break;
        }
        if (node.renderGroup->spec.duration &&
            node.renderGroup->spec.duration->endTimeUs >
                node.renderGroup->spec.duration->startTimeUs) {
          wholeTimeUs = node.renderGroup->spec.duration->endTimeUs -
                        node.renderGroup->spec.duration->startTimeUs;
          authoredTimeUs = node.renderGroup->spec.duration->startTimeUs;
        }
        if (wholeTimeUs <= 0)
          wholeTimeUs = std::max<std::int64_t>(1, request.durationUs);
        authoredTimeUs += static_cast<std::int64_t>(std::llround(
            std::clamp(progress, 0.0, 1.0) *
            static_cast<double>(wholeTimeUs)));
        SkMatrix inversePresentation;
        const bool invertiblePresentation =
            presentationMatrix.invert(&inversePresentation);
        for (std::size_t rangeIndex = 0U;
             rangeIndex < topology.ranges.size(); ++rangeIndex) {
          const auto &range = topology.ranges[rangeIndex];
          if (range.intensity < 0.9999F)
            independentNativeCompositeEligible = false;
          auto rangeSource = BuildIndependentRangeSource(
              unitSources, topology.orderedUnitIndexes, range, recordingBounds);
          if ((!rangeSource.picture && !nativeSource) ||
              rangeSource.fixedRenderGroupBounds.isEmpty() ||
              !invertiblePresentation)
            continue;
          const auto deviceGeometry =
              presentationMatrix.mapRect(rangeSource.fixedRenderGroupBounds);
          if (deviceGeometry.isEmpty() || !deviceGeometry.isFinite())
            continue;
          const text::TextRenderGroupContinuousRect sourceRect{
              static_cast<double>(deviceGeometry.left()),
              static_cast<double>(deviceGeometry.top()),
              static_cast<double>(deviceGeometry.width()),
              static_cast<double>(deviceGeometry.height())};
          const auto resolved = text::ResolveTextRenderGroupFrame(
              range, sourceRect, node.renderGroup->spec.expandRatioX,
              node.renderGroup->spec.expandRatioY);
          if (!resolved)
            continue;
          const auto &expanded = resolved->expandedRect;
          const int targetWidth =
              static_cast<int>(resolved->rasterSize.width);
          const int targetHeight =
              static_cast<int>(resolved->rasterSize.height);
          if (targetWidth <= 0 || targetHeight <= 0)
            continue;

          // Qt allocates the integer RGBA target around the continuous
          // expanded range centre.  The fractional expanded rectangle is
          // used to choose the raster dimensions, but it is not itself the
          // texture's device-space rectangle.  Keeping this allocation
          // rectangle explicit makes the Letter MVP, scoped inputs and the
          // final replay share one coordinate domain.
          const float rangeRasterScaleX =
              static_cast<float>(resolved->rasterScaleX);
          const float rangeRasterScaleY =
              static_cast<float>(resolved->rasterScaleY);
          const float rangeDeviceWidth =
              static_cast<float>(targetWidth) / rangeRasterScaleX;
          const float rangeDeviceHeight =
              static_cast<float>(targetHeight) / rangeRasterScaleY;
          const SkRect rangeDeviceTargetBounds = SkRect::MakeXYWH(
              static_cast<float>(expanded.originX +
                                 expanded.extentWidth * 0.5) -
                  rangeDeviceWidth * 0.5F,
              static_cast<float>(expanded.originY +
                                 expanded.extentHeight * 0.5) -
                  rangeDeviceHeight * 0.5F,
              rangeDeviceWidth, rangeDeviceHeight);

          const auto clock = text::SampleTextRenderGroupClock(
              range, authoredTimeUs, wholeTimeUs);
          if (!clock.valid)
            continue;
          float localProgress = static_cast<float>(progress);
          std::int64_t localEffectTimeUs = node.effectTimeUs;
          if (clock.hasLocalTime) {
            localProgress = static_cast<float>(std::clamp(
                clock.heldEffectTimeUs / static_cast<double>(wholeTimeUs),
                0.0, 1.0));
            localEffectTimeUs = static_cast<std::int64_t>(std::llround(
                std::clamp(clock.heldEffectTimeUs, 0.0,
                           static_cast<double>(std::numeric_limits<
                               std::int64_t>::max()))));
          }
          const SkRect localBounds = SkRect::MakeWH(
              static_cast<float>(targetWidth), static_cast<float>(targetHeight));

          std::unordered_set<std::uint64_t> rangeUnitIds;
          for (std::size_t position =
                   std::min(range.startIndex, topology.orderedUnitIndexes.size());
               position <
               std::min(range.endIndex, topology.orderedUnitIndexes.size());
               ++position) {
            const auto layoutIndex = topology.orderedUnitIndexes[position];
            if (layoutIndex < layout.units.size())
              rangeUnitIds.insert(layout.units[layoutIndex].binding.stableUnitId);
          }

          // Prefer the deferred native packet for SDF text.  It reproduces
          // the old Qt range path's atlas, derivative and target quantization;
          // the presentation-sized component picture is only a fallback for
          // non-native materials (or packets without unit metadata).
          PageRenderGroupDomain rangeDomain;
          rangeDomain.execution = *node.renderGroup;
          rangeDomain.authoredToDevice = presentationMatrix;
          rangeDomain.deviceToAuthored = inversePresentation;
          rangeDomain.deviceTargetBounds = rangeDeviceTargetBounds;
          rangeDomain.localBounds = localBounds;
          // Qt normalizes the deferred Letter MVP by the quantized render
          // target dimensions.  The continuous expanded rectangle still
          // supplies the target origin, but its fractional extent must not be
          // folded back into the MVP through quantScale.  Fresh Metal vertex
          // binding captures lock this distinction: a 427.22666-wide range
          // allocated as 428 pixels uses 2/428, not 2/427.22666.  rasterScale
          // is only non-unit when the target is capped by the global maximum.
          rangeDomain.rasterScaleX = rangeRasterScaleX;
          rangeDomain.rasterScaleY = rangeRasterScaleY;
          rangeDomain.targetWidth = targetWidth;
          rangeDomain.targetHeight = targetHeight;
          std::string nativeRangeError;
          auto nativeLocalPicture =
              nativeSource
                  ? RenderQtTextNativePageSource(
                        *nativeSource, rangeDomain, &rangeUnitIds,
                        gpuContext, nativeRangeError)
                  : sk_sp<SkPicture>{};
          SkPictureRecorder sourceRecorder;
          auto *sourceCanvas = sourceRecorder.beginRecording(localBounds);
          if (nativeLocalPicture)
            sourceCanvas->drawPicture(nativeLocalPicture);
          if (rangeSource.picture) {
            // Non-native scoped components share the same target transform.
            sourceCanvas->scale(rangeRasterScaleX, rangeRasterScaleY);
            sourceCanvas->translate(-rangeDeviceTargetBounds.left(),
                                    -rangeDeviceTargetBounds.top());
            sourceCanvas->concat(presentationMatrix);
            sourceCanvas->drawPicture(rangeSource.picture);
          }
          auto sourcePicture = sourceRecorder.finishRecordingAsPicture();
          if (!nativeLocalPicture && !rangeSource.picture)
            sourcePicture.reset();
          if (!sourcePicture)
            continue;

          QtTextPostEffectStateContext stateContext;
          stateContext.renderGraphInstanceId = graphInstanceId;
          stateContext.lifecycleEpoch = lifecycleEpoch;
          stateContext.effectNodeInstanceId =
              node.effectNodeInstanceId != 0U
                  ? node.effectNodeInstanceId
                  : StableNodeIdentity(node.nodeId);
          stateContext.effectTimeSeconds =
              static_cast<double>(localEffectTimeUs) / 1'000'000.0;
          stateContext.effectRuntimeClockRevision =
              static_cast<std::uint64_t>(
                  std::max<std::int64_t>(0, localEffectTimeUs));
          stateContext.presentationWidth = static_cast<int>(request.outputWidth);
          stateContext.presentationHeight =
              static_cast<int>(request.outputHeight);
          stateContext.renderGroupInstanceId =
              ((static_cast<std::uint64_t>(rangeIndex) + 1U) << 32U) |
              (static_cast<std::uint64_t>(topology.ranges.size()) + 1U);
          stateContext.renderGroupExpandRatioX =
              node.renderGroup->spec.expandRatioX;
          stateContext.renderGroupExpandRatioY =
              node.renderGroup->spec.expandRatioY;
          if (!BindPostEffectRuntimeClock(request, node, runtimeClockStore,
                                          stateContext, error)) {
            return false;
          }
          auto executed = ExecuteQtTextProPostEffectPipeline(
              sourcePicture, node, localProgress, localBounds, gpuContext,
              &stateContext, executionEvidence != nullptr);
          RecordPostEffectExecution(executionEvidence, node.nodeId,
                                    stateContext, executed);
          if (!executed) {
            error = executed.error.empty()
                        ? "native post-effect execution failed closed"
                        : executed.error;
            return false;
          }
          sk_sp<SkPicture> effected = std::move(executed.picture);

          // Preserve the RGBA8 handoff and linear texture replay used by the
          // native Qt RenderGroup path.  This is also the quantization point
          // that keeps small per-letter blur kernels from being resampled at
          // presentation resolution.
          sk_sp<SkPicture> texturePicture = effected;
          if (gpuContext) {
            const auto targetInfo = SkImageInfo::Make(
                targetWidth, targetHeight, kRGBA_8888_SkColorType,
                kPremul_SkAlphaType, nullptr);
            std::string targetError;
            auto targetSurface = gpuContext->MakeSurface(targetInfo, targetError);
            if (targetSurface) {
              targetSurface->getCanvas()->clear(SK_ColorTRANSPARENT);
              targetSurface->getCanvas()->drawPicture(effected);
              if (auto image = targetSurface->makeImageSnapshot()) {
                if (independentNativeCompositeEligible) {
                  if (auto compositeRequest =
                          BuildQtTextRenderGroupCompositeRequest(
                          rangeDomain, static_cast<int>(request.outputWidth),
                          static_cast<int>(request.outputHeight), referenceWidth,
                          referenceHeight)) {
                    independentNativeComposites.push_back(
                        {image, std::move(*compositeRequest)});
                  } else {
                    independentNativeCompositeEligible = false;
                  }
                }
                SkPictureRecorder textureRecorder;
                auto *textureCanvas =
                    textureRecorder.beginRecording(localBounds);
                textureCanvas->drawImageRect(
                    image, localBounds,
                    SkSamplingOptions(SkFilterMode::kLinear), nullptr);
                if (auto recorded = textureRecorder.finishRecordingAsPicture())
                  texturePicture = std::move(recorded);
              }
            } else {
              independentNativeCompositeEligible = false;
            }
          }
          const SkRect authoredTargetBounds = MapRect(
              inversePresentation, rangeDeviceTargetBounds);
          SkPictureRecorder wrapperRecorder;
          auto *wrapper = wrapperRecorder.beginRecording(authoredTargetBounds);
          wrapper->concat(inversePresentation);
          wrapper->translate(rangeDeviceTargetBounds.left(),
                            rangeDeviceTargetBounds.top());
          wrapper->scale(1.0F / rangeRasterScaleX,
                        1.0F / rangeRasterScaleY);
          wrapper->drawPicture(texturePicture);
          auto wrapped = wrapperRecorder.finishRecordingAsPicture();
          if (!wrapped)
            continue;
          wrapped = MixRenderGroupIntensity(rangeSource.picture, wrapped,
                                             recordingBounds, range.intensity);
          if (!wrapped)
            continue;
          for (std::size_t position =
                   std::min(range.startIndex, covered.size());
               position < std::min(range.endIndex, covered.size()); ++position)
            covered[position] = true;
          renderedRanges.push_back(
              {range.startIndex, range.endIndex, std::move(wrapped)});
        }

        if (independentNativeCompositeEligible &&
            !independentNativeComposites.empty() &&
            independentNativeComposites.size() == renderedRanges.size() &&
            std::all_of(covered.begin(), covered.end(),
                        [](const bool value) { return value; })) {
          nativeComposites = std::move(independentNativeComposites);
        }

        SkPictureRecorder compositor;
        auto *compositeCanvas = compositor.beginRecording(recordingBounds);
        if (!compositeCanvas) {
          error = "text independent RenderGroup compositor could not be recorded";
          return false;
        }
        {
          std::size_t nextRange = 0U;
          for (std::size_t position = 0U;
               position < topology.orderedUnitIndexes.size(); ++position) {
            while (nextRange < renderedRanges.size() &&
                   renderedRanges[nextRange].start == position) {
              if (renderedRanges[nextRange].picture)
                compositeCanvas->drawPicture(
                    renderedRanges[nextRange].picture);
              ++nextRange;
            }
            if (!covered[position]) {
              const auto layoutIndex = topology.orderedUnitIndexes[position];
              if (layoutIndex < unitSources.size() &&
                  unitSources[layoutIndex].picture)
                compositeCanvas->drawPicture(unitSources[layoutIndex].picture);
            }
          }
          while (nextRange < renderedRanges.size()) {
            if (renderedRanges[nextRange].picture)
              compositeCanvas->drawPicture(renderedRanges[nextRange].picture);
            ++nextRange;
          }
        }
        sk_sp<SkPicture> output = compositor.finishRecordingAsPicture();
        if (!output) {
          error = "text independent RenderGroup produced no source";
          return false;
        }
        outputs[node.nodeId] = output;
        current = output;
        lastExecutedNodeId = node.nodeId;
        executedNodeIds.insert(node.nodeId);
        madeProgress = true;
        cacheIdentity.AddString(node.nodeId);
        cacheIdentity.Add(static_cast<unsigned>(node.kind));
        cacheIdentity.Add(node.amount);
        cacheIdentity.Add(node.paddingPx);
        cacheIdentity.Add(progress);
        cacheIdentity.Add(node.effectTimeUs);
        cacheIdentity.Add(topology.orderedUnitIndexes.size());
        for (const auto &unitIndex : topology.orderedUnitIndexes)
          cacheIdentity.Add(layout.units[unitIndex].binding.stableUnitId);
        for (const auto &inputId : node.inputIds)
          cacheIdentity.AddString(inputId);
        for (const auto &parameter : node.parameters) {
          cacheIdentity.AddString(parameter.name);
          for (const auto value : parameter.values)
            cacheIdentity.Add(value);
        }
        continue;
      }

      const bool inPage = nodeRunsInPage(node);
      const SkRect executionBounds =
          inPage ? pageDomain->localBounds : recordingBounds;
      std::vector<RenderComponent> convertedPageComponents;
      const std::vector<RenderComponent> *executionComponents = &components;
      if (inPage) {
        convertedPageComponents = pageComponents;
        for (const auto &inputId : node.inputIds) {
          if (outputs.contains(inputId) ||
              std::any_of(convertedPageComponents.begin(),
                          convertedPageComponents.end(),
                          [&](const auto &component) {
                            return component.id == inputId;
                          })) {
            continue;
          }
          const auto found = std::find_if(
              components.begin(), components.end(),
              [&](const auto &component) { return component.id == inputId; });
          if (found == components.end())
            continue;
          auto converted = MakePageLocalComponents({*found}, *pageDomain,
                                                   gpuContext, error);
          if (converted.empty())
            return false;
          convertedPageComponents.push_back(std::move(converted.front()));
        }
        executionComponents = &convertedPageComponents;
      }

      auto resolvedOutputs = outputs;
      for (const auto &inputId : node.inputIds) {
        const auto found = outputs.find(inputId);
        if (found == outputs.end())
          continue;
        const bool inputInPage = pageOutputIds.contains(inputId);
        if (inPage == inputInPage)
          continue;
        if (inPage) {
          resolvedOutputs[inputId] =
              MakePageLocalPicture(found->second, *pageDomain);
        } else {
          const auto *materialized = materializePage(found->second);
          if (!materialized)
            return false;
          outputs[inputId] = materialized->authoredPicture;
          resolvedOutputs[inputId] = materialized->authoredPicture;
          pageOutputIds.erase(inputId);
        }
      }

      const bool hasPostEffectInput = std::any_of(
          node.inputIds.begin(), node.inputIds.end(),
          [&](const auto &inputId) {
            return declaredNodeIds.contains(inputId);
          });
      if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
        std::fprintf(stderr,
                     "[VIDEOCUT_TEXT_POST_NODE_INPUT] node=%s root=%d "
                     "inputs=%zu page=%d\n",
                     node.nodeId.c_str(), hasPostEffectInput ? 0 : 1,
                     node.inputIds.size(), inPage ? 1 : 0);
      }
      // A Page node's first source is the descriptor-owned composition, even
      // when an older producer redundantly serializes component IDs in
      // inputIds.  Those IDs describe the Page source topology, not a second
      // crop/composite operation.  Once a node explicitly depends on another
      // post node, resolve that DAG edge normally; the post output then
      // becomes the source for the next node.
      const bool pageRoot = inPage && !hasPostEffectInput;
      const auto source =
          pageRoot
              ? pageSource
              : (node.inputIds.empty()
                     ? (inPage ? pageSource : globalSource)
                     : ResolveNodeInputPicture(
                           node, *executionComponents, resolvedOutputs,
                           inPage ? pageSource : globalSource, executionBounds));
      if (!source) {
        error = "text post-effect node has no resolved input picture: " +
                node.nodeId;
        return false;
      }
      const double progress = ResolvePostEffectProgress(
          framePlan, node.nodeId, request.localTimeUs, request.durationUs);
      QtTextPostEffectStateContext stateContext;
      stateContext.renderGraphInstanceId = graphInstanceId;
      stateContext.lifecycleEpoch = lifecycleEpoch;
      stateContext.effectNodeInstanceId =
          node.effectNodeInstanceId != 0U
              ? node.effectNodeInstanceId
              : StableNodeIdentity(node.nodeId);
      stateContext.effectTimeSeconds =
          static_cast<double>(node.effectTimeUs) / 1'000'000.0;
      stateContext.effectRuntimeClockRevision =
          static_cast<std::uint64_t>(
              std::max<std::int64_t>(0, node.effectTimeUs));
      stateContext.presentationWidth = static_cast<int>(request.outputWidth);
      stateContext.presentationHeight = static_cast<int>(request.outputHeight);
      stateContext.renderGroupExpandRatioX = 1.0F;
      stateContext.renderGroupExpandRatioY = 1.0F;
      if (node.renderGroup) {
        stateContext.renderGroupInstanceId =
            node.renderGroup->renderGroupInstanceId;
        stateContext.renderGroupExpandRatioX =
            node.renderGroup->spec.expandRatioX;
        stateContext.renderGroupExpandRatioY =
            node.renderGroup->spec.expandRatioY;
      } else {
        stateContext.renderGroupInstanceId =
            node.layerId.empty() ? StableNodeIdentity(node.nodeId)
                                 : StableNodeIdentity(node.layerId);
      }
      sk_sp<SkPicture> output;
      if (!BindPostEffectRuntimeClock(request, node, runtimeClockStore,
                                      stateContext, error)) {
        return false;
      }
      auto executed = ExecuteQtTextProPostEffectPipeline(
          source, node, progress, executionBounds, gpuContext,
          &stateContext, executionEvidence != nullptr);
      RecordPostEffectExecution(executionEvidence, node.nodeId,
                                stateContext, executed);
      if (!executed) {
        diagnostics.push_back(MakeDiagnostic(
            "text.post_effect.unavailable", DiagnosticSeverity::Error,
            "post_effect", node.nodeId,
            executed.error.empty()
                ? "native post-effect execution failed closed"
                : executed.error));
        error = executed.error.empty()
                    ? "native post-effect execution failed closed"
                    : executed.error;
        return false;
      } else {
        output = std::move(executed.picture);
      }
      outputs[node.nodeId] = output;
      current = std::move(output);
      if (inPage)
        pageOutputIds.insert(node.nodeId);
      else
        pageOutputIds.erase(node.nodeId);
      lastExecutedNodeId = node.nodeId;
      executedNodeIds.insert(node.nodeId);
      madeProgress = true;
      cacheIdentity.AddString(node.nodeId);
      cacheIdentity.Add(static_cast<unsigned>(node.kind));
      cacheIdentity.Add(node.amount);
      cacheIdentity.Add(node.paddingPx);
      cacheIdentity.Add(progress);
      cacheIdentity.Add(node.effectTimeUs);
      cacheIdentity.Add(stateContext.effectRuntimeClockRevision);
      for (const auto &inputId : node.inputIds)
        cacheIdentity.AddString(inputId);
      for (const auto &parameter : node.parameters) {
        cacheIdentity.AddString(parameter.name);
        for (const auto value : parameter.values)
          cacheIdentity.Add(value);
      }
    }
    if (!madeProgress) {
      error = "text post-effect graph contains a dependency cycle";
      return false;
    }
  }

  std::unordered_set<std::string> referencedNodeIds;
  for (const auto &node : framePlan.postEffectNodes) {
    for (const auto &inputId : node.inputIds) {
      if (declaredNodeIds.contains(inputId))
        referencedNodeIds.insert(inputId);
    }
  }
  std::vector<std::string> terminalNodeIds;
  for (const auto &node : framePlan.postEffectNodes) {
    if (!referencedNodeIds.contains(node.nodeId))
      terminalNodeIds.push_back(node.nodeId);
  }
  sk_sp<SkImage> nativeTerminalPageImage;
  if (pageDomain && terminalNodeIds.size() == 1U &&
      pageOutputIds.contains(terminalNodeIds.front())) {
    const auto found = outputs.find(terminalNodeIds.front());
    const auto *materialized =
        found == outputs.end() ? nullptr : materializePage(found->second);
    if (!materialized)
      return false;
    nativeTerminalPageImage = materialized->pageImage;
  }
  const auto remainingPageOutputIds = pageOutputIds;
  for (const auto &nodeId : remainingPageOutputIds) {
    const auto found = outputs.find(nodeId);
    if (found == outputs.end())
      continue;
    const auto *materialized = materializePage(found->second);
    if (!materialized)
      return false;
    found->second = materialized->authoredPicture;
    pageOutputIds.erase(nodeId);
  }
  if (!lastExecutedNodeId.empty()) {
    const auto found = outputs.find(lastExecutedNodeId);
    if (found != outputs.end())
      current = found->second;
  }
  if (pageDomain && nativeTerminalPageImage &&
      std::fabs(pageDomain->rasterScaleX - 1.0F) < 0.000001F &&
      std::fabs(pageDomain->rasterScaleY - 1.0F) < 0.000001F) {
    if (auto compositeRequest = BuildQtTextRenderGroupCompositeRequest(
            *pageDomain, static_cast<int>(request.outputWidth),
            static_cast<int>(request.outputHeight), referenceWidth,
            referenceHeight)) {
      nativeComposites.push_back(QtTextRenderGroupCompositeDraw{
          std::move(nativeTerminalPageImage), std::move(*compositeRequest)});
    }
  }
  identity = cacheIdentity.Finish();
  return true;
}


sk_sp<SkPicture> EmptyExecutionGraphPicture(const SkRect &bounds) {
  SkPictureRecorder recorder;
  recorder.beginRecording(bounds);
  return recorder.finishRecordingAsPicture();
}


sk_sp<SkPicture> MaterializeExecutionGraphPicture(
    const sk_sp<SkPicture> &source, const SkRect &bounds,
    SkiaGpuContext *gpuContext, std::string &error) {
  if (!source || bounds.isEmpty() || !bounds.isFinite()) {
    error = "text execution graph render target has invalid source bounds";
    return {};
  }
  SkIRect pixels;
  bounds.roundOut(&pixels);
  const std::uint64_t pixelCount =
      static_cast<std::uint64_t>(std::max(0, pixels.width())) *
      static_cast<std::uint64_t>(std::max(0, pixels.height()));
  if (pixels.isEmpty() || pixelCount > kMaximumDecodedAssetPixels) {
    error = "text execution graph render target exceeds the surface budget";
    return {};
  }
  const auto info = SkImageInfo::Make(
      pixels.width(), pixels.height(), kRGBA_8888_SkColorType,
      kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
  auto surface = gpuContext ? gpuContext->MakeSurface(info, error)
                            : SkSurfaces::Raster(info);
  if (!surface) {
    if (error.empty())
      error = "text execution graph render target allocation failed";
    return {};
  }
  auto *canvas = surface->getCanvas();
  canvas->clear(SK_ColorTRANSPARENT);
  canvas->translate(-static_cast<float>(pixels.left()),
                    -static_cast<float>(pixels.top()));
  canvas->drawPicture(source);
  auto image = surface->makeImageSnapshot();
  if (!image) {
    error = "text execution graph render target snapshot failed";
    return {};
  }
  SkPictureRecorder recorder;
  auto *output = recorder.beginRecording(bounds);
  output->drawImage(image, static_cast<float>(pixels.left()),
                    static_cast<float>(pixels.top()),
                    // This picture is the storage view of the same-sized RT,
                    // not a presentation rescale. Filtering it here would
                    // sample the Letter coverage once before the authored
                    // material/composite pass samples the RT, widening SDF
                    // edges by a device row. Preserve texels at this identity
                    // boundary; real material passes choose their own filter.
                    SkSamplingOptions(SkFilterMode::kNearest,
                                      SkMipmapMode::kNone));
  auto picture = recorder.finishRecordingAsPicture();
  if (!picture)
    error = "text execution graph render target recording failed";
  return picture;
}


bool MaterializeExecutionGraphRaster(
    const sk_sp<SkPicture> &source, const SkRect &bounds,
    SkiaGpuContext *gpuContext, ExecutionGraphRaster &raster,
    std::string &error) {
  raster = {};
  if (!source || bounds.isEmpty() || !bounds.isFinite()) {
    error = "text execution material has invalid source bounds";
    return false;
  }
  bounds.roundOut(&raster.bounds);
  const std::uint64_t pixelCount =
      static_cast<std::uint64_t>(std::max(0, raster.bounds.width())) *
      static_cast<std::uint64_t>(std::max(0, raster.bounds.height()));
  if (raster.bounds.isEmpty() || pixelCount > kMaximumDecodedAssetPixels) {
    error = "text execution material exceeds the surface budget";
    return false;
  }
  const auto info = SkImageInfo::Make(
      raster.bounds.width(), raster.bounds.height(), kRGBA_8888_SkColorType,
      kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
  auto surface = gpuContext ? gpuContext->MakeSurface(info, error)
                            : SkSurfaces::Raster(info);
  if (!surface) {
    if (error.empty())
      error = "text execution material source allocation failed";
    return false;
  }
  auto *canvas = surface->getCanvas();
  canvas->clear(SK_ColorTRANSPARENT);
  canvas->translate(-static_cast<float>(raster.bounds.left()),
                    -static_cast<float>(raster.bounds.top()));
  canvas->drawPicture(source);
  raster.image = surface->makeImageSnapshot();
  if (!raster.image) {
    error = "text execution material source snapshot failed";
    return false;
  }
  return true;
}


sk_sp<SkPicture> ExecutionGraphPictureFromRaster(
    const sk_sp<SkImage> &image, const SkIRect &rasterBounds,
    const SkRect &recordingBounds, std::string &error) {
  if (!image || rasterBounds.isEmpty()) {
    error = "text execution material produced no raster";
    return {};
  }
  SkPictureRecorder recorder;
  auto *canvas = recorder.beginRecording(recordingBounds);
  // A Page picture can later acquire a fractional presentation translation.
  // Preserve the source render-target sampler there; nearest sampling snaps
  // the already rasterized Letter edges to a different pixel grid.
  canvas->drawImage(
      image, static_cast<float>(rasterBounds.left()),
      static_cast<float>(rasterBounds.top()),
      SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone), nullptr);
  auto picture = recorder.finishRecordingAsPicture();
  if (!picture)
    error = "text execution material result recording failed";
  return picture;
}

} // namespace videocut::skia_runtime::internal::text_lane
