#include "text/TextRenderPipeline.h"

namespace videocut::skia_runtime::internal::text_lane {


bool ExecuteTextEffectExecutionGraph(
    const TextRenderFramePlan &renderPlan,
    const text::TextRenderRequest &request,
    const std::vector<RenderComponent> &components,
    const std::vector<RenderComponent> &materialComponents,
    const ResolvedLayout &layout, const PageRenderGroupDomain *pageDomain,
    const SkRect &recordingBounds, const SkPoint pageScenePivot,
    const float referenceToExecutionScaleX,
    const float referenceToExecutionScaleY, const float referenceWidth,
    const float referenceHeight, const float sourceToReferenceScale,
    TextVisualAssetStore &assets,
    const std::uint64_t graphInstanceId, SkiaGpuContext *gpuContext,
    std::unordered_map<std::string, TextExecutionGraphHistoryState> &histories,
    std::unordered_map<std::string, sk_sp<SkPicture>> &outputs,
    sk_sp<SkPicture> &terminal, bool &terminalUsesPresentation,
    text::TextExecutionEvidence *executionEvidence,
    const text::TextExecutionNodeEvidence *sourceAttachmentEvidence,
    std::string &identity, std::string &error) {
  const auto &framePlan = renderPlan.value();
  outputs.clear();
  terminal.reset();
  terminalUsesPresentation = false;
  error.clear();
  const bool hasPresentationParticleDomain =
      pageDomain && ExecutionGraphContainsMediaParticle(
                        framePlan.executionGraph.nodes);
  const auto presentationBounds = SkRect::MakeWH(
      static_cast<float>(request.outputWidth),
      static_cast<float>(request.outputHeight));
  // Once an input is promoted, it lives in output pixels. Preserve the
  // authored parent transform, but remove the offscreen target's origin and
  // raster downsampling from subsequent material and scene operations.
  std::optional<PageRenderGroupDomain> presentationDomain;
  if (pageDomain) {
    presentationDomain = *pageDomain;
    presentationDomain->deviceTargetBounds = presentationBounds;
    presentationDomain->localBounds = presentationBounds;
    presentationDomain->rasterScaleX = 1.0F;
    presentationDomain->rasterScaleY = 1.0F;
    presentationDomain->targetWidth = static_cast<int>(request.outputWidth);
    presentationDomain->targetHeight = static_cast<int>(request.outputHeight);
  }
  const auto promotePagePicture = [&](const sk_sp<SkPicture> &picture) {
    return pageDomain
               ? PromotePageLocalPictureToPresentation(
                     picture, *pageDomain, presentationBounds)
               : picture;
  };
  SkPoint presentationScenePivot = pageScenePivot;
  if (pageDomain) {
    presentationScenePivot = SkPoint::Make(
        pageDomain->deviceTargetBounds.left() +
            pageScenePivot.x() / pageDomain->rasterScaleX,
        pageDomain->deviceTargetBounds.top() +
            pageScenePivot.y() / pageDomain->rasterScaleY);
  }
  std::unordered_set<std::string> presentationOutputs;
  std::unordered_map<std::string, sk_sp<SkImage>> sampledMediaInputs;
  const auto globalSource = ComposePictures(components, recordingBounds);
  const auto glyphSource =
      ComposePictures(materialComponents, recordingBounds);
  std::vector<RenderComponent> captionComponents;
  captionComponents.reserve(components.size());
  for (const auto &component : components) {
    if (component.kind ==
            text::TextEffectCompositeItemKind::GlyphMaterial ||
        component.kind == text::TextEffectCompositeItemKind::Decoration) {
      captionComponents.push_back(component);
    }
  }
  const auto captionSource =
      ComposePictures(captionComponents, recordingBounds);
  const auto recordPass = [&](const sk_sp<SkPicture> &source,
                              const SkRect &bounds,
                              const SkRect *clip = nullptr,
                              const float opacity = 1.0F) {
    if (!source)
      return sk_sp<SkPicture>{};
    SkPictureRecorder recorder;
    auto *canvas = recorder.beginRecording(bounds);
    if (clip)
      canvas->clipRect(*clip, SkClipOp::kIntersect, false);
    SkPaint paint;
    paint.setAlphaf(std::clamp(opacity, 0.0F, 1.0F));
    canvas->drawPicture(source, nullptr, &paint);
    return recorder.finishRecordingAsPicture();
  };
  IdentityBuilder graphIdentity;
  std::vector<std::string> authoredNodeIds;
  std::unordered_set<std::string> referencedNodeIds;
  const auto historySnapshot = histories;
  std::unordered_map<std::string, TextExecutionGraphHistoryState>
      pendingHistories;
  std::size_t runtimeNodeCount = 0U;
  std::size_t nextMaterialComponent = 0U;
  sk_sp<SkPicture> orderedMaterialColorOutput;
  bool hasDrawableNode = false;

  std::unordered_map<std::string,
                     const text::TextEffectExecutionNodeFramePlan *>
      authoredNodes;
  const auto indexAuthoredNodes =
      [&](const auto &self,
          const std::vector<text::TextEffectExecutionNodeFramePlan> &nodes)
      -> bool {
    for (const auto &node : nodes) {
      if (node.nodeId.empty() ||
          !authoredNodes.emplace(node.nodeId, &node).second) {
        error = "text execution graph contains a duplicate node identity";
        return false;
      }
      if (!self(self, node.children))
        return false;
    }
    return true;
  };
  if (!indexAuthoredNodes(indexAuthoredNodes,
                          framePlan.executionGraph.nodes)) {
    return false;
  }

  // Replay the existing Letter packets by identity for an unchanged glyph
  // attachment. Cropping a composed Page by overlapping glyph quads copies
  // neighbour strokes into independently transformed clones.
  std::unordered_map<std::uint64_t, sk_sp<SkPicture>> nativeSceneUnits;
  const bool hasNativeSceneUnits = pageDomain && !materialComponents.empty() &&
      std::all_of(materialComponents.begin(), materialComponents.end(),
                  [](const auto &component) {
                    return component.nativePageSource != nullptr;
                  });
  const auto isUnmodifiedGlyphInput = [&](std::string inputId) {
    using Capability = text::TextEffectExecutionCapability;
    for (std::size_t depth = 0U; depth < 32U; ++depth) {
      const auto found = authoredNodes.find(inputId);
      if (found == authoredNodes.end() || !outputs.contains(inputId))
        return false;
      const auto &ancestor = *found->second;
      if (!ancestor.active || ancestor.staticAffine || ancestor.camera ||
          !ancestor.parameters.empty() || !ancestor.children.empty())
        return false;
      if (ancestor.capability == Capability::LayoutGlyphRun)
        return ancestor.inputIds.empty();
      switch (ancestor.capability) {
      case Capability::SelectorBase:
      case Capability::OperatorProgram:
      case Capability::OperatorMaterial:
      case Capability::OperatorTransform:
      case Capability::SceneEntity:
        break;
      default:
        return false;
      }
      if (ancestor.inputIds.size() != 1U)
        return false;
      inputId = ancestor.inputIds.front();
    }
    return false;
  };
  const auto prepareNativeSceneUnit = [&](const std::uint64_t unitId) {
    if (nativeSceneUnits.contains(unitId))
      return true;
    const std::unordered_set<std::uint64_t> filter{unitId};
    std::vector<RenderComponent> filtered;
    for (const auto &component : materialComponents) {
      auto copy = component;
      copy.picture = RenderQtTextNativePageSource(
          *component.nativePageSource, *pageDomain, &filter, gpuContext, error);
      if (!error.empty())
        return false;
      if (copy.picture)
        filtered.push_back(std::move(copy));
    }
    auto picture = ComposePictures(filtered, recordingBounds);
    if (!picture) {
      SkPictureRecorder empty;
      empty.beginRecording(recordingBounds);
      picture = empty.finishRecordingAsPicture();
    }
    nativeSceneUnits.emplace(unitId, std::move(picture));
    return true;
  };

  struct ParticleExecutionState final {
    std::uint64_t identitySeed{
        StableNodeIdentity("text-execution-particle-seed")};
    float randomSeed{0.0F};
    std::int64_t elapsedUs{0};
  };
  const auto resolveParticleExecutionState =
      [&](const text::TextEffectExecutionNodeFramePlan &particleNode,
          ParticleExecutionState &state) -> bool {
    state.elapsedUs = request.localTimeUs;
    std::unordered_set<std::string> visited;
    std::unordered_set<std::string> visiting;
    const auto visitInput = [&](const auto &visitSelf,
                                const std::string &inputId) -> bool {
      if (visited.contains(inputId))
        return true;
      if (!visiting.insert(inputId).second) {
        error = "text execution particle state dependency contains a cycle: " +
                particleNode.nodeId;
        return false;
      }
      const auto found = authoredNodes.find(inputId);
      if (found == authoredNodes.end()) {
        error = "text execution particle state dependency is unknown: " +
                particleNode.nodeId + " <- " + inputId;
        return false;
      }
      // A state edge is an authored-order control dependency. Its producer may
      // intentionally have no picture, so execution is established by the
      // output entry rather than by a non-null raster attachment.
      if (!outputs.contains(inputId)) {
        error = "text execution particle state dependency was not executed in "
                "authored order: " +
                particleNode.nodeId + " <- " + inputId;
        return false;
      }
      for (const auto &ancestorId : found->second->inputIds) {
        if (!visitSelf(visitSelf, ancestorId))
          return false;
      }
      if (found->second->active) {
        if (found->second->capability ==
            text::TextEffectExecutionCapability::StateDeterministicRandom) {
          state.randomSeed =
              static_cast<float>(found->second->randomSeed);
          state.identitySeed ^= found->second->randomSeed;
          state.identitySeed ^= StableNodeIdentity(found->second->nodeId);
        } else if (found->second->capability ==
                   text::TextEffectExecutionCapability::StateParticle) {
          state.elapsedUs = found->second->stateElapsedUs;
          state.identitySeed ^= StableNodeIdentity(found->second->stateId);
        }
      }
      visiting.erase(inputId);
      visited.insert(inputId);
      return true;
    };
    for (const auto &inputId : particleNode.inputIds) {
      if (!visitInput(visitInput, inputId))
        return false;
    }
    return true;
  };

  const auto processNodes =
      [&](const auto &self,
          const std::vector<text::TextEffectExecutionNodeFramePlan> &nodes,
          const sk_sp<SkPicture> &inherited,
          const bool inheritedUsesPresentation,
          const std::string &parentId, const std::size_t depth) -> bool {
    if (depth > 32U) {
      error = "text execution graph nesting exceeds the runtime budget";
      return false;
    }
    for (const auto &node : nodes) {
      const bool postNode =
          node.kind == text::TextEffectExecutionNodeKind::PostEffectPass;
      const bool invalidPostKind =
          node.postEffectKind &&
          (*node.postEffectKind < text::TextPostEffectKind::TurbulenceDisplacement ||
           *node.postEffectKind >
               text::TextPostEffectKind::PulseEnvelope);
      if (++runtimeNodeCount > 4'096U || node.nodeId.empty() ||
          outputs.contains(node.nodeId) ||
          !ExecutionGraphCapabilityMatchesKind(node.kind, node.capability) ||
          postNode != node.postEffectKind.has_value() || invalidPostKind ||
          !std::isfinite(node.progress) || node.stateElapsedUs < 0 ||
          std::any_of(node.inputIds.begin(), node.inputIds.end(),
                      [](const auto &value) { return value.empty(); }) ||
          std::any_of(node.resourceIds.begin(), node.resourceIds.end(),
                      [](const auto &value) { return value.empty(); }) ||
          ((postNode ||
            node.kind == text::TextEffectExecutionNodeKind::Composite) &&
           node.inputIds.empty()) ||
          (node.kind == text::TextEffectExecutionNodeKind::MediaInput &&
           node.resourceIds.empty()) ||
          (node.kind == text::TextEffectExecutionNodeKind::State &&
           node.stateId.empty()) ||
          (!node.stateId.empty() && node.stateRevision == 0U) ||
          (node.kind == text::TextEffectExecutionNodeKind::History &&
           node.historyId.empty())) {
        error = "text execution graph contains an invalid closed runtime "
                "node: " +
                node.nodeId;
        return false;
      }
      if (!ValidateExecutionGraphParameters(node, error))
        return false;
      TextExecutionParameterConsumptionScope parameterConsumption(
          node, executionEvidence != nullptr);
      authoredNodeIds.push_back(node.nodeId);
      graphIdentity.AddString(parentId);
      graphIdentity.AddString(node.nodeId);
      graphIdentity.Add(node.kind);
      graphIdentity.Add(node.capability);
      graphIdentity.Add(node.effectTimeUs);
      graphIdentity.Add(node.progress);
      graphIdentity.Add(node.active);
      graphIdentity.Add(node.hasAuthoredTimeDriver);
      graphIdentity.Add(node.stateRevision);
      graphIdentity.Add(node.stateElapsedUs);
      graphIdentity.Add(node.randomSeed);
      graphIdentity.Add(node.historyRevision);
      graphIdentity.Add(node.staticAffine.has_value());
      if (node.staticAffine) {
        graphIdentity.Add(node.staticAffine->translationX);
        graphIdentity.Add(node.staticAffine->translationY);
        graphIdentity.Add(node.staticAffine->scaleX);
        graphIdentity.Add(node.staticAffine->scaleY);
        graphIdentity.Add(node.staticAffine->rotationDegrees);
        graphIdentity.Add(node.staticAffine->pivotX);
        graphIdentity.Add(node.staticAffine->pivotY);
      }
      AddExecutionCameraIdentity(graphIdentity, node.camera);
      graphIdentity.Add(node.parameters.size());
      for (const auto &parameter : node.parameters)
        AddExecutionGraphParameterIdentity(graphIdentity, parameter);
      graphIdentity.AddString(node.stateId);
      graphIdentity.AddString(node.historyId);
      graphIdentity.Add(node.resourceIds.size());
      for (const auto &resourceId : node.resourceIds)
        graphIdentity.AddString(resourceId);
      for (const auto &inputId : node.inputIds) {
        referencedNodeIds.insert(inputId);
        graphIdentity.AddString(inputId);
      }
      graphIdentity.Add(node.children.size());
      if (node.kind != text::TextEffectExecutionNodeKind::MediaInput) {
        for (const auto &resourceId : node.resourceIds) {
          const auto sample = std::find_if(
              framePlan.resources.begin(), framePlan.resources.end(),
              [&](const auto &candidate) {
                return candidate.resourceId == resourceId;
              });
          if (sample == framePlan.resources.end() ||
              sample->assetId.empty() || sample->digest.empty()) {
            error = "text execution node resource is not closed: " +
                    resourceId;
            return false;
          }
          std::string resourceIdentity;
          const bool typed = sample->kind !=
                             text::TextEffectResourceKind::Unspecified;
          if (!assets.AdmitOpaque(
                  sample->assetId, sample->digest,
                  typed ? RuntimeKindForFrameResource(sample->kind)
                        : RuntimeAssetKind::Vector,
                  typed ? ProjectKindForFrameResource(sample->kind)
                        : text::TextAssetKind::Image,
                  resourceIdentity, error, nullptr, nullptr, !typed)) {
            return false;
          }
          graphIdentity.AddString(sample->assetId);
          graphIdentity.AddString(sample->digest);
          graphIdentity.Add(sample->kind);
          graphIdentity.AddString(sample->mediaType);
          graphIdentity.AddString(resourceIdentity);
        }
      }

      std::vector<std::string> colorInputIds;
      const std::vector<std::string> *resolvedInputIds = &node.inputIds;
      const bool particleMediaNode =
          node.kind == text::TextEffectExecutionNodeKind::MediaInput &&
          node.capability ==
              text::TextEffectExecutionCapability::MediaParticle;
      if ((node.kind == text::TextEffectExecutionNodeKind::PostEffectPass &&
           node.inputIds.size() > 1U) ||
          particleMediaNode) {
        colorInputIds.reserve(node.inputIds.size());
        for (std::size_t inputIndex = 0U;
             inputIndex < node.inputIds.size(); ++inputIndex) {
          const auto &inputId = node.inputIds[inputIndex];
          const auto producer = authoredNodes.find(inputId);
          const bool sampledAuxiliaryPort =
              inputIndex > 0U && producer != authoredNodes.end() &&
              producer->second->kind ==
                  text::TextEffectExecutionNodeKind::MediaInput &&
              (producer->second->capability ==
                   text::TextEffectExecutionCapability::MediaImage ||
               producer->second->capability ==
                   text::TextEffectExecutionCapability::MediaImageSequence ||
               producer->second->capability ==
                   text::TextEffectExecutionCapability::MediaVideo);
          const bool particleStatePort =
              particleMediaNode && producer != authoredNodes.end() &&
              producer->second->kind ==
                  text::TextEffectExecutionNodeKind::State;
          if (sampledAuxiliaryPort || particleStatePort) {
            graphIdentity.AddString(
                particleStatePort
                    ? "particle-state-control-port-v1"
                    : "post-effect-sampled-resource-port-v1");
            graphIdentity.Add(inputIndex);
            graphIdentity.AddString(inputId);
            continue;
          }
          colorInputIds.push_back(inputId);
        }
        resolvedInputIds = &colorInputIds;
      }
      bool inputsResolved = true;
      bool inputUsesPresentation =
          resolvedInputIds->empty() && inherited &&
          inheritedUsesPresentation;
      for (const auto &inputId : *resolvedInputIds) {
        inputUsesPresentation =
            inputUsesPresentation ||
            presentationOutputs.contains(inputId);
      }
      if (node.kind == text::TextEffectExecutionNodeKind::History) {
        const auto previous = historySnapshot.find(node.historyId);
        inputUsesPresentation =
            inputUsesPresentation ||
            (previous != historySnapshot.end() &&
             previous->second.picture &&
             previous->second.presentationDomain);
      }
      // Flux, Multimesh, radial decay and encoded sweep masks read screen RTs with
      // fullscreen UV. Promote Page attachments before the first material and
      // propagate that domain through downstream blur, history and composition.
      if (pageDomain && node.active &&
          node.kind == text::TextEffectExecutionNodeKind::MaterialPass &&
          (node.capability ==
               text::TextEffectExecutionCapability::MaterialFluxTurbulentBlend ||
           node.capability ==
               text::TextEffectExecutionCapability::MaterialMultimeshTextComposite ||
           node.capability ==
               text::TextEffectExecutionCapability::MaterialRadialDecayHsvGlow ||
           node.capability ==
               text::TextEffectExecutionCapability::MaterialEncodedAlphaDistanceBlur)) {
        inputUsesPresentation = true;
        graphIdentity.AddString("fullscreen-material-input-v1");
      }
      // Entity motion is applied before the camera viewport clips the text.
      // Promote the source first: recording a translated entity in its original
      // tight Page would discard pixels before downstream fullscreen passes.
      if (pageDomain && node.active &&
          node.kind == text::TextEffectExecutionNodeKind::Scene &&
          node.capability == text::TextEffectExecutionCapability::SceneEntity &&
          FindTextExecutionParameter(
              node, text::TextEffectExecutionParameterKind::SceneTranslation,
              0U, std::nullopt)) {
        inputUsesPresentation = true;
        graphIdentity.AddString("scene-entity-presentation-input-v1");
      }
      if (pageDomain && node.active &&
          node.capability == text::TextEffectExecutionCapability::MaterialTurbulenceDisplacement) {
        const auto *mode = FindTextExecutionParameter(
            node, text::TextEffectExecutionParameterKind::MaterialScalar, 13U);
        // The ordinary turbulence pass also samples a fullscreen attachment.
        // Keeping it in the expanded text Page stretches the noise lattice and
        // downstream blur taps when that Page is projected onto the canvas.
        // Promote once before displacement, as for the sprite variants, and
        // preserve this domain through the cumulative glow passes.
        if (!mode || mode->values.front() == 0.0F ||
            mode->values.front() == 1.0F || mode->values.front() == 2.0F) {
          inputUsesPresentation = true;
          graphIdentity.AddString("turbulence-presentation");
        }
      }
      const auto &nodeRecordingBounds =
          inputUsesPresentation ? presentationBounds : recordingBounds;
      const auto *nodeDomain = inputUsesPresentation && presentationDomain
                                   ? &*presentationDomain : pageDomain;
      float projectionCanvasHeight = static_cast<float>(request.outputHeight);
      if (nodeDomain) {
        const auto parentAxis = nodeDomain->authoredToDevice.mapVector(
            SkVector::Make(0.0F, sourceToReferenceScale));
        projectionCanvasHeight *= std::hypot(parentAxis.x(), parentAxis.y()) *
                                  nodeDomain->rasterScaleY;
      }
      sk_sp<SkPicture> input;
      if (inputUsesPresentation) {
        std::vector<RenderComponent> inputPictures;
        inputPictures.reserve(resolvedInputIds->size());
        std::size_t inputOrder = 0U;
        for (const auto &inputId : *resolvedInputIds) {
          const auto found = outputs.find(inputId);
          if (found == outputs.end()) {
            error = "text execution graph input was not executed in authored "
                    "order: " +
                    node.nodeId + " <- " + inputId;
            inputsResolved = false;
            break;
          }
          auto picture = found->second;
          if (picture && !presentationOutputs.contains(inputId))
            picture = promotePagePicture(picture);
          if (found->second && !picture) {
            error = "text execution graph Page input could not be promoted: " +
                    node.nodeId + " <- " + inputId;
            inputsResolved = false;
            break;
          }
          if (picture) {
            inputPictures.push_back(
                {inputId,
                 text::TextEffectCompositeItemKind::PostEffect,
                 0,
                 text::TextBlendMode::SourceOver,
                 std::move(picture),
                 "execution:" + inputId,
                 inputOrder++});
          }
        }
        if (inputsResolved)
          input = ComposePictures(inputPictures, presentationBounds);
      } else {
        input = ComposeExecutionGraphInputs(
            node.nodeId, *resolvedInputIds, outputs, recordingBounds,
            inputsResolved, error);
      }
      if (!inputsResolved)
        return false;
      if (!input && node.inputIds.empty())
        input = inherited;

      sk_sp<SkPicture> output;
      bool materialUsesPresentation = false;
      if (!node.active) {
        output = input;
      } else {
        switch (node.kind) {
        case text::TextEffectExecutionNodeKind::Layout: {
          sk_sp<SkPicture> layoutSource = input;
          if (!layoutSource) {
            switch (node.capability) {
            case text::TextEffectExecutionCapability::LayoutGlyphRun:
              layoutSource = glyphSource;
              graphIdentity.AddString("layout-glyph-run-source");
              break;
            case text::TextEffectExecutionCapability::LayoutCaptionModule:
              layoutSource = captionSource;
              graphIdentity.AddString("layout-caption-module-source");
              break;
            case text::TextEffectExecutionCapability::LayoutPagedText:
              layoutSource = globalSource;
              graphIdentity.AddString("layout-paged-text-source");
              break;
            case text::TextEffectExecutionCapability::LayoutTimedLyric:
              layoutSource = glyphSource;
              graphIdentity.AddString("layout-timed-lyric-source");
              break;
            default:
              break;
            }
          }
          output = recordPass(layoutSource, nodeRecordingBounds);
          if (!output) {
            error = "text execution layout has no glyph source: " +
                    node.nodeId;
            return false;
          }
          break;
        }
        case text::TextEffectExecutionNodeKind::Selector:
          if (!input) {
            error = "text execution selector has no layout input: " +
                    node.nodeId;
            return false;
          }
          if (node.capability ==
              text::TextEffectExecutionCapability::SelectorBase) {
            output = recordPass(input, nodeRecordingBounds);
            graphIdentity.AddString("selector-base-evaluated-units");
          } else {
            output = recordPass(input, nodeRecordingBounds,
                                &nodeRecordingBounds);
            graphIdentity.AddString("selector-time-evaluated-units");
            graphIdentity.Add(node.effectTimeUs);
          }
          break;
        case text::TextEffectExecutionNodeKind::Operator:
          if (!input) {
            error = "text execution operator has no selected input: " +
                    node.nodeId;
            return false;
          }
          switch (node.capability) {
          case text::TextEffectExecutionCapability::OperatorProgram:
            output = recordPass(input, nodeRecordingBounds);
            graphIdentity.AddString("operator-program-frame-plan");
            break;
          case text::TextEffectExecutionCapability::OperatorKeyframe:
            output = recordPass(input, nodeRecordingBounds,
                                &nodeRecordingBounds);
            graphIdentity.AddString("operator-keyframe-frame-plan");
            break;
          case text::TextEffectExecutionCapability::OperatorStagger:
            output = recordPass(input, nodeRecordingBounds);
            graphIdentity.AddString("operator-stagger-unit-order");
            break;
          case text::TextEffectExecutionCapability::OperatorWipe:
            output = recordPass(input, nodeRecordingBounds,
                                &nodeRecordingBounds);
            graphIdentity.AddString("operator-wipe-frame-plan");
            break;
          case text::TextEffectExecutionCapability::OperatorTypewriter:
            output = recordPass(input, nodeRecordingBounds);
            graphIdentity.AddString("operator-typewriter-frame-plan");
            break;
          case text::TextEffectExecutionCapability::OperatorReveal:
            output = recordPass(input, nodeRecordingBounds,
                                &nodeRecordingBounds);
            graphIdentity.AddString("operator-reveal-frame-plan");
            break;
          case text::TextEffectExecutionCapability::OperatorClone:
            output = MaterializeExecutionGraphPicture(
                input, nodeRecordingBounds, gpuContext, error);
            graphIdentity.AddString("operator-clone-independent-attachment");
            break;
          case text::TextEffectExecutionCapability::OperatorTransform:
            output = recordPass(input, nodeRecordingBounds);
            graphIdentity.AddString("operator-transform-frame-plan");
            break;
          case text::TextEffectExecutionCapability::OperatorMaterial:
            output = MaterializeExecutionGraphPicture(
                input, nodeRecordingBounds, gpuContext, error);
            graphIdentity.AddString("operator-material-attachment");
            break;
          case text::TextEffectExecutionCapability::OperatorSceneLookup:
            output = recordPass(input, nodeRecordingBounds);
            graphIdentity.AddString("operator-scene-lookup");
            break;
          case text::TextEffectExecutionCapability::OperatorResource:
            output = recordPass(input, nodeRecordingBounds);
            graphIdentity.AddString("operator-resource-binding");
            break;
          case text::TextEffectExecutionCapability::OperatorCommand:
            output = recordPass(input, nodeRecordingBounds);
            graphIdentity.AddString("operator-command-frame-plan");
            break;
          case text::TextEffectExecutionCapability::OperatorEvent:
            output = recordPass(input, nodeRecordingBounds);
            graphIdentity.AddString("operator-event-frame-plan");
            break;
          case text::TextEffectExecutionCapability::OperatorGlyphSubstitution:
            output = recordPass(input, nodeRecordingBounds);
            graphIdentity.AddString("operator-glyph-substitution-frame-plan");
            break;
          case text::TextEffectExecutionCapability::OperatorLifecycle:
            output = recordPass(input, nodeRecordingBounds,
                                &nodeRecordingBounds);
            graphIdentity.AddString("operator-lifecycle-frame-plan");
            graphIdentity.Add(node.stateElapsedUs);
            break;
          default:
            error = "text execution operator capability is unavailable: " +
                    node.nodeId;
            return false;
          }
          if (!output)
            return false;
          break;
        case text::TextEffectExecutionNodeKind::State:
          switch (node.capability) {
          case text::TextEffectExecutionCapability::StateDeterministicRandom:
            output = input ? recordPass(input, nodeRecordingBounds)
                           : sk_sp<SkPicture>{};
            graphIdentity.AddString("state-deterministic-random");
            graphIdentity.Add(node.randomSeed);
            break;
          case text::TextEffectExecutionCapability::StatePhysics:
            output = input ? recordPass(input, nodeRecordingBounds,
                                        &nodeRecordingBounds)
                           : sk_sp<SkPicture>{};
            graphIdentity.AddString("state-physics-frame-plan");
            break;
          case text::TextEffectExecutionCapability::StateCollision:
            output = input ? recordPass(input, nodeRecordingBounds)
                           : sk_sp<SkPicture>{};
            graphIdentity.AddString("state-collision-frame-plan");
            break;
          case text::TextEffectExecutionCapability::StateParticle:
            output = input ? MaterializeExecutionGraphPicture(
                                 input, nodeRecordingBounds, gpuContext, error)
                           : sk_sp<SkPicture>{};
            graphIdentity.AddString("state-particle-attachment");
            graphIdentity.Add(node.stateElapsedUs);
            break;
          default:
            error = "text execution state capability is unavailable: " +
                    node.nodeId;
            return false;
          }
          if (input && !output)
            return false;
          break;
        case text::TextEffectExecutionNodeKind::Scene: {
          hasDrawableNode = true;
          auto source = input ? input : globalSource;
          if (!source) {
            error = "text execution scene has no drawable source: " +
                    node.nodeId;
            return false;
          }
          if (node.staticAffine)
            source = ApplyExecutionGraphStaticAffine(
                source, *node.staticAffine, nodeRecordingBounds);
          if (!node.parameters.empty()) {
            const std::unordered_map<std::uint64_t, sk_sp<SkPicture>>
                *isolatedUnits = nullptr;
            if (hasNativeSceneUnits && !inputUsesPresentation &&
                !node.staticAffine && node.inputIds.size() == 1U &&
                isUnmodifiedGlyphInput(node.inputIds.front())) {
              for (const auto &parameter : node.parameters) {
                if (parameter.domain ==
                        text::TextEffectExecutionParameterDomain::PerUnit &&
                    parameter.stableUnitId) {
                  if (!prepareNativeSceneUnit(*parameter.stableUnitId))
                    return false;
                  isolatedUnits = &nativeSceneUnits;
                }
              }
            }
            source = ExecuteExecutionSceneCloneParameters(
                source, node, renderPlan, layout, nodeDomain,
                nodeRecordingBounds,
                inputUsesPresentation ? presentationScenePivot
                                      : pageScenePivot,
                isolatedUnits, error);
            if (!source)
              return false;
          }
          switch (node.capability) {
          case text::TextEffectExecutionCapability::ScenePrefab:
            output = recordPass(source, nodeRecordingBounds);
            graphIdentity.AddString("scene-prefab-instance");
            break;
          case text::TextEffectExecutionCapability::SceneEntity:
            output = recordPass(source, nodeRecordingBounds,
                                &nodeRecordingBounds);
            graphIdentity.AddString("scene-entity-transform");
            break;
          case text::TextEffectExecutionCapability::SceneMesh:
            output = MaterializeExecutionGraphPicture(
                source, nodeRecordingBounds, gpuContext, error);
            graphIdentity.AddString("scene-mesh-attachment");
            break;
          case text::TextEffectExecutionCapability::SceneCamera:
            output = recordPass(source, nodeRecordingBounds,
                                &nodeRecordingBounds);
            graphIdentity.AddString("scene-camera-viewport");
            break;
          case text::TextEffectExecutionCapability::SceneProjection:
            output = MaterializeExecutionGraphPicture(
                source, nodeRecordingBounds, gpuContext, error);
            graphIdentity.AddString("scene-projection-target");
            break;
          case text::TextEffectExecutionCapability::SceneClone:
            output = recordPass(source, nodeRecordingBounds);
            graphIdentity.AddString("scene-clone-instance");
            break;
          default:
            error = "text execution scene capability is unavailable: " +
                    node.nodeId;
            return false;
          }
          if (!output)
            return false;
          break;
        }
        case text::TextEffectExecutionNodeKind::MaterialPass: {
          hasDrawableNode = true;
          auto source = input;
          std::vector<sk_sp<SkPicture>> orderedInputs;
          orderedInputs.reserve(node.inputIds.size());
          for (const auto &inputId : node.inputIds) {
            const auto found = outputs.find(inputId);
            if (found != outputs.end() && found->second) {
              auto orderedInput = found->second;
              if (inputUsesPresentation &&
                  !presentationOutputs.contains(inputId)) {
                orderedInput = promotePagePicture(orderedInput);
              }
              if (!orderedInput) {
                error = "text execution material Page input could not be "
                        "promoted: " +
                        node.nodeId + " <- " + inputId;
                return false;
              }
              orderedInputs.push_back(std::move(orderedInput));
            }
          }
          std::string sourceIdentity = node.nodeId;
          const bool orderedColorPass =
              node.capability ==
              text::TextEffectExecutionCapability::MaterialColorPass;
          const bool authoredComponentPass =
              orderedColorPass ||
              node.capability ==
                  text::TextEffectExecutionCapability::MaterialProgramPass;
          if (authoredComponentPass) {
            while (nextMaterialComponent < materialComponents.size() &&
                   !materialComponents[nextMaterialComponent].picture) {
              ++nextMaterialComponent;
            }
            if (nextMaterialComponent < materialComponents.size()) {
              const auto &materialComponent =
                  materialComponents[nextMaterialComponent];
              source = materialComponent.picture;
              if (inputUsesPresentation)
                source = promotePagePicture(source);
              sourceIdentity = materialComponent.identity;
              graphIdentity.AddString(
                  materialComponent.identity);
              ++nextMaterialComponent;
            } else if (!source) {
              source = glyphSource;
              if (inputUsesPresentation)
                source = promotePagePicture(source);
            }
          }
          if (!source) {
            error = "text execution material pass has no glyph source: " +
                    node.nodeId;
            return false;
          }
          // A material node's affine belongs to that authored attachment,
          // before the attachment is accumulated into the loaded color
          // target. Applying it after ordered composition would incorrectly
          // move every previously written material layer as well.
          if (node.staticAffine)
            source = ApplyExecutionGraphStaticAffine(
                source, *node.staticAffine, nodeRecordingBounds);
          if (node.staticAffine) {
            for (auto &orderedInput : orderedInputs) {
              orderedInput = ApplyExecutionGraphStaticAffine(
                  orderedInput, *node.staticAffine, nodeRecordingBounds);
            }
          }
          if (orderedColorPass && orderedMaterialColorOutput) {
            const std::vector<RenderComponent> orderedLayers{
                {node.nodeId + ":previous-color",
                 text::TextEffectCompositeItemKind::GlyphMaterial,
                 0,
                 text::TextBlendMode::SourceOver,
                 orderedMaterialColorOutput,
                 "execution-material-color-prefix",
                 0U},
                {node.nodeId + ":color",
                 text::TextEffectCompositeItemKind::GlyphMaterial,
                 0,
                 text::TextBlendMode::SourceOver,
                 source,
                 sourceIdentity,
                 1U}};
            source = ComposePictures(orderedLayers, nodeRecordingBounds);
          }
          if (node.capability ==
              text::TextEffectExecutionCapability::MaterialDepthPass) {
            auto depth =
                DepthExecutionGraphPicture(source, nodeRecordingBounds);
            if (!depth) {
              error = "text execution depth pass recording failed";
              return false;
            }
            source = std::move(depth);
            graphIdentity.AddString("material-depth-readable-attachment");
          }
          const bool closedMaterialProgram =
              node.capability >= text::TextEffectExecutionCapability::
                                     MaterialAlphaModulate &&
              node.capability <= text::TextEffectExecutionCapability::
                                     MaterialNoiseThresholdDissolve;
          output = closedMaterialProgram
                      ? ExecuteClosedExecutionMaterialDomains(
                             source, node, orderedInputs, sampledMediaInputs,
                             renderPlan, layout,
                             nodeDomain, nodeRecordingBounds,
                             referenceToExecutionScaleX,
                             referenceToExecutionScaleY, projectionCanvasHeight,
                             SkSize::Make(static_cast<float>(request.outputWidth),
                                          static_cast<float>(request.outputHeight)),
                             assets, gpuContext,
                             error)
                       : source;
          if (!output)
            return false;
          if (pageDomain && !inputUsesPresentation &&
              (node.capability == text::TextEffectExecutionCapability::
                                      MaterialDirectionalBoxBlur ||
               node.capability == text::TextEffectExecutionCapability::
                                      MaterialLineColumnNoiseTrail ||
               node.capability == text::TextEffectExecutionCapability::
                                      MaterialGlyphUvBallisticEchoComposite ||
               node.capability == text::TextEffectExecutionCapability::
                                      MaterialMaskedCutLine ||
               node.capability == text::TextEffectExecutionCapability::
                                      MaterialCutLineReveal ||
               node.capability == text::TextEffectExecutionCapability::
                                      MaterialParticleScatterComposite ||
               node.capability == text::TextEffectExecutionCapability::
                                      MaterialCylinderProjectionComposite ||
               node.capability == text::TextEffectExecutionCapability::
                                      MaterialCubeProjectionComposite)) {
            output = promotePagePicture(output);
            if (!output) {
              error = "text terminal material could not publish its Page shader";
              return false;
            }
            materialUsesPresentation = true;
          }
          if (closedMaterialProgram)
            graphIdentity.AddString("closed-material-program-rgba8-v1");
          if (orderedColorPass)
            orderedMaterialColorOutput = output;
          break;
        }
        case text::TextEffectExecutionNodeKind::RenderTarget: {
          hasDrawableNode = true;
          auto source = input;
          if (!source && node.inputIds.empty())
            source = globalSource;
          if (!source) {
            error = "text execution render target has no input: " +
                    node.nodeId;
            return false;
          }
          // The caller supplies the closed graph target domain. For ordinary
          // graphs it is the resolved authored visual envelope; for a Page
          // RenderGroup it is the bounded page-local RT. Re-reading authored
          // FramePlan bounds here would escape that Page domain and allocate
          // the same expansion twice.
          const auto targetBounds = nodeRecordingBounds;
          output = MaterializeExecutionGraphPicture(
              source, targetBounds, gpuContext, error);
          if (!output)
            return false;
          graphIdentity.Add(targetBounds.left());
          graphIdentity.Add(targetBounds.top());
          graphIdentity.Add(targetBounds.width());
          graphIdentity.Add(targetBounds.height());
          break;
        }
        case text::TextEffectExecutionNodeKind::MediaInput: {
          if (node.capability ==
              text::TextEffectExecutionCapability::MediaMesh) {
            ResidentVatMesh residentMesh;
            for (const auto &resourceId : node.resourceIds) {
              const auto sample = std::find_if(
                  framePlan.resources.begin(), framePlan.resources.end(),
                  [&](const auto &candidate) {
                    return candidate.resourceId == resourceId;
                  });
              if (sample == framePlan.resources.end() ||
                  sample->kind != text::TextEffectResourceKind::Mesh ||
                  sample->assetId.empty() || sample->digest.empty()) {
                error = "text execution mesh resource is not closed: " +
                        resourceId;
                return false;
              }
              std::shared_ptr<const std::vector<std::uint8_t>> meshBytes;
              std::string meshIdentity;
              std::string meshMediaType;
              if (!assets.AdmitOpaque(
                      sample->assetId, sample->digest, RuntimeAssetKind::Mesh,
                      text::TextAssetKind::Mesh, meshIdentity, error,
                      &meshBytes, &meshMediaType, false) ||
                  !DecodeResidentVatMesh(meshBytes, residentMesh, error)) {
                return false;
              }
              graphIdentity.AddString(meshIdentity);
              graphIdentity.AddString(meshMediaType);
              graphIdentity.Add(residentMesh.vertices.size());
              graphIdentity.Add(residentMesh.indices.size());
            }
            if (input) {
              output = MaterializeExecutionGraphPicture(
                  input, nodeRecordingBounds, gpuContext, error);
            } else {
              float minimumX = std::numeric_limits<float>::max();
              float minimumY = std::numeric_limits<float>::max();
              float maximumX = std::numeric_limits<float>::lowest();
              float maximumY = std::numeric_limits<float>::lowest();
              for (const auto &vertex : residentMesh.vertices) {
                minimumX = std::min(minimumX, vertex.position[0]);
                minimumY = std::min(minimumY, vertex.position[1]);
                maximumX = std::max(maximumX, vertex.position[0]);
                maximumY = std::max(maximumY, vertex.position[1]);
              }
              const float meshWidth = maximumX - minimumX;
              const float meshHeight = maximumY - minimumY;
              if (meshWidth <= 0.0F || meshHeight <= 0.0F) {
                error = "text execution mesh has no drawable two-dimensional "
                        "extent: " +
                        node.nodeId;
                return false;
              }
              const float scale =
                  std::min(nodeRecordingBounds.width() / meshWidth,
                           nodeRecordingBounds.height() / meshHeight);
              std::vector<SkPoint> meshPositions;
              meshPositions.reserve(residentMesh.vertices.size());
              for (const auto &vertex : residentMesh.vertices) {
                meshPositions.push_back(SkPoint::Make(
                    nodeRecordingBounds.centerX() +
                        (vertex.position[0] -
                         (minimumX + maximumX) * 0.5F) *
                            scale,
                    nodeRecordingBounds.centerY() -
                        (vertex.position[1] -
                         (minimumY + maximumY) * 0.5F) *
                            scale));
              }
              auto vertices = SkVertices::MakeCopy(
                  SkVertices::kTriangles_VertexMode,
                  static_cast<int>(meshPositions.size()),
                  meshPositions.data(), nullptr, nullptr,
                  static_cast<int>(residentMesh.indices.size()),
                  residentMesh.indices.data());
              if (!vertices) {
                error = "text execution mesh publication failed: " +
                        node.nodeId;
                return false;
              }
              SkPictureRecorder recorder;
              auto *canvas = recorder.beginRecording(nodeRecordingBounds);
              SkPaint paint;
              paint.setAntiAlias(true);
              paint.setColor(SK_ColorWHITE);
              canvas->drawVertices(vertices, SkBlendMode::kSrcOver, paint);
              output = recorder.finishRecordingAsPicture();
            }
            if (!output) {
              if (error.empty())
                error = "text execution mesh produced no output: " +
                        node.nodeId;
              return false;
            }
            hasDrawableNode = true;
            graphIdentity.AddString("media-mesh-executed");
            break;
          }
          if (node.capability ==
              text::TextEffectExecutionCapability::MediaParticle) {
            ParticleExecutionState particleState;
            if (!resolveParticleExecutionState(node, particleState)) {
              return false;
            }
            std::vector<sk_sp<SkImage>> particleResources;
            particleResources.reserve(node.resourceIds.size());
            for (const auto &resourceId : node.resourceIds) {
              const bool externallyComposited =
                  std::find(request.externallyCompositedResourceIds.begin(),
                            request.externallyCompositedResourceIds.end(),
                            resourceId) !=
                  request.externallyCompositedResourceIds.end();
              const auto sample = std::find_if(
                  framePlan.resources.begin(), framePlan.resources.end(),
                  [&](const auto &candidate) {
                    return candidate.resourceId == resourceId;
                  });
              if (sample == framePlan.resources.end() ||
                  sample->assetId.empty() || sample->digest.empty()) {
                error = "text execution particle resource is not closed: " +
                        resourceId;
                return false;
              }
              graphIdentity.AddString(sample->assetId);
              graphIdentity.AddString(sample->digest);
              graphIdentity.Add(sample->localTimeUs);
              graphIdentity.Add(externallyComposited);
              if (externallyComposited)
                continue;
              auto *asset = assets.ResolveUnqualified(
                  sample->assetId, sample->digest, error);
              if (!asset)
                return false;
              std::string sampleIdentity;
              auto sampled = assets.SampleRaster(
                  *asset, sample->localTimeUs, sampleIdentity, error);
              if (!sampled)
                return false;
              graphIdentity.AddString(sampleIdentity);
              particleState.identitySeed ^=
                  StableNodeIdentity(sample->digest);
              particleResources.push_back(std::move(sampled));
            }

            using ParameterKind =
                text::TextEffectExecutionParameterKind;
            using ParameterSpace =
                text::TextEffectExecutionParameterSpace;
            const auto *emitter = ConsumeTextExecutionParameter(
                node, ParameterKind::NodeVector4, 0U);
            const auto *life = ConsumeTextExecutionParameter(
                node, ParameterKind::NodeVector4, 1U);
            const auto *velocity = ConsumeTextExecutionParameter(
                node, ParameterKind::NodeVector4, 2U);
            const auto *acceleration = ConsumeTextExecutionParameter(
                node, ParameterKind::NodeVector4, 3U);
            const auto *spriteSize = ConsumeTextExecutionParameter(
                node, ParameterKind::NodeVector4, 4U);
            const auto *emitterDepth = ConsumeTextExecutionParameter(
                node, ParameterKind::NodeVector2, 0U);
            const auto *velocityDepth = ConsumeTextExecutionParameter(
                node, ParameterKind::NodeVector2, 1U);
            const auto *accelerationDepth = ConsumeTextExecutionParameter(
                node, ParameterKind::NodeVector2, 2U);
            const auto *frontierNoiseGeometry = ConsumeTextExecutionParameter(
                node, ParameterKind::NodeVector3, 2U);
            const auto *motion = ConsumeTextExecutionParameter(
                node, ParameterKind::NodeVector4, 5U);
            const auto *frontier = ConsumeTextExecutionParameter(
                node, ParameterKind::NodeVector4, 6U);
            const auto *appearance = ConsumeTextExecutionParameter(
                node, ParameterKind::NodeVector4, 7U);
            const auto *rotationRate = ConsumeTextExecutionParameter(
                node, ParameterKind::NodeVector3, 0U);
            const auto *rotationRandom = ConsumeTextExecutionParameter(
                node, ParameterKind::NodeVector3, 1U);
            const auto *authoredCount = ConsumeTextExecutionParameter(
                node, ParameterKind::NodeScalar, 0U);
            const auto *sizeDecayPower = ConsumeTextExecutionParameter(
                node, ParameterKind::NodeScalar, 1U);
            const auto *sizeMinimumFactor = ConsumeTextExecutionParameter(
                node, ParameterKind::NodeScalar, 2U);
            const bool hasAuthoredContract = !node.parameters.empty();
            const auto pageParameterInSpace = [](const auto *parameter,
                                                 const ParameterSpace space) {
              return parameter &&
                     parameter->domain ==
                         text::TextEffectExecutionParameterDomain::Page &&
                     parameter->valueSpace == space;
            };
            const bool worldCameraContract = node.camera.has_value();
            sk_sp<SkImage> noiseTexture;
            sk_sp<SkImage> sizeOverLifeTexture;
            sk_sp<SkImage> opacityOverLifeTexture;
            sk_sp<SkImage> particleTexture;
            if (worldCameraContract) {
              if (!particleResources.empty())
                noiseTexture = particleResources[0U];
              if (particleResources.size() > 1U)
                sizeOverLifeTexture = particleResources[1U];
              if (particleResources.size() > 2U)
                opacityOverLifeTexture = particleResources[2U];
              if (particleResources.size() > 3U)
                particleTexture = particleResources[3U];
            } else if (!particleResources.empty()) {
              particleTexture = particleResources[0U];
            }
            const auto particleSpatialSpace =
                worldCameraContract ? ParameterSpace::WorldUnits
                                    : ParameterSpace::ReferencePixels;
            if (hasAuthoredContract &&
                (!pageParameterInSpace(emitter,
                                       particleSpatialSpace) ||
                 !pageParameterInSpace(life, ParameterSpace::Unitless) ||
                 !pageParameterInSpace(velocity,
                                       particleSpatialSpace) ||
                 !pageParameterInSpace(acceleration,
                                       particleSpatialSpace) ||
                 !pageParameterInSpace(spriteSize,
                                       particleSpatialSpace) ||
                 (worldCameraContract &&
                  (!pageParameterInSpace(emitterDepth,
                                         ParameterSpace::WorldUnits) ||
                   !pageParameterInSpace(velocityDepth,
                                         ParameterSpace::WorldUnits) ||
                   !pageParameterInSpace(accelerationDepth,
                                         ParameterSpace::WorldUnits) ||
                   !pageParameterInSpace(frontierNoiseGeometry,
                                         ParameterSpace::Unitless))) ||
                 !pageParameterInSpace(motion, ParameterSpace::Unitless) ||
                 !pageParameterInSpace(frontier, ParameterSpace::Unitless) ||
                 !pageParameterInSpace(appearance,
                                       ParameterSpace::Unitless) ||
                 !pageParameterInSpace(rotationRate,
                                       ParameterSpace::Unitless) ||
                 !pageParameterInSpace(rotationRandom,
                                       ParameterSpace::Unitless) ||
                 !pageParameterInSpace(authoredCount,
                                       ParameterSpace::Unitless) ||
                 !pageParameterInSpace(sizeDecayPower,
                                       ParameterSpace::Unitless) ||
                 !pageParameterInSpace(sizeMinimumFactor,
                                       ParameterSpace::Unitless) ||
                 node.parameters.size() !=
                     (worldCameraContract ? 17U : 13U))) {
              error = "text execution particle parameter contract is "
                      "incomplete: " +
                      node.nodeId;
              return false;
            }
            if (!input) {
              error = "text execution particle has no emitter mask input: " +
                      node.nodeId;
              return false;
            }

            const std::array<float, 4> emitterValues =
                emitter ? std::array<float, 4>{emitter->values[0],
                                               emitter->values[1],
                                               emitter->values[2],
                                               emitter->values[3]}
                        : std::array<float, 4>{recordingBounds.centerX(),
                                               recordingBounds.centerY(),
                                               recordingBounds.width() * 0.5F,
                                               recordingBounds.height() *
                                                   0.5F};
            const std::array<float, 4> lifeValues =
                life ? std::array<float, 4>{life->values[0], life->values[1],
                                            life->values[2], life->values[3]}
                     : std::array<float, 4>{0.485F, 0.1F, 4.5F, 1.0F};
            const std::array<float, 4> velocityValues =
                velocity
                    ? std::array<float, 4>{velocity->values[0],
                                           velocity->values[1],
                                           velocity->values[2],
                                           velocity->values[3]}
                    : std::array<float, 4>{12.0F, 5.2F, 7.5F, 5.5F};
            const std::array<float, 4> accelerationValues =
                acceleration
                    ? std::array<float, 4>{acceleration->values[0],
                                           acceleration->values[1],
                                           acceleration->values[2],
                                           acceleration->values[3]}
                    : std::array<float, 4>{5.5F, 1.5F, 5.5F, 5.5F};
            const std::array<float, 4> sizeValues =
                spriteSize
                    ? std::array<float, 4>{spriteSize->values[0],
                                           spriteSize->values[1],
                                           spriteSize->values[2],
                                           spriteSize->values[3]}
                    : std::array<float, 4>{1.5F, 1.5F, 0.5F, 0.5F};
            const std::array<float, 2> emitterDepthValues =
                emitterDepth
                    ? std::array<float, 2>{emitterDepth->values[0],
                                           emitterDepth->values[1]}
                    : std::array<float, 2>{0.0F, 0.0F};
            const std::array<float, 2> velocityDepthValues =
                velocityDepth
                    ? std::array<float, 2>{velocityDepth->values[0],
                                           velocityDepth->values[1]}
                    : std::array<float, 2>{0.0F, 0.0F};
            const std::array<float, 2> accelerationDepthValues =
                accelerationDepth
                    ? std::array<float, 2>{accelerationDepth->values[0],
                                           accelerationDepth->values[1]}
                    : std::array<float, 2>{0.0F, 0.0F};
            const std::array<float, 3> frontierNoiseGeometryValues =
                frontierNoiseGeometry
                    ? std::array<float, 3>{
                          frontierNoiseGeometry->values[0],
                          frontierNoiseGeometry->values[1],
                          frontierNoiseGeometry->values[2]}
                    : std::array<float, 3>{1.0F, 180.0F, 1.0F};
            const std::array<float, 4> motionValues =
                motion ? std::array<float, 4>{motion->values[0],
                                              motion->values[1],
                                              motion->values[2],
                                              motion->values[3]}
                       : std::array<float, 4>{0.31F, 0.1F, 8.5F / 3.0F,
                                              0.0F};
            const std::array<float, 4> frontierValues =
                frontier
                    ? std::array<float, 4>{frontier->values[0],
                                           frontier->values[1],
                                           frontier->values[2],
                                           frontier->values[3]}
                    : std::array<float, 4>{
                          static_cast<float>(particleState.elapsedUs) /
                              1'000'000.0F,
                          3.0F, 0.05F, 0.01F};
            const std::array<float, 4> appearanceValues =
                appearance
                    ? std::array<float, 4>{appearance->values[0],
                                           appearance->values[1],
                                           appearance->values[2],
                                           appearance->values[3]}
                    : std::array<float, 4>{1.0F, 0.0F, 0.0F, 1.0F};
            const std::array<float, 3> rotationRateValues =
                rotationRate
                    ? std::array<float, 3>{rotationRate->values[0],
                                           rotationRate->values[1],
                                           rotationRate->values[2]}
                    : std::array<float, 3>{0.0F, 0.0F, 0.0F};
            const std::array<float, 3> rotationRandomValues =
                rotationRandom
                    ? std::array<float, 3>{rotationRandom->values[0],
                                           rotationRandom->values[1],
                                           rotationRandom->values[2]}
                    : std::array<float, 3>{0.0F, 0.0F, 0.0F};
            constexpr std::size_t kMaximumParticleInstances = 131'072U;
            const double particleCountValue =
                authoredCount
                    ? static_cast<double>(authoredCount->values[0])
                    : 1'024.0;
            const float decayPower =
                sizeDecayPower ? sizeDecayPower->values[0] : 2.2F;
            const float minimumSize =
                sizeMinimumFactor ? sizeMinimumFactor->values[0] : 0.2F;
            const bool invalidParticleContract =
                particleCountValue < 1.0 ||
                particleCountValue >
                    static_cast<double>(kMaximumParticleInstances) ||
                lifeValues[0] <= 0.0F || lifeValues[1] < 0.0F ||
                lifeValues[1] >= lifeValues[0] || lifeValues[2] < 0.0F ||
                lifeValues[3] < 0.0F || lifeValues[3] > 1.0F ||
                emitterValues[2] < 0.0F || emitterValues[3] < 0.0F ||
                emitterDepthValues[1] < 0.0F ||
                frontierNoiseGeometryValues[0] <= 0.0F ||
                frontierNoiseGeometryValues[1] <= 0.0F ||
                frontierNoiseGeometryValues[2] <= 0.0F ||
                sizeValues[0] < 0.0F || sizeValues[1] < 0.0F ||
                sizeValues[2] < 0.0F || sizeValues[3] < 0.0F ||
                motionValues[0] <= 0.0F || motionValues[1] < 0.0F ||
                frontierValues[1] <= 0.0F || frontierValues[2] < 0.0F ||
                frontierValues[3] < 0.0F || frontierValues[3] > 1.0F ||
                appearanceValues[0] < 0.0F ||
                appearanceValues[0] > 1.0F || appearanceValues[1] < 0.0F ||
                appearanceValues[1] > appearanceValues[0] ||
                appearanceValues[2] < 0.0F ||
                appearanceValues[2] > 2.0F ||
                appearanceValues[3] < 0.0F ||
                appearanceValues[3] > 1.0F ||
                decayPower < 0.0F || minimumSize < 0.0F ||
                minimumSize > 1.0F;
            if (invalidParticleContract) {
              error = "text execution particle parameters are outside their "
                      "closed ranges: " +
                      node.nodeId;
              return false;
            }
            const std::size_t particleCount =
                static_cast<std::size_t>(std::floor(particleCountValue));
            const int particleShape =
                static_cast<int>(std::lround(appearanceValues[2]));
            if (worldCameraContract &&
                (!noiseTexture || !sizeOverLifeTexture ||
                 !opacityOverLifeTexture)) {
              error = "text execution world particle texture slots are "
                      "incomplete: " +
                      node.nodeId;
              return false;
            }
            if (std::fabs(appearanceValues[2] -
                          static_cast<float>(particleShape)) > 0.0001F ||
                (particleShape == 2 && !particleTexture)) {
              error = "text execution particle shape contract is invalid: " +
                      node.nodeId;
              return false;
            }

            const bool particleUsesPresentation =
                hasPresentationParticleDomain;
            const auto &particleOutputBounds =
                particleUsesPresentation ? presentationBounds
                                         : recordingBounds;
            const auto mapReferencePoint = [&](const float x, const float y) {
              if (pageDomain) {
                auto point = pageDomain->authoredToDevice.mapPoint(
                    SkPoint::Make(x, y));
                if (particleUsesPresentation)
                  return point;
                point.offset(-pageDomain->deviceTargetBounds.left(),
                             -pageDomain->deviceTargetBounds.top());
                return SkPoint::Make(point.x() * pageDomain->rasterScaleX,
                                     point.y() * pageDomain->rasterScaleY);
              }
              return SkPoint::Make(x * referenceToExecutionScaleX,
                                   y * referenceToExecutionScaleY);
            };
            const auto mapReferenceVector = [&](const float x,
                                                const float y) {
              if (pageDomain) {
                auto vector =
                    pageDomain->authoredToDevice.mapVector(SkVector::Make(x, y));
                if (particleUsesPresentation)
                  return vector;
                return SkVector::Make(
                    vector.x() * pageDomain->rasterScaleX,
                    vector.y() * pageDomain->rasterScaleY);
              }
              return SkVector::Make(x * referenceToExecutionScaleX,
                                    y * referenceToExecutionScaleY);
            };
            const float particlePresentationWidth =
                particleUsesPresentation || worldCameraContract
                    ? static_cast<float>(request.outputWidth)
                    : recordingBounds.width();
            const float particlePresentationHeight =
                particleUsesPresentation || worldCameraContract
                    ? static_cast<float>(request.outputHeight)
                    : recordingBounds.height();
            if (!std::isfinite(particlePresentationWidth) ||
                !std::isfinite(particlePresentationHeight) ||
                particlePresentationWidth <= 0.0F ||
                particlePresentationHeight <= 0.0F) {
              error = "text execution particle presentation viewport is "
                      "invalid: " +
                      node.nodeId;
              return false;
            }
            const float particleScreenAspect =
                particlePresentationWidth / particlePresentationHeight;
            const auto shaderUvToExecutionPoint = [&](const SkPoint uv) {
              float deviceX = uv.x() * particlePresentationWidth;
              float deviceY =
                  (1.0F - uv.y()) * particlePresentationHeight;
              if (particleUsesPresentation) {
                deviceX += presentationBounds.left();
                deviceY += presentationBounds.top();
              } else if (pageDomain) {
                deviceX = (deviceX - pageDomain->deviceTargetBounds.left()) *
                          pageDomain->rasterScaleX;
                deviceY = (deviceY - pageDomain->deviceTargetBounds.top()) *
                          pageDomain->rasterScaleY;
              } else {
                deviceX += recordingBounds.left();
                deviceY += recordingBounds.top();
              }
              return SkPoint::Make(deviceX, deviceY);
            };
            const auto executionPointToMaskPoint = [&](SkPoint point) {
              if (particleUsesPresentation && pageDomain) {
                point.offset(-pageDomain->deviceTargetBounds.left(),
                             -pageDomain->deviceTargetBounds.top());
                point.set(point.x() * pageDomain->rasterScaleX,
                          point.y() * pageDomain->rasterScaleY);
              }
              return point;
            };
            const auto projectWorldClip = [&](const float x, const float y,
                                              const float z,
                                              std::array<float, 4> &clip) {
              if (!node.camera) {
                return false;
              }
              const auto &matrix = node.camera->worldToClip;
              const float aspect = std::max(particleScreenAspect, 1.0F);
              const float worldX = x * aspect;
              const float worldY = y * aspect;
              const float clipX = matrix[0U] * worldX +
                                  matrix[4U] * worldY + matrix[8U] * z +
                                  matrix[12U];
              const float clipY = matrix[1U] * worldX +
                                  matrix[5U] * worldY + matrix[9U] * z +
                                  matrix[13U];
              const float clipW = matrix[3U] * worldX +
                                  matrix[7U] * worldY + matrix[11U] * z +
                                  matrix[15U];
              if (!std::isfinite(clipX) || !std::isfinite(clipY) ||
                  !std::isfinite(clipW) || std::fabs(clipW) <= 1.0e-6F) {
                return false;
              }
              const float clipZ = matrix[2U] * worldX +
                                  matrix[6U] * worldY + matrix[10U] * z +
                                  matrix[14U];
              if (!std::isfinite(clipZ))
                return false;
              clip = {clipX, clipY, clipZ, clipW};
              return true;
            };
            const auto projectWorldUv = [&](const float x, const float y,
                                            const float z, SkPoint &uv) {
              std::array<float, 4> clip;
              if (!projectWorldClip(x, y, z, clip))
                return false;
              const auto &viewport = node.camera->viewport;
              const float normalizedX =
                  viewport[0U] + viewport[2U] *
                                     (clip[0U] / clip[3U] * 0.5F + 0.5F);
              const float normalizedY =
                  viewport[1U] + viewport[3U] *
                                     (clip[1U] / clip[3U] * 0.5F + 0.5F);
              uv = SkPoint::Make(normalizedX, normalizedY);
              return uv.isFinite();
            };
            const auto projectWorldPoint = [&](const float x, const float y,
                                               const float z,
                                               SkPoint &point) {
              SkPoint uv;
              if (!projectWorldUv(x, y, z, uv))
                return false;
              point = shaderUvToExecutionPoint(uv);
              return point.isFinite();
            };

            SkPoint particleSourceOriginUv = SkPoint::Make(0.5F, 0.5F);
            SkPoint particleSourceOriginClip = SkPoint::Make(0.0F, 0.0F);
            SkPoint particleSourceLowerUv = SkPoint::Make(0.0F, 0.0F);
            SkPoint particleSourceUpperUv = SkPoint::Make(1.0F, 1.0F);
            const auto rotateParticleUv = [&](const SkPoint point,
                                               const float angle) {
              float x = point.x() - particleSourceOriginUv.x();
              float y = point.y() - particleSourceOriginUv.y();
              x *= particleScreenAspect;
              const float cosine = std::cos(angle);
              const float sine = std::sin(angle);
              const float rotatedX = cosine * x + sine * y;
              const float rotatedY = -sine * x + cosine * y;
              return SkPoint::Make(
                  particleSourceOriginUv.x() +
                      rotatedX / particleScreenAspect,
                  particleSourceOriginUv.y() + rotatedY);
            };
            if (worldCameraContract) {
              if (!pageDomain ||
                  pageDomain->resolvedFixedGeometryBounds.isEmpty() ||
                  !pageDomain->resolvedFixedGeometryBounds.isFinite()) {
                error = "text execution world particle has no stable Page "
                        "source quad: " +
                        node.nodeId;
                return false;
              }
              const auto authoredSourceBounds =
                  pageDomain->resolvedFixedGeometryBounds;
              if (!std::isfinite(referenceWidth) || referenceWidth <= 0.0F ||
                  !std::isfinite(referenceHeight) || referenceHeight <= 0.0F) {
                error = "text execution world particle reference canvas is "
                        "invalid: " +
                        node.nodeId;
                return false;
              }
              const auto sourcePointToPresentation =
                  [&](const SkPoint point) {
                return pageDomain->authoredToDevice.mapPoint(point);
              };
              const auto authoredSourceOrigin = SkPoint::Make(
                  referenceWidth * 0.5F, referenceHeight * 0.5F);
              const auto sourceOrigin =
                  sourcePointToPresentation(authoredSourceOrigin);
              const auto sourceLower = sourcePointToPresentation(
                  SkPoint::Make(
                      authoredSourceOrigin.x() -
                          authoredSourceBounds.width() * 0.5F,
                      authoredSourceOrigin.y() +
                          authoredSourceBounds.height() * 0.5F));
              const auto sourceUpper = sourcePointToPresentation(
                  SkPoint::Make(
                      authoredSourceOrigin.x() +
                          authoredSourceBounds.width() * 0.5F,
                      authoredSourceOrigin.y() -
                          authoredSourceBounds.height() * 0.5F));
              if (!sourceOrigin.isFinite() || !sourceLower.isFinite() ||
                  !sourceUpper.isFinite()) {
                error = "text execution world particle Page source quad is "
                        "invalid: " +
                        node.nodeId;
                return false;
              }
              const auto toShaderUv = [&](const float x, const float y) {
                return SkPoint::Make(
                    x / particlePresentationWidth,
                    1.0F - y / particlePresentationHeight);
              };
              particleSourceOriginUv =
                  toShaderUv(sourceOrigin.x(), sourceOrigin.y());
              const auto &viewport = node.camera->viewport;
              particleSourceOriginClip = SkPoint::Make(
                  ((particleSourceOriginUv.x() - viewport[0U]) /
                       viewport[2U] -
                   0.5F) *
                      2.0F,
                  ((particleSourceOriginUv.y() - viewport[1U]) /
                       viewport[3U] -
                   0.5F) *
                      2.0F);
              particleSourceLowerUv =
                  toShaderUv(sourceLower.x(), sourceLower.y());
              particleSourceUpperUv =
                  toShaderUv(sourceUpper.x(), sourceUpper.y());

              particleSourceLowerUv = rotateParticleUv(
                  particleSourceLowerUv, -motionValues[3]);
              particleSourceUpperUv = rotateParticleUv(
                  particleSourceUpperUv, -motionValues[3]);
              if (std::fabs(particleSourceUpperUv.x() -
                            particleSourceLowerUv.x()) <= 1.0e-6F ||
                  std::fabs(particleSourceUpperUv.y() -
                            particleSourceLowerUv.y()) <= 1.0e-6F) {
                error = "text execution world particle Page source quad is "
                        "singular: " +
                        node.nodeId;
                return false;
              }
            }

            SkPoint emitterCenter;
            float emitterHalfWidth = 0.0F;
            float emitterHalfHeight = 0.0F;
            if (worldCameraContract) {
              SkPoint emitterXEdge;
              SkPoint emitterYEdge;
              if (!projectWorldPoint(emitterValues[0], emitterValues[1],
                                     emitterDepthValues[0], emitterCenter) ||
                  !projectWorldPoint(emitterValues[0] + emitterValues[2],
                                     emitterValues[1],
                                     emitterDepthValues[0], emitterXEdge) ||
                  !projectWorldPoint(emitterValues[0],
                                     emitterValues[1] + emitterValues[3],
                                     emitterDepthValues[0], emitterYEdge)) {
                error = "text execution particle camera projection is "
                        "singular: " +
                        node.nodeId;
                return false;
              }
              emitterHalfWidth = std::hypot(emitterXEdge.x() -
                                                 emitterCenter.x(),
                                             emitterXEdge.y() -
                                                 emitterCenter.y());
              emitterHalfHeight = std::hypot(emitterYEdge.x() -
                                                  emitterCenter.x(),
                                              emitterYEdge.y() -
                                                  emitterCenter.y());
            } else {
              emitterCenter = emitter
                                  ? mapReferencePoint(emitterValues[0],
                                                      emitterValues[1])
                                  : SkPoint::Make(
                                        particleOutputBounds.centerX(),
                                        particleOutputBounds.centerY());
              const auto emitterXAxis =
                  emitter ? mapReferenceVector(emitterValues[2], 0.0F)
                          : SkVector::Make(
                                particleOutputBounds.width() * 0.5F,
                                           0.0F);
              const auto emitterYAxis =
                  emitter ? mapReferenceVector(0.0F, emitterValues[3])
                          : SkVector::Make(0.0F,
                                particleOutputBounds.height() * 0.5F);
              emitterHalfWidth =
                  std::hypot(emitterXAxis.x(), emitterXAxis.y());
              emitterHalfHeight =
                  std::hypot(emitterYAxis.x(), emitterYAxis.y());
            }
            auto baseVelocity = SkVector::Make(velocityValues[0],
                                               velocityValues[1]);
            auto randomVelocity = SkVector::Make(velocityValues[2],
                                                 velocityValues[3]);
            auto baseAcceleration = SkVector::Make(accelerationValues[0],
                                                   accelerationValues[1]);
            auto randomAcceleration = SkVector::Make(accelerationValues[2],
                                                     accelerationValues[3]);
            float baseWidth = sizeValues[0];
            float baseHeight = sizeValues[1];
            float randomWidth = sizeValues[2];
            float randomHeight = sizeValues[3];
            if (!worldCameraContract) {
              baseVelocity = mapReferenceVector(velocityValues[0],
                                                velocityValues[1]);
              randomVelocity = mapReferenceVector(velocityValues[2],
                                                  velocityValues[3]);
              baseAcceleration = mapReferenceVector(accelerationValues[0],
                                                    accelerationValues[1]);
              randomAcceleration = mapReferenceVector(
                  accelerationValues[2], accelerationValues[3]);
              const auto baseSizeX =
                  mapReferenceVector(sizeValues[0], 0.0F);
              const auto baseSizeY =
                  mapReferenceVector(0.0F, sizeValues[1]);
              const auto randomSizeX =
                  mapReferenceVector(sizeValues[2], 0.0F);
              const auto randomSizeY =
                  mapReferenceVector(0.0F, sizeValues[3]);
              baseWidth = std::hypot(baseSizeX.x(), baseSizeX.y());
              baseHeight = std::hypot(baseSizeY.x(), baseSizeY.y());
              randomWidth = std::hypot(randomSizeX.x(), randomSizeX.y());
              randomHeight = std::hypot(randomSizeY.x(), randomSizeY.y());
            }

            SkIRect maskBounds;
            recordingBounds.roundOut(&maskBounds);
            const std::uint64_t maskPixelCount =
                static_cast<std::uint64_t>(std::max(0, maskBounds.width())) *
                static_cast<std::uint64_t>(std::max(0, maskBounds.height()));
            if (maskBounds.isEmpty() ||
                maskPixelCount > kMaximumDecodedAssetPixels) {
              error = "text execution particle emitter mask exceeds the "
                      "surface budget: " +
                      node.nodeId;
              return false;
            }
            const auto maskInfo = SkImageInfo::Make(
                maskBounds.width(), maskBounds.height(),
                kRGBA_8888_SkColorType, kPremul_SkAlphaType,
                SkColorSpace::MakeSRGB());
            // Native Letter/Page pictures can contain GPU-only images. A
            // raster canvas cannot replay those images into an emitter mask.
            // Materialize on their owning GPU before the current particle
            // sampler reads the mask; do not silently lose the glyph source.
            auto maskSurface = gpuContext
                ? gpuContext->MakeSurface(maskInfo, error)
                : SkSurfaces::Raster(maskInfo);
            if (!maskSurface) {
              error = "text execution particle emitter mask allocation "
                      "failed: " +
                      node.nodeId;
              return false;
            }
            maskSurface->getCanvas()->clear(SK_ColorTRANSPARENT);
            maskSurface->getCanvas()->translate(
                -static_cast<float>(maskBounds.left()),
                -static_cast<float>(maskBounds.top()));
            maskSurface->getCanvas()->drawPicture(input);
            const std::size_t maskRowBytes =
                static_cast<std::size_t>(maskBounds.width()) * 4U;
            std::vector<std::uint8_t> maskPixels(
                maskRowBytes * static_cast<std::size_t>(maskBounds.height()));
            if (!maskSurface->readPixels(maskInfo, maskPixels.data(),
                                         maskRowBytes, 0, 0)) {
              error = "text execution particle emitter mask readback failed: " +
                      node.nodeId;
              return false;
            }

            struct ParticleTexturePixels final {
              int width{0};
              int height{0};
              std::size_t rowBytes{0U};
              std::vector<std::uint8_t> pixels;
            };
            const auto readParticleTexture =
                [&](const sk_sp<SkImage> &texture, const char *role,
                    ParticleTexturePixels &result) -> bool {
              result = {};
              if (!texture)
                return true;
              result.width = texture->width();
              result.height = texture->height();
              const std::uint64_t pixelCount =
                  static_cast<std::uint64_t>(result.width) *
                  static_cast<std::uint64_t>(result.height);
              if (result.width <= 0 || result.height <= 0 ||
                  pixelCount > kMaximumDecodedAssetPixels) {
                error = "text execution particle " + std::string(role) +
                        " texture exceeds the surface budget: " + node.nodeId;
                return false;
              }
              const auto info = SkImageInfo::Make(
                  result.width, result.height, kRGBA_8888_SkColorType,
                  kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
              if (texture->isTextureBacked() && !gpuContext) {
                error = "text execution particle " + std::string(role) +
                        " GPU texture has no owning context: " + node.nodeId;
                return false;
              }
              auto surface = texture->isTextureBacked()
                  ? gpuContext->MakeSurface(info, error)
                  : SkSurfaces::Raster(info);
              if (!surface) {
                error = "text execution particle " + std::string(role) +
                        " texture allocation failed: " + node.nodeId;
                return false;
              }
              surface->getCanvas()->clear(SK_ColorTRANSPARENT);
              surface->getCanvas()->drawImage(texture, 0.0F, 0.0F);
              result.rowBytes = static_cast<std::size_t>(result.width) * 4U;
              result.pixels.resize(
                  result.rowBytes * static_cast<std::size_t>(result.height));
              if (!surface->readPixels(info, result.pixels.data(),
                                       result.rowBytes, 0, 0)) {
                error = "text execution particle " + std::string(role) +
                        " texture readback failed: " + node.nodeId;
                return false;
              }
              return true;
            };
            ParticleTexturePixels noisePixels;
            ParticleTexturePixels sizeOverLifePixels;
            ParticleTexturePixels opacityOverLifePixels;
            if (!readParticleTexture(noiseTexture, "frontier-noise",
                                     noisePixels) ||
                !readParticleTexture(sizeOverLifeTexture, "size-over-life",
                                     sizeOverLifePixels) ||
                !readParticleTexture(opacityOverLifeTexture,
                                     "opacity-over-life",
                                     opacityOverLifePixels)) {
              return false;
            }
            const auto sampleRedLinear =
                [](const ParticleTexturePixels &texture, const float u,
                   const float v) noexcept {
              if (texture.pixels.empty())
                return 1.0F;
              const float x = u * static_cast<float>(texture.width) - 0.5F;
              const float y = v * static_cast<float>(texture.height) - 0.5F;
              const int x0 = static_cast<int>(std::floor(x));
              const int y0 = static_cast<int>(std::floor(y));
              const int x1 = x0 + 1;
              const int y1 = y0 + 1;
              const float fx = x - static_cast<float>(x0);
              const float fy = y - static_cast<float>(y0);
              const auto texel = [&](const int sampleX, const int sampleY) {
                const int clampedX =
                    std::clamp(sampleX, 0, texture.width - 1);
                const int clampedY =
                    std::clamp(sampleY, 0, texture.height - 1);
                const std::size_t offset =
                    static_cast<std::size_t>(clampedY) * texture.rowBytes +
                    static_cast<std::size_t>(clampedX) * 4U;
                return static_cast<float>(texture.pixels[offset]) / 255.0F;
              };
              const float top = texel(x0, y0) +
                                (texel(x1, y0) - texel(x0, y0)) * fx;
              const float bottom = texel(x0, y1) +
                                   (texel(x1, y1) - texel(x0, y1)) * fx;
              return top + (bottom - top) * fy;
            };
            const auto sampleOverLife =
                [&](const ParticleTexturePixels &texture,
                    const float progress) noexcept {
              return sampleRedLinear(texture,
                                     std::clamp(progress, 0.0F, 1.0F), 0.5F);
            };
            const auto mirrorRepeat = [](const float value) noexcept {
              const float shifted = value - 1.0F;
              const float wrapped =
                  shifted - 2.0F * std::floor(shifted / 2.0F);
              return std::fabs(wrapped - 1.0F);
            };
            const auto sampleFrontierNoise = [&](const SkPoint spawnUv) {
              float relativeX =
                  (spawnUv.x() - particleSourceLowerUv.x()) /
                      (particleSourceUpperUv.x() -
                       particleSourceLowerUv.x()) -
                  0.5F;
              float relativeY =
                  (spawnUv.y() - particleSourceLowerUv.y()) /
                      (particleSourceUpperUv.y() -
                       particleSourceLowerUv.y()) -
                  0.5F;
              relativeX /= frontierNoiseGeometryValues[0U];
              const float screenMinimum = std::min(
                  particlePresentationWidth, particlePresentationHeight);
              float frontierScale =
                  frontierNoiseGeometryValues[1U] *
                  (screenMinimum / 720.0F);
              frontierScale /= screenMinimum;
              frontierScale *= 4.0F;
              const float frontierDivisor = 1.0F / frontierScale;
              relativeX /= frontierDivisor;
              relativeY /= frontierDivisor;
              relativeX /= frontierNoiseGeometryValues[2U];
              relativeY /= frontierNoiseGeometryValues[2U];
              relativeX += 0.5F;
              relativeY += 0.5F;
              const float noiseU = mirrorRepeat(relativeX);
              const float noiseV = mirrorRepeat(relativeY);
              return sampleRedLinear(noisePixels, noiseU, noiseV);
            };

            const auto particleRandom = [](const float p0,
                                           const float p1) noexcept {
              const auto fract = [](const float value) noexcept {
                return value - std::floor(value);
              };
              const float x = fract(p0 * 12.23099994659423828125F);
              const float y =
                  x + x * ((x + 22.118999481201171875F) + p1);
              return fract((y + y) * y);
            };
            const auto mixParticleIdentity = [](std::uint64_t value) noexcept {
              value ^= value >> 30U;
              value *= 0xbf58476d1ce4e5b9ULL;
              value ^= value >> 27U;
              value *= 0x94d049bb133111ebULL;
              return value ^ (value >> 31U);
            };
            const auto fallbackFrontierNoise = [&](const std::size_t index) {
              const auto bits = mixParticleIdentity(
                  particleState.identitySeed ^
                  (static_cast<std::uint64_t>(index) *
                   0xd6e8feb86659fd93ULL) ^
                  (4U * 0x9e3779b97f4a7c15ULL));
              return static_cast<float>(bits >> 40U) /
                     static_cast<float>(1U << 24U);
            };
            const auto sampleEmitter = [&](const float x, const float y,
                                           std::array<float, 4> &color) {
              if (x < static_cast<float>(maskBounds.left()) ||
                  x > static_cast<float>(maskBounds.right()) ||
                  y < static_cast<float>(maskBounds.top()) ||
                  y > static_cast<float>(maskBounds.bottom())) {
                return false;
              }
              const float pixelX = x -
                                   static_cast<float>(maskBounds.left()) -
                                   0.5F;
              const float pixelY = y -
                                   static_cast<float>(maskBounds.top()) -
                                   0.5F;
              const int x0 = static_cast<int>(std::floor(pixelX));
              const int y0 = static_cast<int>(std::floor(pixelY));
              const int x1 = x0 + 1;
              const int y1 = y0 + 1;
              const float fx = pixelX - static_cast<float>(x0);
              const float fy = pixelY - static_cast<float>(y0);
              const auto texel = [&](const int sampleX, const int sampleY,
                                     const std::size_t channel) {
                const int clampedX =
                    std::clamp(sampleX, 0, maskBounds.width() - 1);
                const int clampedY =
                    std::clamp(sampleY, 0, maskBounds.height() - 1);
                const std::size_t offset =
                    static_cast<std::size_t>(clampedY) * maskRowBytes +
                    static_cast<std::size_t>(clampedX) * 4U + channel;
                return static_cast<float>(maskPixels[offset]) / 255.0F;
              };
              const auto sampleChannel = [&](const std::size_t channel) {
                const float top = texel(x0, y0, channel) +
                                  (texel(x1, y0, channel) -
                                   texel(x0, y0, channel)) *
                                      fx;
                const float bottom = texel(x0, y1, channel) +
                                     (texel(x1, y1, channel) -
                                      texel(x0, y1, channel)) *
                                         fx;
                return top + (bottom - top) * fy;
              };
              const float alpha = sampleChannel(3U);
              if (alpha < frontierValues[3])
                return false;
              color[3] = alpha;
              if (alpha > 0.0F) {
                color[0] = std::clamp(
                    sampleChannel(0U) / alpha,
                    0.0F, 1.0F);
                color[1] = std::clamp(
                    sampleChannel(1U) / alpha,
                    0.0F, 1.0F);
                color[2] = std::clamp(
                    sampleChannel(2U) / alpha,
                    0.0F, 1.0F);
              }
              return true;
            };

            SkPictureRecorder recorder;
            auto *canvas = recorder.beginRecording(particleOutputBounds);
            const float maximumLife = lifeValues[0] + lifeValues[1];
            // The draw count is integral, while particleTotalNum remains a
            // float uniform in the source shader. Truncating before deriving
            // its seed changes every particle's random coordinates.
            const float particleTotalNum =
                static_cast<float>(particleCountValue);
            const float totalSeed = std::max(
                particleTotalNum / 100.0F, 20.0F);
            const std::size_t activeParticleCount =
                std::min(particleCount, static_cast<std::size_t>(std::floor(
                    lifeValues[3] * particleTotalNum)));
            text::TextParticleExecutionEvidence *particleEvidence = nullptr;
            if (executionEvidence) {
              executionEvidence->particles.push_back(
                  {node.nodeId, frontierValues[0], activeParticleCount});
              particleEvidence = &executionEvidence->particles.back();
            }
            if (particleState.elapsedUs == 0 &&
                std::getenv("VIDEOCUT_TRACE_TEXT_RENDER_GROUP_TOPOLOGY") != nullptr) {
              std::fprintf(stderr,
                           "[VIDEOCUT_TEXT_PARTICLE] time=%.9g count=%.9g "
                           "seed=%.9g emitter=%.9g noise=%.9g,%.9g,%.9g\n",
                           frontierValues[0], particleTotalNum, totalSeed,
                           emitterValues[2], frontierNoiseGeometryValues[0],
                           frontierNoiseGeometryValues[1],
                           frontierNoiseGeometryValues[2]);
            }
            const float localCosine = std::cos(motionValues[3]);
            const float localSine = std::sin(motionValues[3]);
            for (std::size_t index = 0U; index < activeParticleCount;
                 ++index) {
              const float particleIndex = static_cast<float>(index);
              const auto indexOver = [&](const float denominator) noexcept {
                return particleIndex / denominator;
              };
              const float seedCoordinate = indexOver(totalSeed);
              const float time =
                  frontierValues[0] - 0.1F +
                  particleRandom(seedCoordinate,
                                 14.86999988555908203125F +
                                     seedCoordinate) *
                      maximumLife;
              const float cycle = std::floor(time / maximumLife);
              if (cycle > lifeValues[2])
                continue;
              if (particleEvidence)
                ++particleEvidence->withinCycle;
              const float cyclePhase = cycle / 3.0F;
              const float lifeRandom = particleRandom(
                  particleIndex / 105.0F,
                  11.11999988555908203125F + particleIndex / 104.0F +
                      cyclePhase + particleState.randomSeed);
              const float actualLife =
                  (lifeValues[0] - lifeValues[1]) +
                  2.0F * lifeValues[1] * lifeRandom;
              const float age = time - cycle * maximumLife;
              if (age > actualLife)
                continue;
              if (particleEvidence)
                ++particleEvidence->alive;
              const float normalizedAge =
                  std::clamp(age / actualLife, 0.0F, 1.0F);

              const float spawnRandomX = particleRandom(
                  indexOver(totalSeed - 5.0F),
                  11.86999988555908203125F +
                      indexOver(totalSeed - 3.0F) + cyclePhase +
                      particleState.randomSeed);
              const float spawnRandomY = particleRandom(
                  indexOver(totalSeed + 4.0F),
                  12.22000026702880859375F +
                      indexOver(totalSeed + 3.0F) + cyclePhase +
                      particleState.randomSeed);
              const float spawnRandomZ = particleRandom(
                  indexOver(totalSeed - 1.0F),
                  13.3299999237060546875F +
                      indexOver(totalSeed - 2.0F) + cyclePhase +
                      particleState.randomSeed);
              const float spawnWorldX =
                  emitterValues[0] +
                  (spawnRandomX * 2.0F - 1.0F) *
                      emitterValues[2];
              const float spawnWorldY =
                  emitterValues[1] +
                  (spawnRandomY * 2.0F - 1.0F) *
                      emitterValues[3];
              const float spawnWorldZ =
                  emitterDepthValues[0] +
                  (spawnRandomZ * 2.0F - 1.0F) *
                      emitterDepthValues[1];
              float spawnX = 0.0F;
              float spawnY = 0.0F;
              SkPoint spawnUv = SkPoint::Make(0.0F, 0.0F);
              if (worldCameraContract) {
                if (!projectWorldUv(spawnWorldX, spawnWorldY, spawnWorldZ,
                                    spawnUv)) {
                  continue;
                }
                const auto spawnPoint =
                    shaderUvToExecutionPoint(spawnUv);
                spawnX = spawnPoint.x();
                spawnY = spawnPoint.y();
              } else {
                spawnX = emitterCenter.x() +
                         (spawnRandomX * 2.0F - 1.0F) *
                             emitterHalfWidth;
                spawnY = emitterCenter.y() +
                         (spawnRandomY * 2.0F - 1.0F) *
                             emitterHalfHeight;
              }
              float sampleX = 0.0F;
              float sampleY = 0.0F;
              if (worldCameraContract) {
                const auto sampleUv =
                    rotateParticleUv(spawnUv, motionValues[3]);
                const auto samplePoint =
                    shaderUvToExecutionPoint(sampleUv);
                sampleX = samplePoint.x();
                sampleY = samplePoint.y();
              } else {
                const float sampleDx = spawnX - emitterCenter.x();
                const float sampleDy = spawnY - emitterCenter.y();
                sampleX = emitterCenter.x() + localCosine * sampleDx -
                          localSine * sampleDy;
                sampleY = emitterCenter.y() + localSine * sampleDx +
                          localCosine * sampleDy;
              }
              const auto maskSamplePoint = executionPointToMaskPoint(
                  SkPoint::Make(sampleX, sampleY));
              std::array<float, 4> sourceColor{1.0F, 1.0F, 1.0F, 0.0F};
              if (!sampleEmitter(maskSamplePoint.x(), maskSamplePoint.y(),
                                 sourceColor))
                continue;
              if (particleEvidence)
                ++particleEvidence->sourceCovered;
              const float frontierPosition =
                  (frontierValues[0] - age) / frontierValues[1];
              const float frontierNoise =
                  worldCameraContract ? sampleFrontierNoise(spawnUv)
                                      : fallbackFrontierNoise(index);
              if (std::fabs(frontierNoise - frontierPosition) >
                  frontierValues[2]) {
                continue;
              }
              if (particleEvidence)
                ++particleEvidence->withinFrontier;

              const float velocityRandomX = particleRandom(
                  indexOver(totalSeed - 2.0F),
                  12.86999988555908203125F +
                      indexOver(totalSeed - 13.0F) + cyclePhase +
                      particleState.randomSeed);
              const float velocityRandomY = particleRandom(
                  indexOver(totalSeed + 10.0F),
                  13.22000026702880859375F +
                      indexOver(totalSeed + 13.0F) + cyclePhase +
                      particleState.randomSeed);
              const float velocityRandomZ = particleRandom(
                  indexOver(totalSeed - 11.0F),
                  18.3299999237060546875F +
                      indexOver(totalSeed - 12.0F) + cyclePhase +
                      particleState.randomSeed);
              const float accelerationRandomX = particleRandom(
                  indexOver(totalSeed - 1.0F),
                  12.11999988555908203125F +
                      indexOver(totalSeed - 2.0F) + cyclePhase +
                      particleState.randomSeed);
              const float accelerationRandomY = particleRandom(
                  indexOver(totalSeed + 5.0F),
                  12.21000003814697265625F +
                      indexOver(totalSeed + 4.0F) + cyclePhase +
                      particleState.randomSeed);
              const float accelerationRandomZ = particleRandom(
                  indexOver(totalSeed - 12.0F),
                  14.13000011444091796875F +
                      indexOver(totalSeed - 5.0F) + cyclePhase +
                      particleState.randomSeed);
              float velocityX =
                  baseVelocity.x() +
                  (velocityRandomX * 2.0F - 1.0F) *
                      std::fabs(randomVelocity.x());
              float velocityY =
                  baseVelocity.y() +
                  (velocityRandomY * 2.0F - 1.0F) *
                      std::fabs(randomVelocity.y());
              float velocityZ =
                  velocityDepthValues[0] +
                  (velocityRandomZ * 2.0F - 1.0F) *
                      std::fabs(velocityDepthValues[1]);
              float accelerationX =
                  baseAcceleration.x() +
                  (accelerationRandomX * 2.0F - 1.0F) *
                      std::fabs(randomAcceleration.x());
              float accelerationY =
                  baseAcceleration.y() +
                  (accelerationRandomY * 2.0F - 1.0F) *
                      std::fabs(randomAcceleration.y());
              float accelerationZ =
                  accelerationDepthValues[0] +
                  (accelerationRandomZ * 2.0F - 1.0F) *
                      std::fabs(accelerationDepthValues[1]);
              if (index % 8U == 0U) {
                velocityY *= -0.5F;
                accelerationY *= -0.5F;
              }
              const float drag = motionValues[0];
              const float exponential = std::pow(
                  2.7182800769805908203125F, -drag * age);
              float displacementX =
                  ((accelerationX / drag) * age +
                   ((velocityX - accelerationX / drag) / drag) *
                       (1.0F - exponential)) *
                  motionValues[1];
              float displacementY =
                  ((accelerationY / drag) * age +
                   ((velocityY - accelerationY / drag) / drag) *
                       (1.0F - exponential)) *
                  motionValues[1];
              const float displacementZ =
                  ((accelerationZ / drag) * age +
                   ((velocityZ - accelerationZ / drag) / drag) *
                       (1.0F - exponential)) *
                  motionValues[1];
              const float turbulenceRandomX =
                  particleRandom(indexOver(totalSeed + 7.0F),
                                 26.979999542236328125F +
                                     indexOver(totalSeed + 12.0F) +
                                     cyclePhase) -
                  0.5F;
              const float turbulenceRandomY =
                  particleRandom(indexOver(totalSeed - 2.0F),
                                 26.979999542236328125F +
                                     indexOver(totalSeed + 5.0F) +
                                     cyclePhase) -
                  0.5F;
              const float turbulenceDirection =
                  (0.5F + std::fabs(turbulenceRandomY)) *
                  (turbulenceRandomY >= 0.0F ? 1.0F : -1.0F) * 0.5F *
                  (1.0F - motionValues[0] / 10.0F);
              const float turbulencePhase =
                  particleRandom(
                      indexOver(totalSeed - 1.0F),
                      12.86999988555908203125F +
                          indexOver(totalSeed - 3.0F) + cyclePhase +
                          particleState.randomSeed) *
                      6.280000209808349609375F +
                  (particleRandom(indexOver(totalSeed - 1.0F),
                                  13.77999973297119140625F +
                                      indexOver(totalSeed + 2.0F) +
                                      cyclePhase) -
                   0.5F);
              const float turbulenceMagnitude =
                  std::pow(std::max(age, 0.0F), 0.4F) *
                  motionValues[2] *
                  (turbulenceRandomX +
                   0.5F * (turbulenceRandomX >= 0.0F ? 1.0F : -1.0F)) *
                  0.300000011920928955078125F * motionValues[1];
              displacementX +=
                  std::sin(turbulencePhase *
                           13.14000034332275390625F *
                           turbulenceDirection) *
                  turbulenceMagnitude;
              displacementY +=
                  std::cos(turbulencePhase *
                           16.1399993896484375F *
                           turbulenceDirection) *
                  turbulenceMagnitude;
              const float rotatedDisplacementX =
                  localCosine * displacementX - localSine * displacementY;
              const float rotatedDisplacementY =
                  localSine * displacementX + localCosine * displacementY;

              const float sizeOverLife =
                  sampleOverLife(sizeOverLifePixels, normalizedAge);
              const float lifeScale =
                  std::max(std::pow(1.0F - normalizedAge, decayPower),
                           minimumSize) *
                  sizeOverLife;
              const float sizeRandom = particleRandom(
                  indexOver(totalSeed - 5.0F),
                  11.229999542236328125F +
                      indexOver(totalSeed + 7.0F) + cyclePhase +
                      particleState.randomSeed);
              float halfWidth =
                  std::max(0.0F,
                           baseWidth +
                               (sizeRandom * 2.0F - 1.0F) *
                                   randomWidth) *
                  lifeScale * 4.0F;
              float halfHeight =
                  std::max(0.0F,
                           baseHeight +
                               (sizeRandom * 2.0F - 1.0F) *
                                   randomHeight) *
                  lifeScale * 4.0F;
              const float rotationRandomX = particleRandom(
                  indexOver(totalSeed - 1.0F),
                  12.86999988555908203125F +
                      indexOver(totalSeed - 12.0F) + cyclePhase +
                      particleState.randomSeed);
              const float rotationRandomY = particleRandom(
                  indexOver(totalSeed + 2.0F),
                  13.22000026702880859375F +
                      indexOver(totalSeed + 16.0F) + cyclePhase +
                      particleState.randomSeed);
              const float rotationRandomZ = particleRandom(
                  indexOver(totalSeed - 9.0F),
                  18.3299999237060546875F +
                      indexOver(totalSeed - 19.0F) + cyclePhase +
                      particleState.randomSeed);
              const float rotationX =
                  (rotationRateValues[0] +
                   (rotationRandomX * 2.0F - 1.0F) *
                       std::fabs(rotationRandomValues[0])) *
                  age;
              const float rotationY =
                  (rotationRateValues[1] +
                   (rotationRandomY * 2.0F - 1.0F) *
                       std::fabs(rotationRandomValues[1])) *
                  age;
              const float rotationZ =
                  (rotationRateValues[2] +
                   (rotationRandomZ * 2.0F - 1.0F) *
                       std::fabs(rotationRandomValues[2])) *
                  age;
              float centerX = spawnX + rotatedDisplacementX;
              float centerY = spawnY + rotatedDisplacementY;
              SkMatrix worldParticleSprite = SkMatrix::I();
              bool hasWorldParticleSprite = false;
              if (worldCameraContract && particleShape == 0) {
                const float worldCenterX =
                    spawnWorldX + displacementX;
                const float worldCenterY =
                    spawnWorldY + displacementY;
                const float worldCenterZ = spawnWorldZ + displacementZ;
                const float cosineX = std::cos(rotationX * 0.5F);
                const float sineX = std::sin(rotationX * 0.5F);
                const float cosineY = std::cos(rotationY * 0.5F);
                const float sineY = std::sin(rotationY * 0.5F);
                const float cosineZ = std::cos(rotationZ * 0.5F);
                const float sineZ = std::sin(rotationZ * 0.5F);
                const float cosineXY = cosineX * cosineY;
                const float sineXY = sineX * sineY;
                const std::array<float, 4> quaternion{
                    cosineXY * cosineZ + sineXY * sineZ,
                    cosineXY * sineZ - sineXY * cosineZ,
                    sineX * cosineY * sineZ +
                        cosineX * sineY * cosineZ,
                    sineX * cosineY * cosineZ -
                        cosineX * sineY * sineZ};
                const auto rotateParticleVertex =
                    [&](const float x, const float y) {
                      const float qx = quaternion[0U];
                      const float qy = quaternion[1U];
                      const float qz = quaternion[2U];
                      const float qw = quaternion[3U];
                      const float qw2 = qw * qw;
                      const float qx2 = qx * qx;
                      const float qy2 = qy * qy;
                      const float qz2 = qz * qz;
                      const float qxqy = qx * qy;
                      const float qwqz = qw * qz;
                      const float qxqz = qx * qz;
                      const float qwqy = qw * qy;
                      const float qyqz = qy * qz;
                      const float qwqx = qw * qx;
                      return std::array<float, 3>{
                          (qw2 + qx2 - qy2 - qz2) * x +
                              2.0F * (qxqy - qwqz) * y +
                              2.0F * (qxqz + qwqy),
                          2.0F * (qxqy + qwqz) * x +
                              (qw2 - qx2 + qy2 - qz2) * y +
                              2.0F * (qyqz - qwqx),
                          2.0F * (qxqz - qwqy) * x +
                              2.0F * (qyqz + qwqx) * y +
                              (qw2 - qx2 - qy2 + qz2)};
                    };
                const auto projectParticleVertex =
                    [&](const std::array<float, 3> &rotated,
                        SkPoint &point) {
                      std::array<float, 4> clip;
                      if (!projectWorldClip(
                              worldCenterX + rotated[0U] * halfWidth,
                              worldCenterY + rotated[1U] * halfWidth,
                              worldCenterZ + rotated[2U] * halfWidth *
                                                 std::max(
                                                     particleScreenAspect,
                                                     1.0F),
                              clip)) {
                        return false;
                      }
                      float x = clip[0U] - particleSourceOriginClip.x();
                      float y = clip[1U] - particleSourceOriginClip.y();
                      x *= particleScreenAspect;
                      const float cosine = std::cos(motionValues[3]);
                      const float sine = std::sin(motionValues[3]);
                      const float rotatedX = cosine * x + sine * y;
                      const float rotatedY = -sine * x + cosine * y;
                      clip[0U] = particleSourceOriginClip.x() +
                                 rotatedX / particleScreenAspect;
                      clip[1U] = particleSourceOriginClip.y() + rotatedY;
                      const auto &viewport = node.camera->viewport;
                      const float normalizedX =
                          viewport[0U] +
                          viewport[2U] *
                              (clip[0U] / clip[3U] * 0.5F + 0.5F);
                      const float normalizedY =
                          viewport[1U] +
                          viewport[3U] *
                              (clip[1U] / clip[3U] * 0.5F + 0.5F);
                      point = shaderUvToExecutionPoint(
                          SkPoint::Make(normalizedX, normalizedY));
                      return point.isFinite();
                    };
                // The source mesh is a quad on z=1, followed by a circular
                // fragment mask. Rotating a z=0 outline loses its translated
                // centre, and path antialiasing changes the subpixel tail.
                const std::array<SkPoint, 4> spriteCorners{
                    SkPoint::Make(-1.0F, -1.0F),
                    SkPoint::Make(1.0F, -1.0F),
                    SkPoint::Make(1.0F, 1.0F),
                    SkPoint::Make(-1.0F, 1.0F)};
                std::array<SkPoint, 4> projectedCorners;
                bool validSprite = true;
                for (std::size_t corner = 0; corner < spriteCorners.size();
                     ++corner) {
                  if (!projectParticleVertex(
                          rotateParticleVertex(spriteCorners[corner].x(),
                                               spriteCorners[corner].y()),
                          projectedCorners[corner])) {
                    validSprite = false;
                    break;
                  }
                }
                if (!validSprite ||
                    !worldParticleSprite.setPolyToPoly(
                        spriteCorners, projectedCorners))
                  continue;
                hasWorldParticleSprite = true;
              } else if (worldCameraContract) {
                const float rotationWidthFactor =
                    std::max(0.05F, std::fabs(std::cos(rotationY)));
                const float rotationHeightFactor =
                    std::max(0.05F, std::fabs(std::cos(rotationX)));
                const float worldCenterX =
                    spawnWorldX + rotatedDisplacementX;
                const float worldCenterY =
                    spawnWorldY + rotatedDisplacementY;
                const float worldCenterZ = spawnWorldZ + displacementZ;
                SkPoint projectedCenter;
                SkPoint projectedXEdge;
                SkPoint projectedYEdge;
                if (!projectWorldPoint(worldCenterX, worldCenterY,
                                       worldCenterZ, projectedCenter) ||
                    !projectWorldPoint(
                        worldCenterX + halfWidth * rotationWidthFactor,
                        worldCenterY, worldCenterZ, projectedXEdge) ||
                    !projectWorldPoint(
                        worldCenterX,
                        worldCenterY + halfHeight * rotationHeightFactor,
                        worldCenterZ, projectedYEdge)) {
                  continue;
                }
                centerX = projectedCenter.x();
                centerY = projectedCenter.y();
                halfWidth = std::hypot(projectedXEdge.x() - centerX,
                                       projectedXEdge.y() - centerY);
                halfHeight = std::hypot(projectedYEdge.x() - centerX,
                                        projectedYEdge.y() - centerY);
              } else {
                const float rotationWidthFactor =
                    std::max(0.05F, std::fabs(std::cos(rotationY)));
                const float rotationHeightFactor =
                    std::max(0.05F, std::fabs(std::cos(rotationX)));
                halfWidth *= rotationWidthFactor;
                halfHeight *= rotationHeightFactor;
              }
              if (halfWidth <= 0.0F ||
                  (!worldCameraContract && halfHeight <= 0.0F))
                continue;
              const float opacityRandom = particleRandom(
                  indexOver(totalSeed - 2.0F),
                  12.53999996185302734375F +
                      indexOver(totalSeed + 5.0F) + cyclePhase +
                      particleState.randomSeed);
              const float opacity =
                  std::clamp(appearanceValues[0] -
                                 appearanceValues[1] * opacityRandom,
                             0.0F, 1.0F) *
                  sampleOverLife(opacityOverLifePixels, normalizedAge) *
                  sourceColor[3];
              if (opacity <= 0.0F)
                continue;
              SkPaint particlePaint;
              particlePaint.setAntiAlias(true);
              const bool useInputColor = appearanceValues[3] >= 0.5F;
              particlePaint.setColor(SkColorSetARGB(
                  ColorByte(opacity),
                  ColorByte(useInputColor ? sourceColor[0] : 1.0F),
                  ColorByte(useInputColor ? sourceColor[1] : 1.0F),
                  ColorByte(useInputColor ? sourceColor[2] : 1.0F)));
              const SkRect particleBounds = SkRect::MakeXYWH(
                  centerX - halfWidth, centerY - halfHeight,
                  halfWidth * 2.0F, halfHeight * 2.0F);
              if (particleShape == 0) {
                if (hasWorldParticleSprite) {
                  const auto &program = GetTextRuntimeProgram(
                      TextRuntimeShader::MaterialParticleCircle);
                  if (!program.effect) {
                    error = "text execution particle circle shader failed: " +
                            program.error;
                    return false;
                  }
                  SkRuntimeShaderBuilder shader(program.effect);
                  const std::array<float, 4> color{
                      (useInputColor ? sourceColor[0] : 1.0F) * sourceColor[3],
                      (useInputColor ? sourceColor[1] : 1.0F) * sourceColor[3],
                      (useInputColor ? sourceColor[2] : 1.0F) * sourceColor[3],
                      opacity};
                  shader.uniform("color").set(color.data(), color.size());
                  auto spriteShader = shader.makeShader();
                  if (!spriteShader) {
                    error = "text execution particle circle shader binding failed";
                    return false;
                  }
                  particlePaint.setColor(SK_ColorWHITE);
                  particlePaint.setAntiAlias(false);
                  particlePaint.setShader(std::move(spriteShader));
                  canvas->save();
                  canvas->concat(worldParticleSprite);
                  canvas->drawRect(SkRect::MakeLTRB(-1.0F, -1.0F, 1.0F, 1.0F),
                                   particlePaint);
                  canvas->restore();
                } else {
                  canvas->drawOval(particleBounds, particlePaint);
                }
              } else if (particleShape == 1) {
                canvas->save();
                canvas->rotate(rotationZ *
                                   (180.0F / std::numbers::pi_v<float>),
                               centerX, centerY);
                canvas->drawRect(particleBounds, particlePaint);
                canvas->restore();
              } else if (particleShape == 2) {
                particlePaint.setAlphaf(opacity);
                particlePaint.setColorFilter(SkColorFilters::Blend(
                    SkColorSetARGB(
                        255U,
                        ColorByte(useInputColor ? sourceColor[0] : 1.0F),
                        ColorByte(useInputColor ? sourceColor[1] : 1.0F),
                        ColorByte(useInputColor ? sourceColor[2] : 1.0F)),
                    SkBlendMode::kSrcIn));
                canvas->save();
                canvas->rotate(rotationZ *
                                   (180.0F / std::numbers::pi_v<float>),
                               centerX, centerY);
                canvas->drawImageRect(
                    particleTexture, particleBounds,
                    SkSamplingOptions(SkFilterMode::kLinear,
                                      SkMipmapMode::kNone),
                    &particlePaint);
                canvas->restore();
              } else {
                error = "text execution particle shape is unsupported: " +
                        node.nodeId;
                return false;
              }
              if (particleEvidence) {
                ++particleEvidence->submittedDraws;
                const auto drawBounds = hasWorldParticleSprite
                    ? worldParticleSprite.mapRect(SkRect::MakeLTRB(-1, -1, 1, 1))
                    : particleBounds;
                if (SkRect::Intersects(drawBounds, particleOutputBounds))
                  ++particleEvidence->intersectingDrawBounds;
              }
            }
            output = recorder.finishRecordingAsPicture();
            if (!output) {
              error = "text execution particle media produced no output: " +
                      node.nodeId;
              return false;
            }
            hasDrawableNode = true;
            graphIdentity.AddString(
                worldCameraContract
                    ? "media-particle-world-camera-lifecycle-v1"
                    : "media-particle-mask-emitter-lifecycle");
            graphIdentity.Add(particleState.identitySeed);
            graphIdentity.Add(particleState.randomSeed);
            graphIdentity.Add(particleCount);
            break;
          }
          const bool drawable =
              node.capability ==
                  text::TextEffectExecutionCapability::MediaImage ||
              node.capability ==
                  text::TextEffectExecutionCapability::MediaImageSequence ||
              node.capability ==
                  text::TextEffectExecutionCapability::MediaVideo;
          bool hasInternallyCompositedDrawable = false;
          std::vector<RenderComponent> mediaPictures;
          std::size_t order = 0U;
          if (input) {
            mediaPictures.push_back(
                {node.nodeId + ":input",
                 text::TextEffectCompositeItemKind::PostEffect,
                 0,
                 text::TextBlendMode::SourceOver,
                 input,
                 "execution-input:" + node.nodeId,
                 order++});
          }
          for (const auto &resourceId : node.resourceIds) {
            const bool externallyComposited =
                std::find(request.externallyCompositedResourceIds.begin(),
                          request.externallyCompositedResourceIds.end(),
                          resourceId) !=
                request.externallyCompositedResourceIds.end();
            const auto sample = std::find_if(
                framePlan.resources.begin(), framePlan.resources.end(),
                [&](const auto &candidate) {
                  return candidate.resourceId == resourceId;
                });
            if (sample == framePlan.resources.end() ||
                sample->assetId.empty() || sample->digest.empty()) {
              error = "text execution media resource is not closed: " +
                      resourceId;
              return false;
            }
            graphIdentity.AddString(sample->assetId);
            graphIdentity.AddString(sample->digest);
            graphIdentity.Add(sample->localTimeUs);
            graphIdentity.Add(sample->kind);
            graphIdentity.AddString(sample->mediaType);
            graphIdentity.Add(externallyComposited);
            if (!drawable) {
              const bool fontResource =
                  node.capability ==
                  text::TextEffectExecutionCapability::MediaFont;
              const bool typed = sample->kind !=
                                 text::TextEffectResourceKind::Unspecified;
              const auto builtinKind = typed
                                           ? RuntimeKindForFrameResource(
                                                 sample->kind)
                                       : fontResource
                                           ? RuntimeAssetKind::Font
                                       : node.capability ==
                                                 text::TextEffectExecutionCapability::
                                                     MediaMesh
                                           ? RuntimeAssetKind::Mesh
                                           : RuntimeAssetKind::NestedAnimation;
              const auto projectKind =
                  typed ? ProjectKindForFrameResource(sample->kind)
                        : fontResource ? text::TextAssetKind::Font
                                       : node.capability ==
                                                 text::TextEffectExecutionCapability::
                                                     MediaMesh
                                             ? text::TextAssetKind::Mesh
                                             : text::TextAssetKind::Image;
              std::string opaqueIdentity;
              if (!assets.AdmitOpaque(sample->assetId, sample->digest,
                                      builtinKind, projectKind,
                                      opaqueIdentity, error, nullptr, nullptr,
                                      !typed)) {
                return false;
              }
              graphIdentity.AddString(opaqueIdentity);
              continue;
            }
            if (externallyComposited) {
              graphIdentity.AddString("externally-composited-media");
              continue;
            }
            auto *asset = assets.ResolveUnqualified(sample->assetId,
                                                    sample->digest, error);
            if (!asset)
              return false;
            std::string sampleIdentity;
            auto image = assets.SampleRaster(*asset, sample->localTimeUs,
                                             sampleIdentity, error);
            if (!image)
              return false;
            graphIdentity.AddString(sampleIdentity);
            hasInternallyCompositedDrawable = true;
            if (node.resourceIds.size() == 1U && node.inputIds.empty() &&
                !node.staticAffine && node.children.empty() &&
                (node.capability == text::TextEffectExecutionCapability::MediaImage ||
                 node.capability == text::TextEffectExecutionCapability::MediaImageSequence)) {
              sampledMediaInputs.emplace(node.nodeId, image);
            }
            SkPictureRecorder recorder;
            auto *canvas = recorder.beginRecording(nodeRecordingBounds);
            canvas->drawImageRect(
                image, nodeRecordingBounds,
                SkSamplingOptions(SkFilterMode::kLinear,
                                  SkMipmapMode::kNone),
                nullptr);
            mediaPictures.push_back(
                {resourceId,
                 text::TextEffectCompositeItemKind::Decoration,
                 0,
                 text::TextBlendMode::SourceOver,
                 recorder.finishRecordingAsPicture(),
                 sampleIdentity,
                 order++});
          }
          hasDrawableNode =
              hasDrawableNode || hasInternallyCompositedDrawable;
          if (drawable && !hasInternallyCompositedDrawable) {
            output = input ? input
                           : EmptyExecutionGraphPicture(nodeRecordingBounds);
          } else {
            output = drawable
                         ? ComposePictures(mediaPictures,
                                           nodeRecordingBounds)
                         : input;
          }
          if (drawable && !output) {
            error = "text execution media input produced no drawable frame: " +
                    node.nodeId;
            return false;
          }
          break;
        }
        case text::TextEffectExecutionNodeKind::History: {
          hasDrawableNode = true;
          using ParameterKind = text::TextEffectExecutionParameterKind;
          const auto *foldGains = ConsumeTextExecutionParameter(
              node, ParameterKind::NodeVector2, 0U);
          const auto *finalMix = ConsumeTextExecutionParameter(
              node, ParameterKind::NodeScalar, 0U);
          if (foldGains || finalMix) {
            if (!foldGains || foldGains->valueSpace !=
                                  text::TextEffectExecutionParameterSpace::
                                      Unitless ||
                (finalMix &&
                 finalMix->valueSpace !=
                     text::TextEffectExecutionParameterSpace::Unitless)) {
              error = "text same-frame history parameter contract is "
                      "incomplete: " +
                      node.nodeId;
              return false;
            }
            std::vector<sk_sp<SkPicture>> orderedHistoryInputs;
            orderedHistoryInputs.reserve(node.inputIds.size());
            for (const auto &inputId : node.inputIds) {
              const auto found = outputs.find(inputId);
              if (found == outputs.end() || !found->second) {
                error = "text same-frame history input is unavailable: " +
                        node.nodeId + " <- " + inputId;
                return false;
              }
              auto historyInput = found->second;
              if (inputUsesPresentation &&
                  !presentationOutputs.contains(inputId)) {
                historyInput = promotePagePicture(historyInput);
              }
              if (!historyInput) {
                error = "text same-frame history Page input could not be "
                        "promoted: " +
                        node.nodeId + " <- " + inputId;
                return false;
              }
              orderedHistoryInputs.push_back(std::move(historyInput));
            }
            output = ExecuteSameFrameHistoryFold(
                orderedHistoryInputs, nodeRecordingBounds,
                foldGains->values[0], foldGains->values[1],
                finalMix ? std::optional<float>{finalMix->values[0]}
                         : std::nullopt,
                gpuContext, error);
            if (!output)
              return false;
            graphIdentity.AddString("history-same-frame-ordered-feedback-v1");
            break;
          }
          const auto found = historySnapshot.find(node.historyId);
          const bool forward =
              found != historySnapshot.end() && found->second.picture &&
              request.localTimeUs > found->second.lastTimeUs;
          auto previous =
              forward ? found->second.picture
                      : EmptyExecutionGraphPicture(nodeRecordingBounds);
          if (forward && inputUsesPresentation &&
              !found->second.presentationDomain) {
            previous = promotePagePicture(previous);
          }
          if (!previous) {
            error = "text execution history could not create its initial frame";
            return false;
          }
          if (!input) {
            output = previous;
            break;
          }
          if (pendingHistories.contains(node.historyId)) {
            error = "text execution graph contains multiple writers for "
                    "one history identity";
            return false;
          }
          SkPictureRecorder recorder;
          auto *canvas = recorder.beginRecording(nodeRecordingBounds);
          const auto *writeMode = ConsumeTextExecutionParameter(
              node, ParameterKind::NodeScalar,
              kTextExecutionHistoryWriteModeSlot);
          const bool replaceHistory = writeMode && writeMode->values.front() == 1.0F;
          // An explicit feedback material has already combined the previous
          // frame. Its RT copy must replace history, not accumulate it again.
          if (!replaceHistory)
            canvas->drawPicture(previous);
          canvas->drawPicture(input);
          output = MaterializeExecutionGraphPicture(
              recorder.finishRecordingAsPicture(), nodeRecordingBounds,
              gpuContext, error);
          if (!output)
            return false;
          const auto revision =
              forward ? found->second.revision : std::uint64_t{0U};
          if (revision == std::numeric_limits<std::uint64_t>::max()) {
            error = "text execution history revision overflow";
            return false;
          }
          TextExecutionGraphHistoryState pending;
          pending.picture = output;
          pending.lastTimeUs = request.localTimeUs;
          pending.revision = revision + 1U;
          pending.presentationDomain = inputUsesPresentation;
          graphIdentity.Add(pending.revision);
          pendingHistories.emplace(node.historyId, std::move(pending));
          graphIdentity.AddString(replaceHistory
              ? "history-temporal-replace-v1"
              : "history-temporal-source-over-feedback-v1");
          break;
        }
        case text::TextEffectExecutionNodeKind::PostEffectPass: {
          hasDrawableNode = true;
          if (!input || !node.postEffectKind) {
            error = "text execution post-effect has no closed input or kind: " +
                    node.nodeId;
            return false;
          }
          const auto authored = std::find_if(
              framePlan.postEffectNodes.begin(),
              framePlan.postEffectNodes.end(), [&](const auto &candidate) {
                return candidate.nodeId == node.nodeId &&
                       candidate.kind == *node.postEffectKind;
              });
          if (authored == framePlan.postEffectNodes.end()) {
            error = "text execution post-effect has no authored parameter "
                    "closure: " +
                    node.nodeId;
            return false;
          }
          QtTextPostEffectStateContext stateContext;
          stateContext.renderGraphInstanceId = graphInstanceId;
          stateContext.lifecycleEpoch =
              request.effectRuntimeClock
                  ? request.effectRuntimeClock->lifecycleEpoch
                  : 1U;
          stateContext.effectNodeInstanceId = StableNodeIdentity(node.nodeId);
          stateContext.renderGroupInstanceId =
              StableNodeIdentity(parentId.empty() ? node.nodeId : parentId);
          stateContext.presentationWidth =
              static_cast<int>(request.outputWidth);
          stateContext.presentationHeight =
              static_cast<int>(request.outputHeight);
          stateContext.renderGroupExpandRatioX = 1.0F;
          stateContext.renderGroupExpandRatioY = 1.0F;
          stateContext.effectTimeSeconds =
              static_cast<double>(node.stateId.empty()
                                      ? node.effectTimeUs
                                      : node.stateElapsedUs) /
              1'000'000.0;
          stateContext.effectRuntimeClockRevision =
              node.stateRevision != 0U
                  ? node.stateRevision
                  : static_cast<std::uint64_t>(
                        std::max<std::int64_t>(0, node.effectTimeUs));
          stateContext.randomSeed = node.randomSeed;
          auto effectiveEffect = *authored;
          double executionProgress = node.progress;
          if (!ApplyPostEffectExecutionParameters(
                  node, effectiveEffect, executionProgress, error)) {
            return false;
          }
          auto executed = ExecuteQtTextProPostEffectPipeline(
              input, effectiveEffect, executionProgress,
              nodeRecordingBounds,
              gpuContext, &stateContext, executionEvidence != nullptr);
          RecordPostEffectExecution(executionEvidence, node.nodeId,
                                    stateContext, executed);
          if (!executed) {
            error = executed.error.empty()
                        ? "text execution post-effect failed closed"
                        : executed.error;
            return false;
          }
          output = std::move(executed.picture);
          break;
        }
        case text::TextEffectExecutionNodeKind::Composite:
          hasDrawableNode = true;
          if (!input) {
            error = "text execution composite has no drawable input: " +
                    node.nodeId;
            return false;
          }
          output = input;
          break;
        }
      }

      if (executionEvidence) {
        text::TextExecutionNodeEvidence nodeEvidence{
            node.nodeId, node.active, true, node.parameters.size()};
        parameterConsumption.Finish(nodeEvidence, sourceAttachmentEvidence);
        executionEvidence->nodes.push_back(std::move(nodeEvidence));
      }
      outputs.emplace(node.nodeId, output);
      const bool outputUsesPresentation =
          inputUsesPresentation || materialUsesPresentation ||
          (node.active && particleMediaNode &&
           hasPresentationParticleDomain);
      if (output && outputUsesPresentation)
        presentationOutputs.insert(node.nodeId);
      if (!node.children.empty()) {
        referencedNodeIds.insert(node.nodeId);
        if (!self(self, node.children, output ? output : inherited,
                  output ? outputUsesPresentation
                         : inheritedUsesPresentation,
                  node.nodeId, depth + 1U)) {
          return false;
        }
      }
    }
    return true;
  };

  if (!processNodes(processNodes, framePlan.executionGraph.nodes, {}, false,
                    {}, 0U))
    return false;
  for (const auto &nodeId : authoredNodeIds) {
    if (!referencedNodeIds.contains(nodeId) &&
        presentationOutputs.contains(nodeId)) {
      terminalUsesPresentation = true;
      break;
    }
  }
  std::vector<RenderComponent> terminals;
  std::size_t order = 0U;
  for (const auto &nodeId : authoredNodeIds) {
    if (referencedNodeIds.contains(nodeId))
      continue;
    const auto found = outputs.find(nodeId);
    if (found == outputs.end() || !found->second)
      continue;
    auto picture = found->second;
    if (terminalUsesPresentation &&
        !presentationOutputs.contains(nodeId)) {
      picture = promotePagePicture(picture);
      if (!picture) {
        error = "text execution graph terminal Page output could not be "
                "promoted: " +
                nodeId;
        return false;
      }
    }
    terminals.push_back({nodeId,
                         text::TextEffectCompositeItemKind::PostEffect,
                         0,
                         text::TextBlendMode::SourceOver,
                         std::move(picture),
                         "execution:" + nodeId,
                         order++});
  }
  terminal = ComposePictures(
      terminals,
      terminalUsesPresentation ? presentationBounds : recordingBounds);
  if (!terminal && !hasDrawableNode) {
    terminal = globalSource;
    terminalUsesPresentation = false;
    graphIdentity.AddString("control-graph-global-source");
  }
  graphIdentity.Add(terminalUsesPresentation);
  if (!terminal) {
    error = "text execution graph produced no drawable terminal";
    return false;
  }
  for (auto &[historyId, history] : pendingHistories)
    histories[historyId] = std::move(history);
  identity = graphIdentity.Finish();
  return true;
}


sk_sp<SkPicture> ResolveFinalComposite(
    const text::TextEffectFramePlan &framePlan,
    const std::vector<RenderComponent> &components,
    const std::unordered_map<std::string, sk_sp<SkPicture>> &postOutputs,
    const sk_sp<SkPicture> &postDefault, const SkRect &recordingBounds,
    std::string &identity) {
  if (!framePlan.postEffectNodes.empty() && !postOutputs.empty()) {
    std::unordered_map<std::string,
                       const text::TextEffectPostEffectNode *>
        nodes;
    std::unordered_set<std::string> referencedNodes;
    for (const auto &node : framePlan.postEffectNodes)
      nodes.emplace(node.nodeId, &node);
    for (const auto &node : framePlan.postEffectNodes) {
      for (const auto &inputId : node.inputIds) {
        if (nodes.contains(inputId))
          referencedNodes.insert(inputId);
      }
    }

    std::vector<std::string> terminalIds;
    for (const auto &node : framePlan.postEffectNodes) {
      if (!referencedNodes.contains(node.nodeId) &&
          postOutputs.contains(node.nodeId)) {
        terminalIds.push_back(node.nodeId);
      }
    }

    std::unordered_set<std::string> consumedComponentIds;
    std::unordered_set<std::string> visitedNodes;
    std::unordered_set<std::string> afterEffectDecorations;
    for (const auto &pass : framePlan.decorationPasses) {
      if (pass.effectScope ==
          text::TextEffectDecorationEffectScope::AfterRenderGroupEffect) {
        afterEffectDecorations.insert(pass.passId);
      }
    }
    const auto consumeNode = [&](const auto &self,
                                 const std::string &nodeId) -> void {
      if (!visitedNodes.insert(nodeId).second)
        return;
      const auto found = nodes.find(nodeId);
      if (found == nodes.end())
        return;
      const auto &node = *found->second;
      const bool pageNode =
          node.renderGroup &&
          node.renderGroup->spec.mode == text::TextRenderGroupMode::Page;
      const bool independentNode =
          node.renderGroup &&
          node.renderGroup->spec.mode != text::TextRenderGroupMode::Page;
      if (pageNode || independentNode) {
        for (const auto &component : components) {
          if (PageRenderGroupIncludesComponent(
                  *node.renderGroup, afterEffectDecorations, component)) {
            consumedComponentIds.insert(component.id);
          }
        }
      }
      if (node.inputIds.empty()) {
        if (!pageNode && !independentNode) {
          for (const auto &component : components)
            consumedComponentIds.insert(component.id);
        }
        return;
      }
      for (const auto &inputId : node.inputIds) {
        if (nodes.contains(inputId))
          self(self, inputId);
        else
          consumedComponentIds.insert(inputId);
      }
    };
    for (const auto &terminalId : terminalIds)
      consumeNode(consumeNode, terminalId);

    std::vector<RenderComponent> terminalComposite;
    terminalComposite.reserve(terminalIds.size() + components.size());
    std::unordered_set<std::string> includedTerminalIds;
    std::unordered_set<std::string> includedComponentIds;
    std::size_t authoredOrder = 0U;
    IdentityBuilder terminalIdentity;
    for (const auto &item : framePlan.compositeOrder) {
      if (item.kind == text::TextEffectCompositeItemKind::PostEffect) {
        if (std::find(terminalIds.begin(), terminalIds.end(), item.itemId) ==
            terminalIds.end()) {
          continue;
        }
        const auto output = postOutputs.find(item.itemId);
        if (output == postOutputs.end() || !output->second)
          continue;
        terminalComposite.push_back(
            {item.itemId, item.kind, item.zOrder, item.blend, output->second,
             "post:" + item.itemId, authoredOrder++});
        includedTerminalIds.insert(item.itemId);
        terminalIdentity.AddString("post:" + item.itemId);
        continue;
      }
      if (consumedComponentIds.contains(item.itemId))
        continue;
      const auto *component = FindComponent(components, item.itemId, item.kind);
      if (!component || !component->picture)
        continue;
      terminalComposite.push_back(
          {item.itemId, item.kind, item.zOrder, item.blend,
           component->picture, component->identity, authoredOrder++});
      includedComponentIds.insert(item.itemId);
      terminalIdentity.AddString(component->identity);
    }
    for (const auto &component : components) {
      if (!component.picture || consumedComponentIds.contains(component.id) ||
          includedComponentIds.contains(component.id)) {
        continue;
      }
      auto copy = component;
      copy.authoredOrder = authoredOrder++;
      terminalComposite.push_back(std::move(copy));
      terminalIdentity.AddString(component.identity);
    }
    for (const auto &terminalId : terminalIds) {
      if (includedTerminalIds.contains(terminalId))
        continue;
      const auto output = postOutputs.find(terminalId);
      if (output == postOutputs.end() || !output->second)
        continue;
      terminalComposite.push_back(
          {terminalId, text::TextEffectCompositeItemKind::PostEffect, 0,
           text::TextBlendMode::SourceOver, output->second,
           "post:" + terminalId, authoredOrder++});
      terminalIdentity.AddString("post:" + terminalId);
    }
    identity = terminalIdentity.Finish();
    if (!terminalComposite.empty())
      return ComposePictures(terminalComposite, recordingBounds);
    return postDefault;
  }
  if (framePlan.compositeOrder.empty()) {
    IdentityBuilder fallbackIdentity;
    for (const auto &component : components)
      fallbackIdentity.AddString(component.identity);
    fallbackIdentity.AddString(postDefault ? "post-default" : "no-post");
    identity = fallbackIdentity.Finish();
    return postDefault ? postDefault : ComposePictures(components,
                                                       recordingBounds);
  }
  std::vector<RenderComponent> ordered;
  ordered.reserve(framePlan.compositeOrder.size());
  std::size_t authoredOrder = 0U;
  IdentityBuilder cacheIdentity;
  std::unordered_set<std::string> includedComponentIds;
  bool includesPostEffect = false;
  for (const auto &item : framePlan.compositeOrder) {
    sk_sp<SkPicture> picture;
    std::string sourceIdentity;
    if (item.kind == text::TextEffectCompositeItemKind::PostEffect) {
      includesPostEffect = true;
      const auto found = postOutputs.find(item.itemId);
      if (found != postOutputs.end())
        picture = found->second;
      sourceIdentity = "post:" + item.itemId;
    } else if (const auto *component =
                   FindComponent(components, item.itemId, item.kind)) {
      picture = component->picture;
      sourceIdentity = component->identity;
    }
    if (!picture)
      continue;
    ordered.push_back({item.itemId, item.kind, item.zOrder, item.blend,
                       std::move(picture), sourceIdentity, authoredOrder++});
    if (item.kind != text::TextEffectCompositeItemKind::PostEffect)
      includedComponentIds.insert(item.itemId);
    cacheIdentity.AddString(item.itemId);
    cacheIdentity.Add(static_cast<unsigned>(item.kind));
    cacheIdentity.Add(item.zOrder);
    cacheIdentity.Add(static_cast<unsigned>(item.blend));
    cacheIdentity.AddString(sourceIdentity);
  }
  if (!includesPostEffect) {
    for (const auto &component : components) {
      if (!component.picture ||
          includedComponentIds.find(component.id) !=
              includedComponentIds.end()) {
        continue;
      }
      auto copy = component;
      copy.authoredOrder = authoredOrder++;
      ordered.push_back(std::move(copy));
      cacheIdentity.AddString(component.identity);
    }
  }
  identity = cacheIdentity.Finish();
  return ComposePictures(ordered, recordingBounds);
}

} // namespace videocut::skia_runtime::internal::text_lane
