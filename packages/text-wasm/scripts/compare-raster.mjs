// Real scalar/SIMD CPU output comparison, independent of browser timing.
import { readFile, writeFile } from 'node:fs/promises';
import assert from 'node:assert/strict';
import { createTextEngine } from '../dist/index.mjs';
import { createTextEngine as createBaseline } from '../.cache/perf/index.mjs';
import { recipes, loadRecipe } from '../demo/complex-recipes.mjs';
import { registerFonts, fonts } from '../tests/helpers.mjs';
const engines = [await createBaseline({wasmBinary:await readFile(new URL('../.cache/perf/baseline.wasm',import.meta.url))}),await createTextEngine()];
const pixels = (f,w,h) => {
  const p = new Uint8Array(w*h*4);
  for(let y=0;y<f.height;y++)for(let x=0;x<f.width;x++){
    const dx=x+f.originX,dy=y+f.originY;if(dx<0||dy<0||dx>=w||dy>=h)continue;
    const i=(y*f.width+x)*4,o=(dy*w+dx)*4,a=f.data[i+3];
    for(let c=0;c<3;c++)p[o+c]=Math.round(f.data[i+c]*a/255);p[o+3]=a;
  } return p;
};
const cases=[];
try {
  for(const recipe of recipes){
    const {bundle}=await loadRecipe(recipe,async url=>new Response(await readFile(url)));
    const renderers=engines.map(e=>e.createRenderer());
    try {
      for(const r of renderers){registerFonts(r);await r.loadTemplate(bundle,{bindings:{content:recipe.text},allowRasterFallback:true,experimentalComposition:!!recipe.external,fallbackFonts:[{id:fonts[1].id,family:'Source Han Sans SC'}]});}
      const samples=[];
      for(const text of [recipe.text,'文字动画']) {
        for(const r of renderers)r.setText(text);
        for(const timeUs of [350000,1000000,1800000,2600000]) {
          const frames=renderers.map(r=>r.render({timeUs,width:640,height:360}));
          assert.deepEqual(frames[0].compositionPlan,frames[1].compositionPlan);
          const [a,b]=frames.map(f=>pixels(f,640,360));let max=0,sum=0,over2=0;
          for(let i=0;i<a.length;i++){const d=Math.abs(a[i]-b[i]);max=Math.max(max,d);sum+=d;if(d>2)over2++;}
          const mean=sum/a.length,fractionOver2=over2/a.length,passed=mean<=.1&&fractionOver2<=.005;
          samples.push({text,timeUs,max,mean,fractionOver2,passed});
        }
      }
      cases.push({id:recipe.id,samples,passed:samples.every(s=>s.passed)});
    }finally{renderers.forEach(r=>r.dispose());}
  }
}finally{engines.forEach(e=>e.dispose());}
const report={profile:'scalar-simd-raster-comparison-v1',scope:'640x360 premultiplied RGBA, six recipes, two Chinese bindings and four times each. External-composition cases compare text raster and native plans, not GPU post effects.',cases,passed:cases.every(c=>c.passed)};
await writeFile(new URL('../reports/raster-simd-validation.json',import.meta.url),JSON.stringify(report,null,2)+'\n');
console.log(JSON.stringify({passed:report.passed,cases:cases.map(c=>({id:c.id,passed:c.passed,max:Math.max(...c.samples.map(s=>s.max)),meanMax:Math.max(...c.samples.map(s=>s.mean))}))},null,2));
assert.ok(report.passed,'Scalar/SIMD pixel differences exceed the acceptance thresholds');
