import {cp,readFile,writeFile} from 'node:fs/promises';
import {fileURLToPath,pathToFileURL}from'node:url';
import{resolve}from'node:path';
const workspace=fileURLToPath(new URL('../../../../',import.meta.url));
const root=pathToFileURL(resolve(process.env.VIDEOCUT_GPU_EXPERIMENT_DIR||workspace+'/.local/qa/wasm-gpu-next')+'/'),source=new URL('source/',root);
for(const file of ['index.mjs','index.d.mts','recipes.mjs','recipes.d.mts'])await cp(new URL('src/'+file,source),new URL('dist/'+file,source));
const entry=new URL('dist/index.mjs',source);
let js=await readFile(entry,'utf8');
js=js.replace("profile: 'wasm-raster-v1',",`profile: 'wasm-raster-v1',
    gpuContext(selector,strategy=0) { return module.cwrap('vct_gpu_context','number',['string','number'])(selector,strategy); },
    gpuEnable(value) { return module._vct_gpu_enable(Number(value)); },
    gpuSurfaceCount() { return module._vct_gpu_surface_count(); },
    gpuDispose() { module._vct_gpu_dispose(); },`);
await writeFile(entry,js);
