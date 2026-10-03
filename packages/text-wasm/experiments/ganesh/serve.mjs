import { createServer } from 'node:http';
import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { resolve, join, extname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadRecipe } from '../../demo/complex-recipes.mjs';
import { chooseTemplateFonts, rewriteTemplateFonts } from '../../src/system-fonts.mjs';
import { systemFonts } from '../../../server/system-fonts.mjs';
const workspace=fileURLToPath(new URL('../../../../',import.meta.url));
const experiment=resolve(process.env.VIDEOCUT_GPU_EXPERIMENT_DIR||join(workspace,'.local/qa/wasm-gpu-next'));
process.chdir(workspace);
const catalog=await systemFonts.catalog(),selected=chooseTemplateFonts(catalog);
const fonts=await Promise.all([...new Map(Object.values(selected).map(f=>[f.id,f])).values()].map(async f=>({...f,...await systemFonts.read(f.id)})));
const recipes=[
  {id:'flower03',base:'flower-style-03'},
  {id:'flower38',base:'flower-style-38'},
  {id:'flower03-tile-letter',base:'flower-style-03',backdrop:'bubble-tile',animation:'anim-lua-letter-transform'},
  {id:'flower38-nine-letter',base:'flower-style-38',backdrop:'bubble-nine-slice',animation:'anim-lua-letter-transform'}
];
const bundles=await Promise.all(recipes.map(async recipe=>{
  const {bundle}=await loadRecipe(recipe,async url=>new Response(await readFile(url)));
  const value=rewriteTemplateFonts(bundle,catalog);
  return {...recipe,bundle:{...value,assets:[...value.assets].map(([key,asset])=>[key,{...asset,bytes:Buffer.from(asset.bytes).toString('base64')}])}};
}));
const config={width:1920,height:1080,fonts:fonts.map((f,i)=>({id:f.id,mediaType:f.mime,family:f.family,url:'/font-'+i})),recipes:bundles};
const server=createServer(async(req,res)=>{
 try {
  const path=new URL(req.url,'http://localhost').pathname;
  if(path==='/config.json'){res.setHeader('Content-Type','application/json');res.end(JSON.stringify(config));return;}
  if(/^\/font-\d+$/.test(path)){const font=fonts[Number(path.slice(6))];if(!font)throw Error('Font missing');res.setHeader('Content-Type',font.mime);res.end(font.bytes);return;}
  if(path==='/report'&&req.method==='POST'){
   const chunks=[];for await(const chunk of req)chunks.push(chunk);
   const json=JSON.parse(Buffer.concat(chunks).toString());
   await writeFile(join(experiment,'report.json'),JSON.stringify(json,null,2)+'\n');
   res.end('saved');return;
  }
  if(/^\/artifact\/[a-z0-9-]+\.png$/.test(path)&&req.method==='POST'){
   const chunks=[];for await(const chunk of req)chunks.push(chunk);
   await mkdir(join(experiment,'artifacts'),{recursive:true});
   await writeFile(join(experiment,'artifacts',path.slice(10)),Buffer.concat(chunks));res.end('saved');return;
  }
  const relative=path==='/'?'demo.html':path.slice(1);
  const file=resolve(experiment,relative);
  if(!file.startsWith(experiment+'/'))throw Error('Invalid path');
  const bytes=await readFile(file);
  res.setHeader('Content-Type',({'.html':'text/html','.mjs':'text/javascript','.js':'text/javascript','.wasm':'application/wasm','.json':'application/json','.png':'image/png'})[extname(file)]||'application/octet-stream');
  res.end(bytes);
 }catch(error){res.writeHead(404);res.end(error.message);}
});
server.listen(Number(process.env.VIDEOCUT_GPU_EXPERIMENT_PORT||0),'127.0.0.1',()=>console.log('WebGL flower experiment '+JSON.stringify({port:server.address().port,url:'http://127.0.0.1:'+server.address().port})));
