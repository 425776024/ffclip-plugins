import http from 'node:http';
import { writeFile, mkdir } from 'node:fs/promises';
import { resolve } from 'node:path';
const root = resolve(process.argv[2] || '.local/architecture-qa/render-results');
const origin = process.argv[3] || 'http://127.0.0.1:5189';
await mkdir(root, { recursive: true });
http
  .createServer(async (req, res) => {
    res.setHeader('Access-Control-Allow-Origin', origin);
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type, X-QA-Report');
    res.setHeader('Access-Control-Allow-Methods', 'POST, OPTIONS');
    if (req.method === 'OPTIONS') {
      res.writeHead(204).end();
      return;
    }
    if (
      req.method !== 'POST' ||
      req.headers.origin !== origin ||
      req.headers['x-qa-report'] !== 'render'
    ) {
      res.writeHead(403).end();
      return;
    }
    try {
      const url = new URL(req.url, 'http://127.0.0.1'),
        chunks = [];
      let length = 0;
      for await (const chunk of req) {
        length += chunk.length;
        if (length > 5 * 1024 * 1024) throw new Error('Report too large');
        chunks.push(chunk);
      }
      const body = Buffer.concat(chunks);
      if (url.pathname === '/report') {
        const data = JSON.parse(body);
        await writeFile(resolve(root, 'browser-report.json'), JSON.stringify(data, null, 2));
        if (data.phase === 'complete')
          await writeFile(
            resolve(root, `browser-report-complete-${Date.now()}.json`),
            JSON.stringify(data, null, 2)
          );
      } else if (url.pathname === '/image' && /^[a-z0-9-]+$/.test(url.searchParams.get('name'))) {
        await writeFile(resolve(root, url.searchParams.get('name') + '.png'), body);
      } else if (
        url.pathname === '/audio-file' &&
        /^aac-sync-[a-z]+\.(mp4|webm|f32)$/.test(url.searchParams.get('name'))
      ) {
        await writeFile(resolve(root, url.searchParams.get('name')), body);
      } else throw new Error('Unknown artifact');
      res.writeHead(200).end('ok');
    } catch (e) {
      res.writeHead(400).end(e.message);
    }
  })
  .listen(4332, '127.0.0.1', () => console.log(`Render QA reports: ${root}`));
