import {createTextEngine as createGpuEngine} from './source/dist/index.mjs';
import {createTextEngine as createBaselineEngine} from './baseline/index.mjs';
const mode=Number(new URL(location.href).searchParams.get('mode')||1),timingOnly=new URL(location.href).searchParams.has('timing');
const $=id=>document.getElementById(id),report={kind:'portable-skp-ganesh-webgl-experiment',phase:timingOnly?'timing-only':'quality-and-timing',mode,requestedSamples:mode===3?8:mode===2?4:0,scope:'GPU final replay of unchanged portable materials; includes RGBA publication/readback',width:1920,height:1080,samples:[],performance:[]};
const [config,gpu,baseline]=await Promise.all([fetch('/config.json').then(r=>r.json()),createGpuEngine(),createBaselineEngine()]);
const fonts=await Promise.all(config.fonts.map(async font=>({...font,bytes:new Uint8Array(await(await fetch(font.url)).arrayBuffer())})));
if(!gpu.gpuContext('#gpu-context',mode>=4?mode:0))throw Error('Ganesh WebGL initialization failed');
report.fonts=config.fonts.map(({id,family})=>({id,family}));
const gl=$('gpu-context').getContext('webgl2');
const rendererInfo=gl?.getExtension('WEBGL_debug_renderer_info');
report.gpu={vendor:rendererInfo?gl.getParameter(rendererInfo.UNMASKED_VENDOR_WEBGL):gl?.getParameter(gl.VENDOR),renderer:rendererInfo?gl.getParameter(rendererInfo.UNMASKED_RENDERER_WEBGL):gl?.getParameter(gl.RENDERER)};
const decode=b64=>Uint8Array.from(atob(b64),c=>c.charCodeAt(0));
const bundles=config.recipes.map(recipe=>({...recipe,bundle:{...recipe.bundle,assets:new Map(recipe.bundle.assets.map(([key,asset])=>[key,{...asset,bytes:decode(asset.bytes)}]))}}));
const full=frame=>{
 const data=new Uint8Array(1920*1080*4);
 for(let y=0;y<frame.height;y++)data.set(frame.data.subarray(y*frame.rowBytes,y*frame.rowBytes+frame.width*4),((frame.originY+y)*1920+frame.originX)*4);
 return data;
};
const display=(id,frame)=>{
 const canvas=$(id);canvas.width=1920;canvas.height=1080;
 canvas.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(full(frame)),1920,1080),0,0);
};
const diff=(a,b)=>{
 let sum=0,max=0,changed=0,over1=0,alphaChanged=0;
 for(let i=0;i<a.length;i++){const d=Math.abs(a[i]-b[i]);sum+=d;max=Math.max(max,d);if(d){changed++;if(i%4===3)alphaChanged++;}if(d>1)over1++;}
 return {max,mean:sum/a.length,changed,over1,alphaChanged};
};
const visibleDiff=(a,b)=>{
 const metrics={alpha:{},black:{},white:{},opaque:{}};
 for(const m of Object.values(metrics))Object.assign(m,{max:0,sum:0,count:0,changedPixels:0,over1Pixels:0});
 let active=0,changedPixels=0;
 for(let i=0;i<a.length;i+=4){
  const aa=a[i+3],ba=b[i+3];if(!aa&&!ba)continue;active++;
  if(a.subarray(i,i+4).some((v,j)=>v!==b[i+j]))changedPixels++;
  const values={alpha:[Math.abs(aa-ba)],black:[],white:[],opaque:[]};
  for(let c=0;c<3;c++){
   const av=a[i+c]*aa/255,bv=b[i+c]*ba/255;
   values.black.push(Math.abs(av-bv));values.white.push(Math.abs(av+255-aa-(bv+255-ba)));
   if(aa===255&&ba===255)values.opaque.push(Math.abs(a[i+c]-b[i+c]));
  }
  for(const[k,v]of Object.entries(values)){
   if(!v.length)continue;const m=metrics[k];m.count+=v.length;m.sum+=v.reduce((s,n)=>s+n,0);m.max=Math.max(m.max,...v);if(v.some(n=>n>0))m.changedPixels++;if(v.some(n=>n>1))m.over1Pixels++;
  }
 }
 return {activePixels:active,changedPixels,...Object.fromEntries(Object.entries(metrics).map(([k,{sum,count,...m}])=>[k,{...m,mae:count?sum/count:0,samples:count}]))};
};
const artifact=async(name,data)=>{
 const canvas=document.createElement('canvas');canvas.width=1920;canvas.height=1080;canvas.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(data),1920,1080),0,0);
 const png=await new Promise(resolve=>canvas.toBlob(resolve,'image/png'));
 await fetch('/artifact/'+name,{method:'POST',body:png});
};
const hash=async data=>[...new Uint8Array(await crypto.subtle.digest('SHA-256',data))].map(n=>n.toString(16).padStart(2,'0')).join('');
const make=async(engine,bundle,text)=>{
 const renderer=engine.createRenderer();for(const font of fonts)renderer.registerAsset(font.id,font.mediaType,font.bytes);
 await renderer.loadTemplate(bundle,{bindings:{content:text},allowRasterFallback:true});return renderer;
};
const render=(r,timeUs,useGPU)=>{if(useGPU!==undefined)gpu.gpuEnable(useGPU?mode:0);const start=performance.now();const frame=r.render({timeUs,width:1920,height:1080,profile:true});return {frame,ms:performance.now()-start};};
window.runExperiment=async()=>{
 $('run').disabled=true;
 try {
  for(const recipe of bundles){
   $('status').textContent='比较 '+recipe.id;
   const old=await make(baseline,recipe.bundle,'灵感花开'),cpu=await make(gpu,recipe.bundle,'灵感花开'),next=await make(gpu,recipe.bundle,'灵感花开');
   for(const timeUs of timingOnly?[]:[0,200000,800000,1300000,2600000,4800000,200000]){
    const a=render(old,timeUs),c=render(cpu,timeUs,false),b=render(next,timeUs,true);
    const af=full(a.frame),bf=full(b.frame);
    report.samples.push({recipe:recipe.id,timeUs,cpuBuildEquivalence:diff(af,full(c.frame)),gpu:diff(af,bf),visible:visibleDiff(af,bf),baselineMs:a.ms,gpuMs:b.ms,baselineHash:await hash(af),gpuHash:await hash(bf),geometryEqual:JSON.stringify([a.frame.width,a.frame.height,a.frame.originX,a.frame.originY,a.frame.controlBounds])===JSON.stringify([b.frame.width,b.frame.height,b.frame.originX,b.frame.originY,b.frame.controlBounds])});
    if(timeUs===800000){await artifact('mode'+mode+'-'+recipe.id+'-cpu.png',af);await artifact('mode'+mode+'-'+recipe.id+'-gpu.png',bf);const delta=new Uint8Array(af.length);for(let i=0;i<af.length;i+=4){for(let c=0;c<3;c++)delta[i+c]=Math.min(255,Math.abs(af[i+c]*af[i+3]/255-bf[i+c]*bf[i+3]/255)*8);delta[i+3]=255;}await artifact('mode'+mode+'-'+recipe.id+'-diff8.png',delta);}
   }
   for(let i=0;i<20;i++){
    const timeUs=1000000+i*47513;
    let a,b;if(i%2){b=render(next,timeUs,true);a=render(old,timeUs);}else{a=render(old,timeUs);b=render(next,timeUs,true);}
    if(i>=5)report.performance.push({recipe:recipe.id,timeUs,baselineMs:a.ms,gpuMs:b.ms});
   }
   display('before',render(old,1500000).frame);display('after',render(next,1500000,true).frame);
   old.dispose();cpu.dispose();next.dispose();
   await new Promise(resolve=>setTimeout(resolve,0));
  }
  report.gpuSurfaceCount=gpu.gpuSurfaceCount();report.exact=report.samples.every(s=>s.gpu.max===0);report.withinOneLSB=report.samples.every(s=>s.gpu.max<=1);report.cpuBuildExact=report.samples.every(s=>s.cpuBuildEquivalence.max===0);report.geometryExact=report.samples.every(s=>s.geometryEqual);report.reverseExact=timingOnly?null:bundles.every(({id})=>{const s=report.samples.filter(s=>s.recipe===id&&s.timeUs===200000);return s[0].gpuHash===s[1].gpuHash;});
  report.summary=bundles.map(recipe=>{const rows=report.performance.filter(r=>r.recipe===recipe.id);const stats=k=>{const v=rows.map(r=>r[k]).sort((a,b)=>a-b);return {mean:v.reduce((a,b)=>a+b,0)/v.length,p95:v[Math.ceil(v.length*.95)-1]};};return {recipe:recipe.id,baseline:stats('baselineMs'),gpu:stats('gpuMs')};});
  if(timingOnly){report.exact=null;report.withinOneLSB=null;report.cpuBuildExact=null;report.geometryExact=null;report.reverseExact=null;}
  report.complete=true;$('status').textContent=report.exact?'全 RGBA 字节一致':'GPU 输出存在差异，保持实验，不替换生产';
 }catch(error){report.error=String(error);report.errorStack=error.stack;$('status').textContent=String(error);}
 $('report').textContent=JSON.stringify(report,null,2);await fetch('/report',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(report)});
 $('run').disabled=false;return report;
};
$('status').textContent='实验就绪；两个引擎使用相同安装字体与素材';
$('run').onclick=window.runExperiment;
window.experimentReady=true;
