import { cp, readFile, writeFile, mkdir, access, utimes } from 'node:fs/promises';
import { spawn } from 'node:child_process';
import { resolve, join } from 'node:path';
import { fileURLToPath } from 'node:url';
const workspace=fileURLToPath(new URL('../../../../',import.meta.url));
const experiment=resolve(process.env.VIDEOCUT_GPU_EXPERIMENT_DIR||join(workspace,'.local/qa/wasm-gpu-next'));
const tools=fileURLToPath(new URL('.',import.meta.url));
const source=join(experiment,'source'),skia=join(workspace,'.local/text-wasm-tools/skia');
const skiaBuild=join(experiment,'skia-build'),sdkBuild=join(experiment,'sdk-build');
const emsdk=join(workspace,'.local/text-wasm-tools/emsdk');
const run=(cmd,args)=>new Promise((ok,no)=>{const p=spawn(cmd,args,{cwd:experiment,stdio:'inherit'});p.on('error',no);p.on('exit',code=>code===0?ok():no(Error(cmd+' failed '+code)));});
await mkdir(source,{recursive:true});await mkdir(skiaBuild,{recursive:true});await mkdir(join(source,'dist'),{recursive:true});
for(const path of ['src','vendor','third_party'])await cp(join(workspace,'packages/text-wasm',path),join(source,path),{recursive:true});
const frozen=resolve(process.env.VIDEOCUT_GPU_BASELINE||join(workspace,'packages/text-wasm/dist'));
if(frozen===join(experiment,'baseline'))throw Error('Baseline input must differ from experiment output');
await cp(frozen,join(experiment,'baseline'),{recursive:true});
for(const file of ['demo.html','demo.mjs'])await cp(join(tools,file),join(experiment,file));
for(const [file,target] of [['TextComponentRecorder.cpp','text'],['TextGlyphMaterials.cpp','text'],['SkiaFramePublication.cpp','raster']])
    try { await access(join(frozen,file));await cp(join(frozen,file),join(source,'vendor/videocut/sdk/skia_runtime/src',target,file)); }
  catch(error){ if(error.code!=='ENOENT')throw error; }
let args=await readFile(join(workspace,'.local/text-wasm-tools/skia-wasm-simd/args.gn'),'utf8');
for(const name of ['skia_enable_ganesh','skia_use_webgl','skia_use_gl'])args=args.replace(name+' = false',name+' = true');
await writeFile(join(skiaBuild,'args.gn'),args);
await run(join(skia,'bin/gn'),['gen',skiaBuild,'--root='+skia]);
// Re-archive the original cached unit if a prior software-mask overlay changed the archive.
try { await access(join(experiment,'skia-overlay/libskia-native-paths.a'));const now=new Date();await utimes(join(skiaBuild,'obj/src/gpu/ganesh/gpu.GrRecordingContext.o'),now,now); } catch(error) { if(error.code!=='ENOENT')throw error; }
await run('ninja',['-C',skiaBuild,'-j8','skia','skparagraph','skshaper','skunicode','skottie','skresources','svg']);
let lane=await readFile(join(source,'vendor/videocut/sdk/skia_runtime/src/text/SkiaTextRenderLane.cpp'),'utf8');
lane='#include "portable_gpu.h"\n'+lane;
const before=': SkSurfaces::Raster(info);';
if(!lane.includes(before))throw Error('Frozen source raster allocation not found');
lane=lane.replace(before,': videocut::text_wasm::MakePortableSurface(info);');
await writeFile(join(source,'vendor/videocut/sdk/skia_runtime/src/text/SkiaTextRenderLane.cpp'),lane);
for(const file of ['portable_gpu.h','portable_gpu.cpp'])await cp(join(tools,file),join(source,'src',file));
let cmake=await readFile(join(workspace,'packages/text-wasm/CMakeLists.txt'),'utf8');
cmake=cmake.replace('add_executable(videocut-text src/bridge.cpp','target_include_directories(VideoCutSkiaRuntime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")\ntarget_compile_definitions(skia_all INTERFACE SK_GANESH SK_GL)\nadd_executable(videocut-text src/portable_gpu.cpp src/bridge.cpp');
cmake=cmake.replace("'_vct_sdf_mesh']","'_vct_sdf_mesh','_vct_gpu_context','_vct_gpu_enable','_vct_gpu_surface_count','_vct_gpu_dispose']");
cmake+='\ntarget_link_options(videocut-text PRIVATE -lGL -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sFULL_ES3=1)\n';
await writeFile(join(source,'CMakeLists.txt'),cmake);
await run(join(emsdk,'upstream/emscripten/emcmake'),['cmake','-S',source,'-B',sdkBuild,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DSKIA_ROOT='+skia,'-DSKIA_BUILD='+skiaBuild]);
await run('cmake',['--build',sdkBuild,'-j8']);
for(const file of ['index.mjs','index.d.mts','recipes.mjs','recipes.d.mts'])await cp(join(source,'src',file),join(source,'dist',file));
const entry=join(source,'dist/index.mjs');
let js=await readFile(entry,'utf8');
js=js.replace("profile: 'wasm-raster-v1',",`profile: 'wasm-raster-v1',
    gpuContext(selector,strategy=0) { return module.cwrap('vct_gpu_context','number',['string','number'])(selector,strategy); },
    gpuEnable(value) { return module._vct_gpu_enable(Number(value)); },
    gpuSurfaceCount() { return module._vct_gpu_surface_count(); },
    gpuDispose() { module._vct_gpu_dispose(); },`);
await writeFile(entry,js);
console.log('Independent WebGL presentation prototype built at '+source+'/dist');
