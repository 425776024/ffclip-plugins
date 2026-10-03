import { mkdir, writeFile, access } from 'node:fs/promises';
import { resolve, join } from 'node:path';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
const exec=promisify(execFile),root=resolve(process.argv[2]||'.local/architecture-qa');
await mkdir(root,{recursive:true});
const exists=async(path)=>{try{await access(path);return true;}catch{return false;}};
if(!await exists(join(root,'timecode.mp4')))await exec('ffmpeg',['-v','error','-y','-f','lavfi','-i','testsrc2=size=640x360:rate=30000/1001:duration=4','-c:v','libx264','-preset','fast','-crf','18','-movflags','+faststart',join(root,'timecode.mp4')]);
if(!await exists(join(root,'long-play.mp4')))await exec('ffmpeg',['-v','error','-n','-f','lavfi','-i','testsrc2=size=640x360:rate=30000/1001:duration=120','-c:v','libx264','-threads','4','-preset','fast','-crf','18','-bf','3','-movflags','+faststart',join(root,'long-play.mp4')]);
if(!await exists(join(root,'stereo.wav')))await exec('ffmpeg',['-v','error','-y','-f','lavfi','-i','aevalsrc=0.7*sin(2*PI*440*t)|-0.7*sin(2*PI*440*t):s=48000:d=4','-c:a','pcm_s16le',join(root,'stereo.wav')]);
await exec('ffmpeg',['-v','error','-y','-i',join(root,'timecode.mp4'),'-an','-vf',"select='if(lt(t,2),1,not(mod(n,3)))'",'-vsync','vfr','-c:v','libx264','-preset','fast','-crf','18','-movflags','+faststart',join(root,'vfr.mp4')]);
await exec('ffmpeg',['-v','error','-y','-f','lavfi','-i','aevalsrc=if(eq(mod(n\\,480000)\\,24000)\\,0.9\\,0)|if(eq(mod(n\\,480000)\\,264000)\\,-0.8\\,0):s=48000:d=3600','-c:a','flac',join(root,'hour-pulses.flac')]);
const metadataRoot=join(root,'probe');await mkdir(metadataRoot,{recursive:true});
if(!await exists(join(root,'portrait.mp4')))await exec('ffmpeg',['-v','error','-y','-f','lavfi','-i','testsrc2=size=360x640:rate=30000/1001:duration=4','-vf','drawgrid=width=8:height=8:thickness=1:color=white','-c:v','libx264','-bf','3','-crf','16','-movflags','+faststart',join(root,'portrait.mp4')]);
if(!await exists(join(root,'anamorphic.mp4')))await exec('ffmpeg',['-v','error','-y','-i',join(root,'timecode.mp4'),'-an','-vf','setsar=2/1','-c:v','libx264','-bf','3','-crf','16','-movflags','+faststart',join(root,'anamorphic.mp4')]);
await writeFile(join(metadataRoot,'captions.srt'),'1\n00:00:00,000 --> 00:00:02,000\nmetadata stream order\n');
await exec('ffmpeg',['-v','error','-y','-i',join(root,'timecode.mp4'),'-c','copy','-metadata:s:v:0','rotate=90',join(metadataRoot,'rotated.mov')]);
for(const extension of ['mp4','mkv'])await exec('ffmpeg',['-v','error','-y','-i',join(root,'timecode.mp4'),'-i',join(metadataRoot,'captions.srt'),'-map','1:0','-map','0:v','-map','0:a?','-c','copy',...(extension==='mp4'?['-c:s','mov_text']:[]),join(metadataRoot,`subtitle-first.${extension}`)]);
const reference={};
for(const name of ['timecode.mp4','vfr.mp4','portrait.mp4','anamorphic.mp4','probe/rotated.mov']){
 const {stdout}=await exec('ffprobe',['-v','error','-select_streams','v:0','-show_streams','-show_frames','-show_entries','frame=best_effort_timestamp:stream=time_base','-of','json',join(root,name)]);
 const data=JSON.parse(stdout),[numerator,denominator]=data.streams[0].time_base.split('/').map(Number);
 reference[name]=data.frames.map(frame=>frame.best_effort_timestamp*numerator/denominator);
}
await writeFile(join(root,'media-pts.json'),JSON.stringify(reference));
console.log(`Prepared deterministic media fixtures: ${root}`);
