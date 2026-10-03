#include "text/TextRenderPipeline.h"

namespace videocut::skia_runtime::internal::text_lane {


std::string CombineIdentity(const std::string &declared,
                            const std::string &resolved) {
  IdentityBuilder identity;
  identity.AddString(declared);
  identity.AddString(resolved);
  return identity.Finish();
}


void AddTextureIdentity(IdentityBuilder &identity,
                        const text::TextureReference &texture,
                        const std::int64_t localTimeUs) {
  identity.AddString(TextureKey(texture));
  if (IsStreamingMediaType(texture.mediaType) ||
      IsLottieMediaType(texture.mediaType))
    identity.Add(localTimeUs);
}


void AddInsetsIdentity(IdentityBuilder &identity,
                       const text::Insets &insets) {
  identity.Add(insets.left);
  identity.Add(insets.top);
  identity.Add(insets.right);
  identity.Add(insets.bottom);
}


void AddMaterialCoordinatesIdentity(
    IdentityBuilder &identity,
    const text::TextMaterialCoordinates &coordinates) {
  identity.Add(coordinates.coordinateSpace);
  identity.Add(coordinates.coordinateOutset);
  identity.Add(coordinates.coordinateScale);
}


void AddGradientStopsIdentity(
    IdentityBuilder &identity,
    const std::vector<text::GradientStop> &stops) {
  identity.Add(stops.size());
  for (const auto &stop : stops) {
    identity.Add(stop.offset);
    identity.AddColor(stop.color);
  }
}


void AddTextBoxBackgroundIdentity(
    IdentityBuilder &identity, const text::TextBoxBackground &background) {
  identity.Add(background.enabled);
  identity.AddColor(background.color);
  AddInsetsIdentity(identity, background.padding);
  identity.Add(background.cornerRadius);
}


void AddMaterialIdentity(IdentityBuilder &identity,
                         const text::TextMaterialBinding &binding,
                         const std::int64_t localTimeUs) {
  identity.Add(binding.index());
  if (const auto *slot = std::get_if<text::EditableTextStyleSlot>(&binding)) {
    identity.AddString(slot->semanticRole);
    identity.Add(slot->replacementMask.has_value());
    if (slot->replacementMask)
      AddTextureIdentity(identity, *slot->replacementMask, localTimeUs);
  }
  const auto &material = ResolveTextMaterial(binding);
  identity.Add(material.index());
  std::visit(
      [&](const auto &value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, text::SolidTextMaterial>) {
          identity.AddColor(value.color);
        } else if constexpr (std::is_same_v<
                                 Value, text::LinearGradientTextMaterial>) {
          identity.Add(value.startX);
          identity.Add(value.startY);
          identity.Add(value.endX);
          identity.Add(value.endY);
          identity.Add(value.spread);
          identity.Add(value.sampling);
          AddMaterialCoordinatesIdentity(identity, value.coordinates);
          AddGradientStopsIdentity(identity, value.stops);
        } else if constexpr (std::is_same_v<
                                 Value, text::RadialGradientTextMaterial>) {
          identity.Add(value.centerX);
          identity.Add(value.centerY);
          identity.Add(value.radius);
          identity.Add(value.spread);
          identity.Add(value.sampling);
          AddMaterialCoordinatesIdentity(identity, value.coordinates);
          AddGradientStopsIdentity(identity, value.stops);
        } else {
          AddTextureIdentity(identity, value.texture, localTimeUs);
          identity.Add(value.fit);
          identity.Add(value.mapping);
          AddMaterialCoordinatesIdentity(identity, value.coordinates);
          identity.Add(value.scale);
          identity.Add(value.rotationDegrees);
          identity.Add(value.offsetX);
          identity.Add(value.offsetY);
          identity.Add(value.flipX);
          identity.Add(value.flipY);
          identity.Add(value.atlasColumns);
          identity.Add(value.atlasRows);
          identity.Add(value.textureOpacity);
          identity.Add(value.opacity);
          identity.Add(value.sourceAlpha);
          identity.Add(value.underlayColor.has_value());
          if (value.underlayColor)
            identity.AddColor(*value.underlayColor);
          AddGradientStopsIdentity(identity, value.underlayGradient);
          identity.Add(value.underlayGradientProjection.startX);
          identity.Add(value.underlayGradientProjection.startY);
          identity.Add(value.underlayGradientProjection.endX);
          identity.Add(value.underlayGradientProjection.endY);
          identity.Add(value.underlayGradientProjection.spread);
          identity.Add(value.underlayGradientProjection.sampling);
        }
      },
      material);
}


void AddGlyphLayerIdentity(IdentityBuilder &identity,
                           const text::TextGlyphMaterialLayer &layer,
                           const std::int64_t localTimeUs) {
  identity.Add(layer.index());
  std::visit(
      [&](const auto &value) {
        identity.AddString(value.layerId);
        identity.Add(value.zOrder);
        identity.Add(value.blend);
        AddMaterialIdentity(identity, value.material, localTimeUs);
        using Value = std::decay_t<decltype(value)>;
        const auto addPolarOffset = [&](const auto &polarOffset) {
          identity.Add(polarOffset.has_value());
          if (polarOffset) {
            identity.Add(polarOffset->radius);
            identity.Add(polarOffset->angleRadians);
          }
        };
        if constexpr (std::is_same_v<Value, text::TextFillLayer>) {
          identity.Add(value.offsetX);
          identity.Add(value.offsetY);
          addPolarOffset(value.normalizedPolarOffset);
        } else if constexpr (std::is_same_v<Value,
                                             text::TextStrokeLayer>) {
          identity.Add(value.width);
          identity.Add(value.innerRingWidth);
          identity.Add(value.signedStartWidth.has_value());
          if (value.signedStartWidth)
            identity.Add(*value.signedStartWidth);
          identity.Add(value.offsetX);
          identity.Add(value.offsetY);
          identity.Add(value.blurRadius);
          identity.Add(value.spread);
          addPolarOffset(value.normalizedPolarOffset);
        } else if constexpr (std::is_same_v<Value,
                                            text::TextShadowLayer>) {
          identity.Add(value.kind);
          identity.Add(value.offsetX);
          identity.Add(value.offsetY);
          identity.Add(value.blurRadius);
          identity.Add(value.spread);
          identity.Add(value.thicknessAngleDegrees);
          identity.Add(value.thicknessDistance);
          identity.Add(value.smoothing);
          identity.Add(value.roundMaskIntensity);
          identity.Add(value.sdfBlurScale);
          addPolarOffset(value.normalizedPolarOffset);
          identity.Add(value.normalizedUvOffset.has_value());
          if (value.normalizedUvOffset) {
            identity.Add(value.normalizedUvOffset->x);
            identity.Add(value.normalizedUvOffset->y);
          }
          identity.Add(value.strokes.size());
          for (const auto &stroke : value.strokes) {
            AddGlyphLayerIdentity(
                identity, text::TextGlyphMaterialLayer{stroke}, localTimeUs);
          }
        } else if constexpr (std::is_same_v<Value, text::TextGlowLayer>) {
          identity.Add(value.radius);
          identity.Add(value.spread);
          identity.Add(value.directionX);
          identity.Add(value.directionY);
          addPolarOffset(value.normalizedPolarOffset);
        }
      },
      layer);
}


void AddFontSpecIdentity(IdentityBuilder &identity,
                         const text::FontSpec &font) {
  identity.AddString(
      FontInstanceResolver::ReferenceCacheKey(font.primary));
  identity.Add(font.fallbacks.size());
  for (const auto &fallback : font.fallbacks) {
    identity.AddString(
        FontInstanceResolver::ReferenceCacheKey(fallback));
  }
  identity.AddString(font.family);
  identity.AddString(font.postscriptName);
  identity.Add(font.weight);
  identity.Add(font.width);
  identity.Add(font.slant);
  identity.Add(font.faceIndex);
  identity.Add(font.variationAxes.size());
  for (const auto &axis : font.variationAxes) {
    identity.AddString(axis.tag);
    identity.Add(axis.value);
  }
  identity.Add(font.features.size());
  for (const auto &feature : font.features) {
    identity.AddString(feature.tag);
    identity.Add(feature.value);
  }
  identity.Add(font.allowSystemGlyphFallback);
}


void AddLayoutBoxIdentity(IdentityBuilder &identity,
                          const text::LayoutBox &box) {
  identity.Add(box.x);
  identity.Add(box.y);
  identity.Add(box.width);
  identity.Add(box.height);
  identity.Add(box.sizingMode);
  identity.Add(box.minimumWidth);
  identity.Add(box.minimumHeight);
  identity.Add(box.maximumWidth.has_value());
  if (box.maximumWidth)
    identity.Add(*box.maximumWidth);
  identity.Add(box.maximumHeight.has_value());
  if (box.maximumHeight)
    identity.Add(*box.maximumHeight);
  identity.Add(box.minimumFitFontSize);
  identity.Add(box.maximumFitFontSize);
  AddInsetsIdentity(identity, box.padding);
  identity.Add(box.verticalAlignment);
  identity.Add(box.clipOverflow);
  identity.Add(box.pixelSnap);
}


void AddParagraphStyleIdentity(IdentityBuilder &identity,
                               const text::ParagraphStyle &style) {
  identity.Add(style.alignment);
  identity.Add(style.direction);
  identity.AddString(style.locale);
  identity.Add(style.maximumLines.has_value());
  if (style.maximumLines)
    identity.Add(*style.maximumLines);
  identity.Add(style.overflow);
  identity.Add(style.lineHeight);
  identity.Add(style.wrap);
  identity.Add(style.lineBreakPolicy);
  identity.Add(style.hyphenation);
  identity.Add(style.firstLineIndent);
  identity.Add(style.startIndent);
  identity.Add(style.endIndent);
  identity.Add(style.spacingBefore);
  identity.Add(style.spacingAfter);
  identity.Add(style.hangingPunctuation);
  identity.Add(style.tabStops.size());
  for (const auto &tab : style.tabStops) {
    identity.Add(tab.position);
    identity.Add(tab.alignment);
    identity.Add(tab.decimalCharacter);
  }
}


std::string LayoutIdentity(const text::TextRenderDocument &document,
                           const text::TextEffectFramePlan &framePlan,
                           const std::uint64_t generation,
                           const std::string &fontIdentity) {
  IdentityBuilder identity;
  identity.Add(generation);
  identity.AddString(fontIdentity);
  identity.AddString(FlattenDocumentText(document.text));
  identity.Add(document.text.referenceCanvas.width);
  identity.Add(document.text.referenceCanvas.height);
  identity.Add(document.text.referenceCanvas.scalePolicy);
  AddLayoutBoxIdentity(identity, document.text.layoutBox);
  identity.Add(document.text.writingMode);
  for (const auto &paragraph : document.text.paragraphs) {
    identity.AddString(paragraph.paragraphId);
    AddParagraphStyleIdentity(identity, paragraph.style);
    for (const auto &run : paragraph.runs) {
      identity.AddString(run.runId);
      identity.AddString(run.utf8Text);
      identity.AddString(run.locale);
      AddFontSpecIdentity(identity, run.style.font);
      identity.Add(run.style.fontSize);
      identity.Add(run.style.letterSpacing);
      identity.Add(run.style.wordSpacing);
      identity.Add(run.style.baselineShift);
    }
  }
  for (const auto &mutation : framePlan.layoutMutations) {
    identity.Add(mutation.kind);
    identity.Add(mutation.stableUnitId);
    identity.Add(mutation.combineMode);
    identity.Add(TextEffectLayoutScalarIdentity(mutation.value));
  }
  for (const auto &unit : framePlan.units) {
    if (!unit.absoluteFontSize && !unit.replacementCodepoint)
      continue;
    identity.Add(unit.stableUnitId);
    identity.Add(unit.absoluteFontSize.has_value());
    if (unit.absoluteFontSize)
      identity.Add(TextEffectLayoutScalarIdentity(*unit.absoluteFontSize));
    identity.Add(unit.replacementCodepoint.has_value());
    if (unit.replacementCodepoint)
      identity.Add(*unit.replacementCodepoint);
  }
  return identity.Finish();
}


std::string GlyphFrameIdentity(const text::TextRenderDocument &document,
                               const text::TextEffectFramePlan &framePlan,
                               const std::string &layoutIdentity,
                               const std::string &resourceIdentity,
                               const std::int64_t localTimeUs) {
  IdentityBuilder identity;
  identity.AddString(layoutIdentity);
  identity.AddString(resourceIdentity);
  for (const auto &paragraph : document.text.paragraphs) {
    for (const auto &run : paragraph.runs) {
      identity.AddString(paragraph.paragraphId);
      identity.AddString(run.runId);
      AddTextBoxBackgroundIdentity(identity, paragraph.style.background);
      AddTextBoxBackgroundIdentity(identity, run.style.background);
      for (const auto &layer : run.style.materials.layers)
        AddGlyphLayerIdentity(identity, layer, localTimeUs);
      const auto addDecoration = [&](const text::TextDecorationLine &line) {
        identity.Add(line.enabled);
        if (line.enabled)
          AddMaterialIdentity(identity, line.material, localTimeUs);
        identity.Add(line.thickness);
        identity.Add(line.offset);
        identity.Add(static_cast<unsigned>(line.style));
        identity.Add(line.skipInk);
      };
      addDecoration(run.style.decoration.underline);
      addDecoration(run.style.decoration.strikeThrough);
    }
  }
  for (const auto &unit : framePlan.units) {
    identity.Add(unit.stableUnitId);
    identity.Add(unit.replacementCodepoint.has_value());
    if (unit.replacementCodepoint)
      identity.Add(*unit.replacementCodepoint);
    if (unit.opacity)
      identity.Add(TextEffectLayoutScalarIdentity(*unit.opacity));
    if (unit.instanceColor)
      identity.AddColor(*unit.instanceColor);
    if (unit.sdfBlurRadius)
      identity.Add(TextEffectLayoutScalarIdentity(*unit.sdfBlurRadius));
    for (const auto &transform : unit.transforms) {
      for (const auto component : transform.localToText.columnMajor)
        identity.Add(component);
      identity.Add(transform.tightAnchorBounds.x);
      identity.Add(transform.tightAnchorBounds.y);
      identity.Add(transform.tightAnchorBounds.width);
      identity.Add(transform.tightAnchorBounds.height);
      identity.Add(transform.layoutAnchorBounds.x);
      identity.Add(transform.layoutAnchorBounds.y);
      identity.Add(transform.layoutAnchorBounds.width);
      identity.Add(transform.layoutAnchorBounds.height);
    }
  }
  for (const auto &pass : framePlan.glyphMaterialPasses) {
    identity.AddString(pass.passId);
    identity.AddString(pass.layerId);
    identity.Add(pass.zOrder);
    identity.Add(pass.blend);
    identity.Add(pass.combineMode);
    AddGlyphLayerIdentity(identity, pass.material, localTimeUs);
    identity.Add(pass.stableUnitIds.size());
    for (const auto stableUnitId : pass.stableUnitIds)
      identity.Add(stableUnitId);
  }
  return identity.Finish();
}


void AddKeyframeIdentity(IdentityBuilder &identity,
                         const text::TextKeyframe &keyframe) {
  identity.Add(keyframe.offset);
  identity.Add(keyframe.value);
  identity.Add(keyframe.tangentIn);
  identity.Add(keyframe.tangentOut);
  identity.Add(keyframe.cubicBezier);
  identity.Add(keyframe.bezierTimeIn);
  identity.Add(keyframe.bezierTimeOut);
}


std::string DecorationFrameIdentity(
    const text::TextEffectFramePlan &framePlan) {
  IdentityBuilder identity;
  identity.Add(framePlan.decorationPasses.size());
  for (const auto &pass : framePlan.decorationPasses) {
    identity.AddString(pass.passId);
    identity.AddString(pass.decorationId);
    identity.AddString(pass.assetId);
    identity.Add(pass.zOrder);
    AddDecorationPassGeometryIdentity(identity, pass);
    identity.Add(pass.assetTimeUs);
    const auto resource = std::find_if(
        framePlan.resources.begin(), framePlan.resources.end(),
        [&](const auto &sample) {
          return sample.resourceId == pass.decorationId ||
                 sample.assetId == pass.assetId;
        });
    identity.Add(resource != framePlan.resources.end());
    if (resource != framePlan.resources.end()) {
      identity.AddString(resource->resourceId);
      identity.AddString(resource->assetId);
      identity.AddString(resource->digest);
      identity.Add(resource->localTimeUs);
      identity.Add(resource->kind);
      identity.AddString(resource->mediaType);
    }
  }
  return identity.Finish();
}


std::string PostEffectFrameIdentity(
    const text::TextEffectFramePlan &framePlan,
    const text::TextRenderRequest &request) {
  IdentityBuilder identity;
  identity.Add(framePlan.postEffectNodes.size());
  for (const auto &node : framePlan.postEffectNodes) {
    identity.AddString(node.nodeId);
    identity.AddString(node.layerId);
    identity.Add(node.effectNodeInstanceId);
    identity.Add(node.kind);
    identity.Add(node.inputIds.size());
    for (const auto &inputId : node.inputIds)
      identity.AddString(inputId);
    identity.Add(node.parameters.size());
    for (const auto &parameter : node.parameters) {
      identity.AddString(parameter.name);
      identity.Add(parameter.values.size());
      for (const auto value : parameter.values)
        identity.Add(value);
      identity.Add(parameter.keyframes.size());
      for (const auto &keyframe : parameter.keyframes)
        AddKeyframeIdentity(identity, keyframe);
    }
    identity.Add(node.amount);
    identity.Add(node.paddingPx);
    identity.Add(node.effectTimeUs);
    identity.Add(node.renderGroup.has_value());
    if (node.renderGroup) {
      const auto &group = *node.renderGroup;
      identity.AddString(group.layerId);
      identity.Add(group.renderGroupInstanceId);
      identity.Add(group.spec.expandRatioX);
      identity.Add(group.spec.expandRatioY);
      identity.Add(group.spec.mode);
      identity.Add(group.spec.offset);
      identity.Add(group.spec.duration.has_value());
      if (group.spec.duration) {
        identity.Add(group.spec.duration->startTimeUs);
        identity.Add(group.spec.duration->endTimeUs);
      }
      identity.Add(group.spec.priority);
      identity.Add(group.spec.randomSeed);
      identity.Add(group.spec.randomSort);
      identity.Add(group.spec.shape);
      identity.Add(group.spec.customRanges.size());
      for (const auto &range : group.spec.customRanges) {
        identity.Add(range.startIndex);
        identity.Add(range.endIndex);
        identity.Add(range.intensity);
        identity.Add(range.localTime.has_value());
        if (range.localTime) {
          identity.Add(range.localTime->startTimeUs);
          identity.Add(range.localTime->endTimeUs);
        }
      }
      identity.Add(group.fixedUnitRange.begin);
      identity.Add(group.fixedUnitRange.end);
      identity.Add(group.fixedGeometryBounds.x);
      identity.Add(group.fixedGeometryBounds.y);
      identity.Add(group.fixedGeometryBounds.width);
      identity.Add(group.fixedGeometryBounds.height);
      identity.Add(group.visualSourceBounds.x);
      identity.Add(group.visualSourceBounds.y);
      identity.Add(group.visualSourceBounds.width);
      identity.Add(group.visualSourceBounds.height);
      for (const auto &inputId : group.sourceInputIds)
        identity.AddString(inputId);
      for (const auto &inputId : group.behindDecorationInputIds)
        identity.AddString(inputId);
      for (const auto &inputId : group.frontDecorationInputIds)
        identity.AddString(inputId);
    }
    identity.Add(ResolvePostEffectProgress(framePlan, node.nodeId,
                                           request.localTimeUs,
                                           request.durationUs));
  }
  const bool observesRuntimeClock =
      !framePlan.postEffectNodes.empty() && request.effectRuntimeClock.has_value();
  identity.Add(observesRuntimeClock);
  if (observesRuntimeClock) {
    identity.Add(request.effectRuntimeClock->event);
    identity.Add(request.effectRuntimeClock->lifecycleEpoch);
    identity.Add(request.effectRuntimeClock->advanceUs);
    identity.Add(request.effectRuntimeClock->restoreElapsedUs.has_value());
    if (request.effectRuntimeClock->restoreElapsedUs)
      identity.Add(*request.effectRuntimeClock->restoreElapsedUs);
    identity.Add(request.effectRuntimeClock->restoreStateRevision);
  }
  return identity.Finish();
}


void AddExecutionGraphNodeFrameIdentity(
    IdentityBuilder &identity,
    const text::TextEffectExecutionNodeFramePlan &node) {
  identity.AddString(node.nodeId);
  identity.Add(node.kind);
  identity.Add(node.capability);
  identity.Add(node.inputIds.size());
  for (const auto &inputId : node.inputIds)
    identity.AddString(inputId);
  identity.Add(node.resourceIds.size());
  for (const auto &resourceId : node.resourceIds)
    identity.AddString(resourceId);
  identity.Add(node.postEffectKind.has_value());
  if (node.postEffectKind)
    identity.Add(*node.postEffectKind);
  identity.AddString(node.stateId);
  identity.AddString(node.historyId);
  identity.Add(node.effectTimeUs);
  identity.Add(node.progress);
  identity.Add(node.active);
  identity.Add(node.hasAuthoredTimeDriver);
  identity.Add(node.stateRevision);
  identity.Add(node.stateElapsedUs);
  identity.Add(node.randomSeed);
  identity.Add(node.historyRevision);
  AddExecutionCameraIdentity(identity, node.camera);
  identity.Add(node.parameters.size());
  for (const auto &parameter : node.parameters)
    AddExecutionGraphParameterIdentity(identity, parameter);
  identity.Add(node.children.size());
  for (const auto &child : node.children)
    AddExecutionGraphNodeFrameIdentity(identity, child);
}


std::string ExecutionGraphFrameIdentity(
    const text::TextEffectFramePlan &framePlan) {
  IdentityBuilder identity;
  identity.Add(framePlan.executionGraph.nodes.size());
  for (const auto &node : framePlan.executionGraph.nodes)
    AddExecutionGraphNodeFrameIdentity(identity, node);
  return identity.Finish();
}


std::string CompositeOrderIdentity(
    const text::TextEffectFramePlan &framePlan) {
  IdentityBuilder identity;
  identity.Add(framePlan.compositeOrder.size());
  for (const auto &item : framePlan.compositeOrder) {
    identity.AddString(item.itemId);
    identity.Add(item.kind);
    identity.Add(item.zOrder);
    identity.Add(item.blend);
  }
  return identity.Finish();
}


std::string AppearanceFrameIdentity(
    const text::TextLayerAppearance &appearance) {
  IdentityBuilder identity;
  identity.Add(appearance.sdfMaterial.enabled);
  identity.Add(appearance.sdfMaterial.sourceCreationComponent);
  identity.Add(appearance.sdfMaterial.distanceRange);
  identity.Add(appearance.sdfMaterial.rasterDistanceRange);
  identity.Add(appearance.sdfMaterial.smoothingScale);
  identity.Add(appearance.sdfMaterial.sourceDesignWidth);
  identity.Add(appearance.sdfMaterial.sourceDesignHeight);
  identity.Add(appearance.bend.enabled);
  identity.Add(appearance.bend.amount);
  identity.Add(appearance.path.enabled);
  identity.AddString(appearance.path.geometryId);
  identity.AddString(appearance.path.presetId);
  identity.Add(appearance.path.commands.size());
  for (const auto &command : appearance.path.commands) {
    identity.Add(command.kind);
    identity.Add(command.control1.x);
    identity.Add(command.control1.y);
    identity.Add(command.control2.x);
    identity.Add(command.control2.y);
    identity.Add(command.end.x);
    identity.Add(command.end.y);
  }
  identity.Add(appearance.path.startOffset);
  identity.Add(appearance.path.baselineOffset);
  identity.Add(appearance.path.overflow);
  identity.Add(appearance.path.loop);
  identity.Add(appearance.path.rotateToTangent);
  identity.Add(appearance.path.keepUpright);
  identity.Add(appearance.visualExtent.allowControlOverflow);
  identity.Add(appearance.visualExtent.maximumExtentWidth.has_value());
  if (appearance.visualExtent.maximumExtentWidth)
    identity.Add(*appearance.visualExtent.maximumExtentWidth);
  identity.Add(appearance.visualExtent.maximumExtentHeight.has_value());
  if (appearance.visualExtent.maximumExtentHeight)
    identity.Add(*appearance.visualExtent.maximumExtentHeight);
  identity.Add(appearance.globalAlpha);
  return identity.Finish();
}


std::string PresentationIdentity(const text::TextRenderRequest &request) {
  IdentityBuilder identity;
  identity.Add(request.outputWidth);
  identity.Add(request.outputHeight);
  identity.Add(static_cast<unsigned>(request.quality));
  identity.Add(request.presentation.positionX);
  identity.Add(request.presentation.positionY);
  identity.Add(request.presentation.scaleX);
  identity.Add(request.presentation.scaleY);
  identity.Add(request.presentation.rotationDegrees);
  identity.Add(request.presentation.opacity);
  identity.Add(request.presentation.flipHorizontal);
  identity.Add(request.presentation.flipVertical);
  identity.Add(request.suppressAnimation);
  identity.Add(request.externalEffectOutset);
  return identity.Finish();
}


void AddBackdropTransformIdentity(
    IdentityBuilder &identity,
    const text::TextBackdropTransform &transform) {
  identity.Add(transform.offsetX);
  identity.Add(transform.offsetY);
  identity.Add(transform.scaleX);
  identity.Add(transform.scaleY);
  identity.Add(transform.rotationDegrees);
  identity.Add(transform.opacity);
}


void AddBackdropSourceIdentity(
    IdentityBuilder &identity, const text::TextBackdropSource &source,
    const std::int64_t sourceTimeUs) {
  identity.Add(source.index());
  std::visit(
      [&](const auto &value) {
        using Source = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Source, text::RoundedRectBackdrop>) {
          identity.Add(value.family);
          AddMaterialIdentity(identity, value.fill, sourceTimeUs);
          identity.Add(value.strokes.size());
          for (const auto &stroke : value.strokes) {
            AddGlyphLayerIdentity(
                identity, text::TextGlyphMaterialLayer{stroke}, sourceTimeUs);
          }
          identity.Add(value.cornerRadius);
          identity.Add(value.tail.edge);
          identity.Add(value.tail.position);
          identity.Add(value.tail.width);
          identity.Add(value.tail.length);
          identity.Add(value.authoredWidth.has_value());
          if (value.authoredWidth)
            identity.Add(*value.authoredWidth);
          identity.Add(value.authoredHeight.has_value());
          if (value.authoredHeight)
            identity.Add(*value.authoredHeight);
        } else if constexpr (std::is_same_v<Source,
                                            text::NineSliceBackdrop>) {
          AddTextureIdentity(identity, value.asset, sourceTimeUs);
          AddInsetsIdentity(identity, value.capInsets);
          AddInsetsIdentity(identity, value.contentInsets);
          identity.Add(value.minimumContentWidth);
          identity.Add(value.minimumContentHeight);
          identity.Add(value.stretchMode);
          identity.AddColor(value.fallbackColor);
        } else if constexpr (std::is_same_v<Source,
                                            text::VectorBackdrop>) {
          AddTextureIdentity(identity, value.asset, sourceTimeUs);
          identity.Add(value.fit);
          identity.AddColor(value.fallbackColor);
        } else {
          AddTextureIdentity(identity, value.asset, sourceTimeUs);
          AddInsetsIdentity(identity, value.contentInsets);
          identity.Add(value.timeSource);
          identity.Add(value.playback);
          identity.Add(value.sourceInUs);
          identity.Add(value.sourceOutUs);
          identity.Add(value.phaseUs);
          identity.AddColor(value.fallbackColor);
        }
      },
      source);
  identity.Add(sourceTimeUs);
}


std::string BackdropFrameIdentity(
    const text::TextRenderDocument &document, const ResolvedLayout &layout,
    const text::TextRenderRequest &request,
    const text::TextEffectFramePlan &framePlan) {
  IdentityBuilder identity;
  const auto passes = ResolveBackdropPasses(document, layout, request,
                                             framePlan);
  identity.Add(passes.size());
  for (const auto &pass : passes) {
    identity.AddString(pass.passId);
    identity.AddString(pass.layerId);
    identity.Add(pass.zOrder);
    AddBackdropSourceIdentity(identity, pass.source, pass.sourceTimeUs);
    identity.Add(pass.materialOverride.has_value());
    if (pass.materialOverride) {
      AddMaterialIdentity(identity, *pass.materialOverride,
                          pass.sourceTimeUs);
    }
    AddBackdropTransformIdentity(identity, pass.transform);
    AddResolvedBackdropGeometryIdentity(identity, pass);
    identity.Add(pass.authoredOrder);
  }
  return identity.Finish();
}

} // namespace videocut::skia_runtime::internal::text_lane
