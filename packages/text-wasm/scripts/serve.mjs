import { createServer } from 'node:http';
import { readFile, realpath, stat, writeFile } from 'node:fs/promises';
import { resolve, extname, sep } from 'node:path';
import { root } from './common.mjs';
const port = Number(process.env.PORT || 4321);
const types = { '.html': 'text/html; charset=utf-8', '.mjs': 'text/javascript', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.wasm': 'application/wasm', '.png': 'image/png', '.ttf': 'font/ttf', '.otf': 'font/otf' };
const server = createServer(async (req, res) => {
  try {
    const url = new URL(req.url, `http://127.0.0.1:${port}`);
    if (req.method === 'POST' && ['/gpu-test-report','/complex-test-report','/performance-test-report'].includes(url.pathname)) {
      if (req.headers.origin !== `http://127.0.0.1:${port}`) { res.writeHead(403).end(); return; }
      const chunks=[]; let size=0;
      for await (const chunk of req) { size+=chunk.length; if(size>1024*1024) {res.writeHead(413).end();return;} chunks.push(chunk); }
      const report=JSON.parse(Buffer.concat(chunks).toString('utf8'));
      const complex=url.pathname==='/complex-test-report';
      const perf=url.pathname==='/performance-test-report';
      if(report.profile!==(perf?'wasm-simd-comparison-v1':complex?'browser-composition-experimental-v1':'webgpu-effects-experimental-v1')||!Array.isArray(report.cases)) {res.writeHead(400).end();return;}
      if(complex){
        let previous=[];
        try{previous=JSON.parse(await readFile(resolve(root,'reports/complex-validation.json'),'utf8')).cases||[];}catch{}
        report.cases=[...new Map([...previous,...report.cases].map(value=>[value.id,value])).values()];
      }
      await writeFile(resolve(root,perf?'reports/performance-validation.json':complex?'reports/complex-validation.json':'reports/gpu-validation.json'),JSON.stringify(report,null,2)+'\n');
      res.writeHead(200,{'content-type':'application/json'}).end('{"saved":true}'); return;
    }
    if (req.method !== 'GET' && req.method !== 'HEAD') { res.writeHead(405).end(); return; }
    if (url.pathname === '/' || url.pathname === '/demo/') { res.writeHead(302, { location: '/demo/index.html' }).end(); return; }
    const pathname = decodeURIComponent(url.pathname);
    const perfFiles = new Map(['/perf-reference/baseline.wasm','/perf-reference/index.mjs','/perf-reference/videocut-text.mjs'].map(p=>[p,p.replace('/perf-reference/','/.cache/perf/')]));
    if (!/^\/(demo|dist|fixtures|gpu-reference)\//.test(pathname) && !perfFiles.has(pathname)) { res.writeHead(404).end(); return; }
    const mapped=perfFiles.get(pathname)|| (pathname.startsWith('/gpu-reference/')?pathname.replace('/gpu-reference/','/.cache/gpu-reference/'):pathname);
    const path = await realpath(resolve(root, '.' + mapped));
    if (!path.startsWith(resolve(root) + sep) || !(await stat(path)).isFile()) { res.writeHead(404).end(); return; }
    res.writeHead(200, { 'content-type': types[extname(path)] || 'application/octet-stream', 'cache-control': 'no-cache', 'x-content-type-options': 'nosniff' });
    res.end(req.method === 'HEAD' ? undefined : await readFile(path));
  } catch { res.writeHead(404).end(); }
});
server.listen(port, '127.0.0.1', () => console.log(`Text WASM demo: http://127.0.0.1:${port}/`));
