import test from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { build } from 'esbuild';
import { Chromium, findChromium } from '../packages/server/chromium.mjs';

const fixture = `
import { createGpuCompositor } from './packages/render/gpu.mjs';
import { layerGeometry, makeLut } from './packages/render/plan.mjs';
const W=1920,H=1080,project={canvas:{width:W,height:H}},bounds={x:320,y:210,width:632,height:236};
let baseline, cropped, source;
window.init=async()=>{
  source=new OffscreenCanvas(W,H);
  const ctx=source.getContext('2d');
  const pixels=ctx.createImageData(630,234);
  for(let y=0;y<234;y++)for(let x=0;x<630;x++){
    const i=(y*630+x)*4;
    pixels.data[i]=(x*29+y*17)%256;
    pixels.data[i+1]=(x*7+y*31)%256;
    pixels.data[i+2]=(x*3+y*11)%256;
    pixels.data[i+3]=1+(x*13+y*19)%255;
  }
  ctx.putImageData(pixels,321,211);
  baseline=await createGpuCompositor(new OffscreenCanvas(W,H));
  cropped=await createGpuCompositor(new OffscreenCanvas(W,H));
  if(!baseline||!cropped)throw Error('Real Chromium WebGPU is unavailable');
  return {available:true};
};
window.compare=async(options)=>{
  const width=options.width||W,height=options.height||H;
  const visual={fitPolicy:'stretch',opacity:1,...options.visual};
  const geometry=layerGeometry(project,visual,W,H,width,height);
  const effects=options.effects||[];
  const draw=async(gpu,roi)=>{
    let base=gpu.begin([0,0,0,0],width,height);
    const sourceId=options.sourceId||'source';
    const input=gpu.upload(source,'input:'+sourceId,sourceId,roi?bounds:undefined,1);
    const g=roi?{...geometry,rasterBounds:bounds,sourceWidth:W,sourceHeight:H}:geometry;
    const mappedEffects=effects.map(effect=>{
      if(effect.templateId!=='lut')return effect;
      const lut=makeLut('warm');
      return {...effect,lut:{size:lut.size,texture:gpu.lut(lut.data,lut.size,'warm')}};
    });
    let layer=gpu.layer(input,g,mappedEffects,'source');
    if(options.transition)layer=gpu.transition(base,layer,{style:'wipe',direction:'right',progress:.43,parameters:{}},'wipe');
    base=gpu.blend(base,layer,'normal',0);
    await gpu.finish(base);
    return {pixels:await gpu.readPixels(),stats:gpu.stats(),inputBytes:input.width*input.height*4};
  };
  const a=await draw(baseline,false),b=await draw(cropped,true);
  let changed=0,max=0,sum=0;
  for(let i=0;i<a.pixels.length;i++){const d=Math.abs(a.pixels[i]-b.pixels[i]);if(d)changed++;max=Math.max(max,d);sum+=d;}
  return {name:options.name,changed,max,mean:sum/a.pixels.length,baseline:a.stats.textureBytes,cropped:b.stats.textureBytes,fullInput:a.inputBytes,roiInput:b.inputBytes};
};
window.sampleHistory=async()=>{
  baseline.dispose();cropped.dispose();
  baseline=await createGpuCompositor(new OffscreenCanvas(W,H));
  cropped=await createGpuCompositor(new OffscreenCanvas(W,H));
  let result;
  for(let i=0;i<12;i++)result=await window.compare({name:'history',sourceId:'frame-'+i,visual:{scaleX:.72,scaleY:.72}});
  const metadata={inputKey:'input:frame-0',uploadIdentity:'frame-0',width:W,height:H,rasterBounds:bounds};
  const retained=cropped.retainInputs([metadata],true);
  const base=cropped.begin([0,0,0,0],W,H);
  const input=cropped.reuseInput(metadata);
  const geometry={...layerGeometry(project,{fitPolicy:'stretch',scaleX:.72,scaleY:.72},W,H,W,H),rasterBounds:bounds,sourceWidth:W,sourceHeight:H};
  await cropped.finish(cropped.blend(base,cropped.layer(input,geometry,[],'source'),'normal',0));
  return {...result,retained:retained.has(metadata.inputKey),hits:cropped.stats().uploadCacheHits,uploads:cropped.stats().uploadCount};
};
window.closeFixture=()=>{baseline?.dispose();cropped?.dispose();};
`;

test(
  'Native ROI uploads preserve full-source WebGPU sampling, transforms and effects',
  { timeout: 60000 },
  async (t) => {
    try {
      await findChromium();
    } catch (error) {
      t.skip(error.message);
      return;
    }
    const bundle = await build({
      stdin: { contents: fixture, resolveDir: process.cwd() },
      bundle: true,
      format: 'iife',
      platform: 'browser',
      write: false,
      logLevel: 'silent'
    });
    const server = createServer((req, res) => {
      if (req.url === '/fixture.js') {
        res.setHeader('Content-Type', 'text/javascript');
        res.end(bundle.outputFiles[0].text);
      } else {
        res.setHeader('Content-Type', 'text/html');
        res.end('<script src="/fixture.js"></script>');
      }
    });
    await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
    const browser = await new Chromium().start({ args: ['--enable-unsafe-webgpu'] });
    let sessionId;
    try {
      const { targetId } = await browser.send('Target.createTarget', {
        url: `http://127.0.0.1:${server.address().port}`
      });
      ({ sessionId } = await browser.send('Target.attachToTarget', { targetId, flatten: true }));
      const evaluate = async (expression) => {
        const result = await browser.send(
          'Runtime.evaluate',
          { expression, awaitPromise: true, returnByValue: true },
          sessionId
        );
        if (result.exceptionDetails)
          throw Error(
            result.exceptionDetails.exception?.description || result.exceptionDetails.text
          );
        return result.result.value;
      };
      await evaluate(
        `new Promise((resolve,reject)=>{const start=Date.now();const poll=()=>window.init?resolve(window.init()):Date.now()-start>5000?reject(Error('Fixture did not load')):setTimeout(poll,10);poll();})`
      );
      const cases = [
        { name: '1080 identity' },
        { name: '540 identity', width: 960, height: 540 },
        { name: 'fractional translation', visual: { positionX: 17.3, positionY: -41.7 } },
        {
          name: 'rotation and flipped anchor',
          visual: {
            rotationDegrees: 17.3,
            flipHorizontal: true,
            anchorX: 0.18,
            anchorY: 0.81,
            scaleX: 0.7,
            scaleY: 1.21
          }
        },
        {
          name: 'user crop',
          visual: {
            crop: { left: 0.12, top: 0.09, right: 0.08, bottom: 0.13 },
            fitPolicy: 'contain'
          }
        },
        { name: 'native crop', visual: { fitPolicy: 'nativeCrop', scaleX: 0.913, scaleY: 0.829 } },
        { name: 'current native benchmark scale', visual: { scaleX: 0.72, scaleY: 0.72 } },
        {
          name: 'blur',
          visual: { positionX: 17.3, rotationDegrees: 6.7 },
          effects: [{ id: 'blur', templateId: 'blur', parameters: { radius: 5 } }]
        },
        {
          name: 'glow',
          visual: { positionY: -11.7, rotationDegrees: 3.1 },
          effects: [
            {
              id: 'glow',
              templateId: 'glow',
              parameters: { radius: 3, threshold: 0.3, strength: 0.7 }
            }
          ]
        },
        {
          name: 'LUT opacity',
          visual: { scaleX: 0.72, scaleY: 0.72, opacity: 0.63 },
          effects: [{ id: 'lut', templateId: 'lut', parameters: { amount: 0.73 } }]
        },
        { name: 'wipe transition', transition: true }
      ];
      const results = [];
      for (const options of cases)
        results.push(await evaluate(`window.compare(${JSON.stringify(options)})`));
      t.diagnostic(JSON.stringify(results));
      assert.ok(
        results.every((row) => row.changed === 0),
        JSON.stringify(results.filter((row) => row.changed))
      );
      assert.equal(results[0].fullInput, 1920 * 1080 * 4);
      assert.equal(results[0].roiInput, 632 * 236 * 4);
      const history = await evaluate('window.sampleHistory()');
      t.diagnostic(JSON.stringify(history));
      assert.equal(history.changed, 0);
      assert.ok(history.cropped < history.baseline / 2);
      assert.equal(history.retained, true);
      assert.equal(history.hits, 1);
      assert.equal(history.uploads, 0);
    } finally {
      if (sessionId)
        await browser
          .send('Runtime.evaluate', { expression: 'window.closeFixture?.()' }, sessionId)
          .catch(() => {});
      await browser.close();
      await new Promise((resolve) => server.close(resolve));
    }
  }
);
