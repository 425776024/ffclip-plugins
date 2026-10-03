import {readFile,writeFile} from 'node:fs/promises';
import {resolve} from 'node:path';
import {pathToFileURL,fileURLToPath} from 'node:url';
import {Chromium} from '../../../server/chromium.mjs';
const workspace=fileURLToPath(new URL('../../../../',import.meta.url));
const output=pathToFileURL(resolve(process.env.VIDEOCUT_GPU_EXPERIMENT_DIR||workspace+'/.local/qa/wasm-gpu-next')+'/');
const log=await readFile(new URL('./server.log',output),'utf8');
const baseUrl=/"url":"([^"]+)"/.exec(log)?.[1];if(!baseUrl)throw Error('Fixture URL missing: '+log);
const url=baseUrl+'/?mode='+(process.env.VIDEOCUT_GPU_MODE||1)+(process.env.VIDEOCUT_GPU_TIMING_ONLY?'&timing=1':'');
const browser=await new Chromium().start();
try {
 const {targetId}=await browser.send('Target.createTarget',{url});
 const {sessionId}=await browser.send('Target.attachToTarget',{targetId,flatten:true});
 browser.listeners.add(event=>{if(event.sessionId===sessionId&&event.method==='Runtime.consoleAPICalled'&&['error','warning'].includes(event.params.type))console.log('Native/browser '+event.params.type+': '+event.params.args.map(value=>value.value||value.description).join(' '));});
 await browser.send('Runtime.enable',{},sessionId);
 const evaluate=async expression=>{
  const value=await browser.send('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true},sessionId);
  if(value.exceptionDetails)throw Error(value.exceptionDetails.exception?.description||value.exceptionDetails.text);
  return value.result.value;
 };
 const start=Date.now();
 while(!await evaluate('window.experimentReady===true')){
  if(Date.now()-start>20000){const error=await evaluate('document.body.innerText');throw Error('Fixture initialization timeout '+error);}
  await new Promise(resolve=>setTimeout(resolve,200));
 }
 await browser.send('Runtime.evaluate',{expression:'window.runExperiment().then(value=>window.experimentResult=value).catch(error=>window.experimentResult={error:String(error)})'},sessionId);
 let report;
 for(let i=0;i<90;i++){
  await new Promise(resolve=>setTimeout(resolve,1000));
  report=await evaluate('window.experimentResult||null');if(report)break;
  if(i%5===0)console.log(await evaluate('document.querySelector("#status").innerText'));
 }
 if(!report)throw Error('GPU fixture timed out');
 if(!report.complete||report.error)process.exitCode=1;
 if(process.env.VIDEOCUT_GPU_REQUIRE_EXACT&&!report.exact)process.exitCode=1;
 console.log(JSON.stringify({complete:report.complete,error:report.error,gpu:report.gpu,gpuSurfaceCount:report.gpuSurfaceCount,cpuBuildExact:report.cpuBuildExact,exact:report.exact,withinOneLSB:report.withinOneLSB,geometryExact:report.geometryExact,summary:report.summary,sampleSummary:report.samples?.map(({recipe,timeUs,gpu,cpuBuildEquivalence})=>({recipe,timeUs,gpu,cpuBuildEquivalence}))}));
 for(const id of ['before','after']){
  const png=await evaluate(`document.querySelector('#${id}').toDataURL('image/png')`);
  if(png.startsWith('data:image/png;base64,'))await writeFile(new URL('./'+id+'.png',output),Buffer.from(png.split(',')[1],'base64'));
 }
} finally {await browser.close();}
