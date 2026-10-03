#include "package_decoder.h"
#include "composition_plan.h"
#include "videocut/text_composition/TextTemplatePatch.h"
#include "videocut/text_composition/TextCompositionSnapshot.h"
#include "videocut/skia_runtime/SkiaRuntimeFactory.h"
#include "videocut/base/Sha256.h"
#include "internal/Factories.h"
#include "text/TextRenderPipeline.h"
#include "include/core/SkPathBuilder.h"
#include "src/core/SkOpts.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Json = nlohmann::json;
namespace txt = videocut::text;
namespace comp = videocut::text_composition;
namespace vsk = videocut::skia_runtime;
class Assets final : public vsk::RuntimeAssetResolver {
public:
  std::map<std::string, vsk::RuntimeAsset> values;
  bool Resolve(vsk::RuntimeAssetKind, const std::string& id, vsk::RuntimeAsset& out,
               std::string& error) const override {
    auto it = values.find(id);
    if (it == values.end()) { error = "Missing registered asset: " + id; return false; }
    out = it->second; return true;
  }
};
struct Renderer {
  std::shared_ptr<Assets> assets = std::make_shared<Assets>();
  txt::TextRenderLaneHolder lane;
  comp::TextCompositionDocument document;
  videocut::frame::VideoFrame frame;
  videocut::frame::CpuReadMap read;
  const std::byte* pixels = nullptr;
  Json warnings = Json::array();
  std::string result = "{}";
  bool loaded = false;
  bool externalComposition = false;
  vsk::internal::TextSdfGpuMesh sdfMesh;
};
std::map<unsigned, std::unique_ptr<Renderer>> renderers;
unsigned nextId = 1;
std::string globalError = "{}";
Renderer& Get(unsigned handle) {
  auto it = renderers.find(handle);
  if (it == renderers.end()) throw std::invalid_argument("Renderer is disposed or invalid");
  return *it->second;
}
template<class T> std::string Diagnostics(const T& values) {
  std::string result;
  for (const auto& d : values) { if (!result.empty()) result += "; "; result += d.code + ": " + d.message; }
  return result;
}
Json Rect(const txt::Rect& r) { return {{"x",r.x},{"y",r.y},{"width",r.width},{"height",r.height}}; }
template<class F> int Guard(unsigned handle, F&& f) {
  try { f(Get(handle)); return 1; }
  catch (const std::exception& e) {
    const auto result = Json{{"ok",false},{"error",e.what()}}.dump();
    auto it = renderers.find(handle);
    if (it != renderers.end()) it->second->result = result; else globalError = result;
    return 0;
  }
}
}
extern "C" {
unsigned vct_raster_lanes() { return SkOpts::raster_pipeline_highp_stride; }
unsigned vct_create() {
  if (renderers.size() >= 32) return 0;
  const auto id = nextId++;
  renderers.emplace(id, std::make_unique<Renderer>());
  return id;
}
void vct_destroy(unsigned handle) { renderers.erase(handle); }
const char* vct_result(unsigned handle) {
  auto it = renderers.find(handle);
  return it == renderers.end() ? globalError.c_str() : it->second->result.c_str();
}
int vct_register_asset(unsigned handle, const char* id, const char* mediaType,
                       const unsigned char* bytes, unsigned length) {
  return Guard(handle, [&](Renderer& r) {
    if (!id || !mediaType || !bytes || length == 0 || length > 64U*1024U*1024U)
      throw std::invalid_argument("Invalid asset or asset exceeds 64 MiB");
    if (r.loaded) throw std::invalid_argument("Register assets before loading a document");
    std::size_t total = length;
    for (const auto& [key, value] : r.assets->values) if (key != id) total += value.bytes->size();
    if (total > 256U*1024U*1024U) throw std::invalid_argument("Asset budget exceeds 256 MiB");
    auto owned = std::make_shared<const std::vector<std::uint8_t>>(bytes, bytes+length);
    auto digest = std::string("sha256:") + videocut::base::Sha256::HexDigest(
        std::string_view(reinterpret_cast<const char*>(owned->data()), owned->size()));
    r.assets->values[id] = vsk::RuntimeAsset{id, mediaType, digest, owned};
    r.result = Json{{"ok",true},{"digest",digest}}.dump();
  });
}
int vct_load(unsigned handle, const char* composition, const char* animation,
             const char* effect, const char* bindings, int allowRasterFallback) {
  return Guard(handle, [&](Renderer& r) {
    r.pixels = nullptr;
    auto patch = comp::ParseTextTemplateJson(composition ? composition : "");
    if (!patch.valid) throw std::invalid_argument(Diagnostics(patch.diagnostics));
    std::string error;
    if (!videocut::text_wasm::DecodePackageAnimation(Json::parse(effect), Json::parse(animation), patch.value.value, error))
      throw std::invalid_argument(error);
    // This is a WASM instance admission identity, not the desktop catalog's
    // package digest. Include every registered immutable asset in the closure.
    Json identities = Json::object();
    for (const auto& [id, asset] : r.assets->values) identities[id] = asset.contentDigest;
    const auto closure = Json{{"profile","videocut.text-wasm.package.v1"},
      {"composition",Json::parse(composition)},{"animation",Json::parse(animation)},
      {"effect",Json::parse(effect)},{"assets",std::move(identities)}}.dump();
    patch.value.packageDigest = "sha256:" + videocut::base::Sha256::HexDigest(closure);
    patch.value.value.templateOrigin = comp::TextTemplateOrigin{patch.value.templateId, patch.value.packageDigest};
    auto doc = patch.value.value;
    const auto overrides = Json::parse(bindings).get<std::map<std::string,std::string>>();
    auto applied = comp::ApplyTextTemplatePatch(doc, patch.value, overrides);
    if (!applied.valid) throw std::invalid_argument(Diagnostics(applied.diagnostics));
    Json warnings = Json::array();
    const bool externalComposition = (allowRasterFallback & 2) != 0;
    if (doc.presentation.appearance.sdfMaterial.enabled) {
      if (!(allowRasterFallback & 1)) throw std::invalid_argument("SDF/Metal fidelity is unavailable in wasm-raster-v1. Explicitly enable allowRasterFallback for portable Skia rendering.");
      warnings.push_back({{"code","portable-raster"},{"message","Uses the SDK's portable Skia glyph path; native Metal SDF pixels are not reproduced."}});
    }
    if (externalComposition) {
      videocut::text_wasm::ValidateBrowserComposition(doc);
      warnings.push_back({{"code","external-composition"},{"message","Text raster requires the browser compositor: native decoration/parameter sampling, browser video decoding, and output-canvas WebGPU post effects. Full native render-group/SDF parity is not claimed."}});
    }
    if (!externalComposition && !doc.decorations.empty()) throw std::invalid_argument("Composition-owned vector decorations are not supported by wasm-raster-v1");
    for (const auto& layer : doc.animations.layers) {
      if (!externalComposition && !layer.postEffects.empty()) throw std::invalid_argument("Native post-effects are not supported by wasm-raster-v1");
    }
    const auto snap = comp::BuildTextCompositionSnapshot(doc, {0,0,doc.durationTicks*25/3,0,0,doc.durationTicks});
    if (!snap.valid || !snap.snapshot) throw std::invalid_argument(Diagnostics(snap.diagnostics));
    vsk::SkiaRuntimeConfig config; config.assets = r.assets; config.allowSystemFonts = false;
    const auto factory = vsk::internal::MakeTextRendererFactory(config,error);
    if (!factory) throw std::runtime_error(error);
    txt::TextRenderOptions options; options.maximumCacheBytes = 32U*1024U*1024U;
    options.maximumCachedFrames = 8;
    auto lane = factory->CreateLane(options,error);
    if (!lane) throw std::runtime_error(error);
    txt::TextRenderDocument renderDoc;
    renderDoc.text = snap.snapshot->resolvedText; renderDoc.appearance = snap.snapshot->appearance;
    renderDoc.animations = snap.snapshot->animations; renderDoc.timedSpans = snap.snapshot->timedSpans;
    if (externalComposition) {
      renderDoc.animations = comp::BuildTextLaneAnimations(renderDoc.animations, doc.decorations);
      // This exact, admitted studio topology is split into text animation,
      // decoration composition, and the single downstream post pass.
      renderDoc.animations.executionGraph.nodes.clear();
      for (auto& layer : renderDoc.animations.layers) layer.postEffects.clear();
    }
    if (!lane->ReplaceDocument(std::move(renderDoc),error)) throw std::runtime_error(error);
    r.lane = std::move(lane); r.document = std::move(doc); r.warnings = std::move(warnings); r.loaded = true;
    r.externalComposition = externalComposition;
    r.frame = {}; r.read = {};
    r.result = Json{{"ok",true},{"profile","wasm-raster-v1"},{"warnings",r.warnings},
      {"durationUs",r.document.durationTicks*25/3},{"requiresBrowserComposition",externalComposition},
      {"referenceWidth",r.document.presentation.referenceCanvas.width},
      {"referenceHeight",r.document.presentation.referenceCanvas.height}}.dump();
  });
}
int vct_render(unsigned handle, double timeUs, unsigned width, unsigned height) {
  return Guard(handle, [&](Renderer& r) {
    r.pixels = nullptr;
    if (!r.loaded || !std::isfinite(timeUs) || timeUs < 0 || timeUs > 86400000000.0 ||
        width < 1 || height < 1 || width > 4096 || height > 4096 || std::uint64_t(width)*height > 8388608)
      throw std::invalid_argument("Invalid render request (maximum 4096 per dimension, 8M pixels)");
    txt::TextRenderRequest request; request.localTimeUs = static_cast<std::int64_t>(timeUs);
    request.durationUs = r.document.durationTicks*25/3;
    request.outputWidth = width; request.outputHeight = height;
    request.quality = txt::RenderQuality::PreviewPaused;
    auto frame = r.lane->Render(request);
    if (!frame) throw std::runtime_error(Diagnostics(frame.diagnostics));
    r.frame = std::move(frame.image);
    auto mapped = r.frame.mapRead();
    if (!mapped) throw std::runtime_error("Frame cannot be mapped to RGBA");
    r.read = std::move(mapped).value();
    auto plane = r.read.plane(0);
    if (!plane) throw std::runtime_error("Frame has no RGBA plane");
    r.pixels = plane.value().data;
    Json result = {{"ok",true},{"width",r.frame.desc().width},{"height",r.frame.desc().height},
      {"rowBytes",plane.value().row_stride},{"byteLength",plane.value().size},
      {"originX",frame.originX},{"originY",frame.originY},{"timeUs",request.localTimeUs},
      {"logicalBounds",Rect(frame.logicalBounds)},{"inkBounds",Rect(frame.inkBounds)},
      {"controlBounds",Rect(frame.layout.controlBounds)},{"visualExtent",Rect(frame.layout.visualExtent)},
      {"warnings",r.warnings},{"profile","wasm-raster-v1"},{"cacheBytes",r.lane->cachedBytes()}};
    if (r.externalComposition) result["compositionPlan"] = videocut::text_wasm::SampleBrowserComposition(
        r.document, frame.layout, request.localTimeUs, width, height);
    r.result = result.dump();
  });
}
const void* vct_pixels(unsigned handle) {
  auto it = renderers.find(handle); return it == renderers.end() ? nullptr : it->second->pixels;
}
// Experimental geometry export. This intentionally exports the base shaped
// outline, not the template's animated material/execution graph.
int vct_sdf_mesh(unsigned handle, unsigned width, unsigned height, float range) {
  return Guard(handle, [&](Renderer& r) {
    if (!r.loaded || width < 1 || height < 1 || width > 2048 || height > 2048 ||
        !std::isfinite(range) || range < 1 || range > 128)
      throw std::invalid_argument("Invalid SDF mesh dimensions or distance range");
    const auto snap = comp::BuildTextCompositionSnapshot(r.document,
        {0,0,r.document.durationTicks*25/3,0,0,r.document.durationTicks});
    if (!snap.valid || !snap.snapshot) throw std::runtime_error(Diagnostics(snap.diagnostics));
    auto& view = snap.snapshot->resolvedText;
    vsk::SkiaRuntimeConfig config; config.assets = r.assets; config.allowSystemFonts = false;
    vsk::internal::FontContextCache cache;
    std::string error;
    const auto component = snap.snapshot->appearance.sdfMaterial.sourceCreationComponent;
    const auto fonts = cache.Resolve(vsk::internal::FontResourceIdentity(view, component),
        view, component, config, {}, nullptr, error);
    if (!fonts) throw std::runtime_error(error);
    namespace lane = vsk::internal::text_lane;
    lane::ResolvedLayout layout;
    txt::TextEffectFramePlan plan; plan.sourceCreationComponent = component;
    if (!lane::BuildLayout(view, plan, *fonts, layout, error)) throw std::runtime_error(error);
    txt::TextRenderRequest request; request.outputWidth = width; request.outputHeight = height;
    auto matrix = lane::ResolvePresentationMatrix(view, request, layout.logicalBounds);
    SkPathBuilder builder;
    std::size_t glyphs = 0;
    for (const auto& paragraph : layout.paragraphs) {
      vsk::internal::TextGlyphOutlineCaptureRequest capture;
      capture.paragraph = paragraph.measurement.get();
      capture.paragraphX = paragraph.x; capture.paragraphY = paragraph.y;
      capture.writingTransform = layout.writingTransform;
      for (auto index : paragraph.unitIndexes) {
        const auto& u = layout.units[index];
        capture.units.push_back({u.binding.stableUnitId,u.binding.paragraphId,u.binding.runId,
          u.paragraphUtf8Begin,u.paragraphUtf8End,u.binding.utf8Begin,u.binding.utf8End});
      }
      std::vector<vsk::internal::CapturedTextGlyphOutline> outlines;
      if (!vsk::internal::CaptureTextGlyphOutlines(capture, outlines, error)) throw std::runtime_error(error);
      for (const auto& outline : outlines) { builder.addPath(outline.path, matrix); ++glyphs; }
    }
    auto path = builder.detach();
    vsk::internal::TextSdfMeshBuildRequest meshRequest;
    meshRequest.glyphIdentity = {1U,"wasm-outline-batch","wasm-outline-batch",0U,0U,0U,1U};
    meshRequest.path = &path; meshRequest.sourceBounds = SkRect::MakeWH(width,height);
    meshRequest.atlasWidth = width; meshRequest.atlasHeight = height; meshRequest.rasterDistanceRange = range;
    if (!vsk::internal::BuildTextSdfGpuMesh(meshRequest, r.sdfMesh, error)) throw std::runtime_error(error);
    // The SDK builder packs renderer-Y-down outlines into a tight, Y-up
    // atlas cell. Restore their authored output placement for this full-canvas
    // experiment; retain all native distance/parabola values unchanged.
    const auto bounds = path.computeTightBounds();
    const auto restorePlacement = [&](auto& vertex) {
      vertex.positionX += (bounds.left() - range) / width;
      vertex.positionY = (bounds.bottom() + range) / height - vertex.positionY;
    };
    for (auto& vertex : r.sdfMesh.distanceVertices) restorePlacement(vertex);
    for (auto& vertex : r.sdfMesh.shapeVertices) restorePlacement(vertex);
    if (r.sdfMesh.distanceVertices.empty() || r.sdfMesh.shapeVertices.empty()) throw std::runtime_error("SDF outline is empty");
    r.result = Json{{"ok",true},{"width",width},{"height",height},{"range",range},{"glyphCount",glyphs},
      {"distanceCount",r.sdfMesh.distanceVertices.size()},{"shapeCount",r.sdfMesh.shapeVertices.size()},
      {"distancePointer",reinterpret_cast<std::uintptr_t>(r.sdfMesh.distanceVertices.data())},
      {"shapePointer",reinterpret_cast<std::uintptr_t>(r.sdfMesh.shapeVertices.data())},
      {"scope","base-layout-outline-only"}}.dump();
  });
}
}
