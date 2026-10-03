// Ephemeral browser projection of the canonical desktop Project Format 1
// owners.
#pragma once
#include "videocut/contract/TimeWarp.h"
#include <iomanip>
#include <set>

const std::map<std::string, d::VisualPropertyIdentity> visualProperties = {
    {"visual.positionX", d::VisualPropertyIdentity::PositionX},
    {"visual.positionY", d::VisualPropertyIdentity::PositionY},
    {"visual.scaleX", d::VisualPropertyIdentity::ScaleX},
    {"visual.scaleY", d::VisualPropertyIdentity::ScaleY},
    {"visual.rotationDegrees", d::VisualPropertyIdentity::RotationDegrees},
    {"visual.opacity", d::VisualPropertyIdentity::Opacity},
    {"visual.anchorX", d::VisualPropertyIdentity::AnchorX},
    {"visual.anchorY", d::VisualPropertyIdentity::AnchorY}};
const std::map<std::string, d::VisualBlendMode> blendModes = {
    {"normal", d::VisualBlendMode::Normal},
    {"multiply", d::VisualBlendMode::Multiply},
    {"screen", d::VisualBlendMode::Screen},
    {"overlay", d::VisualBlendMode::Overlay},
    {"darken", d::VisualBlendMode::Darken},
    {"lighten", d::VisualBlendMode::Lighten}};
const std::map<std::string, d::VisualFitPolicy> fitPolicies = {
    {"contain", d::VisualFitPolicy::Contain},
    {"cover", d::VisualFitPolicy::Cover},
    {"stretch", d::VisualFitPolicy::Stretch},
    {"nativeCrop", d::VisualFitPolicy::NativeCrop}};
const std::map<std::string, d::EffectCurveInterpolation> interpolations = {
    {"linear", d::EffectCurveInterpolation::Linear},
    {"hold", d::EffectCurveInterpolation::Hold},
    {"easeIn", d::EffectCurveInterpolation::EaseIn},
    {"easeOut", d::EffectCurveInterpolation::EaseOut},
    {"easeInOut", d::EffectCurveInterpolation::EaseInOut}};
template <class T>
std::string enumName(const std::map<std::string, T> &table, T value) {
  for (const auto &[name, v] : table)
    if (value == v)
      return name;
  throw std::runtime_error(
      "Native property is outside the browser projection capabilities");
}
void applyVisual(d::VisualPropertyModel &visual, const json &v) {
  visual.transform.anchorX = v.value("anchorX", .5);
  visual.transform.anchorY = v.value("anchorY", .5);
  visual.flipHorizontal = v.value("flipHorizontal", false);
  visual.flipVertical = v.value("flipVertical", false);
  visual.fitPolicy =
      fitPolicies.at(v.value("fitPolicy", std::string("contain")));
  visual.blendMode = blendModes.at(v.value("blendMode", std::string("normal")));
  if (v.contains("crop")) {
    const auto &c = v.at("crop");
    visual.crop = {c.at("left"), c.at("top"), c.at("right"), c.at("bottom")};
  }
}
void applyAudio(d::AudioPropertyModel &audio, const json &a) {
  if (a.value("fadeIn", std::int64_t(0)) > 0)
    audio.fadeIn = d::AudioFade{time(a.at("fadeIn"), TimeSpace::Presentation),
                                d::AudioFadeInterpolation::Linear};
  if (a.value("fadeOut", std::int64_t(0)) > 0)
    audio.fadeOut = d::AudioFade{time(a.at("fadeOut"), TimeSpace::Presentation),
                                 d::AudioFadeInterpolation::Linear};
}
void applyAutomation(d::TimelineClip &clip, const json &authored,
                     std::vector<d::EffectCurve> &curves,
                     bool audioOnly = false) {
  const auto automation = authored.value("automation", json::object());
  for (const auto &[property, binding] : automation.items()) {
    // Effect parameter curves are attached to canonical catalog instances by
    // applyNativeEffects once their typed parameter IDs exist.
    if (property.rfind("effects.", 0) == 0)
      continue;
    const bool audio = property.rfind("audio.", 0) == 0;
    if (audio != audioOnly)
      continue;
    if (binding.at("timeDomain") != "itemLocal")
      throw std::runtime_error("Unsupported automation time domain");
    const auto curveId =
        id<CurveId>("curve-" + clip.id.value() + "-" + property);
    const auto automationId =
        id<AutomationId>("automation-" + clip.id.value() + "-" + property);
    d::EffectCurveTarget target = d::VisualPropertyCurveTarget{
        clip.id, d::VisualPropertyIdentity::Opacity};
    if (audio) {
      if (property != "audio.gainLinear" || !clip.audio)
        throw std::runtime_error("Unsupported audio curve");
      target = d::AudioPropertyCurveTarget{
          clip.id, d::AudioPropertyIdentity::GainLinear};
      clip.audio->automation.push_back(
          {automationId, d::AudioPropertyIdentity::GainLinear, curveId,
           d::AudioAutomationTimeDomain::ItemLocal});
    } else {
      if (!clip.visual || !visualProperties.count(property))
        throw std::runtime_error("Unsupported visual curve");
      const auto key = visualProperties.at(property);
      target = d::VisualPropertyCurveTarget{clip.id, key};
      clip.visual->automation.push_back(
          {automationId, key, curveId, d::VisualAutomationTimeDomain::ItemLocal,
           true});
    }
    d::EffectCurve curve{curveId, target, {}};
    for (const auto &f : binding.at("keyframes")) {
      d::EffectCurveKeyframe frame{
          id<EntityId>(f.at("id")), time(f.at("time"), TimeSpace::Presentation),
          f.at("value").get<double>(),
          interpolations.at(f.at("interpolation").get<std::string>())};
      if (f.contains("segment")) {
        const auto &s = f.at("segment");
        frame.outgoingShape.kind =
            videocut::contract::CurveSegmentShapeKind::CubicBezier;
        frame.outgoingShape.firstControl = {s.at("firstControl").at("x"),
                                            s.at("firstControl").at("y")};
        frame.outgoingShape.secondControl = {s.at("secondControl").at("x"),
                                             s.at("secondControl").at("y")};
      }
      curve.keyframes.push_back(std::move(frame));
    }
    curves.push_back(std::move(curve));
  }
}
void applyRetime(d::TimelineClip &clip, const json &c,
                 const d::TimelineSpan &placement,
                 std::vector<d::RetimeDocument::Holder> &retimes) {
  const auto ppm = c.value("retime", json::object())
                       .value("constantRatePpm", std::int64_t(1000000));
  if (ppm == 1000000)
    return;
  videocut::contract::ClipRetimeDocument authored;
  authored.constantRatePpm = ppm;
  const auto rid = id<RetimeId>("retime-" + clip.id.value());
  d::RetimeDocument::Definition preserved{rid};
  preserved.authoredRevision = 1;
  auto owner =
      take(d::BuildAuthoredRetimeOwner(rid, {placement.begin, placement.end},
                                       {clip.source.begin, clip.source.end},
                                       authored, preserved, {rid.value()}));
  clip.retime = d::RetimeReference{rid, owner->authoredRevision()};
  retimes.push_back(std::move(owner));
}
json visualProjection(const std::optional<d::VisualPropertyModel> &value) {
  const d::VisualPropertyModel defaults;
  const auto &v = value ? *value : defaults;
  if (std::any_of(v.animations.begin(), v.animations.end(),
                  [](const auto &a) { return !a.source.empty(); }) ||
      v.cornerPin.enabled || v.fadeIn || v.fadeOut)
    throw std::runtime_error("Native corner pin / visual fade is not supported "
                             "by this browser projection");
  return {{"positionX", v.transform.positionX},
          {"positionY", v.transform.positionY},
          {"scaleX", v.transform.scaleX},
          {"scaleY", v.transform.scaleY},
          {"rotationDegrees", v.transform.rotationDegrees},
          {"opacity", v.opacity},
          {"anchorX", v.transform.anchorX},
          {"anchorY", v.transform.anchorY},
          {"flipHorizontal", v.flipHorizontal},
          {"flipVertical", v.flipVertical},
          {"fitPolicy", enumName(fitPolicies, v.fitPolicy)},
          {"blendMode", enumName(blendModes, v.blendMode)},
          {"crop",
           {{"left", v.crop.left},
            {"top", v.crop.top},
            {"right", v.crop.right},
            {"bottom", v.crop.bottom}}}};
}
json audioProjection(const std::optional<d::AudioPropertyModel> &value) {
  const d::AudioPropertyModel defaults;
  const auto &a = value ? *value : defaults;
  if (a.panEnabled || a.denoiseEnabled ||
      (a.fadeIn &&
       a.fadeIn->interpolation != d::AudioFadeInterpolation::Linear) ||
      (a.fadeOut &&
       a.fadeOut->interpolation != d::AudioFadeInterpolation::Linear))
    throw std::runtime_error("Native pan, denoise or nonlinear audio fade is "
                             "not supported by this browser projection");
  return {{"gainLinear", a.gainLinear},
          {"muted", a.muted},
          {"fadeIn", a.fadeIn ? a.fadeIn->duration.value() : 0},
          {"fadeOut", a.fadeOut ? a.fadeOut->duration.value() : 0}};
}
json curveProjection(const d::EffectMaskDocument &effects, CurveId identity) {
  const d::EffectCurve *curve = nullptr;
  for (const auto &candidate : effects.partitions().curves)
    if (candidate.id == identity)
      curve = &candidate;
  if (!curve)
    throw std::runtime_error("Native automation curve is missing");
  json frames = json::array();
  for (const auto &frame : curve->keyframes) {
    if (frame.time.time_base() != 120000 ||
        frame.time.space() != TimeSpace::Presentation ||
        !std::holds_alternative<double>(frame.value))
      throw std::runtime_error("Unsupported native automation value");
    json f = {
        {"id", frame.id.value()},
        {"time", frame.time.value()},
        {"value", std::get<double>(frame.value)},
        {"interpolation", enumName(interpolations, frame.interpolationToNext)}};
    if (frame.outgoingShape.kind ==
        videocut::contract::CurveSegmentShapeKind::CubicBezier)
      f["segment"] = {{"firstControl",
                       {{"x", frame.outgoingShape.firstControl.x},
                        {"y", frame.outgoingShape.firstControl.y}}},
                      {"secondControl",
                       {{"x", frame.outgoingShape.secondControl.x},
                        {"y", frame.outgoingShape.secondControl.y}}}};
    if (frame.incomingTangent || frame.outgoingTangent)
      throw std::runtime_error("Native Hermite tangents are not supported by "
                               "this browser projection");
    frames.push_back(f);
  }
  return {{"timeDomain", "itemLocal"}, {"keyframes", frames}};
}
json automationProjection(const d::TimelineClip &clip,
                          const d::EffectMaskDocument &effects) {
  json result = json::object();
  if (clip.visual)
    for (const auto &binding : clip.visual->automation) {
      if (binding.timeDomain != d::VisualAutomationTimeDomain::ItemLocal ||
          !binding.enabled)
        throw std::runtime_error(
            "Unsupported native visual automation binding");
      result[enumName(visualProperties, binding.property)] =
          curveProjection(effects, binding.curve);
    }
  if (clip.audio)
    for (const auto &binding : clip.audio->automation) {
      if (binding.timeDomain != d::AudioAutomationTimeDomain::ItemLocal ||
          binding.property != d::AudioPropertyIdentity::GainLinear)
        throw std::runtime_error("Unsupported native audio automation binding");
      result["audio.gainLinear"] = curveProjection(effects, binding.curve);
    }
  return result;
}

const std::map<std::string, videocut::text::FontSlant> plainFontSlants = {
    {"upright", videocut::text::FontSlant::Upright},
    {"italic", videocut::text::FontSlant::Italic},
    {"oblique", videocut::text::FontSlant::Oblique}};

json systemFontProjection(const videocut::text::FontReference &font) {
  if (font.kind != videocut::text::FontSourceKind::System ||
      !font.assetId.empty() || !font.digest.empty() ||
      !font.variationAxes.empty() || font.faceFingerprint.size() != 32 ||
      !std::all_of(font.faceFingerprint.begin(), font.faceFingerprint.end(),
                   [](char c) { return (c >= 'a' && c <= 'f') ||
                                       (c >= '0' && c <= '9'); }))
    throw std::runtime_error("Native text font has no supported system identity");
  return {{"family", font.family},
          {"postscriptName", font.postscriptName},
          {"weight", font.weight},
          {"width", font.width},
          {"slant", enumName(plainFontSlants, font.slant)},
          {"sourceFaceIndex", font.faceIndex},
          {"platform", font.platform},
          {"identity", font.faceFingerprint}};
}

void applyTextLayoutWidth(videocut::text_composition::TextCompositionDocument &document,
                          double width, const json &canvas) {
  const auto &reference = document.presentation.referenceCanvas;
  const auto scale = std::min(canvas.at("width").get<double>() / reference.width,
                              canvas.at("height").get<double>() / reference.height);
  auto &box = document.presentation.authoredLayoutFrame;
  const auto next = float(width / scale);
  box.x += (box.width - next) / 2;
  box.width = next;
  box.sizingMode = videocut::text::TextLayoutSizingMode::AutoHeight;
  box.clipOverflow = false;
  for (auto &paragraph : document.content) {
    paragraph.style.wrap = videocut::text::TextWrap::Word;
    paragraph.style.maximumLines.reset();
  }
}

videocut::text_composition::TextCompositionDocument
plainTextDocument(const json &c, const json &canvas) {
  videocut::text_composition::TextCompositionDocument result;
  result.durationTicks = c.at("source").at("end");
  auto &presentation = result.presentation;
  presentation.referenceCanvas.width = canvas.at("width");
  presentation.referenceCanvas.height = canvas.at("height");
  presentation.authoredLayoutFrame.width = presentation.referenceCanvas.width;
  presentation.authoredLayoutFrame.height = presentation.referenceCanvas.height;
  namespace txt = videocut::text;
  if (!c.at("text").contains("font"))
    throw std::runtime_error("Resolve the system font before saving basic text");
  const auto &authoredFont = c.at("text").at("font");
  txt::FontReference font;
  font.kind = txt::FontSourceKind::System;
  font.family = authoredFont.at("family");
  font.postscriptName = authoredFont.at("postscriptName");
  font.weight = authoredFont.at("weight");
  font.width = authoredFont.at("width");
  font.slant = plainFontSlants.at(authoredFont.at("slant").get<std::string>());
  font.faceIndex = authoredFont.at("sourceFaceIndex");
  font.platform = authoredFont.at("platform");
  font.faceFingerprint = authoredFont.at("identity");
  if (c.at("text").at("fontFamily") != font.family ||
      systemFontProjection(font) != authoredFont)
    throw std::runtime_error("Basic text system font identity is inconsistent");
  txt::TextContentSlot slot;
  slot.slotId = "primary";
  slot.semanticRole = "primary";
  std::istringstream lines(c.at("text").at("content").get<std::string>());
  std::string line;
  int paragraphIndex = 0;
  while (std::getline(lines, line)) {
    txt::RichTextParagraph paragraph;
    paragraph.paragraphId = "paragraph-" + std::to_string(paragraphIndex++);
    paragraph.style.wrap = txt::TextWrap::None;
    txt::RichTextRun run;
    run.runId = "run-" + paragraph.paragraphId;
    run.utf8Text = line.empty() ? " " : line;
    run.style.fontSize = c.at("text").at("fontSize");
    run.style.font.primary = font;
    run.style.font.family = font.family;
    run.style.font.postscriptName = font.postscriptName;
    run.style.font.weight = font.weight;
    run.style.font.width = font.width;
    run.style.font.slant = font.slant;
    run.style.font.faceIndex = font.faceIndex;
    run.style.font.allowSystemGlyphFallback = false;
    const auto rgb = std::stoul(
        c.at("text").at("color").get<std::string>().substr(1), nullptr, 16);
    txt::SolidTextMaterial solid;
    solid.color = {float((rgb >> 16) & 255) / 255,
                   float((rgb >> 8) & 255) / 255, float(rgb & 255) / 255, 1};
    txt::TextFillLayer fill;
    fill.material = txt::LiteralTextMaterial{solid};
    run.style.materials.layers = {fill};
    slot.paragraphIds.push_back(paragraph.paragraphId);
    slot.runIds.push_back(run.runId);
    paragraph.runs.push_back(std::move(run));
    result.content.push_back(std::move(paragraph));
  }
  result.contentSlots.push_back(std::move(slot));
  if (c.at("text").contains("layoutWidth"))
    applyTextLayoutWidth(result, c.at("text").at("layoutWidth"), canvas);
  return result;
}
