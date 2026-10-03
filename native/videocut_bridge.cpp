// Uses VideoCut's canonical document validator, codec, publication store and
// locator sidecar. No private reimplementation of the binary format and no
// modification to the desktop checkout.
#include "../packages/text-wasm/src/package_decoder.h"
#include "videocut/base/Sha256.h"
#include "videocut/editor/application/ProjectDocumentFactory.h"
#include "videocut/editor/infrastructure/ProjectFormatAssetLocatorPort.h"
#include "videocut/editor/infrastructure/ProjectFormatDocumentCodec.h"
#include "videocut/editor/infrastructure/ProjectFormatRevisionLeaseAuthority.h"
#include "videocut/editor/infrastructure/ProjectFormatStore.h"
#include "videocut/text_composition/TextTemplatePatch.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

using namespace videocut::editor;
namespace d = videocut::editor::domain;
namespace infra = videocut::editor::infrastructure;
using json = nlohmann::json;

std::string encodeRecipe(const json &value) {
  const char *digits = "0123456789abcdef";
  std::string encoded;
  for (unsigned char c : value.dump()) {
    encoded += digits[c >> 4];
    encoded += digits[c & 15];
  }
  return encoded;
}
json decodeRecipe(const std::string &encoded) {
  if (encoded.empty() || encoded.size() > 4096 || encoded.size() % 2)
    throw std::runtime_error("Malformed native recipe descriptor");
  const auto digit = [](char c) -> unsigned {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    throw std::runtime_error("Malformed native recipe descriptor");
  };
  std::string decoded;
  for (std::size_t i = 0; i < encoded.size(); i += 2)
    decoded += static_cast<char>(digit(encoded[i]) * 16 + digit(encoded[i + 1]));
  auto value = json::parse(decoded);
  if (!value.is_object() || !value.contains("id") || !value["id"].is_string() ||
      (!value.contains("recipe") && !value.contains("style")) ||
      (value.contains("recipe") && !value["recipe"].is_object()) ||
      (value.contains("style") && !value["style"].is_object()))
    throw std::runtime_error("Malformed native recipe descriptor");
  return value;
}

template <class T> T take(Result<T> result) {
  if (!result.ok()) {
    std::string message;
    for (const auto &e : result.diagnostics())
      message += e.message + " [" + e.context + "]\n";
    throw std::runtime_error(message);
  }
  return std::move(result).value();
}
void check(Result<void> result) {
  if (!result.ok()) {
    std::string message;
    for (const auto &e : result.diagnostics())
      message += e.message + " [" + e.context + "]\n";
    throw std::runtime_error(message);
  }
}
template <class T> T id(const std::string &v) { return take(T::Create(v)); }
RationalTime time(std::int64_t ticks, TimeSpace space) {
  return take(RationalTime::Create(ticks, 120000, space));
}
std::string digest(const std::string &text) {
  return "sha256:" + videocut::base::Sha256::HexDigest(text);
}
d::PortableMetadataEntry stringMetadata(const std::string &key,
                                        const std::string &value) {
  d::PortableMetadataValue v;
  v.kind = d::PortableMetadataValueKind::String;
  v.stringValue = value;
  return {key, v};
}
d::PortableMetadataEntry indexMetadata(std::uint64_t value) {
  d::PortableMetadataValue v;
  v.kind = d::PortableMetadataValueKind::UnsignedInteger;
  v.unsignedValue = value;
  return {"videocut.media.stream_index", v};
}

std::unique_ptr<infra::ProjectFormatStore>
openStore(const std::filesystem::path &path, ProjectId project,
          infra::ProjectFormatOpenMode mode) {
  auto session = id<WorkspaceSessionId>("web-bridge-session");
  auto leases = take(infra::ProjectFormatRevisionLeaseAuthority::Create(
      {project, session, Generation::Initial()}));
  return take(infra::ProjectFormatStore::Open(path, mode, session,
                                              Generation::Initial(), project,
                                              take(leases->CreatePort())));
}

#include "effects_bridge.h"
#include "projection_bridge.h"

std::string fileBytes(const std::filesystem::path &path);

json inspect(const std::filesystem::path &path) {
  auto project =
      take(infra::ProjectFormatStore::PersistedProjectIdForPackage(path));
  auto store =
      openStore(path, project, infra::ProjectFormatOpenMode::OpenExisting);
  auto published = take(store->ReadPublished());
  if (!published)
    throw std::runtime_error("No published project HEAD");
  const auto &parts = published->document->partitions();
  json result = {{"verified", true},
                 {"projectId", project.value()},
                 {"name", parts.metadata.displayName},
                 {"canvas",
                  {{"width", parts.metadata.canvas.width},
                   {"height", parts.metadata.canvas.height}}},
                 {"revision", published->document->revision().value()},
                 {"tracks", json::array()},
                 {"assets", json::array()}};
  for (const auto &track : parts.timeline->partitions().tracks) {
    json t = {{"id", track->id.value()},
              {"type", static_cast<int>(track->type)},
              {"visible", track->visible},
              {"muted", track->muted},
              {"syncLocked", track->syncLocked},
              {"items", json::array()}};
    for (const auto &item : track->items) {
      const auto &clip = *item->clip;
      json i = {{"id", item->id.value()},
                {"name", item->name},
                {"begin", item->placement.begin.value()},
                {"end", item->placement.end.value()},
                {"sourceBegin", clip.source.begin.value()},
                {"sourceEnd", clip.source.end.value()}};
      if (clip.visual)
        i["visual"] = {{"x", clip.visual->transform.positionX},
                       {"y", clip.visual->transform.positionY},
                       {"scaleX", clip.visual->transform.scaleX},
                       {"scaleY", clip.visual->transform.scaleY},
                       {"rotation", clip.visual->transform.rotationDegrees},
                       {"opacity", clip.visual->opacity}};
      if (clip.audio)
        i["audio"] = {{"gain", clip.audio->gainLinear},
                      {"muted", clip.audio->muted}};
      t["items"].push_back(i);
    }
    result["tracks"].push_back(t);
  }
  result["texts"] = json::array();
  for (const auto &text :
       parts.contentAssetJob->partitions().textCompositions) {
    std::string plain;
    for (const auto &paragraph : text.document.content) {
      if (!plain.empty())
        plain += "\n";
      for (const auto &run : paragraph.runs)
        plain += run.utf8Text;
    }
    json details = {
        {"id", text.id.value()},
        {"content", plain},
        {"fontSize", text.document.content.front().runs.front().style.fontSize},
        {"durationTicks", text.document.durationTicks},
        {"resources", text.document.resources.size()},
        {"animationLayers", text.document.animations.layers.size()},
        {"decorations", text.document.decorations.size()},
        {"documentIdentity",
         videocut::text_composition::ComputeTextCompositionDocumentIdentity(
             text.document)}};
    details["postEffects"] = 0;
    for (const auto &layer : text.document.animations.layers)
      details["postEffects"] =
          details["postEffects"].get<int>() + layer.postEffects.size();
    if (text.document.templateOrigin)
      details["templateId"] = text.document.templateOrigin->templateId;
    result["texts"].push_back(details);
  }
  result["managedAssets"] = json::array();
  for (const auto &asset : parts.contentAssetJob->partitions().assets) {
    if (!asset.portableLocator ||
        asset.portableLocator->kind !=
            d::AssetPortableLocatorKind::ManagedProjectCopy)
      continue;
    const auto file = path / asset.portableLocator->identity;
    const auto bytes = fileBytes(file);
    if (digest(bytes) != asset.contentFingerprint ||
        bytes.size() != asset.byteLength)
      throw std::runtime_error("Managed text resource is missing or corrupt");
    result["managedAssets"].push_back(
        {{"id", asset.id.value()},
         {"relativePath", asset.portableLocator->identity},
         {"digest", asset.contentFingerprint},
         {"bytes", asset.byteLength}});
  }
  auto locators = take(infra::ProjectFormatAssetLocatorPort::Open(path));
  for (const auto &a : take(locators->ReadAssetLocators()))
    result["assets"].push_back({{"id", a.asset.value()},
                                {"path", a.platformLocator},
                                {"fingerprint", a.sourceFingerprint}});
  check(store->CloseAndDrain({id<WorkspaceSessionId>("web-bridge-session"),
                              Generation::Initial(), project,
                              published->document->revision()}));
  return result;
}

std::string fileBytes(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("Missing managed text resource: " + path.string());
  return {std::istreambuf_iterator<char>(file), {}};
}

void exportProject(const json &p, const std::string &input,
                   const std::filesystem::path &path) {
  if (std::filesystem::exists(path))
    throw std::runtime_error(
        "Output already exists; refusing to overwrite a project");
  if (p.at("format") != "videocut.edit-session" || p.at("version") != 1)
    throw std::runtime_error("Unsupported edit session");
  const auto projectId = id<ProjectId>(p.at("id"));
  auto seed = take(application::ProjectDocumentFactory::Create(
      {projectId, id<TimelineId>(p.at("timeline").at("id")), p.at("name"),
       p.at("canvas").at("width"), p.at("canvas").at("height"),
       p.at("frameRate").at("numerator"),
       p.at("frameRate").at("denominator")}));
  auto parts = seed->partitions();
  auto timeline = parts.timeline->partitions();
  auto content = parts.contentAssetJob->partitions();
  std::vector<d::AssetRecord> assets;
  std::vector<infra::ProjectFormatAssetLocator> locators;
  std::map<std::string, json> assetMap;
  for (const auto &a : p.at("assets")) {
    const std::string identity = a.at("id"), kind = a.at("kind"),
                      fingerprint = a.at("fingerprint");
    d::AssetRecord asset{id<AssetId>(identity)};
    asset.kind = kind == "audio"   ? d::AssetKind::Audio
                 : kind == "image" ? d::AssetKind::Image
                                   : d::AssetKind::Video;
    asset.contentFingerprint = fingerprint;
    asset.mediaFingerprint = fingerprint;
    asset.displayName = a.at("name");
    asset.byteLength = a.at("size");
    asset.authoredRevision = 1;
    d::CurrentAssetFacts facts;
    facts.durationTicks = a.at("duration");
    if (a.at("width").get<int>() > 0)
      facts.displayWidth = a.at("width");
    if (a.at("height").get<int>() > 0)
      facts.displayHeight = a.at("height");
    json videoStream, audioStream;
    for (const auto &stream : a.at("nativeProbe").at("streams")) {
      if (stream.at("codec_type") == "video" &&
          !stream.value("disposition", json::object())
               .value("attached_pic", 0) &&
          videoStream.is_null())
        videoStream = stream;
      if (stream.at("codec_type") == "audio" && audioStream.is_null())
        audioStream = stream;
    }
    if (!videoStream.is_null())
      facts.videoFormatIdentity =
          "web-video-" + videocut::base::Sha256::HexDigest(videoStream.dump());
    if (!audioStream.is_null())
      facts.audioFormatIdentity =
          "web-audio-" + videocut::base::Sha256::HexDigest(audioStream.dump());
    if (!videoStream.is_null())
      facts.metadata.push_back(indexMetadata(videoStream.at("index")));
    else if (!audioStream.is_null())
      facts.metadata.push_back(indexMetadata(audioStream.at("index")));
    if (kind == "video" && !audioStream.is_null())
      facts.metadata.push_back(stringMetadata(
          "videocut.media.embedded_audio_asset", "audio-" + identity));
    facts.factsDigest =
        digest("web.asset.facts.v1|" +
               json{{"kind", kind},
                    {"duration", facts.durationTicks},
                    {"width", a.at("width")},
                    {"height", a.at("height")},
                    {"streams", a.at("nativeProbe").at("streams")}}
                   .dump());
    asset.currentFacts = facts;
    asset.provenanceDigest = digest("web.asset.source.v1|" + identity + "|" +
                                    fingerprint + "|" + facts.factsDigest);
    asset.portableLocator = d::AssetPortableLocator{
        d::AssetPortableLocatorKind::ExternalIdentity, identity, {}, {}};
    assets.push_back(asset);
    assetMap[identity] = a;
    locators.push_back({asset.id, id<LocatorId>("locator-" + identity),
                        a.at("path"), fingerprint, false,
                        d::AssetPortableLocatorKind::ExternalIdentity, identity,
                        fingerprint});
    if (kind == "video" && !audioStream.is_null()) {
      auto companion = asset;
      const auto audioIdentity = "audio-" + identity;
      companion.id = id<AssetId>(audioIdentity);
      companion.kind = d::AssetKind::Audio;
      companion.displayName += " · Audio";
      companion.mediaFingerprint =
          fingerprint +
          ":audio:" + std::to_string(audioStream.at("index").get<int>());
      d::CurrentAssetFacts audioFacts;
      audioFacts.durationTicks = facts.durationTicks;
      audioFacts.audioFormatIdentity = facts.audioFormatIdentity;
      audioFacts.metadata = {
          stringMetadata("videocut.media.embedded_video_asset", identity),
          indexMetadata(audioStream.at("index"))};
      audioFacts.factsDigest =
          digest("web.audio.facts.v1|" + audioStream.dump() + "|" +
                 std::to_string(facts.durationTicks));
      companion.currentFacts = audioFacts;
      companion.portableLocator = d::AssetPortableLocator{
          d::AssetPortableLocatorKind::ExternalIdentity, audioIdentity, {}, {}};
      companion.provenanceDigest =
          digest("web.asset.source.v1|" + audioIdentity + "|" + fingerprint +
                 "|" + audioFacts.factsDigest);
      assets.push_back(companion);
      locators.push_back({companion.id,
                          id<LocatorId>("locator-" + audioIdentity),
                          a.at("path"), fingerprint, false,
                          d::AssetPortableLocatorKind::ExternalIdentity,
                          audioIdentity, fingerprint});
    }
  }
  for (const auto &a : p.value("templateAssets", json::array())) {
    const std::string identity = a.at("id");
    d::AssetRecord asset{id<AssetId>(identity)};
    asset.kind =
        a.at("kind") == "font" ? d::AssetKind::Font : d::AssetKind::Image;
    asset.contentFingerprint = a.at("digest");
    asset.mediaFingerprint = asset.contentFingerprint;
    asset.displayName = identity;
    asset.byteLength = a.at("byteLength");
    asset.authoredRevision = 1;
    d::CurrentAssetFacts facts;
    if (asset.kind == d::AssetKind::Image)
      facts.videoFormatIdentity =
          "text-image-" + videocut::base::Sha256::HexDigest(
                              a.at("mediaType").get<std::string>());
    facts.factsDigest =
        digest("web.text-template.facts|" + asset.contentFingerprint + "|" +
               a.at("mediaType").get<std::string>());
    asset.currentFacts = facts;
    const std::string relative = a.at("relativePath");
    if (relative.rfind("assets/text/", 0) != 0 ||
        relative.find("..") != std::string::npos ||
        relative.find('\\') != std::string::npos)
      throw std::runtime_error("Unsafe managed resource path");
    asset.portableLocator = d::AssetPortableLocator{
        d::AssetPortableLocatorKind::ManagedProjectCopy, relative, {}, {}};
    asset.provenanceDigest =
        digest("web.text-template.asset|" + asset.contentFingerprint);
    assets.push_back(std::move(asset));
  }
  std::vector<d::TextComposition> texts;
  std::vector<d::TimelineTrack::Holder> tracks;
  std::vector<d::TimelineLink> links;
  std::vector<d::TimelineGroup> groups;
  std::vector<d::EffectCurve> curves;
  std::vector<d::RetimeDocument::Holder> retimes;
  std::int64_t extent = 1;
  for (const auto &t : p.at("timeline").at("tracks")) {
    auto track = std::make_shared<d::TimelineTrack>(
        d::TimelineTrack{id<TrackId>(t.at("id"))});
    track->name = t.at("name");
    track->type = t.at("type") == "audio"  ? d::TrackType::Audio
                  : t.at("type") == "text" ? d::TrackType::Text
                                           : d::TrackType::Video;
    track->order = static_cast<std::uint32_t>(tracks.size());
    track->locked = t.at("locked");
    track->muted = t.at("muted");
    track->visible = t.at("visible");
    track->syncLocked = t.value("syncLocked", false);
    auto audioTrack = std::make_shared<d::TimelineTrack>(
        d::TimelineTrack{id<TrackId>("audio-" + track->id.value())});
    audioTrack->name = track->name + " · 音频";
    audioTrack->type = d::TrackType::Audio;
    audioTrack->muted = track->muted;
    audioTrack->locked = track->locked;
    audioTrack->syncLocked = track->syncLocked;
    std::vector<d::TimelineItem::Holder> items, audioItems;
    for (const auto &i : t.at("items")) {
      const auto &c = i.at("clip");
      const std::string kind = c.at("type");
      auto clip = std::make_shared<d::TimelineClip>(d::TimelineClip{
          id<ClipId>(c.at("id")),
          kind == "audio"   ? d::ClipType::Audio
          : kind == "image" ? d::ClipType::Image
          : kind == "text"  ? d::ClipType::Text
                            : d::ClipType::Video,
          id<AssetId>(kind == "text" ? "temporary-text-owner"
                                     : c.at("assetId").get<std::string>()),
          {time(c.at("source").at("begin"), TimeSpace::Source),
           time(c.at("source").at("end"), TimeSpace::Source)}});
      if (kind == "text") {
        const auto textId = id<TextCompositionId>("text-" + clip->id.value());
        d::TextComposition text(textId);
        text.timeline = id<TimelineId>(p.at("timeline").at("id"));
        text.ownerClip = clip->id;
        if (c.at("text").contains("template")) {
          namespace comp = videocut::text_composition;
          const auto &bundle =
              p.at("templateBundles")
                  .at(c.at("text").at("template").at("id").get<std::string>());
          auto patch =
              comp::ParseTextTemplateJson(bundle.at("composition").dump());
          std::string error;
          if (!patch.valid) {
            for (const auto &diag : patch.diagnostics)
              error += diag.message + " ";
            throw std::runtime_error(error);
          }
          if (!videocut::text_wasm::DecodePackageAnimation(
                  bundle.at("effectProgram"), bundle.at("animation"),
                  patch.value.value, error))
            throw std::runtime_error(error);
          patch.value.packageDigest = digest(bundle.dump());
          patch.value.value.templateOrigin = comp::TextTemplateOrigin{
              patch.value.templateId, patch.value.packageDigest};
          text.document = patch.value.value;
          auto applied = comp::ApplyTextTemplatePatch(
              text.document, patch.value,
              {{"content", c.at("text").at("content").get<std::string>()}});
          if (!applied.valid) {
            for (const auto &diag : applied.diagnostics)
              error += diag.message + " ";
            throw std::runtime_error(error);
          }
          text.document.templateOrigin.reset();
          const auto &descriptor = c.at("text").at("template");
          json authored{{"id", descriptor.at("id")}};
          if (descriptor.contains("recipe")) authored["recipe"] = descriptor.at("recipe");
          if (c.at("text").contains("layoutWidth")) {
            authored["layoutWidth"] = c.at("text").at("layoutWidth");
            applyTextLayoutWidth(text.document, c.at("text").at("layoutWidth"), p.at("canvas"));
          }
          if (descriptor.contains("style")) {
            authored["style"] = descriptor.at("style");
            const auto &style = descriptor.at("style");
            for (auto &paragraph : text.document.content) for (auto &run : paragraph.runs) {
              if (style.contains("fontSize")) run.style.fontSize = style.at("fontSize");
              if (style.contains("color")) {
                const auto rgb = std::stoul(style.at("color").get<std::string>().substr(1), nullptr, 16);
                videocut::text::SolidTextMaterial solid;
                solid.color = {float((rgb >> 16) & 255) / 255,
                  float((rgb >> 8) & 255) / 255, float(rgb & 255) / 255, 1};
                for (auto &layer : run.style.materials.layers)
                  if (auto *fill = std::get_if<videocut::text::TextFillLayer>(&layer))
                    fill->material = videocut::text::LiteralTextMaterial{solid};
              }
            }
          }
          const auto contentFingerprint =
              comp::ComputeTextCompositionDocumentIdentity(text.document);
          text.document.templateOrigin = comp::TextTemplateOrigin{
              (descriptor.contains("recipe") || descriptor.contains("style") || authored.contains("layoutWidth")
                ? "videocut.web.recipe.v2:" + encodeRecipe(authored)
                : "videocut.web.recipe.v1:" + c.at("text").at("template").at("id").get<std::string>()) +
                  ":" + contentFingerprint,
              patch.value.packageDigest};
          // Keep the template's authored clock; extending a timeline item must
          // not retime its animation.
        } else {
          text.document = plainTextDocument(c, p.at("canvas"));
        }
        texts.push_back(std::move(text));
        clip->content = textId;
      }
      d::AudioPropertyModel audio;
      audio.gainLinear = c.at("audio").at("gainLinear");
      audio.muted = c.at("audio").at("muted");
      applyAudio(audio, c.at("audio"));
      if (kind == "audio")
        clip->audio = audio;
      else {
        const auto &v = c.at("visual");
        d::VisualPropertyModel visual;
        visual.transform.positionX = v.at("positionX");
        visual.transform.positionY = v.at("positionY");
        visual.transform.scaleX = v.at("scaleX");
        visual.transform.scaleY = v.at("scaleY");
        visual.transform.rotationDegrees = v.at("rotationDegrees");
        visual.opacity = v.at("opacity");
        applyVisual(visual, v);
        clip->visual = visual;
      }
      auto item = std::make_shared<d::TimelineItem>(d::TimelineItem{
          id<ItemId>(i.at("id")),
          i.at("name"),
          {time(i.at("placement").at("begin"), TimeSpace::Timeline),
           time(i.at("placement").at("end"), TimeSpace::Timeline)},
          clip,
          i.at("enabled")});
      applyAutomation(*clip, c, curves, kind == "audio");
      applyRetime(*clip, c, item->placement, retimes);
      extent = std::max(extent, item->placement.end.value());
      items.push_back(item);
      if (kind == "video" &&
          assetMap.at(c.at("assetId")).at("hasAudio") == true) {
        auto audioClip = std::make_shared<d::TimelineClip>(*clip);
        audioClip->id = id<ClipId>("audio-" + clip->id.value());
        audioClip->type = d::ClipType::Audio;
        audioClip->visual.reset();
        audioClip->audio = audio;
        audioClip->content =
            id<AssetId>("audio-" + c.at("assetId").get<std::string>());
        auto audioItem = std::make_shared<d::TimelineItem>(*item);
        audioItem->id = id<ItemId>("audio-" + item->id.value());
        audioItem->clip = audioClip;
        audioClip->retime.reset();
        applyRetime(*audioClip, c, audioItem->placement, retimes);
        applyAutomation(*audioClip, c, curves, true);
        audioItems.push_back(audioItem);
        links.push_back({id<LinkId>("link-" + item->id.value()),
                         d::LinkKind::EmbeddedAudioVideo,
                         {},
                         {item->id, audioItem->id}});
      }
    }
    track->items =
        ImmutableIndexedStorage<d::TimelineItem::Holder>(std::move(items));
    tracks.push_back(track);
    if (!audioItems.empty()) {
      audioTrack->order = static_cast<std::uint32_t>(tracks.size());
      audioTrack->items = ImmutableIndexedStorage<d::TimelineItem::Holder>(
          std::move(audioItems));
      tracks.push_back(audioTrack);
    }
  }
  for (const auto &g : p.at("timeline").value("groups", json::array())) {
    d::TimelineGroup group{id<GroupId>(g.at("id"))};
    for (const auto &member : g.at("itemIds"))
      group.items.push_back(id<ItemId>(member));
    groups.push_back(std::move(group));
  }
  for (const auto &g : p.at("timeline").value("links", json::array())) {
    d::TimelineLink link{
        id<LinkId>(g.at("id")), d::LinkKind::SynchronizedItems, {}, {}};
    for (const auto &member : g.at("itemIds"))
      link.items.push_back(id<ItemId>(member));
    links.push_back(std::move(link));
  }
  timeline.groups =
      ImmutableIndexedStorage<d::TimelineGroup>(std::move(groups));
  timeline.retimes =
      ImmutableIndexedStorage<d::RetimeDocument::Holder>(std::move(retimes));
  auto effects = parts.effectMask->partitions();
  effects.curves = ImmutableIndexedStorage<d::EffectCurve>(std::move(curves));
  timeline.tracks =
      ImmutableIndexedStorage<d::TimelineTrack::Holder>(std::move(tracks));
  timeline.links = ImmutableIndexedStorage<d::TimelineLink>(std::move(links));
  timeline.extent = {time(0, TimeSpace::Timeline),
                     time(extent, TimeSpace::Timeline)};
  timeline.projectPlacement = {time(0, TimeSpace::Project),
                               time(extent, TimeSpace::Project)};
  content.textCompositions =
      ImmutableIndexedStorage<d::TextComposition>(std::move(texts));
  content.assets = ImmutableIndexedStorage<d::AssetRecord>(std::move(assets));
  applyNativeEffects(p, timeline, effects);
  parts.effectMask = take(d::EffectMaskDocument::Create(std::move(effects), 1));
  parts.timeline = take(d::TimelineDocument::Create(std::move(timeline), 1));
  parts.contentAssetJob =
      take(d::ContentAssetJobDocument::Create(std::move(content), 1));
  auto document =
      take(d::ProjectDocument::Create(std::move(parts), Revision(1)));
  // Fingerprint the codec-normalized authored composition, not pre-codec
  // floats.
  if (!p.value("templateBundles", json::object()).empty()) {
    auto normalized =
        take(infra::ProjectFormatDocumentCodec::RoundTrip(*document))
            .reconstructedDocument;
    auto normalizedParts = normalized->partitions();
    auto normalizedContent = normalizedParts.contentAssetJob->partitions();
    std::vector<d::TextComposition> normalizedTexts;
    for (const auto &original : normalizedContent.textCompositions) {
      auto text = original;
      if (text.document.templateOrigin &&
          (text.document.templateOrigin->templateId.rfind("videocut.web.recipe.v1:", 0) == 0 ||
           text.document.templateOrigin->templateId.rfind("videocut.web.recipe.v2:", 0) == 0)) {
        auto origin = *text.document.templateOrigin;
        const auto split = origin.templateId.find(
            ':', std::string("videocut.web.recipe.v1:").size());
        text.document.templateOrigin.reset();
        origin.templateId =
            origin.templateId.substr(0, split + 1) +
            videocut::text_composition::ComputeTextCompositionDocumentIdentity(
                text.document);
        text.document.templateOrigin = std::move(origin);
      }
      normalizedTexts.push_back(std::move(text));
    }
    normalizedContent.textCompositions =
        ImmutableIndexedStorage<d::TextComposition>(std::move(normalizedTexts));
    normalizedParts.contentAssetJob = take(
        d::ContentAssetJobDocument::Create(std::move(normalizedContent), 1));
    document = take(
        d::ProjectDocument::Create(std::move(normalizedParts), Revision(1)));
  }
  auto store = openStore(path, projectId, infra::ProjectFormatOpenMode::Create);
  take(store->ReadPublished());
  for (const auto &a : p.value("templateAssets", json::array())) {
    const auto bytes = fileBytes(a.at("path").get<std::string>());
    if (digest(bytes) != a.at("digest") ||
        bytes.size() != a.at("byteLength").get<std::size_t>())
      throw std::runtime_error("Template resource digest changed");
    const auto output = path / a.at("relativePath").get<std::string>();
    std::filesystem::create_directories(output.parent_path());
    std::ofstream file(output, std::ios::binary);
    file.write(bytes.data(), bytes.size());
    if (!file)
      throw std::runtime_error("Cannot persist managed template resource");
  }
  const auto session = id<WorkspaceSessionId>("web-bridge-session");
  const auto stage = take(
      store->Stage({id<RequestId>("web-export"),
                    "sha256:" + videocut::base::Sha256::HexDigest(input),
                    {session, Generation::Initial(), projectId, Revision(1)},
                    document}));
  const auto seal = take(store->Seal(stage));
  take(store->Publish(seal));
  auto sidecar = take(infra::ProjectFormatAssetLocatorPort::Open(path));
  check(sidecar->ReplaceAssetLocators(std::move(locators)));
  check(store->CloseAndDrain(
      {session, Generation::Initial(), projectId, Revision(1)}));
}

#include "import_projection.h"

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "template-digests") {
      std::string input((std::istreambuf_iterator<char>(std::cin)), {});
      const auto bundles = json::parse(input);
      json result = json::object();
      for (const auto &[recipe, bundle] : bundles.items())
        result[recipe] = digest(bundle.dump());
      std::cout << result.dump() << '\n';
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "capabilities") {
      std::cout << "{\"textTemplates\":1,\"customTextRecipes\":1,\"textTemplateStyles\":1,\"textLayoutWidth\":1}\n";
      return 0;
    }
    if (argc != 3 ||
        (std::string(argv[1]) != "export" &&
         std::string(argv[1]) != "inspect" && std::string(argv[1]) != "import"))
      throw std::runtime_error("Usage: videocut-bridge export|inspect|import "
                               "/absolute/project.vcut");
    auto path = std::filesystem::path(argv[2]);
    if (!path.is_absolute())
      throw std::runtime_error("Output must be absolute");
    if (std::string(argv[1]) == "import") {
      inspect(path);
      std::cout << importProjection(path).dump() << '\n';
      return 0;
    }
    if (std::string(argv[1]) == "export") {
      std::string input((std::istreambuf_iterator<char>(std::cin)), {});
      exportProject(json::parse(input), input, path);
    }
    std::cout << inspect(path).dump() << '\n';
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
