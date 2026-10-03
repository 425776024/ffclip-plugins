import { VideoCutClient, createProject, addAsset, addText, editTimeline, ticks } from '../packages/client/index.mjs';
import { renderTemplateExport } from '../src/editor/template-export';
import { SceneRenderer } from '../packages/render/renderer';
import { sharedMediaEngine } from '../packages/media/browser';
const fixtures='/Users/jxinfa/WebstormProjects/videocut/.local/architecture-qa';
const output=fixtures+'/render-results';
const report:any={phase:'idle',suite:'HD60',results:[]}, status=document.querySelector('#status')!;
async function publish(){status.textContent=JSON.stringify(report,null,2);await fetch('http://127.0.0.1:4332/report',{method:'POST',headers:{'Content-Type':'application/json','X-QA-Report':'render'},body:JSON.stringify(report)}).catch(()=>{});}
const delay=(ms:number)=>new Promise(r=>setTimeout(r,ms));
async function run(){
 const client=new VideoCutClient(location.origin),sessions:string[]=[];let events:EventSource|undefined,timer:ReturnType<typeof setInterval>|undefined,scene:SceneRenderer|undefined;
 const controller=new AbortController();report.started=new Date().toISOString();report.userAgent=navigator.userAgent;report.results=[];delete report.error;
 try{
  report.phase='importing';await publish();await client.connect();
  const video=await client.importMedia(fixtures+'/long-play.mp4'),audio=await client.importMedia(fixtures+'/hour-pulses.flac');
  let p=createProject('1080p60s_browser_acceptance');p.canvas={width:1920,height:1080};p.frameRate={numerator:30000,denominator:1001};
  const clip=addAsset(p,video),sound=addAsset(p,audio);
  const title=addText(p,{content:'VideoCut 系统字体 · 1080p',length:ticks(60),template:{id:'studio-glow',version:1}});
  let edit=editTimeline(p,[{action:'trim_clip',itemId:clip.id,sourceInSeconds:1,durationSeconds:60},{action:'trim_clip',itemId:sound.id,sourceInSeconds:0,durationSeconds:60},{action:'split_clip',itemId:clip.id,atSeconds:30},{action:'set_transform',itemId:title.id,scaleX:.55,scaleY:.55,positionY:340}]);
  const second=edit.operations[2].rightItemId!;p=editTimeline(edit.project,[{action:'add_effect',itemId:clip.id,templateId:'lut',parameters:{preset:'warm',amount:.35}},{action:'add_effect',itemId:second,templateId:'glow',parameters:{radius:8,strength:.2}},{action:'add_transition',fromItemId:clip.id,toItemId:second,templateId:'wipe',durationSeconds:.5,parameters:{direction:'left'}},{action:'set_audio',itemId:sound.id,gainLinear:.5,fadeIn:12000,fadeOut:12000}]).project;
  const snapshot=await client.createSession(p);sessions.push(snapshot.id);
  events=new EventSource(client.eventsUrl(snapshot.id));await new Promise<void>((resolve,reject)=>{events!.onopen=()=>resolve();events!.onerror=()=>reject(Error('SSE unavailable'));});
  const pending=client.renderVideo(snapshot.id,snapshot.version,output,'mp4');pending.catch(()=>{});
  let job:any;for(let n=0;n<200;n++){job=await client.renderStatus(snapshot.id);if(job.id||job.jobId)break;await delay(25);}
  report.phase='exporting';report.project={size:p.canvas,frameRate:p.frameRate,duration:60,clips:p.timeline.tracks.flatMap(t=>t.items).length};report.progress=[];await publish();
  let polling=false;
  timer=setInterval(async()=>{if(polling)return;polling=true;try{const r=await client.renderStatus(snapshot.id);report.progress.push({elapsed:performance.now(),phase:r.phase,completed:r.completed,total:r.total});await publish();}finally{polling=false;}},1000);
  const start=performance.now();
  await renderTemplateExport(client,snapshot.id,job.id||job.jobId,controller.signal,()=>{},{preferredEncoding:'browser'});
  const receipt=await pending;clearInterval(timer);timer=undefined;
  report.export={...receipt,wallMs:performance.now()-start,status:await client.renderStatus(snapshot.id)};
  if(report.export.status.encoding!=='browser')throw Error('Unexpected local encoder');
  const asset=await client.importMedia(receipt.path);if(asset.width!==1920||asset.height!==1080)throw Error('Export resolution changed');
  const decoded=createProject();addAsset(decoded,asset);const playback=await client.createSession(decoded);sessions.push(playback.id);
  const urls=(id:string)=>client.mediaUrl(snapshot.id,id);
  scene=new SceneRenderer(document.querySelector('#preview') as HTMLCanvasElement,urls);
  for(const requested of [1,15,29.9,30.1,45,59.9]){
   const frame=await sharedMediaEngine.videoFrame(client.mediaUrl(playback.id,asset.id),requested);
   const time=ticks(frame.timestamp),render=await scene.render(p,time,1920,1080),reference=await scene.readPixels();
   const c=new OffscreenCanvas(1920,1080),ctx=c.getContext('2d')!;ctx.drawImage(frame.frame,0,0);frame.close();const image=ctx.getImageData(0,0,1920,1080).data;
   let sum=0,lit=0;for(let i=0;i<image.length;i+=4)for(let ch=0;ch<3;ch++){sum+=Math.abs(reference[i+ch]-image[i+ch]);if(image[i+ch]>10)lit++;}
   const mae=sum/(1920*1080*3);if(mae>5||lit<10000)throw Error(`Decoded 1080p comparison failed at ${requested}: ${mae}`);
   report.results.push({requested,actualPTS:frame.timestamp,mae,resourceBytes:render.resourceBytes,workingSetBytes:render.workingSetBytes,textureBudget:render.textureBudget});await publish();
  }
  report.phase='complete';report.completed=new Date().toISOString();await publish();
 }catch(error){report.phase='failed';report.error=String(error);await publish();}
 finally{controller.abort();if(timer)clearInterval(timer);events?.close();scene?.dispose();for(const id of sessions)await client.closeSession(id).catch(()=>{});}
}
document.querySelector('#run')!.addEventListener('click',()=>void run());
