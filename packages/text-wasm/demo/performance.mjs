import { createTextEngine, sha256 } from '../dist/index.mjs';
import { createBrowserTextComposition } from '../dist/browser-composition.mjs';
import { recipes, loadRecipe } from './complex-recipes.mjs';
const $ = id => document.getElementById(id);
const read = async url => {
  const r = await fetch(url);
  if (!r.ok) throw Error(`Missing benchmark input: ${url}`);
  return new Uint8Array(await r.arrayBuffer());
};
const stats = values => {
  const sorted = [...values].sort((a,b)=>a-b);
  return { p50: sorted[Math.floor(sorted.length*.5)], p95: sorted[Math.ceil(sorted.length*.95)-1], max: sorted.at(-1) };
};
const diff = (a,b) => {
  let sum=0,max=0,over2=0;
  for(let i=0;i<a.length;i++){const d=Math.abs(a[i]-b[i]);sum+=d;max=Math.max(max,d);if(d>2)over2++;}
  const mean=sum/a.length, fractionOver2=over2/a.length;
  return {mean,max,fractionOver2,passed:mean<=.1&&fractionOver2<=.005};
};
const fmt = s => `${s.p50.toFixed(1)} / ${s.p95.toFixed(1)} ms`;
$('run').addEventListener('click', async()=>{
  $('run').disabled=true; $('results').replaceChildren();
  const engines=[], players=[];
  try {
    $('status').textContent='加载旧版基线、SIMD 引擎和字体…';
    const [baseline,current,catalog] = await Promise.all([read('/perf-reference/baseline.wasm'),read('../dist/videocut-text.wasm'),fetch('./catalog.json').then(r=>r.json())]);
    const {createTextEngine:createBaselineEngine}=await import('/perf-reference/index.mjs');
    engines.push(await createBaselineEngine({wasmBinary:baseline}),await createTextEngine({wasmBinary:current}));
    const fonts=await Promise.all(catalog.fonts.map(async f=>({...f,bytes:await read(f.url)})));
    const report={profile:'wasm-simd-comparison-v1',timestamp:new Date().toISOString(),width:640,height:360,baselineSha256:await sha256(baseline),currentSha256:await sha256(current),lanes:engines.map(e=>e.capabilities.rasterPipelineLanes??1),cases:[]};
    for(const id of ['pattern-flower','cube']){
      const recipe=recipes.find(r=>r.id===id),{bundle}=await loadRecipe(recipe);
      for(const [i,slot] of ['before','after'].entries()) {
        const c=document.createElement('canvas');c.setAttribute('aria-label',`${recipe.name} ${i?'优化后':'优化前'}`);$(slot).replaceChildren(c);
        players.push(await createBrowserTextComposition(engines[i],c,bundle,{fonts,bindings:{content:recipe.text},fallbackFonts:[{id:fonts[1].id,family:'Source Han Sans SC'}]}));
      }
      const cold=[];
      for(const p of players) cold.push((await p.render({timeUs:123456})).totalMs);
      // Distinct times warm the JIT/material pipeline, not repeated-frame cache hits.
      for(let n=0;n<12;n++) for(const p of players) await p.render({timeUs:200000+n*170017});
      const samples=[], comparisons=[];
      for(let n=0;n<24;n++) {
        $('status').textContent=`${recipe.name}：测量 ${n+1}/24…`;
        const timeUs=250123+n*97777, frames=[];
        for(const i of n%2?[1,0]:[0,1]) frames[i]=await players[i].render({timeUs});
        const pixels=[];
        for(const p of players) pixels.push((await p.readPixels()).data);
        comparisons.push({timeUs,...diff(...pixels)});
        samples.push({timeUs,beforeMs:frames[0].totalMs,afterMs:frames[1].totalMs,beforeTextMs:frames[0].textMs,afterTextMs:frames[1].textMs});
      }
      // Real text reinstallation invalidates all renderer caches.
      const edits=[];
      for(const text of ['你好','文字动画']) {
        for(const p of players){p.setText(text);await p.render({timeUs:350000});}
        edits.push({text,...diff((await players[0].readPixels()).data,(await players[1].readPixels()).data)});
      }
      for(const p of players){p.setText(recipe.text);await p.render({timeUs:recipe.timeUs});}
      // Preserve the displayed result after releasing the measured GPU devices.
      for(const [i,slot] of ['before','after'].entries()) {
        const pixels=await players[i].readPixels(), data=new Uint8ClampedArray(pixels.data);
        for(let n=0;n<data.length;n+=4)if(data[n+3])for(let c=0;c<3;c++)data[n+c]=Math.min(255,Math.round(data[n+c]*255/data[n+3]));
        const canvas=document.createElement('canvas');canvas.width=pixels.width;canvas.height=pixels.height;
        canvas.setAttribute('aria-label',`${recipe.name} ${i?'优化后':'优化前'}`);
        canvas.getContext('2d').putImageData(new ImageData(data,pixels.width,pixels.height),0,0);$(slot).replaceChildren(canvas);
      }
      const result={id,coldMs:cold,before:stats(samples.map(s=>s.beforeMs)),after:stats(samples.map(s=>s.afterMs)),samples,comparisons,edits,pixelsPassed:[...comparisons,...edits].every(d=>d.passed)};
      report.cases.push(result);
      const tr=document.createElement('tr');
      for(const value of [recipe.name,fmt(result.before),fmt(result.after),result.pixelsPassed?'差异检查通过':'差异超过阈值']){const td=document.createElement('td');td.textContent=value;tr.append(td);}
      $('results').append(tr);
      for(const p of players.splice(0))p.dispose();
    }
    report.passed=report.lanes[1]===4&&report.cases.every(c=>c.pixelsPassed&&c.after.p95<=50);
    $('report').textContent=JSON.stringify(report,null,2);
    const saved=await fetch('/performance-test-report',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(report)});
    if(!saved.ok)throw Error('Cannot save performance report');
    $('status').textContent=report.passed?'对比完成：两个模板 P95 ≤50 ms，画面差异检查通过。':'对比完成，部分指标未达标，查看记录。';
  }catch(e){$('status').textContent=e.message; console.error(e);}
  finally{for(const p of players)p.dispose();for(const e of engines)e.dispose();$('run').disabled=false;}
});
