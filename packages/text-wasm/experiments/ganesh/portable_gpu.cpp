#include "portable_gpu.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/GrContextOptions.h"
#include <limits>
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/gpu/ganesh/gl/GrGLMakeWebGLInterface.h"
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>
#include <stdexcept>
namespace { EMSCRIPTEN_WEBGL_CONTEXT_HANDLE handle=0; sk_sp<GrDirectContext> context; bool enabled=false; int samples=0; unsigned surfaces=0; }
namespace videocut::text_wasm {
sk_sp<SkSurface> MakePortableSurface(const SkImageInfo &info) {
  if(!enabled)return SkSurfaces::Raster(info);
  if(!context || emscripten_webgl_make_context_current(handle)!=EMSCRIPTEN_RESULT_SUCCESS)throw std::runtime_error("WebGL presentation context unavailable");
  auto surface=SkSurfaces::RenderTarget(context.get(),skgpu::Budgeted::kYes,info,samples,kTopLeft_GrSurfaceOrigin,nullptr);
  if(!surface)throw std::runtime_error("Ganesh WebGL target allocation failed");
  ++surfaces;return surface;
}}
extern "C" {
int vct_gpu_context(const char *selector,int strategy) {
  if(context)return 1;
  EmscriptenWebGLContextAttributes attr;emscripten_webgl_init_context_attributes(&attr);
  attr.majorVersion=2;attr.minorVersion=0;attr.alpha=true;attr.depth=false;attr.stencil=true;attr.antialias=false;attr.premultipliedAlpha=true;attr.preserveDrawingBuffer=true;
  handle=emscripten_webgl_create_context(selector,&attr);
  if(handle<=0)return 0;
  if(emscripten_webgl_make_context_current(handle)!=EMSCRIPTEN_RESULT_SUCCESS)return 0;
  auto interface=GrGLInterfaces::MakeWebGL();if(!interface)return 0;
  GrContextOptions options;
  if(strategy>=4){ options.fDisableDistanceFieldPaths=true;options.fMinDistanceFieldFontSize=std::numeric_limits<float>::max();if(strategy!=6)options.fGlyphsAsPathsFontSize=std::numeric_limits<float>::max();else options.fMinDistanceFieldFontSize=options.fGlyphsAsPathsFontSize;options.fInternalMultisampleCount=0; if(strategy==4||strategy==6)options.fDisableTessellationPathRenderer=true;}
  context=GrDirectContexts::MakeGL(interface,options);if(!context)return 0;
  context->setResourceCacheLimit(128U*1024U*1024U);return 1;
}
int vct_gpu_enable(int value) { if(value&&!context)return 0;enabled=value!=0;samples=value==3?8:value==2?4:0;return 1; }
unsigned vct_gpu_surface_count(){return surfaces;}
void vct_gpu_dispose(){enabled=false;context.reset();if(handle)emscripten_webgl_destroy_context(handle);handle=0;}
}
