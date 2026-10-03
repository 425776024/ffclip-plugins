#pragma once
json importProjection(const std::filesystem::path &path) {
  const auto projectId =
      take(infra::ProjectFormatStore::PersistedProjectIdForPackage(path));
  auto store =
      openStore(path, projectId, infra::ProjectFormatOpenMode::OpenExisting);
  auto published = take(store->ReadPublished());
  if (!published)
    throw std::runtime_error("No published project HEAD");
  const auto &parts = published->document->partitions();
  const auto &timeline = parts.timeline->partitions();
  const auto &content = parts.contentAssetJob->partitions();
  const auto rateOf = [&](const d::TimelineClip &clip) -> std::int64_t {
    if (!clip.retime)
      return 1000000;
    const d::RetimeDocument *owner = nullptr;
    for (const auto &candidate : timeline.retimes)
      if (candidate->id() == clip.retime->id)
        owner = candidate.get();
    if (!owner)
      throw std::runtime_error("Missing native retime");
    const auto authored =
        take(d::ReadCanonicalAuthoredRetime(*owner, {owner->id().value()}));
    if (authored.mode != videocut::contract::TimeWarpMode::Constant ||
        authored.videoInterpolation !=
            videocut::contract::VideoInterpolationMode::Hold ||
        !authored.curveTemplateId.empty() || authored.provenance ||
        authored.restriction)
      throw std::runtime_error("Native curve speed, restricted retime or "
                               "optical flow cannot be represented in browser");
    return authored.constantRatePpm;
  };
  if (parts.metadata.masterVisualEffects || parts.metadata.masterAudioEffects ||
      parts.metadata.audio.sampleRate != 48000 ||
      parts.metadata.audio.channels != 2 ||
      parts.metadata.audio.channelLayout != 3 ||
      parts.metadata.audio.defaultCodec != "aac" ||
      parts.metadata.defaultVideoCodec != "h264" || parts.metadata.draft ||
      parts.metadata.canvas.pixelAspect.numerator() !=
          parts.metadata.canvas.pixelAspect.denominator() ||
      parts.metadata.canvas.exportFrameRate.numerator() !=
          parts.metadata.canvas.editingFrameRate.numerator() ||
      parts.metadata.canvas.exportFrameRate.denominator() !=
          parts.metadata.canvas.editingFrameRate.denominator())
    throw std::runtime_error("Native master effects or audio format cannot be "
                             "represented in browser");
  if (!parts.effectMask->partitions().maskStacks.empty())
    throw std::runtime_error("Native masks cannot be represented in browser");
  if (!timeline.markers.empty())
    throw std::runtime_error(
        "Native markers cannot be represented in browser yet");
  std::map<std::string, const d::TimelineItem *> items;
  std::map<std::string, const d::TimelineTrack *> itemTracks;
  for (const auto &track : timeline.tracks)
    for (const auto &item : track->items) {
      items[item->id.value()] = item.get();
      itemTracks[item->id.value()] = track.get();
    }
  std::map<std::string, const d::TimelineItem *> companions;
  std::set<std::string> companionIds;
  for (const auto &link : timeline.links)
    if (link.kind == d::LinkKind::EmbeddedAudioVideo) {
      if (link.items.size() != 2)
        throw std::runtime_error("Invalid native audio/video link");
      const auto *a = items.at(link.items[0].value()),
                 *b = items.at(link.items[1].value());
      if (a->clip->type == d::ClipType::Audio)
        std::swap(a, b);
      if (a->clip->type != d::ClipType::Video ||
          b->clip->type != d::ClipType::Audio ||
          a->placement.begin.value() != b->placement.begin.value() ||
          a->placement.end.value() != b->placement.end.value() ||
          a->clip->source.begin.value() != b->clip->source.begin.value() ||
          a->clip->source.end.value() != b->clip->source.end.value() ||
          rateOf(*a->clip) != rateOf(*b->clip))
        throw std::runtime_error("Independent native audio/video edits require "
                                 "separate browser tracks");
      const auto *videoTrack = itemTracks.at(a->id.value()),
                 *audioTrack = itemTracks.at(b->id.value());
      if (a->enabled != b->enabled || videoTrack->muted != audioTrack->muted ||
          videoTrack->locked != audioTrack->locked ||
          videoTrack->syncLocked != audioTrack->syncLocked || b->clip->effectGraph ||
          !b->clip->automation.empty())
        throw std::runtime_error(
            "Native audio companion has independent authored state that cannot "
            "be merged without loss");
      companions[a->id.value()] = b;
      companionIds.insert(b->id.value());
    }
  auto locatorPort = take(infra::ProjectFormatAssetLocatorPort::Open(path));
  std::map<std::string, std::string> locators;
  for (const auto &locator : take(locatorPort->ReadAssetLocators()))
    locators[locator.asset.value()] = locator.platformLocator;
  json result = {
      {"format", "videocut.edit-session"},
      {"version", 1},
      {"id", projectId.value()},
      {"name", parts.metadata.displayName},
      {"canvas",
       {{"width", parts.metadata.canvas.width},
        {"height", parts.metadata.canvas.height}}},
      {"frameRate",
       {{"numerator", parts.metadata.canvas.editingFrameRate.numerator()},
        {"denominator", parts.metadata.canvas.editingFrameRate.denominator()}}},
      {"audio", {{"sampleRate", 48000}, {"channels", 2}}},
      {"assets", json::array()},
      {"timeline",
       {{"id", timeline.id.value()},
        {"tracks", json::array()},
        {"groups", json::array()},
        {"links", json::array()},
        {"transitions",
         importTransitions(*parts.timeline, *parts.effectMask)}}}};
  std::set<std::string> usedAssets, videoAudioAssets;
  for (const auto &[itemId, item] : items)
    if (!companionIds.count(itemId) &&
        std::holds_alternative<AssetId>(item->clip->content)) {
      const auto asset = std::get<AssetId>(item->clip->content).value();
      usedAssets.insert(asset);
      if (companions.count(itemId))
        videoAudioAssets.insert(asset);
    }
  for (const auto &asset : content.assets)
    if (asset.currentFacts) {
      bool companion = false, embeddedAudio = false;
      for (const auto &metadata : asset.currentFacts->metadata) {
        if (metadata.key == "videocut.media.embedded_video_asset")
          companion = true;
        if (metadata.key == "videocut.media.embedded_audio_asset")
          embeddedAudio = true;
      }
      if (embeddedAudio)
        videoAudioAssets.insert(asset.id.value());
      if (!companion &&
          (asset.kind == d::AssetKind::Audio ||
           asset.kind == d::AssetKind::Video ||
           (asset.kind == d::AssetKind::Image && asset.portableLocator &&
            asset.portableLocator->kind ==
                d::AssetPortableLocatorKind::ExternalIdentity)))
        usedAssets.insert(asset.id.value());
    }
  for (const auto &asset : content.assets)
    if (usedAssets.count(asset.id.value())) {
      if (!asset.currentFacts)
        throw std::runtime_error("Native asset has no probed facts");
      const auto &facts = *asset.currentFacts;
      std::string kind = asset.kind == d::AssetKind::Video   ? "video"
                         : asset.kind == d::AssetKind::Audio ? "audio"
                         : asset.kind == d::AssetKind::Image ? "image"
                                                             : "";
      if (kind.empty())
        throw std::runtime_error("Unsupported native asset kind");
      std::string location;
      auto found = locators.find(asset.id.value());
      if (found != locators.end())
        location = found->second;
      else if (asset.portableLocator &&
               asset.portableLocator->kind ==
                   d::AssetPortableLocatorKind::ManagedProjectCopy)
        location = (path / asset.portableLocator->identity).string();
      else
        throw std::runtime_error("Native asset has no available local locator");
      result["assets"].push_back(
          {{"id", asset.id.value()},
           {"name", asset.displayName},
           {"kind", kind},
           {"path", location},
           {"duration", facts.durationTicks},
           {"size", asset.byteLength},
           {"width", facts.displayWidth.value_or(0)},
           {"height", facts.displayHeight.value_or(0)},
           {"hasAudio",
            kind == "audio" || videoAudioAssets.count(asset.id.value()) > 0}});
    }
  for (const auto &track : timeline.tracks) {
    if (track->effectGraph || track->solo)
      throw std::runtime_error(
          "Native track effects or solo cannot be represented in browser");
    json t = {{"id", track->id.value()},
              {"name", track->name},
              {"type", track->type == d::TrackType::Audio  ? "audio"
                       : track->type == d::TrackType::Text ? "text"
                                                           : "video"},
              {"visible", track->visible},
              {"muted", track->muted},
              {"locked", track->locked},
              {"syncLocked", track->syncLocked},
              {"items", json::array()}};
    for (const auto &item : track->items) {
      if (companionIds.count(item->id.value()))
        continue;
      const auto &clip = *item->clip;
      if (!clip.automation.empty())
        throw std::runtime_error(
            "Native clip effect graph cannot be represented in browser yet");
      if (item->placement.begin.time_base() != 120000 ||
          item->placement.end.time_base() != 120000 ||
          clip.source.begin.time_base() != 120000 ||
          clip.source.end.time_base() != 120000)
        throw std::runtime_error(
            "Native project uses a different authored time base");
      std::string kind = clip.type == d::ClipType::Video   ? "video"
                         : clip.type == d::ClipType::Audio ? "audio"
                         : clip.type == d::ClipType::Image ? "image"
                         : clip.type == d::ClipType::Text  ? "text"
                                                           : "";
      if (kind.empty())
        throw std::runtime_error(
            "Native processing/vector clip cannot be represented in browser");
      json c = {{"id", clip.id.value()},
                {"type", kind},
                {"assetId", std::holds_alternative<AssetId>(clip.content)
                                ? std::get<AssetId>(clip.content).value()
                                : ""},
                {"source",
                 {{"begin", clip.source.begin.value()},
                  {"end", clip.source.end.value()}}},
                {"visual", visualProjection(clip.visual)},
                {"audio", audioProjection(clip.audio)},
                {"automation", automationProjection(clip, *parts.effectMask)},
                {"effects", importClipEffects(clip, *parts.effectMask)},
                {"retime",
                 {{"version", 1},
                  {"mode", "constant"},
                  {"constantRatePpm", 1000000}}}};
      c["automation"].update(importEffectsAutomation(clip, *parts.effectMask));
      if (companions.count(item->id.value())) {
        const auto &companion = *companions.at(item->id.value())->clip;
        c["audio"] = audioProjection(companion.audio);
        c["automation"].update(
            automationProjection(companion, *parts.effectMask));
      }
      c["retime"]["constantRatePpm"] = rateOf(clip);
      if (kind == "text") {
        if (!std::holds_alternative<TextCompositionId>(clip.content))
          throw std::runtime_error("Native text owner is invalid");
        const auto textId = std::get<TextCompositionId>(clip.content);
        const d::TextComposition *text = nullptr;
        for (const auto &candidate : content.textCompositions)
          if (candidate.id == textId)
            text = &candidate;
        if (!text)
          throw std::runtime_error("Native text composition missing");
        const auto &doc = text->document;
        if (doc.templateOrigin) {
          const auto &origin = *doc.templateOrigin;
          const bool custom = origin.templateId.rfind("videocut.web.recipe.v2:", 0) == 0;
          const std::string prefix = custom ? "videocut.web.recipe.v2:" : "videocut.web.recipe.v1:";
          if (origin.templateId.rfind(prefix, 0) != 0)
            throw std::runtime_error("Native text template is outside the "
                                     "supported web recipe catalog");
          const auto split = origin.templateId.find(':', prefix.size());
          if (split == std::string::npos)
            throw std::runtime_error("Native web recipe identity is malformed");
          const auto recipe =
              origin.templateId.substr(prefix.size(), split - prefix.size());
          const auto descriptor = custom ? decodeRecipe(recipe) : json{{"id", recipe}};
          auto checkDocument = doc;
          checkDocument.templateOrigin.reset();
          if (videocut::text_composition::
                      ComputeTextCompositionDocumentIdentity(checkDocument) !=
                  origin.templateId.substr(split + 1) ||
              text->effectGraph || !text->propertyAutomations.empty())
            throw std::runtime_error(
                "Native text template has desktop edits that the browser "
                "cannot reproduce without losing authored data");
          std::string plain;
          for (const auto &paragraph : doc.content) {
            if (!plain.empty())
              plain += '\n';
            for (const auto &run : paragraph.runs)
              plain += run.utf8Text;
          }
          c["text"] = {{"content", plain},
                       {"fontSize", 64},
                       {"color", "#ffffff"},
                       {"fontFamily", "system"},
                       {"template",
                        {{"id", descriptor.at("id")},
                         {"version", 1},
                         {"packageDigest", origin.packageDigest}}}};
          if (descriptor.contains("recipe")) c["text"]["template"]["recipe"] = descriptor.at("recipe");
          if (descriptor.contains("style")) c["text"]["template"]["style"] = descriptor.at("style");
          if (descriptor.contains("layoutWidth")) c["text"]["layoutWidth"] = descriptor.at("layoutWidth");
        } else {
          if (!doc.animations.layers.empty() || !doc.decorations.empty() ||
              text->effectGraph || !text->propertyAutomations.empty())
            throw std::runtime_error("Native complex text must retain its full "
                                     "authored composition; browser import is "
                                     "not yet available for this content");
          std::string plain;
          std::optional<double> fontSize;
          std::optional<videocut::text::Color> color;
          std::optional<json> font;
          for (const auto &paragraph : doc.content) {
            if (!plain.empty())
              plain += '\n';
            for (const auto &run : paragraph.runs) {
              plain += run.utf8Text;
              const auto projectedFont = systemFontProjection(run.style.font.primary);
              if ((font && *font != projectedFont) ||
                  run.style.materials.layers.size() != 1)
                throw std::runtime_error(
                    "Native rich text style cannot be represented in browser");
              font = projectedFont;
              const auto *fill = std::get_if<videocut::text::TextFillLayer>(
                  &run.style.materials.layers.front());
              const auto *literal =
                  fill ? std::get_if<videocut::text::LiteralTextMaterial>(
                             &fill->material)
                       : nullptr;
              const auto *solid =
                  literal ? std::get_if<videocut::text::SolidTextMaterial>(
                                &literal->material)
                          : nullptr;
              if (!solid || solid->color.alpha != 1)
                throw std::runtime_error(
                    "Native text material cannot be represented in browser");
              if (fontSize && *fontSize != run.style.fontSize)
                throw std::runtime_error(
                    "Native mixed font sizes cannot be represented in browser");
              if (color && (color->red != solid->color.red ||
                            color->green != solid->color.green ||
                            color->blue != solid->color.blue))
                throw std::runtime_error("Native mixed text colors cannot be "
                                         "represented in browser");
              fontSize = run.style.fontSize;
              color = solid->color;
            }
          }
          if (!fontSize || !color || !font)
            throw std::runtime_error("Native text is empty");
          std::ostringstream hex;
          hex << '#' << std::hex << std::setfill('0') << std::setw(2)
              << int(std::round(color->red * 255)) << std::setw(2)
              << int(std::round(color->green * 255)) << std::setw(2)
              << int(std::round(color->blue * 255));
          c["text"] = {{"content", plain},
                       {"fontSize", *fontSize},
                       {"color", hex.str()},
                       {"fontFamily", font->at("family")},
                       {"font", *font}};
          if (!doc.content.empty() && std::all_of(doc.content.begin(), doc.content.end(), [](const auto &paragraph) { return paragraph.style.wrap == videocut::text::TextWrap::Word; })) {
            const auto &reference = doc.presentation.referenceCanvas;
            const auto scale = std::min(result.at("canvas").at("width").get<double>() / reference.width,
                                        result.at("canvas").at("height").get<double>() / reference.height);
            c["text"]["layoutWidth"] = doc.presentation.authoredLayoutFrame.width * scale;
          }
          const auto expected = plainTextDocument(c, result.at("canvas"));
          if (videocut::text_composition::
                  ComputeTextCompositionDocumentIdentity(expected) !=
              videocut::text_composition::
                  ComputeTextCompositionDocumentIdentity(doc))
            throw std::runtime_error("Native text has layout or style data "
                                     "outside the browser subset");
        }
      }
      t["items"].push_back({{"id", item->id.value()},
                            {"name", item->name},
                            {"enabled", item->enabled},
                            {"placement",
                             {{"begin", item->placement.begin.value()},
                              {"end", item->placement.end.value()}}},
                            {"clip", c}});
    }
    if (!t["items"].empty() || track->items.empty())
      result["timeline"]["tracks"].push_back(t);
  }
  for (const auto &group : timeline.groups) {
    if (group.colorRgba != 0xffffffffU)
      throw std::runtime_error(
          "Native group color cannot be represented in browser");
    json members = json::array();
    for (const auto &member : group.items) {
      if (companionIds.count(member.value()))
        throw std::runtime_error(
            "Native group contains a hidden audio companion");
      members.push_back(member.value());
    }
    result["timeline"]["groups"].push_back(
        {{"id", group.id.value()}, {"itemIds", members}});
  }
  for (const auto &link : timeline.links)
    if (link.kind != d::LinkKind::EmbeddedAudioVideo) {
      if (link.kind != d::LinkKind::SynchronizedItems)
        throw std::runtime_error(
            "Native track links cannot be represented in browser");
      json members = json::array();
      for (const auto &member : link.items) {
        if (companionIds.count(member.value()))
          throw std::runtime_error(
              "Native link contains a hidden audio companion");
        members.push_back(member.value());
      }
      result["timeline"]["links"].push_back(
          {{"id", link.id.value()}, {"itemIds", members}});
    }
  check(store->CloseAndDrain({id<WorkspaceSessionId>("web-bridge-session"),
                              Generation::Initial(), projectId,
                              published->document->revision()}));
  return result;
}
