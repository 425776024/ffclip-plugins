#include "text/TextRenderPipeline.h"

namespace videocut::skia_runtime::internal::text_lane {


sk_sp<SkPicture> ComposePictures(const std::vector<RenderComponent> &components,
                                 const SkRect &recordingBounds) {
  if (components.empty())
    return {};
  std::vector<const RenderComponent *> ordered;
  ordered.reserve(components.size());
  for (const auto &component : components) {
    if (component.picture)
      ordered.push_back(&component);
  }
  std::stable_sort(ordered.begin(), ordered.end(), [](const auto *left,
                                                       const auto *right) {
    if (left->zOrder != right->zOrder)
      return left->zOrder < right->zOrder;
    return left->authoredOrder < right->authoredOrder;
  });
  if (ordered.size() == 1U &&
      ordered.front()->blend == text::TextBlendMode::SourceOver) {
    return ordered.front()->picture;
  }
  SkPictureRecorder recorder;
  auto *canvas = recorder.beginRecording(recordingBounds);
  for (const auto *component : ordered) {
    if (component->blend == text::TextBlendMode::SourceOver) {
      canvas->drawPicture(component->picture);
      continue;
    }
    SkPaint paint;
    paint.setBlendMode(ToSkBlendMode(component->blend));
    canvas->saveLayer(&recordingBounds, &paint);
    canvas->drawPicture(component->picture);
    canvas->restore();
  }
  return recorder.finishRecordingAsPicture();
}


sk_sp<SkImage> RenderDeferredQtTextNativeLetterBatch(
    const DeferredQtTextNativeLetterBatch &batch,
    const PageRenderGroupDomain &domain,
    const std::unordered_set<std::uint64_t> *unitFilter,
    SkiaGpuContext &gpuContext,
    std::string &error) {
  if (!batch.atlasTexture || batch.atlasWidth <= 0 ||
      batch.atlasHeight <= 0 || batch.atlasRowBytes == 0U ||
      batch.vertices.empty() || batch.indices.empty() ||
      !std::isfinite(batch.referenceWidth) || batch.referenceWidth <= 0.0F ||
      !std::isfinite(batch.referenceHeight) || batch.referenceHeight <= 0.0F ||
      domain.authoredToDevice.hasPerspective() ||
      domain.deviceTargetBounds.isEmpty() || domain.targetWidth <= 0 ||
      domain.targetHeight <= 0 || !std::isfinite(domain.rasterScaleX) ||
      !std::isfinite(domain.rasterScaleY) || domain.rasterScaleX <= 0.0F ||
      domain.rasterScaleY <= 0.0F) {
    error = "deferred Qt TextPro Letter Page source is invalid";
    return {};
  }

  std::vector<QtTextTypedLetterVertex> filteredVertices;
  std::vector<std::uint16_t> filteredIndices;
  const std::vector<QtTextTypedLetterVertex> *vertices = &batch.vertices;
  const std::vector<std::uint16_t> *indices = &batch.indices;
  if (unitFilter) {
    if (batch.unitStableIds.size() * 4U != batch.vertices.size() ||
        batch.unitStableIds.size() * 6U != batch.indices.size()) {
      error = "deferred Qt TextPro Letter unit metadata is invalid";
      return {};
    }
    for (std::size_t quad = 0U; quad < batch.unitStableIds.size(); ++quad) {
      if (!unitFilter->contains(batch.unitStableIds[quad]))
        continue;
      if (filteredVertices.size() >
          static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) -
              3U) {
        error = "deferred Qt TextPro Letter unit filter exceeds the uint16 index range";
        return {};
      }
      const auto base = static_cast<std::uint16_t>(filteredVertices.size());
      filteredVertices.insert(filteredVertices.end(),
                              batch.vertices.begin() + quad * 4U,
                              batch.vertices.begin() + (quad + 1U) * 4U);
      filteredIndices.insert(filteredIndices.end(),
                             {base, static_cast<std::uint16_t>(base + 2U),
                              static_cast<std::uint16_t>(base + 1U), base,
                              static_cast<std::uint16_t>(base + 3U),
                              static_cast<std::uint16_t>(base + 2U)});
    }
    if (filteredVertices.empty() || filteredIndices.empty())
      return {};
    vertices = &filteredVertices;
    indices = &filteredIndices;
  }

  QtTextLetterRenderRequest request;
  request.atlas = {batch.atlasRg8MetalRows.data(),
                   batch.atlasRg8MetalRows.size(), batch.atlasWidth,
                   batch.atlasHeight, batch.atlasRowBytes, batch.atlasTexture};
  request.vertices = {
      reinterpret_cast<const std::uint8_t *>(vertices->data()),
      vertices->size() * sizeof(QtTextTypedLetterVertex)};
  request.vertexCount = vertices->size();
  request.indices = indices->data();
  request.indexCount = indices->size();
  request.outputWidth = domain.targetWidth;
  request.outputHeight = domain.targetHeight;
  request.uniforms.offsetInfo[1] = -0.7853981852531433F;
  std::copy(std::begin(batch.materialColor), std::end(batch.materialColor),
            std::begin(request.uniforms.fillColor));

  const float a = domain.authoredToDevice.getScaleX();
  const float b = domain.authoredToDevice.getSkewX();
  const float c = domain.authoredToDevice.getSkewY();
  const float d = domain.authoredToDevice.getScaleY();
  const float translateX = domain.authoredToDevice.getTranslateX();
  const float translateY = domain.authoredToDevice.getTranslateY();
  const float centerX = DivideTextProBinary32(batch.referenceWidth, 2.0F);
  const float centerY = DivideTextProBinary32(batch.referenceHeight, 2.0F);
  constexpr float kTextProLetterCoordinateExtent = 576.0F;
  const float xFactor =
      2.0F * domain.rasterScaleX / static_cast<float>(domain.targetWidth);
  const float yFactor =
      2.0F * domain.rasterScaleY / static_cast<float>(domain.targetHeight);
  std::fill(std::begin(request.uniforms.mvp),
            std::end(request.uniforms.mvp), 0.0F);
  request.uniforms.mvp[0] =
      xFactor * a * kTextProLetterCoordinateExtent;
  request.uniforms.mvp[1] =
      c == 0.0F ? 0.0F : -yFactor * c * kTextProLetterCoordinateExtent;
  request.uniforms.mvp[4] =
      b == 0.0F ? 0.0F : -xFactor * b * kTextProLetterCoordinateExtent;
  request.uniforms.mvp[5] =
      yFactor * d * kTextProLetterCoordinateExtent;
  request.uniforms.mvp[10] = -1.0F / 550.0F;
  request.uniforms.mvp[12] =
      xFactor *
          (a * centerX + b * centerY + translateX -
           domain.deviceTargetBounds.left()) -
      1.0F;
  request.uniforms.mvp[13] =
      1.0F -
      yFactor *
          (c * centerX + d * centerY + translateY -
           domain.deviceTargetBounds.top());
  request.uniforms.mvp[14] = -0.8F;
  request.uniforms.mvp[15] = 1.0F;

  const char *deferredPacketDumpRoot =
      std::getenv("VIDEOCUT_DUMP_QT_TEXT_LETTER_PACKET");
  const char *deferredPacketTimeFilter =
      std::getenv("VIDEOCUT_DUMP_QT_TEXT_LETTER_PACKET_TIME_US");
  bool deferredPacketTimeMatches = true;
  if (deferredPacketTimeFilter != nullptr &&
      deferredPacketTimeFilter[0] != '\0') {
    char *end = nullptr;
    const auto requestedTime =
        std::strtoll(deferredPacketTimeFilter, &end, 10);
    deferredPacketTimeMatches =
        end != deferredPacketTimeFilter && end != nullptr && *end == '\0' &&
        requestedTime == batch.sampledTimeUs;
  }
  if (deferredPacketDumpRoot != nullptr &&
      deferredPacketDumpRoot[0] != '\0' && deferredPacketTimeMatches) {
    const char *targetFilter =
        std::getenv("VIDEOCUT_DUMP_QT_TEXT_LETTER_PACKET_TARGET_WIDTH");
    if (targetFilter == nullptr || targetFilter[0] == '\0' ||
        std::strtol(targetFilter, nullptr, 10) == domain.targetWidth) {
      std::error_code directoryError;
      const std::filesystem::path directory(deferredPacketDumpRoot);
      std::filesystem::create_directories(directory, directoryError);
      if (!directoryError) {
        const auto writeBytes = [&](const std::filesystem::path &path,
                                    const void *bytes,
                                    const std::size_t byteSize) {
          std::ofstream output(path, std::ios::binary | std::ios::trunc);
          if (!output)
            return false;
          output.write(reinterpret_cast<const char *>(bytes),
                       static_cast<std::streamsize>(byteSize));
          return output.good();
        };
        const auto prefix = "time-" + std::to_string(batch.sampledTimeUs) +
                            "-target-" +
                            std::to_string(domain.targetWidth) + "x" +
                            std::to_string(domain.targetHeight);
        writeBytes(directory / (prefix + "-vertices-stride108.bin"),
                   vertices->data(),
                   vertices->size() * sizeof(QtTextTypedLetterVertex));
        writeBytes(directory / (prefix + "-indices-u16.bin"),
                   indices->data(),
                   indices->size() * sizeof(std::uint16_t));
        writeBytes(directory / (prefix + "-mvp-f32.bin"),
                   request.uniforms.mvp, sizeof(request.uniforms.mvp));
        writeBytes(directory / (prefix + "-atlas-rg8-metal-rows.bin"),
                   batch.atlasRg8MetalRows.data(),
                   batch.atlasRg8MetalRows.size());
      }
    }
  }

  struct RuntimeHolder final {
    std::unique_ptr<QtTextLetterMetalRuntime> runtime;
    std::string creationError;
  };
  static RuntimeHolder runtimeHolder = [] {
    RuntimeHolder holder;
    holder.runtime = CreateQtTextLetterMetalRuntime(holder.creationError);
    return holder;
  }();
  if (!runtimeHolder.runtime) {
    error = runtimeHolder.creationError.empty()
                ? "Qt TextPro Letter Page runtime is unavailable"
                : runtimeHolder.creationError;
    return {};
  }
  std::vector<std::uint8_t> metalRows;
  const char *targetFilter =
      std::getenv("VIDEOCUT_DUMP_QT_TEXT_LETTER_PACKET_TARGET_WIDTH");
  const bool dumpPixels = deferredPacketDumpRoot != nullptr &&
      deferredPacketDumpRoot[0] != '\0' && deferredPacketTimeMatches &&
      (targetFilter == nullptr || targetFilter[0] == '\0' ||
       std::strtol(targetFilter, nullptr, 10) == domain.targetWidth);
  auto image = gpuContext.RenderQtTextLetter(
      *runtimeHolder.runtime, request, SkColorSpace::MakeSRGB(), error,
      dumpPixels ? &metalRows : nullptr);
  if (!image) {
    if (error.empty())
      error = "Qt TextPro Letter Page render failed";
    return {};
  }
  // This deferred executor accepts only the base solid-fill Letter contract.
  // Record at GPU publication, never at packet preparation or cache hashing.
  TextMaterialExecutionScope::Record("native-letter-fill", "sdf:enabled", "qt-letter-metal");
  TextMaterialExecutionScope::Record("native-letter-fill", "sdf-layer:fill", "qt-letter-metal");
  TextMaterialExecutionScope::Record("native-letter-fill", "sdf-paint:solid", "qt-letter-metal");
  if (deferredPacketDumpRoot != nullptr &&
      deferredPacketDumpRoot[0] != '\0' && deferredPacketTimeMatches) {
    const char *targetFilter =
        std::getenv("VIDEOCUT_DUMP_QT_TEXT_LETTER_PACKET_TARGET_WIDTH");
    if (targetFilter == nullptr || targetFilter[0] == '\0' ||
        std::strtol(targetFilter, nullptr, 10) == domain.targetWidth) {
      std::error_code directoryError;
      const std::filesystem::path directory(deferredPacketDumpRoot);
      std::filesystem::create_directories(directory, directoryError);
      if (!directoryError) {
        const auto path = directory /
            ("time-" + std::to_string(batch.sampledTimeUs) + "-target-" +
             std::to_string(domain.targetWidth) + "x" +
             std::to_string(domain.targetHeight) +
             "-output-metal-rows.rgba");
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (output) {
          output.write(reinterpret_cast<const char *>(metalRows.data()),
                       static_cast<std::streamsize>(metalRows.size()));
        }
      }
    }
  }
  if (std::getenv("VIDEOCUT_TRACE_QT_TEXT_LETTER_NATIVE") != nullptr) {
    std::fprintf(
        stderr,
        "[VIDEOCUT_QT_TEXT_LETTER_NATIVE] stage=typed_page_rendered "
        "vertices=%zu indices=%zu atlas=%dx%d target=%dx%d "
        "target_bounds=[%.9g %.9g %.9g %.9g] "
        "mvp=[%a %a %a %a] backend=%s\n",
        batch.vertices.size(), batch.indices.size(), batch.atlasWidth,
        batch.atlasHeight, domain.targetWidth, domain.targetHeight,
        domain.deviceTargetBounds.left(), domain.deviceTargetBounds.top(),
        domain.deviceTargetBounds.right(), domain.deviceTargetBounds.bottom(),
        static_cast<double>(request.uniforms.mvp[0]),
        static_cast<double>(request.uniforms.mvp[5]),
        static_cast<double>(request.uniforms.mvp[12]),
        static_cast<double>(request.uniforms.mvp[13]),
        runtimeHolder.runtime->BackendName());
  }
  return image;
}


sk_sp<SkPicture> RenderQtTextNativePageSource(
    const QtTextNativePageSource &source,
    const PageRenderGroupDomain &domain,
    const std::unordered_set<std::uint64_t> *unitFilter,
    SkiaGpuContext *gpuContext,
    std::string &error) {
  if (source.batches.empty())
    return {};
  if (!gpuContext) {
    error = "Qt TextPro Letter Page requires a GPU context";
    return {};
  }
  std::vector<sk_sp<SkImage>> images;
  images.reserve(source.batches.size());
  for (const auto &batch : source.batches) {
    auto image =
        RenderDeferredQtTextNativeLetterBatch(
            batch, domain, unitFilter, *gpuContext, error);
    // A unit filter can legitimately exclude every quad in a batch.  Treat
    // that as an empty contribution and continue; malformed/non-empty
    // batches still carry an error and abort the page source.
    if (!image) {
      if (!error.empty())
        return {};
      continue;
    }
    images.push_back(std::move(image));
  }
  if (images.empty())
    return {};
  SkPictureRecorder recorder;
  auto *canvas = recorder.beginRecording(domain.localBounds);
  if (!canvas) {
    error = "deferred Qt TextPro Page source recorder could not be created";
    return {};
  }
  for (const auto &image : images) {
    canvas->drawImageRect(image.get(), domain.localBounds,
                          SkSamplingOptions(SkFilterMode::kNearest), nullptr);
  }
  return recorder.finishRecordingAsPicture();
}


SkRect ExpandRenderGroupBounds(
    const SkRect &bounds, const text::TextRenderGroupSpec &group) {
  if (bounds.isEmpty())
    return bounds;
  const float width = bounds.width() * group.expandRatioX;
  const float height = bounds.height() * group.expandRatioY;
  return SkRect::MakeXYWH(bounds.centerX() - width * 0.5F,
                          bounds.centerY() - height * 0.5F, width, height);
}


std::optional<PageRenderGroupDomain> ResolvePageRenderGroupDomain(
    const text::TextEffectFramePlan &framePlan,
    const SkMatrix &presentationMatrix,
    const SkRect &fixedPageLetterBounds) {
  if (framePlan.postEffectNodes.empty() || presentationMatrix.hasPerspective())
    return std::nullopt;
  const text::TextEffectRenderGroupExecutionPlan *executionPlan = nullptr;
  for (const auto &node : framePlan.postEffectNodes) {
    if (node.renderGroup &&
        node.renderGroup->spec.mode == text::TextRenderGroupMode::Page) {
      executionPlan = &*node.renderGroup;
      break;
    }
  }
  if (!executionPlan || executionPlan->renderGroupInstanceId == 0U)
    return std::nullopt;
  const auto &execution = *executionPlan;
  for (const auto &node : framePlan.postEffectNodes) {
    if (!node.renderGroup ||
        node.renderGroup->spec.mode != text::TextRenderGroupMode::Page) {
      continue;
    }
    if (node.renderGroup->layerId != execution.layerId ||
        node.renderGroup->renderGroupInstanceId !=
            execution.renderGroupInstanceId) {
      return std::nullopt;
    }
  }
  // Page allocation is part of the Qt Letter pass, not a presentation-sized
  // envelope.  In particular, absoluteFontSize and selector evaluation can
  // change the set of visible Letters for the current frame.  The native
  // packet records the post-layout Letter union that Qt uses for this frame;
  // that union must win whenever it is available.  The FramePlan geometry is
  // retained as a fallback for portable/non-native sources only.  Choosing
  // the immutable descriptor first makes a tiny early-frame Letter get
  // rasterized into the full final-frame target and changes both MVP and
  // hardware fwidth (the observed slide/size animation regression).
  const auto descriptorGeometry = ToSkRect(execution.fixedGeometryBounds);
  const auto fixedGeometry =
      !fixedPageLetterBounds.isEmpty() && fixedPageLetterBounds.isFinite()
          ? fixedPageLetterBounds
          : descriptorGeometry;
  if (fixedGeometry.isEmpty() || !fixedGeometry.isFinite())
    return std::nullopt;
  const SkRect deviceGeometry =
      presentationMatrix.mapRect(fixedGeometry);
  const auto expanded = ExpandRenderGroupBounds(deviceGeometry, execution.spec);
  if (expanded.isEmpty() || !expanded.isFinite())
    return std::nullopt;

  int naturalWidth = std::max(1, static_cast<int>(std::ceil(expanded.width())));
  int naturalHeight =
      std::max(1, static_cast<int>(std::ceil(expanded.height())));
  int targetWidth = naturalWidth;
  int targetHeight = naturalHeight;
  const int maximumDimension = std::max(targetWidth, targetHeight);
  if (maximumDimension > 4096) {
    if (targetWidth >= targetHeight) {
      targetHeight = std::max(
          1, static_cast<int>(static_cast<float>(targetHeight) /
                              static_cast<float>(targetWidth) * 4096.0F));
      targetWidth = 4096;
    } else {
      targetWidth = std::max(
          1, static_cast<int>(static_cast<float>(targetWidth) /
                              static_cast<float>(targetHeight) * 4096.0F));
      targetHeight = 4096;
    }
  }
  const float rasterScaleX = static_cast<float>(targetWidth) /
                             static_cast<float>(naturalWidth);
  const float rasterScaleY = static_cast<float>(targetHeight) /
                             static_cast<float>(naturalHeight);
  const float deviceTargetWidth =
      static_cast<float>(targetWidth) / rasterScaleX;
  const float deviceTargetHeight =
      static_cast<float>(targetHeight) / rasterScaleY;
  PageRenderGroupDomain result;
  result.execution = execution;
  result.resolvedFixedGeometryBounds = fixedGeometry;
  result.authoredToDevice = presentationMatrix;
  if (!presentationMatrix.invert(&result.deviceToAuthored))
    return std::nullopt;
  result.deviceTargetBounds = SkRect::MakeXYWH(
      deviceGeometry.centerX() - deviceTargetWidth * 0.5F,
      deviceGeometry.centerY() - deviceTargetHeight * 0.5F,
      deviceTargetWidth, deviceTargetHeight);
  result.localBounds = SkRect::MakeWH(static_cast<float>(targetWidth),
                                      static_cast<float>(targetHeight));
  result.rasterScaleX = rasterScaleX;
  result.rasterScaleY = rasterScaleY;
  result.targetWidth = targetWidth;
  result.targetHeight = targetHeight;
  return result;
}


bool ContainsComponentId(const std::vector<std::string> &ids,
                         const std::string &id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}


bool PageRenderGroupIncludesComponent(
    const text::TextEffectRenderGroupExecutionPlan &execution,
    const std::unordered_set<std::string> &afterEffectDecorations,
    const RenderComponent &component) {
  if (afterEffectDecorations.contains(component.id))
    return false;
  if (ContainsComponentId(execution.behindDecorationInputIds, component.id) ||
      ContainsComponentId(execution.frontDecorationInputIds, component.id)) {
    return true;
  }
  if (!execution.sourceInputIds.empty())
    return ContainsComponentId(execution.sourceInputIds, component.id);
  // The old Page RenderGroup receives the TextPro Letter source by default.
  // Backdrops remain in the outer composite unless the typed execution plan
  // names them explicitly; inline/scoped text decorations share the Letter
  // source domain and therefore use GlyphMaterial as their canonical scope.
  return component.kind == text::TextEffectCompositeItemKind::GlyphMaterial;
}


std::vector<RenderComponent> ResolvePageRenderGroupComponents(
    const PageRenderGroupDomain &domain,
    const text::TextEffectFramePlan &framePlan,
    const std::vector<RenderComponent> &components) {
  std::vector<RenderComponent> ordered;
  ordered.reserve(components.size());
  std::unordered_set<std::string> included;
  const auto append = [&](const std::string &id) {
    const auto found = std::find_if(
        components.begin(), components.end(),
        [&](const auto &component) { return component.id == id; });
    if (found == components.end() ||
        (!found->picture && !found->nativePageSource) ||
        !included.insert(found->id).second) {
      return;
    }
    auto copy = *found;
    copy.zOrder = 0;
    copy.authoredOrder = ordered.size();
    ordered.push_back(std::move(copy));
  };

  for (const auto &id : domain.execution.behindDecorationInputIds)
    append(id);

  std::unordered_set<std::string> afterEffectDecorations;
  for (const auto &pass : framePlan.decorationPasses) {
    if (pass.effectScope ==
        text::TextEffectDecorationEffectScope::AfterRenderGroupEffect) {
      afterEffectDecorations.insert(pass.passId);
    }
  }
  // An explicit sourceInputIds list is an authored topology contract.  Do
  // not recover its order by scanning the renderer's component vector (that
  // vector is assembled by material kind and can differ from descriptor
  // order); append the IDs exactly as declared, then append front decorations.
  if (!domain.execution.sourceInputIds.empty()) {
    for (const auto &id : domain.execution.sourceInputIds)
      append(id);
  } else {
    for (const auto &component : components) {
      if ((!component.picture && !component.nativePageSource) ||
          included.contains(component.id) ||
          ContainsComponentId(domain.execution.frontDecorationInputIds,
                              component.id)) {
        continue;
      }
      if (!PageRenderGroupIncludesComponent(
              domain.execution, afterEffectDecorations, component)) {
        continue;
      }
      append(component.id);
    }
  }
  for (const auto &id : domain.execution.frontDecorationInputIds)
    append(id);
  return ordered;
}


IndependentRenderGroupTopology BuildIndependentRenderGroupTopology(
    const ResolvedLayout &layout,
    const text::TextEffectRenderGroupExecutionPlan &execution) {
  IndependentRenderGroupTopology result;
  const auto unitCount = layout.units.size();
  auto begin = std::min(execution.fixedUnitRange.begin, unitCount);
  auto end = std::min(execution.fixedUnitRange.end, unitCount);
  // The evaluator leaves the range at {0,0} when a composition-scoped group
  // intentionally targets the whole layout.  Any other empty range is a
  // genuine empty target and must not leak the global glyph picture into the
  // independent group.
  if (begin == 0U && end == 0U &&
      execution.fixedGeometryBounds.width > 0.0F &&
      execution.fixedGeometryBounds.height > 0.0F) {
    begin = 0U;
    end = unitCount;
  }
  if (begin >= end)
    return result;
  result.orderedUnitIndexes.reserve(end - begin);
  for (std::size_t index = begin; index < end; ++index)
    result.orderedUnitIndexes.push_back(index);
  std::stable_sort(
      result.orderedUnitIndexes.begin(), result.orderedUnitIndexes.end(),
      [&](const std::size_t left, const std::size_t right) {
        const auto &lhs = layout.units[left];
        const auto &rhs = layout.units[right];
        const auto lhsLine = VisualLineForUnit(layout, lhs);
        const auto rhsLine = VisualLineForUnit(layout, rhs);
        if (lhsLine != rhsLine)
          return lhsLine < rhsLine;
        if (lhs.layoutBounds.left() != rhs.layoutBounds.left())
          return lhs.layoutBounds.left() < rhs.layoutBounds.left();
        if (lhs.documentUtf8Begin != rhs.documentUtf8Begin)
          return lhs.documentUtf8Begin < rhs.documentUtf8Begin;
        return left < right;
      });

  text::TextRenderGroupUnitTopology topology;
  topology.letterCount = result.orderedUnitIndexes.size();
  const auto appendRange =
      [](std::vector<text::TextRenderGroupIndexRange> &ranges,
         const std::size_t start, const std::size_t endIndex) {
        if (start < endIndex)
          ranges.push_back({static_cast<std::int64_t>(start),
                            static_cast<std::int64_t>(endIndex)});
      };
  std::size_t lineStart = 0U;
  std::size_t wordStart = 0U;
  for (std::size_t position = 1U;
       position <= result.orderedUnitIndexes.size(); ++position) {
    const bool atEnd = position == result.orderedUnitIndexes.size();
    const auto &previous =
        layout.units[result.orderedUnitIndexes[position - 1U]];
    if (atEnd ||
        VisualLineForUnit(layout, layout.units[result.orderedUnitIndexes[position]]) !=
            VisualLineForUnit(layout, previous)) {
      appendRange(topology.lineRanges, lineStart, position);
      lineStart = position;
    }
    if (atEnd) {
      appendRange(topology.wordRanges, wordStart, position);
      wordStart = position;
      continue;
    }
    const auto &current = layout.units[result.orderedUnitIndexes[position]];
    if (current.binding.paragraphId != previous.binding.paragraphId ||
        current.documentWordUtf8Begin != previous.documentWordUtf8Begin ||
        current.documentWordUtf8End != previous.documentWordUtf8End) {
      appendRange(topology.wordRanges, wordStart, position);
      wordStart = position;
    }
  }
  result.ranges = text::ResolveTextRenderGroupTopology(execution.spec, topology);
  return result;
}


std::vector<IndependentUnitSource> BuildIndependentUnitSources(
    const TextRenderFramePlan &renderPlan,
    const text::TextEffectRenderGroupExecutionPlan &execution,
    const ResolvedLayout &layout, const std::vector<RenderComponent> &components,
    const SkRect &recordingBounds) {
  const auto &framePlan = renderPlan.value();
  std::unordered_set<std::string> afterEffectDecorations;
  for (const auto &pass : framePlan.decorationPasses) {
    if (pass.effectScope ==
        text::TextEffectDecorationEffectScope::AfterRenderGroupEffect)
      afterEffectDecorations.insert(pass.passId);
  }
  std::vector<const RenderComponent *> selected;
  std::unordered_set<std::string> selectedIds;
  const auto appendSelected = [&](const std::string &id) {
    const auto found = std::find_if(
        components.begin(), components.end(),
        [&](const auto &component) { return component.id == id; });
    if (found == components.end() || !found->picture ||
        found->nativePageSource ||
        !selectedIds.insert(found->id).second)
      return;
    selected.push_back(&*found);
  };
  for (const auto &id : execution.behindDecorationInputIds)
    appendSelected(id);
  // Preserve the authored source-input order when it is explicit.  For the
  // legacy implicit contract, use the same component predicate as Page.
  if (!execution.sourceInputIds.empty()) {
    for (const auto &id : execution.sourceInputIds)
      appendSelected(id);
  } else {
    for (const auto &component : components) {
      if (!component.picture || component.nativePageSource ||
          !PageRenderGroupIncludesComponent(execution, afterEffectDecorations,
                                            component) ||
          selectedIds.contains(component.id))
        continue;
      appendSelected(component.id);
    }
  }
  for (const auto &id : execution.frontDecorationInputIds)
    appendSelected(id);

  std::vector<IndependentUnitSource> result(layout.units.size());
  for (std::size_t index = 0U; index < layout.units.size(); ++index) {
    const auto &unit = layout.units[index];
    auto &source = result[index];
    source.layoutIndex = index;
    source.stableUnitId = unit.binding.stableUnitId;
    source.fixedRenderGroupBounds =
        !unit.scriptBounds.isEmpty()
            ? unit.scriptBounds
            : (!unit.layoutBounds.isEmpty() ? unit.layoutBounds
                                             : unit.authoredBounds);
    source.geometryBounds = source.fixedRenderGroupBounds;
    source.visualBounds = source.geometryBounds;
    if (const auto *unitPlan = renderPlan.FindUnit(unit.binding.stableUnitId);
        unitPlan && !unitPlan->transforms.empty()) {
      if (const auto composed =
              text::ComposeTextEffectTransforms(unitPlan->transforms)) {
        source.visualBounds =
            MapEffectBounds(source.geometryBounds, *composed)
                .value_or(source.geometryBounds);
      }
    }
    if (source.geometryBounds.isEmpty() || source.visualBounds.isEmpty() ||
        !source.visualBounds.isFinite() || selected.empty())
      continue;
    source.visualBounds.outset(1.0F, 1.0F);
    SkPictureRecorder recorder;
    auto *canvas = recorder.beginRecording(recordingBounds);
    canvas->clipRect(source.visualBounds, SkClipOp::kIntersect, true);
    for (const auto *component : selected)
      canvas->drawPicture(component->picture);
    source.picture = recorder.finishRecordingAsPicture();
  }
  return result;
}


IndependentRangeSource BuildIndependentRangeSource(
    const std::vector<IndependentUnitSource> &units,
    const std::vector<std::size_t> &orderedUnitIndexes,
    const text::ResolvedTextRenderGroupRange &range,
    const SkRect &recordingBounds) {
  IndependentRangeSource result;
  const auto begin = std::min(range.startIndex, orderedUnitIndexes.size());
  const auto end = std::min(range.endIndex, orderedUnitIndexes.size());
  if (begin >= end)
    return result;
  SkPictureRecorder recorder;
  auto *canvas = recorder.beginRecording(recordingBounds);
  bool hasPicture = false;
  for (std::size_t position = begin; position < end; ++position) {
    const auto layoutIndex = orderedUnitIndexes[position];
    if (layoutIndex >= units.size())
      continue;
    const auto &unit = units[layoutIndex];
    result.fixedRenderGroupBounds.join(unit.fixedRenderGroupBounds);
    if (!unit.picture)
      continue;
    canvas->drawPicture(unit.picture);
    result.geometryBounds.join(unit.geometryBounds);
    result.visualBounds.join(unit.visualBounds);
    hasPicture = true;
  }
  if (hasPicture)
    result.picture = recorder.finishRecordingAsPicture();
  return result;
}


sk_sp<SkPicture> MakePageLocalPicture(
    const sk_sp<SkPicture> &source, const PageRenderGroupDomain &domain) {
  if (!source)
    return {};
  SkPictureRecorder recorder;
  auto *canvas = recorder.beginRecording(domain.localBounds);
  canvas->scale(domain.rasterScaleX, domain.rasterScaleY);
  canvas->translate(-domain.deviceTargetBounds.left(),
                    -domain.deviceTargetBounds.top());
  canvas->concat(domain.authoredToDevice);
  canvas->drawPicture(source);
  return recorder.finishRecordingAsPicture();
}


sk_sp<SkPicture> PromotePageLocalPictureToPresentation(
    const sk_sp<SkPicture> &source, const PageRenderGroupDomain &domain,
    const SkRect &presentationBounds) {
  if (!source || presentationBounds.isEmpty() ||
      domain.deviceTargetBounds.isEmpty() ||
      !std::isfinite(domain.rasterScaleX) ||
      !std::isfinite(domain.rasterScaleY) || domain.rasterScaleX <= 0.0F ||
      domain.rasterScaleY <= 0.0F) {
    return {};
  }
  SkPictureRecorder recorder;
  auto *canvas = recorder.beginRecording(presentationBounds);
  canvas->translate(domain.deviceTargetBounds.left(),
                    domain.deviceTargetBounds.top());
  canvas->scale(1.0F / domain.rasterScaleX,
                1.0F / domain.rasterScaleY);
  canvas->drawPicture(source);
  return recorder.finishRecordingAsPicture();
}


std::vector<RenderComponent> MakePageLocalComponents(
    const std::vector<RenderComponent> &components,
    const PageRenderGroupDomain &domain, SkiaGpuContext *gpuContext,
    std::string &error) {
  std::vector<RenderComponent> result;
  result.reserve(components.size());
  for (const auto &component : components) {
    auto copy = component;
    if (component.nativePageSource &&
        std::getenv("VIDEOCUT_DISABLE_PAGE_NATIVE") == nullptr) {
      copy.picture = RenderQtTextNativePageSource(
          *component.nativePageSource, domain, nullptr, gpuContext, error);
      if (!copy.picture) {
        // Keep the component's presentation picture as a compatibility
        // source when a device rejects the deferred Letter packet.  Page
        // execution must remain source-preserving; a native packet failure is
        // not permission to publish an empty render group.
        error.clear();
        copy.nativePageSource.reset();
        copy.picture = MakePageLocalPicture(component.picture, domain);
      }
    } else {
      copy.picture = MakePageLocalPicture(component.picture, domain);
    }
    if (copy.picture)
      result.push_back(std::move(copy));
  }
  return result;
}


std::optional<QtTextRenderGroupCompositeRequest>
BuildQtTextRenderGroupCompositeRequest(
    const PageRenderGroupDomain &domain, const int presentationWidth,
    const int presentationHeight, const float referenceWidth,
    const float referenceHeight) {
  if (domain.deviceTargetBounds.isEmpty() || domain.targetWidth <= 0 ||
      domain.targetHeight <= 0 || presentationWidth <= 0 ||
      presentationHeight <= 0 || !std::isfinite(referenceWidth) ||
      !std::isfinite(referenceHeight) || referenceWidth <= 0.0F ||
      referenceHeight <= 0.0F) {
    return std::nullopt;
  }
  constexpr float kTextProVertexCoordinateExtent = 576.0F;
  QtTextRenderGroupCompositeRequest request;
  request.presentationWidth = presentationWidth;
  request.presentationHeight = presentationHeight;
  request.mvp.fill(0.0F);
  request.customMatrix.fill(0.0F);
  request.mvp[0] =
      2.0F * kTextProVertexCoordinateExtent / referenceWidth;
  request.mvp[5] =
      2.0F * kTextProVertexCoordinateExtent / referenceHeight;
  request.mvp[10] = -1.0F / 550.0F;
  request.mvp[14] = -0.8F;
  request.mvp[15] = 1.0F;
  const float projectionWidth =
      static_cast<float>(presentationWidth) * request.mvp[0];
  const float projectionHeight =
      static_cast<float>(presentationHeight) * request.mvp[5];
  request.customMatrix[0] =
      static_cast<float>(domain.targetWidth) / projectionWidth;
  request.customMatrix[5] =
      static_cast<float>(domain.targetHeight) / projectionHeight;
  request.customMatrix[10] = 1.0F;
  request.customMatrix[12] =
      ((domain.deviceTargetBounds.left() +
        static_cast<float>(domain.targetWidth) * 0.5F) -
       static_cast<float>(presentationWidth) * 0.5F) *
      2.0F / projectionWidth;
  request.customMatrix[13] =
      (static_cast<float>(presentationHeight) * 0.5F -
       (domain.deviceTargetBounds.top() +
        static_cast<float>(domain.targetHeight) * 0.5F)) *
      2.0F / projectionHeight;
  request.customMatrix[15] = 1.0F;
  request.alpha = 1.0F;
  return request;
}


MaterializedPageRenderGroup MaterializePageRenderGroup(
    const sk_sp<SkPicture> &source, const PageRenderGroupDomain &domain,
    SkiaGpuContext &gpuContext, std::string &error) {
  MaterializedPageRenderGroup result;
  const auto info = SkImageInfo::Make(
      domain.targetWidth, domain.targetHeight, kRGBA_8888_SkColorType,
      kPremul_SkAlphaType, nullptr);
  auto surface = gpuContext.MakeSurface(info, error);
  if (!surface)
    return result;
  surface->getCanvas()->clear(SK_ColorTRANSPARENT);
  surface->getCanvas()->drawPicture(source);
  result.pageImage = surface->makeImageSnapshot();
  if (!result.pageImage) {
    error = "text RenderGroup Page snapshot failed";
    return result;
  }
  SkPictureRecorder textureRecorder;
  auto *textureCanvas = textureRecorder.beginRecording(domain.localBounds);
  textureCanvas->drawImageRect(result.pageImage, domain.localBounds,
                               SkSamplingOptions(SkFilterMode::kLinear),
                               nullptr);
  auto texturePicture = textureRecorder.finishRecordingAsPicture();
  if (!texturePicture) {
    error = "text RenderGroup Page texture recording failed";
    return result;
  }
  const auto authoredTargetBounds =
      MapRect(domain.deviceToAuthored, domain.deviceTargetBounds);
  SkPictureRecorder wrapperRecorder;
  auto *wrapper = wrapperRecorder.beginRecording(authoredTargetBounds);
  wrapper->concat(domain.deviceToAuthored);
  wrapper->translate(domain.deviceTargetBounds.left(),
                     domain.deviceTargetBounds.top());
  wrapper->scale(1.0F / domain.rasterScaleX,
                 1.0F / domain.rasterScaleY);
  wrapper->drawPicture(texturePicture);
  result.authoredPicture = wrapperRecorder.finishRecordingAsPicture();
  return result;
}


const RenderComponent *FindComponent(
    const std::vector<RenderComponent> &components,
    const std::string &id,
    const text::TextEffectCompositeItemKind kind) noexcept {
  const auto found = std::find_if(
      components.begin(), components.end(), [&](const auto &component) {
        return component.id == id && component.kind == kind;
      });
  return found == components.end() ? nullptr : &*found;
}


std::uint64_t StableNodeIdentity(const std::string &nodeId) noexcept {
  IdentityBuilder identity;
  identity.AddString(nodeId);
  auto result = identity.value();
  return result == 0U ? 1U : result;
}


bool ExecutionGraphRequiresStablePageDomain(
    const std::vector<text::TextEffectExecutionNodeFramePlan> &nodes) {
  for (const auto &node : nodes) {
    if (node.kind == text::TextEffectExecutionNodeKind::History ||
        (node.kind == text::TextEffectExecutionNodeKind::PostEffectPass &&
         node.postEffectKind == text::TextPostEffectKind::Trail) ||
        ExecutionGraphRequiresStablePageDomain(node.children)) {
      return true;
    }
  }
  return false;
}


bool ExecutionGraphContainsMediaParticle(
    const std::vector<text::TextEffectExecutionNodeFramePlan> &nodes) {
  for (const auto &node : nodes) {
    if ((node.kind == text::TextEffectExecutionNodeKind::MediaInput &&
         node.capability ==
             text::TextEffectExecutionCapability::MediaParticle) ||
        ExecutionGraphContainsMediaParticle(node.children)) {
      return true;
    }
  }
  return false;
}


bool FramePlanRequiresStablePageDomain(
    const text::TextEffectFramePlan &framePlan) {
  if (std::any_of(framePlan.postEffectNodes.begin(),
                  framePlan.postEffectNodes.end(), [](const auto &node) {
                    return node.kind == text::TextPostEffectKind::Trail;
                  })) {
    return true;
  }
  return ExecutionGraphRequiresStablePageDomain(
      framePlan.executionGraph.nodes);
}


bool ExecutionGraphUsesCaptionCanvas(
    const std::vector<text::TextEffectExecutionNodeFramePlan> &nodes) {
  return std::any_of(nodes.begin(), nodes.end(), [](const auto &node) {
    return (node.kind == text::TextEffectExecutionNodeKind::Layout &&
            node.capability ==
                text::TextEffectExecutionCapability::LayoutCaptionModule) ||
           ExecutionGraphUsesCaptionCanvas(node.children);
  });
}


std::optional<PageRenderGroupDomain>
ResolveExecutionGraphPageRenderGroupDomain(
    const text::TextEffectFramePlan &framePlan,
    const text::TextAnimationStack &animations,
    const SkMatrix &presentationMatrix,
    const SkRect &fixedPageLetterBounds) {
  if (auto domain = ResolvePageRenderGroupDomain(
          framePlan, presentationMatrix, fixedPageLetterBounds)) {
    return domain;
  }
  if (framePlan.executionGraph.nodes.empty())
    return std::nullopt;

  const text::TextAnimationLayerSpec *pageLayer = nullptr;
  for (const auto &layer : animations.layers) {
    if (!layer.enabled || !layer.renderGroup ||
        layer.renderGroup->mode != text::TextRenderGroupMode::Page) {
      continue;
    }
    // An execution graph has one closed Page attachment. Multiple authored
    // Page layers require an explicit FramePlan render-group association and
    // must not be merged by inference.
    if (pageLayer)
      return std::nullopt;
    pageLayer = &layer;
  }
  if (!pageLayer || fixedPageLetterBounds.isEmpty() ||
      !fixedPageLetterBounds.isFinite()) {
    return std::nullopt;
  }

  text::TextEffectRenderGroupExecutionPlan execution;
  execution.layerId = pageLayer->layerId;
  execution.renderGroupInstanceId = StableNodeIdentity(
      "execution-page-render-group:" + pageLayer->layerId);
  execution.spec = *pageLayer->renderGroup;
  execution.fixedGeometryBounds = {
      fixedPageLetterBounds.x(), fixedPageLetterBounds.y(),
      fixedPageLetterBounds.width(), fixedPageLetterBounds.height()};
  execution.visualSourceBounds = execution.fixedGeometryBounds;

  text::TextEffectFramePlan carrier;
  text::TextEffectPostEffectNode node;
  node.nodeId = "execution-page-render-group";
  node.layerId = pageLayer->layerId;
  node.renderGroup = std::move(execution);
  carrier.postEffectNodes.push_back(std::move(node));
  return ResolvePageRenderGroupDomain(
      carrier, presentationMatrix, fixedPageLetterBounds);
}


const text::TextEffectExecutionNodeFramePlan *
ResolvePresentationSceneMaterial(
    const std::vector<text::TextEffectExecutionNodeFramePlan> &nodes) {
  using Kind = text::TextEffectExecutionNodeKind;
  using Capability = text::TextEffectExecutionCapability;
  std::unordered_map<
      std::string, const text::TextEffectExecutionNodeFramePlan *>
      indexed;
  const auto collect = [&](const auto &self, const auto &items) -> bool {
    for (const auto &node : items) {
      if (node.nodeId.empty() || !indexed.emplace(node.nodeId, &node).second ||
          !self(self, node.children)) {
        return false;
      }
    }
    return true;
  };
  if (!collect(collect, nodes))
    return nullptr;

  const text::TextEffectExecutionNodeFramePlan *material = nullptr;
  for (const auto &[id, node] : indexed) {
    static_cast<void>(id);
    if (node->kind != Kind::MaterialPass ||
        node->capability != Capability::MaterialVatRbdMesh) {
      continue;
    }
    if (material || node->inputIds.size() != 1U)
      return nullptr;
    const auto producer = indexed.find(node->inputIds.front());
    if (producer == indexed.end() || producer->second->kind != Kind::Scene ||
        producer->second->capability != Capability::SceneProjection) {
      return nullptr;
    }
    material = node;
  }
  if (!material)
    return nullptr;

  const text::TextEffectExecutionNodeFramePlan *terminalComposite = nullptr;
  for (const auto &[id, node] : indexed) {
    static_cast<void>(id);
    if (std::find(node->inputIds.begin(), node->inputIds.end(),
                  material->nodeId) == node->inputIds.end()) {
      continue;
    }
    if (terminalComposite || node->kind != Kind::Composite ||
        node->capability != Capability::CompositeSourceOver ||
        node->inputIds.size() != 1U) {
      return nullptr;
    }
    terminalComposite = node;
  }
  if (!terminalComposite)
    return nullptr;
  for (const auto &[id, node] : indexed) {
    static_cast<void>(id);
    if (std::find(node->inputIds.begin(), node->inputIds.end(),
                  terminalComposite->nodeId) != node->inputIds.end()) {
      return nullptr;
    }
  }
  return material;
}


const text::TextEffectExecutionNodeFramePlan *
FindSdfSourceCanvasMaterial(
    const std::vector<text::TextEffectExecutionNodeFramePlan> &nodes,
    std::string &error) {
  error.clear();
  const text::TextEffectExecutionNodeFramePlan *explicitCanvas = nullptr;
  const text::TextEffectExecutionNodeFramePlan *legacyCanvas = nullptr;
  const text::TextEffectExecutionParameterSample *canvasParameter = nullptr;
  const auto visit = [&](const auto &self, const auto &children) -> bool {
    for (const auto &node : children) {
      if (node.kind == text::TextEffectExecutionNodeKind::MaterialPass) {
        for (const auto &parameter : node.parameters) {
          if (parameter.slot != kTextExecutionSourceCanvasSlot)
            continue;
          if (!ValidateClosedExecutionMaterialParameters(node, error))
            return false;
          if (canvasParameter && canvasParameter->values != parameter.values) {
            error = "text execution graph declares conflicting Page source canvases: " +
                    explicitCanvas->nodeId + " / " + node.nodeId;
            return false;
          }
          explicitCanvas = &node;
          canvasParameter = &parameter;
        }
      }
      if (!legacyCanvas &&
          (node.capability == text::TextEffectExecutionCapability::MaterialDirectionalBoxBlur ||
           node.capability == text::TextEffectExecutionCapability::MaterialCubeProjectionComposite) &&
          std::any_of(node.parameters.begin(), node.parameters.end(),
                      [](const auto &parameter) {
                        return parameter.parameter ==
                                   text::TextEffectExecutionParameterKind::MaterialVector2 &&
                               parameter.slot == 4U;
                      })) {
        legacyCanvas = &node;
      }
      if (!self(self, node.children))
        return false;
    }
    return true;
  };
  if (!visit(visit, nodes)) return nullptr;
  return explicitCanvas ? explicitCanvas : legacyCanvas;
}


std::optional<PageRenderGroupDomain> ResolveSdfSourceAttachmentDomain(
    const text::TextEffectExecutionNodeFramePlan &material,
    const float referenceWidth, const float referenceHeight,
    const float referenceGuard,
    const text::TextEffectFramePlan &framePlan,
    const text::TextAnimationStack &animations,
    const SkMatrix &presentationMatrix) {
  using ParameterKind = text::TextEffectExecutionParameterKind;
  using ParameterDomain = text::TextEffectExecutionParameterDomain;
  using ParameterSpace = text::TextEffectExecutionParameterSpace;
  const auto *explicitCanvas = ConsumeTextExecutionParameter(
      material, ParameterKind::MaterialVector4,
      kTextExecutionSourceCanvasSlot);
  const auto parameter = std::find_if(
      material.parameters.begin(), material.parameters.end(),
      [](const auto &sample) {
        return sample.parameter == ParameterKind::MaterialVector2 &&
               sample.slot == 4U &&
               sample.domain == ParameterDomain::Page &&
               sample.valueSpace == ParameterSpace::ReferencePixels &&
               !sample.stableUnitId && sample.values.size() == 2U;
      });
  const float canvasWidth = explicitCanvas ? explicitCanvas->values[0]
      : parameter != material.parameters.end() ? parameter->values[0] : 0.0F;
  const float canvasHeight = explicitCanvas ? explicitCanvas->values[1]
      : parameter != material.parameters.end() ? parameter->values[1] : 0.0F;
  const float extraWidth = explicitCanvas ? explicitCanvas->values[2] : 0.0F;
  const float extraHeight = explicitCanvas ? explicitCanvas->values[3] : 0.0F;
  if ((!explicitCanvas && parameter == material.parameters.end()) ||
      !std::isfinite(referenceWidth) || !std::isfinite(referenceHeight) ||
      referenceWidth <= 0.0F || referenceHeight <= 0.0F ||
      !std::isfinite(referenceGuard) || referenceGuard < 0.0F ||
      !std::isfinite(canvasWidth) || !std::isfinite(canvasHeight) ||
      canvasWidth <= 0.0F || canvasHeight <= 0.0F ||
      !std::isfinite(extraWidth) || !std::isfinite(extraHeight) ||
      extraWidth < 0.0F || extraHeight < 0.0F) {
    return std::nullopt;
  }

  // The source SDF canvas and its font-em guard are rasterized through the
  // text entity's presentation transform before the material samples _MainTex.
  // Source-design pixels alone omit that transform (e.g. 720 -> 768), while
  // textRect includes the wrapping box rather than the actual canvas.
  const auto guardedCanvas = SkRect::MakeXYWH(
      (referenceWidth - canvasWidth - extraWidth) * 0.5F - referenceGuard,
      (referenceHeight - canvasHeight - extraHeight) * 0.5F - referenceGuard,
      canvasWidth + extraWidth + referenceGuard * 2.0F,
      canvasHeight + extraHeight + referenceGuard * 2.0F);
  const auto deviceCanvas = presentationMatrix.mapRect(guardedCanvas);
  if (!guardedCanvas.isFinite() || !deviceCanvas.isFinite() ||
      deviceCanvas.isEmpty() || deviceCanvas.width() > 4096.0F ||
      deviceCanvas.height() > 4096.0F) {
    return std::nullopt;
  }
  auto domain = ResolveExecutionGraphPageRenderGroupDomain(
      framePlan, animations, presentationMatrix, guardedCanvas);
  // The explicit canvas includes targetRTExtraSize already. A second ratio
  // expansion would change _MainTex's UV-to-glyph mapping.
  if (explicitCanvas && domain &&
      (domain->execution.spec.expandRatioX != 1.0F ||
       domain->execution.spec.expandRatioY != 1.0F)) {
    return std::nullopt;
  }
  if (domain && !explicitCanvas && parameter != material.parameters.end())
    RecordTextExecutionParameterConsumption(material, *parameter);
  return domain;
}


bool BuildTextEffectExecutionEvaluationInputs(
    const text::TextEffectExecutionGraph &graph,
    const text::TextRenderRequest &request,
    const std::uint64_t graphInstanceId,
    text::EffectRuntimeClockStore &clockStore,
    const std::unordered_map<std::string, TextExecutionGraphHistoryState>
        &histories,
    std::vector<text::TextEffectEvaluationStateInput> &stateInputs,
    std::vector<text::TextEffectEvaluationHistoryInput> &historyInputs,
    std::string &error) {
  stateInputs.clear();
  historyInputs.clear();
  error.clear();
  std::vector<std::string> stateIds;
  std::vector<std::string> historyIds;
  std::unordered_set<std::string> uniqueStateIds;
  std::unordered_set<std::string> uniqueHistoryIds;
  const auto collect = [&](const auto &self,
                           const std::vector<text::TextEffectExecutionNode>
                               &nodes) -> void {
    for (const auto &node : nodes) {
      if (!node.stateId.empty() && uniqueStateIds.insert(node.stateId).second)
        stateIds.push_back(node.stateId);
      if (!node.historyId.empty() &&
          uniqueHistoryIds.insert(node.historyId).second)
        historyIds.push_back(node.historyId);
      self(self, node.children);
    }
  };
  collect(collect, graph.nodes);

  stateInputs.reserve(stateIds.size());
  for (const auto &stateId : stateIds) {
    text::TextEffectEvaluationStateInput input;
    input.stateId = stateId;
    input.randomSeed = StableNodeIdentity(stateId);
    if (!request.effectRuntimeClock) {
      input.elapsedUs = std::max<std::int64_t>(0, request.localTimeUs);
      input.revision = static_cast<std::uint64_t>(input.elapsedUs) + 1U;
      stateInputs.push_back(std::move(input));
      continue;
    }

    const auto &control = *request.effectRuntimeClock;
    const text::EffectRuntimeClockIdentity identity{
        graphInstanceId, StableNodeIdentity(stateId),
        StableNodeIdentity("text-execution-graph-state")};
    text::EffectRuntimeClockFrame frame;
    frame.event = control.event;
    frame.lifecycleEpoch = control.lifecycleEpoch;
    frame.timelineTimeUs = request.localTimeUs;
    frame.advanceUs = control.advanceUs;
    if (control.event == text::EffectRuntimeClockEvent::RestoreCheckpoint) {
      if (!control.restoreElapsedUs || control.restoreStateRevision == 0U) {
        error = "text execution graph RestoreCheckpoint has no elapsed state";
        return false;
      }
      const text::EffectRuntimeClockCacheKey cacheKey{
          identity, control.lifecycleEpoch, request.localTimeUs,
          *control.restoreElapsedUs, control.restoreStateRevision};
      frame.checkpoint = text::EffectRuntimeClockCheckpoint{
          identity, control.lifecycleEpoch, request.localTimeUs,
          *control.restoreElapsedUs, control.restoreStateRevision,
          text::BuildEffectRuntimeClockCacheRevision(cacheKey)};
    } else if (control.restoreElapsedUs) {
      error = "text execution graph restore state accompanied a non-restore "
              "event";
      return false;
    }
    auto applied = clockStore.Apply(identity, frame);
    if (!applied &&
        applied.status == text::EffectRuntimeClockStatus::MissingState &&
        control.event != text::EffectRuntimeClockEvent::Begin &&
        control.event != text::EffectRuntimeClockEvent::RestoreCheckpoint) {
      text::EffectRuntimeClockFrame begin;
      begin.event = text::EffectRuntimeClockEvent::Begin;
      begin.lifecycleEpoch = control.lifecycleEpoch;
      begin.timelineTimeUs = request.localTimeUs;
      const auto begun = clockStore.Apply(identity, begin);
      if (begun)
        applied = clockStore.Apply(identity, frame);
    }
    if (!applied) {
      error = "text execution graph runtime clock rejected " +
              std::string(text::EffectRuntimeClockStatusName(applied.status));
      if (!applied.error.empty())
        error += ": " + applied.error;
      return false;
    }
    input.elapsedUs = applied.sample.elapsedUs;
    input.revision = applied.sample.cacheRevision;
    if (input.revision == 0U)
      input.revision = 1U;
    stateInputs.push_back(std::move(input));
  }

  historyInputs.reserve(historyIds.size());
  for (const auto &historyId : historyIds) {
    const auto found = histories.find(historyId);
    const bool forwardSample =
        found != histories.end() && found->second.picture &&
        request.localTimeUs > found->second.lastTimeUs;
    historyInputs.push_back(
        {historyId, forwardSample ? found->second.revision : 0U});
  }
  return true;
}


double ResolvePostEffectProgress(const text::TextEffectFramePlan &framePlan,
                                 const std::string &nodeId,
                                 const std::int64_t localTimeUs,
                                 const std::int64_t durationUs) noexcept {
  const auto transition = std::find_if(
      framePlan.stateTransitions.begin(), framePlan.stateTransitions.end(),
      [&](const auto &state) { return state.stateId == nodeId; });
  if (transition != framePlan.stateTransitions.end())
    return transition->progress;
  if (durationUs <= 0)
    return 0.0;
  return std::clamp(static_cast<double>(localTimeUs) /
                        static_cast<double>(durationUs),
                    0.0, 1.0);
}


bool PostEffectConsumesRuntimeClock(
    const text::TextPostEffectKind kind) noexcept {
  return kind == text::TextPostEffectKind::Shake ||
         kind == text::TextPostEffectKind::Trail;
}


bool BindPostEffectRuntimeClock(
    const text::TextRenderRequest &request,
    const text::TextEffectPostEffectNode &node,
    text::EffectRuntimeClockStore &clockStore,
    QtTextPostEffectStateContext &context, std::string &error) {
  error.clear();
  if (!request.effectRuntimeClock ||
      !PostEffectConsumesRuntimeClock(node.kind)) {
    return true;
  }
  if (context.renderGraphInstanceId == 0U ||
      context.effectNodeInstanceId == 0U ||
      context.renderGroupInstanceId == 0U) {
    error = "stateful text effect has no runtime-clock owner identity";
    return false;
  }
  const auto &control = *request.effectRuntimeClock;
  const text::EffectRuntimeClockIdentity identity{
      context.renderGraphInstanceId, context.effectNodeInstanceId,
      context.renderGroupInstanceId};
  text::EffectRuntimeClockFrame frame;
  frame.event = control.event;
  frame.lifecycleEpoch = control.lifecycleEpoch;
  frame.timelineTimeUs = request.localTimeUs;
  frame.advanceUs = control.advanceUs;
  if (control.event == text::EffectRuntimeClockEvent::RestoreCheckpoint) {
    if (!control.restoreElapsedUs || control.restoreStateRevision == 0U) {
      error = "text effect RestoreCheckpoint has no elapsed state";
      return false;
    }
    const text::EffectRuntimeClockCacheKey cacheKey{
        identity, control.lifecycleEpoch, request.localTimeUs,
        *control.restoreElapsedUs, control.restoreStateRevision};
    frame.checkpoint = text::EffectRuntimeClockCheckpoint{
        identity,
        control.lifecycleEpoch,
        request.localTimeUs,
        *control.restoreElapsedUs,
        control.restoreStateRevision,
        text::BuildEffectRuntimeClockCacheRevision(cacheKey)};
  } else if (control.restoreElapsedUs) {
    error = "text effect runtime restore state accompanied a non-restore event";
    return false;
  }

  auto applied = clockStore.Apply(identity, frame);
  if (!applied &&
      applied.status == text::EffectRuntimeClockStatus::MissingState &&
      control.event != text::EffectRuntimeClockEvent::Begin &&
      control.event != text::EffectRuntimeClockEvent::RestoreCheckpoint) {
    text::EffectRuntimeClockFrame begin;
    begin.event = text::EffectRuntimeClockEvent::Begin;
    begin.lifecycleEpoch = control.lifecycleEpoch;
    begin.timelineTimeUs = request.localTimeUs;
    const auto begun = clockStore.Apply(identity, begin);
    if (begun)
      applied = clockStore.Apply(identity, frame);
  }
  if (!applied) {
    error = "text effect runtime clock rejected " +
            std::string(text::EffectRuntimeClockStatusName(applied.status));
    if (!applied.error.empty())
      error += ": " + applied.error;
    return false;
  }
  context.lifecycleEpoch = applied.sample.lifecycleEpoch;
  context.effectTimeSeconds =
      static_cast<double>(applied.sample.elapsedUs) / 1'000'000.0;
  context.effectRuntimeClockRevision = applied.sample.cacheRevision;
  return true;
}


sk_sp<SkPicture> ResolveNodeInputPicture(
    const text::TextEffectPostEffectNode &node,
    const std::vector<RenderComponent> &components,
    const std::unordered_map<std::string, sk_sp<SkPicture>> &postOutputs,
    const sk_sp<SkPicture> &current, const SkRect &recordingBounds) {
  if (node.inputIds.empty())
    return current;
  if (node.inputIds.size() == 1U) {
    const auto post = postOutputs.find(node.inputIds.front());
    if (post != postOutputs.end() && post->second)
      return post->second;
  }
  std::vector<RenderComponent> inputs;
  std::size_t order = 0U;
  for (const auto &inputId : node.inputIds) {
    const auto post = postOutputs.find(inputId);
    if (post != postOutputs.end()) {
      inputs.push_back({inputId,
                        text::TextEffectCompositeItemKind::PostEffect,
                        0,
                        text::TextBlendMode::SourceOver,
                        post->second,
                        {},
                        order++});
      continue;
    }
    const auto found = std::find_if(
        components.begin(), components.end(), [&](const auto &component) {
          return component.id == inputId;
        });
    if (found != components.end()) {
      auto copy = *found;
      copy.authoredOrder = order++;
      inputs.push_back(std::move(copy));
    }
  }
  return inputs.empty() ? current : ComposePictures(inputs, recordingBounds);
}


sk_sp<SkPicture> MixRenderGroupIntensity(const sk_sp<SkPicture> &source,
                                         const sk_sp<SkPicture> &effected,
                                         const SkRect &recordingBounds,
                                         const float authoredIntensity) {
  if (!source || !effected)
    return effected ? effected : source;
  const float intensity = std::clamp(authoredIntensity, 0.0F, 1.0F);
  if (intensity <= 0.0F)
    return source;
  if (intensity >= 1.0F)
    return effected;
  SkPictureRecorder recorder;
  auto *canvas = recorder.beginRecording(recordingBounds);
  SkPaint sourcePaint;
  sourcePaint.setAlphaf(1.0F - intensity);
  canvas->drawPicture(source, nullptr, &sourcePaint);
  SkPaint effectPaint;
  effectPaint.setAlphaf(intensity);
  effectPaint.setBlendMode(SkBlendMode::kPlus);
  canvas->drawPicture(effected, nullptr, &effectPaint);
  auto mixed = recorder.finishRecordingAsPicture();
  return mixed ? mixed : effected;
}


void RecordPostEffectExecution(
    text::TextExecutionEvidence *evidence, const std::string &nodeId,
    const QtTextPostEffectStateContext &stateContext,
    const QtTextProPostEffectResult &result) {
  if (!evidence)
    return;
  text::TextPostEffectExecutionEvidence invocation;
  invocation.nodeId = nodeId;
  invocation.renderGroupInstanceId = stateContext.renderGroupInstanceId;
  invocation.plannedPassCount = result.plan.passes.size();
  invocation.pageWidth = result.execution.pageWidth;
  invocation.pageHeight = result.execution.pageHeight;
  invocation.topologyValidated = static_cast<bool>(result);
  invocation.error = result.error;
  invocation.parameters = result.parameters;
  invocation.passes.reserve(result.execution.passes.size());
  for (const auto &pass : result.execution.passes) {
    invocation.passes.push_back(
        {pass.chain, pass.stage, pass.ordinal, pass.passCount,
         pass.width, pass.height,
         static_cast<std::uint32_t>(pass.surfaceSemantics),
         pass.executor, pass.executorVersion, pass.pixelFormat});
  }
  evidence->postEffects.push_back(std::move(invocation));
}

} // namespace videocut::skia_runtime::internal::text_lane
