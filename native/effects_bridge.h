#pragma once
#include "projection_bridge.h"
#include "videocut/editor/application/EffectCatalogAdmission.h"
#include "videocut/fx/BuiltinCatalog.h"
#include <cmath>
#include <set>

// Format 1 owns canonical admitted definitions/programs and typed graph
// attachments. This adapter only accepts the exact browser subset; unsupported
// authored graphs are rejected rather than flattened or silently discarded.
namespace webfx {
inline std::string identity(const std::string &value) {
  return "webfx." + videocut::base::Sha256::HexDigest(value).substr(0, 40);
}
template <class Storage, class Id>
const auto &find(const Storage &rows, const Id &key) {
  for (const auto &row : rows)
    if (row.id == key)
      return row;
  throw std::runtime_error("Effect reference is missing: " + key.value());
}
inline const application::EffectCatalogAdmission &catalog() {
  static const auto value =
      take(application::EffectCatalogAdmissionService::Build(
          *videocut::fx::BuiltinEffectCatalog()));
  return value;
}
inline std::string operation(const std::string &name) {
  if (name == "blur")
    return "com.videocut.effect.blur";
  if (name == "glow")
    return "com.videocut.effect.creative-lab";
  if (name == "lut")
    return "com.videocut.effect.looks-lut";
  if (name == "dissolve" || name == "fade" || name == "wipe" || name == "slide")
    return "com.videocut.transition.standard";
  throw std::runtime_error("Unsupported effect template: " + name);
}
using Values = std::map<std::string, d::EffectParameterValue>;
inline Values values(const json &effect) {
  const auto name = effect.at("templateId").get<std::string>();
  const auto &p = effect.at("parameters");
  if (name == "blur") {
    const double radius = p.at("radius");
    if (radius < 0 || radius > 64 || radius != std::round(radius))
      throw std::runtime_error(
          "Blur radius must be an integer from 0 to 64 pixels");
    return {{"mode", std::string("gaussian")},
            {"radius_pixels", std::int64_t(radius)},
            {"sigma_pixels", 0.0},
            {"intensity", 1.0}};
  }
  if (name == "glow")
    return {{"style", std::string("bloom")},
            {"radius_pixels", p.at("radius").get<double>()},
            {"amount", p.at("strength").get<double>()}};
  if (name == "lut") {
    const std::map<std::string, std::string> looks{
        {"warm", "warm_film"}, {"cool", "cool_fade"}, {"cinema", "cinematic"}};
    return {{"look", looks.at(p.at("preset").get<std::string>())},
            {"interpolation", std::string("trilinear")},
            {"intensity", p.at("amount").get<double>()}};
  }
  const std::map<std::string, std::string> styles{
      {"dissolve", "cross_dissolve"},
      {"fade", "fade_color"},
      {"wipe", "wipe"},
      {"slide", "slide"}};
  Values transition{{"style", styles.at(name)},
                    {"direction", p.value("direction", std::string("right"))}};
  if (name == "fade") {
    const auto color = p.value("color", std::string("#000000"));
    if (color != "#000000" && color != "#ffffff")
      throw std::runtime_error("Only black/white fade colors are supported");
    const double channel = color == "#ffffff" ? 1 : 0;
    transition.emplace("color",
                       d::EffectColorValue{{channel, channel, channel, 1}});
  }
  return transition;
}
inline d::EffectInstance instance(const json &effect,
                                  const EffectGraphId &graph,
                                  const EffectInstanceId &instanceId) {
  const auto &admission = catalog();
  const auto *entry = admission.Find(operation(effect.at("templateId")), 1);
  if (!entry)
    throw std::runtime_error("Builtin effect is unavailable");
  const auto &definition =
      find(admission.replacement.definitions, entry->definition);
  const auto &program = find(admission.replacement.programs, entry->program);
  d::EffectInstance result{
      instanceId,
      graph,
      {definition.id, definition.schemaMajor},
      {program.id, program.authoredRevision, program.contentDigest}};
  result.execution.enabled = effect.value("enabled", true);
  const auto supplied = values(effect);
  for (const auto &parameter : definition.parameters) {
    const auto found = supplied.find(parameter.semanticIdentity);
    if (found != supplied.end())
      result.parameters.push_back({parameter.id, found->second, {}});
    else if (parameter.defaultValue)
      result.parameters.push_back({parameter.id, *parameter.defaultValue, {}});
    else if (parameter.required)
      throw std::runtime_error("Unsupported required effect parameter: " +
                               parameter.semanticIdentity);
  }
  return result;
}
inline d::EffectGraph graph(const EffectGraphId &key, d::EffectGraphOwner owner,
                            const std::vector<d::EffectInstance> &instances,
                            const d::EffectMaskDocument::Partitions &effects) {
  d::EffectGraph result{key, std::move(owner)};
  std::optional<d::EffectInstancePortReference> prior;
  for (const auto &instance : instances) {
    result.instances.push_back(instance.id);
    const auto &program = find(effects.programs, instance.program.id);
    if (program.outputs.size() != 1 || program.inputs.empty())
      throw std::runtime_error("Unsupported builtin effect port contract");
    for (std::size_t i = 0; i < program.inputs.size(); ++i) {
      const auto &port = program.inputs[i];
      const d::EffectInstancePortReference destination{instance.id, port.id};
      if (prior && i == 0)
        result.connections.push_back({*prior, destination});
      else {
        auto input = port;
        input.id = id<EntityId>(
            identity(key.value() + ":input:" + port.semanticIdentity));
        result.inputs.push_back(input);
        result.connections.push_back(
            {d::EffectGraphExternalPortReference{input.id}, destination});
      }
    }
    prior =
        d::EffectInstancePortReference{instance.id, program.outputs.front().id};
  }
  if (prior) {
    auto output =
        find(effects.programs, instances.back().program.id).outputs.front();
    output.id = id<EntityId>(identity(key.value() + ":output"));
    result.outputs.push_back(output);
    result.connections.push_back(
        {*prior, d::EffectGraphExternalPortReference{output.id}});
  }
  return result;
}
inline json parameterValue(const d::EffectParameterValue &value) {
  return std::visit(
      [](const auto &v) -> json {
        using V = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<V, d::EffectVectorValue>)
          return v.components;
        else if constexpr (std::is_same_v<V, d::EffectColorValue>)
          return v.rgba;
        else if constexpr (std::is_same_v<V, RationalTime>)
          return {{"value", v.value()},
                  {"timeBase", v.time_base()},
                  {"space", static_cast<int>(v.space())}};
        else
          return v;
      },
      value);
}
inline bool same(const d::EffectParameterValue &a,
                 const d::EffectParameterValue &b) {
  const auto x = parameterValue(a), y = parameterValue(b);
  // Float32 catalog defaults are widened to doubles during admission.
  if (x.is_number() && y.is_number())
    return std::abs(x.get<double>() - y.get<double>()) < 1e-7;
  return x == y;
}
inline json importInstance(const d::EffectInstance &instance,
                           const d::EffectMaskDocument::Partitions &effects) {
  if (instance.execution.bypassed ||
      instance.execution.stage != d::EffectProcessingStage::Main ||
      instance.execution.bypassBehavior !=
          d::EffectBypassBehavior::PassThroughPrimary ||
      instance.activeRange || !instance.resources.empty() ||
      !instance.brushes.empty())
    throw std::runtime_error(
        "Effect execution, range or resources are outside the browser subset");
  const auto &definition = find(effects.definitions, instance.definition.id);
  const auto *admitted =
      catalog().Find(definition.operationIdentity, definition.schemaMajor);
  if (!admitted)
    throw std::runtime_error(
        "Effect definition is outside the browser subset: " +
        definition.operationIdentity);
  const auto &program = find(effects.programs, instance.program.id);
  const auto &canonical =
      find(catalog().replacement.programs, admitted->program);
  if (program.contentDigest != canonical.contentDigest ||
      instance.program.contentDigest != canonical.contentDigest)
    throw std::runtime_error(
        "Effect executable does not match the installed canonical builtin");
  Values parameters;
  for (const auto &p : definition.parameters)
    if (p.defaultValue)
      parameters.emplace(p.semanticIdentity, *p.defaultValue);
  for (const auto &binding : instance.parameters) {
    const auto &parameter = find(definition.parameters, binding.parameter);
    if (binding.curve &&
        (definition.operationIdentity != operation("lut") ||
         parameter.semanticIdentity != "intensity" || !parameter.animatable))
      throw std::runtime_error(
          "Only canonical LUT intensity supports browser effect automation");
    parameters.insert_or_assign(parameter.semanticIdentity, binding.value);
  }
  auto text = [&](const char *name) {
    return std::get<std::string>(parameters.at(name));
  };
  auto number = [&](const char *name) {
    return parameterValue(parameters.at(name)).get<double>();
  };
  std::string name;
  json p = json::object();
  if (definition.operationIdentity == operation("blur")) {
    name = "blur";
    p = {{"radius", number("radius_pixels")}};
  } else if (definition.operationIdentity == operation("glow")) {
    name = "glow";
    p = {{"radius", number("radius_pixels")}, {"strength", number("amount")}};
  } else if (definition.operationIdentity == operation("lut")) {
    name = "lut";
    const std::map<std::string, std::string> presets{
        {"warm_film", "warm"}, {"cool_fade", "cool"}, {"cinematic", "cinema"}};
    p = {{"preset", presets.at(text("look"))}, {"amount", number("intensity")}};
  } else if (definition.operationIdentity == operation("dissolve")) {
    const std::map<std::string, std::string> styles{
        {"cross_dissolve", "dissolve"},
        {"fade_color", "fade"},
        {"wipe", "wipe"},
        {"slide", "slide"}};
    name = styles.at(text("style"));
    if (name == "wipe" || name == "slide")
      p["direction"] = text("direction");
    if (name == "fade") {
      const auto color =
          std::get<d::EffectColorValue>(parameters.at("color")).rgba;
      if (color == std::array<double, 4>{0, 0, 0, 1})
        p["color"] = "#000000";
      else if (color == std::array<double, 4>{1, 1, 1, 1})
        p["color"] = "#ffffff";
      else
        throw std::runtime_error(
            "Native fade color is outside the supported black/white presets");
    }
  } else
    throw std::runtime_error("Unsupported effect operation: " +
                             definition.operationIdentity);
  const json result = {{"id", instance.id.value()},
                       {"templateId", name},
                       {"enabled", instance.execution.enabled},
                       {"parameters", p}};
  const auto expected = webfx::instance(result, instance.graph, instance.id);
  const auto &expectedDefinition =
      find(catalog().replacement.definitions, admitted->definition);
  Values expectedValues;
  for (const auto &binding : expected.parameters)
    expectedValues.emplace(
        find(expectedDefinition.parameters, binding.parameter).semanticIdentity,
        binding.value);
  for (const auto &[key, value] : parameters) {
    const auto found = expectedValues.find(key);
    if (found == expectedValues.end() || !same(value, found->second))
      throw std::runtime_error("Effect parameter cannot be represented without "
                               "changing appearance: " +
                               key);
  }
  return result;
}
inline void validateGraph(const d::EffectGraph &value,
                          const d::EffectMaskDocument::Partitions &effects) {
  if (!value.execution.enabled || value.execution.bypassed ||
      value.execution.stage != d::EffectProcessingStage::Main ||
      value.execution.bypassBehavior !=
          d::EffectBypassBehavior::PassThroughPrimary)
    throw std::runtime_error("Unsupported graph execution state");
  std::vector<d::EffectInstance> instances;
  for (const auto &key : value.instances)
    instances.push_back(find(effects.instances, key));
  const auto expected = graph(value.id, value.owner, instances, effects);
  if (value.inputs.size() != expected.inputs.size() ||
      value.outputs.size() != 1 ||
      value.connections.size() != expected.connections.size())
    throw std::runtime_error("Unsupported effect graph topology");
  auto signature = [&](const auto &edge, const d::EffectGraph &owner) {
    auto endpoint = [&](const auto &reference, bool output) -> std::string {
      return std::visit(
          [&](const auto &r) -> std::string {
            using R = std::decay_t<decltype(r)>;
            if constexpr (std::is_same_v<R,
                                         d::EffectGraphExternalPortReference>) {
              const auto &port =
                  find(output ? owner.outputs : owner.inputs, r.port);
              // Port semantic tags are the authored from/to contract for
              // transitions.
              return std::string(output ? "out:" : "in:") +
                     port.semanticIdentity + ":" +
                     json(port.semanticTags).dump();
            } else {
              return "instance:" + r.instance.value() + ":" + r.port.value();
            }
          },
          reference);
    };
    return endpoint(edge.source, false) + ">" +
           endpoint(edge.destination, true);
  };
  std::multiset<std::string> actual, wanted;
  for (const auto &edge : value.connections)
    actual.insert(signature(edge, value));
  for (const auto &edge : expected.connections)
    wanted.insert(signature(edge, expected));
  if (actual != wanted)
    throw std::runtime_error("Unsupported effect graph connections");
}
} // namespace webfx

inline void applyEffectAutomation(const json &authored,
                                  const d::TimelineClip &clip,
                                  std::vector<d::EffectInstance> &instances,
                                  std::vector<d::EffectCurve> &curves) {
  const auto automation = authored.value("automation", json::object());
  for (const auto &[path, binding] : automation.items()) {
    if (path.rfind("effects.", 0) != 0)
      continue;
    const auto split = path.rfind('.');
    const auto instanceName = path.substr(8, split - 8),
               parameterName = path.substr(split + 1);
    auto instance =
        std::find_if(instances.begin(), instances.end(), [&](const auto &v) {
          return v.id.value() == instanceName;
        });
    if (instance == instances.end() || parameterName != "amount" ||
        binding.at("timeDomain") != "itemLocal")
      throw std::runtime_error(
          "Unsupported effect automation target or time domain: " + path);
    const auto &definition = webfx::find(
        webfx::catalog().replacement.definitions, instance->definition.id);
    if (definition.operationIdentity != webfx::operation("lut"))
      throw std::runtime_error("Builtin effect parameter is not animatable: " +
                               path);
    const auto *entry = webfx::catalog().Find(definition.operationIdentity, 1);
    const auto parameter = entry->parameters.at("intensity");
    const auto curveId =
        id<CurveId>(webfx::identity(clip.id.value() + ":" + path));
    d::EffectCurve curve{
        curveId,
        d::EffectParameterCurveTarget{instance->graph, instance->id, parameter},
        {}};
    const auto offset =
        clip.type == d::ClipType::Text ? clip.source.begin.value() : 0;
    for (const auto &key : binding.at("keyframes")) {
      d::EffectCurveKeyframe frame{
          id<EntityId>(key.at("id")),
          time(key.at("time").get<std::int64_t>() + offset,
               TimeSpace::Presentation),
          key.at("value").get<double>(),
          interpolations.at(key.at("interpolation").get<std::string>())};
      if (key.contains("segment")) {
        const auto &s = key.at("segment");
        frame.outgoingShape.kind =
            videocut::contract::CurveSegmentShapeKind::CubicBezier;
        frame.outgoingShape.firstControl = {s.at("firstControl").at("x"),
                                            s.at("firstControl").at("y")};
        frame.outgoingShape.secondControl = {s.at("secondControl").at("x"),
                                             s.at("secondControl").at("y")};
      }
      curve.keyframes.push_back(std::move(frame));
    }
    for (auto &value : instance->parameters)
      if (value.parameter == parameter)
        value.curve = curveId;
    curves.push_back(std::move(curve));
  }
}

inline void applyNativeEffects(const json &p,
                               d::TimelineDocument::Partitions &timeline,
                               d::EffectMaskDocument::Partitions &effects) {
  bool needed = !p.at("timeline").value("transitions", json::array()).empty();
  for (const auto &track : p.at("timeline").at("tracks"))
    for (const auto &item : track.at("items"))
      needed =
          needed || !item.at("clip").value("effects", json::array()).empty();
  if (!needed)
    return;
  const auto &catalog = webfx::catalog().replacement;
  effects.catalogRevision = catalog.sourceRevision;
  effects.catalogFingerprint = catalog.sourceFingerprint;
  effects.capabilityProfiles =
      ImmutableIndexedStorage<d::EffectCapabilityProfile>(
          catalog.capabilityProfiles);
  effects.assets = ImmutableIndexedStorage<d::EffectAsset>(catalog.assets);
  effects.definitions =
      ImmutableIndexedStorage<d::EffectDefinition>(catalog.definitions);
  effects.programs =
      ImmutableIndexedStorage<d::EffectProgram>(catalog.programs);
  std::map<std::string, json> authored;
  std::map<std::string, json> authoredClips;
  std::map<std::string, std::int64_t> cuts;
  for (const auto &track : p.at("timeline").at("tracks"))
    for (const auto &item : track.at("items")) {
      authored.emplace(item.at("id"),
                       item.at("clip").value("effects", json::array()));
      authoredClips.emplace(item.at("id"), item.at("clip"));
      cuts.emplace(item.at("id"), item.at("placement").at("end"));
    }
  std::vector<d::EffectGraph> graphs;
  std::vector<d::EffectInstance> instances;
  std::vector<d::EffectCurve> curves;
  for (const auto &curve : effects.curves)
    curves.push_back(curve);
  std::vector<d::TimelineTrack::Holder> tracks;
  for (const auto &original : timeline.tracks) {
    auto track = std::make_shared<d::TimelineTrack>(*original);
    std::vector<d::TimelineItem::Holder> items;
    for (const auto &source : track->items) {
      const auto found = authored.find(source->id.value());
      if (found == authored.end() || found->second.empty()) {
        items.push_back(source);
        continue;
      }
      auto item = std::make_shared<d::TimelineItem>(*source);
      auto clip = std::make_shared<d::TimelineClip>(*source->clip);
      const auto key =
          id<EffectGraphId>(webfx::identity("clip:" + clip->id.value()));
      std::vector<d::EffectInstance> ordered;
      for (const auto &fx : found->second)
        ordered.push_back(
            webfx::instance(fx, key, id<EffectInstanceId>(fx.at("id"))));
      applyEffectAutomation(authoredClips.at(source->id.value()), *clip,
                            ordered, curves);
      graphs.push_back(webfx::graph(key, d::ClipEffectGraphOwner{clip->id},
                                    ordered, effects));
      instances.insert(instances.end(), ordered.begin(), ordered.end());
      clip->effectGraph = key;
      item->clip = clip;
      items.push_back(item);
    }
    track->items =
        ImmutableIndexedStorage<d::TimelineItem::Holder>(std::move(items));
    tracks.push_back(track);
  }
  timeline.tracks =
      ImmutableIndexedStorage<d::TimelineTrack::Holder>(std::move(tracks));
  std::vector<d::TimelineTransition> transitions;
  for (const auto &tr : p.at("timeline").value("transitions", json::array())) {
    const auto transition = id<TransitionId>(tr.at("id"));
    const auto key =
        id<EffectGraphId>(webfx::identity("transition:" + transition.value()));
    const auto instance =
        webfx::instance(tr, key,
                        id<EffectInstanceId>(webfx::identity(
                            "transition-instance:" + transition.value())));
    graphs.push_back(webfx::graph(
        key, d::TransitionEffectGraphOwner{transition}, {instance}, effects));
    instances.push_back(instance);
    const std::int64_t length = tr.at("duration"),
                       cut = cuts.at(tr.at("fromItemId"));
    const auto begin = cut - length / 2;
    transitions.push_back({transition,
                           id<ItemId>(tr.at("fromItemId")),
                           id<ItemId>(tr.at("toItemId")),
                           {time(begin, TimeSpace::Timeline),
                            time(begin + length, TimeSpace::Timeline)},
                           d::TransitionChannel::Visual,
                           key,
                           {}});
  }
  timeline.transitions =
      ImmutableIndexedStorage<d::TimelineTransition>(std::move(transitions));
  effects.graphs = ImmutableIndexedStorage<d::EffectGraph>(std::move(graphs));
  effects.instances =
      ImmutableIndexedStorage<d::EffectInstance>(std::move(instances));
  effects.curves = ImmutableIndexedStorage<d::EffectCurve>(std::move(curves));
}
inline json importEffectsAutomation(const d::TimelineClip &clip,
                                    const d::EffectMaskDocument &effects) {
  auto result = json::object();
  if (!clip.effectGraph)
    return result;
  const auto &parts = effects.partitions();
  const auto &graph = webfx::find(parts.graphs, *clip.effectGraph);
  const auto offset =
      clip.type == d::ClipType::Text ? clip.source.begin.value() : 0;
  for (const auto &identity : graph.instances) {
    const auto &instance = webfx::find(parts.instances, identity);
    const auto &definition =
        webfx::find(parts.definitions, instance.definition.id);
    for (const auto &binding : instance.parameters)
      if (binding.curve) {
        const auto &parameter =
            webfx::find(definition.parameters, binding.parameter);
        if (definition.operationIdentity != webfx::operation("lut") ||
            parameter.semanticIdentity != "intensity")
          throw std::runtime_error(
              "Unsupported native effect automation parameter");
        const auto &curve = webfx::find(parts.curves, *binding.curve);
        const auto *target =
            std::get_if<d::EffectParameterCurveTarget>(&curve.target);
        if (!target || target->graph != graph.id ||
            target->instance != instance.id ||
            target->parameter != binding.parameter)
          throw std::runtime_error("Effect automation target mismatch");
        auto projected = curveProjection(effects, *binding.curve);
        for (auto &frame : projected["keyframes"]) {
          const auto t = frame.at("time").get<std::int64_t>() - offset;
          if (t < 0)
            throw std::runtime_error(
                "Text effect keyframe precedes its authored source offset");
          frame["time"] = t;
        }
        result["effects." + instance.id.value() + ".amount"] =
            std::move(projected);
      }
  }
  return result;
}
inline json importClipEffects(const d::TimelineClip &clip,
                              const d::EffectMaskDocument &effects) {
  auto result = json::array();
  if (!clip.effectGraph)
    return result;
  const auto &parts = effects.partitions();
  const auto &graph = webfx::find(parts.graphs, *clip.effectGraph);
  const auto *owner = std::get_if<d::ClipEffectGraphOwner>(&graph.owner);
  if (!owner || owner->clip != clip.id)
    throw std::runtime_error("Effect graph clip owner mismatch");
  webfx::validateGraph(graph, parts);
  for (const auto &key : graph.instances)
    result.push_back(
        webfx::importInstance(webfx::find(parts.instances, key), parts));
  return result;
}
inline json importTransitions(const d::TimelineDocument &timeline,
                              const d::EffectMaskDocument &effects) {
  auto result = json::array();
  const auto &parts = effects.partitions();
  for (const auto &transition : timeline.partitions().transitions) {
    if (transition.channel != d::TransitionChannel::Visual ||
        transition.mixCurve)
      throw std::runtime_error(
          "Unsupported audio transition or custom transition curve");
    const auto &graph = webfx::find(parts.graphs, transition.effectGraph);
    const auto *owner =
        std::get_if<d::TransitionEffectGraphOwner>(&graph.owner);
    if (!owner || owner->transition != transition.id ||
        graph.instances.size() != 1)
      throw std::runtime_error(
          "Unsupported transition graph owner or cardinality");
    webfx::validateGraph(graph, parts);
    auto fx = webfx::importInstance(
        webfx::find(parts.instances, graph.instances.front()), parts);
    if (!fx.at("enabled").get<bool>())
      throw std::runtime_error(
          "Disabled transition cannot be represented in the browser");
    const auto position = timeline.FindItemPosition(transition.fromItem);
    if (!position)
      throw std::runtime_error("Transition endpoint is missing");
    const auto &from =
        timeline.partitions().tracks[position->first]->items[position->second];
    const auto begin = transition.placement.begin.value(),
               end = transition.placement.end.value(),
               cut = from->placement.end.value();
    if (transition.placement.begin.time_base() != 120000 ||
        transition.placement.end.time_base() != 120000 ||
        begin != cut - (end - begin) / 2)
      throw std::runtime_error(
          "Only centered canonical-timebase transitions are supported");
    result.push_back({{"id", transition.id.value()},
                      {"fromItemId", transition.fromItem.value()},
                      {"toItemId", transition.toItem.value()},
                      {"templateId", fx.at("templateId")},
                      {"duration", end - begin},
                      {"parameters", fx.at("parameters")}});
  }
  return result;
}
