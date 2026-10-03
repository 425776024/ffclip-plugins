import http from 'node:http';
import { writeFile, mkdir } from 'node:fs/promises';
import { resolve } from 'node:path';
const root = resolve(process.argv[2] || '.local/architecture-qa/media-results');
await mkdir(root, { recursive: true });
http
  .createServer(async (req, res) => {
    res.setHeader('Access-Control-Allow-Origin', 'http://127.0.0.1:5189');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type, X-QA-Report');
    res.setHeader('Access-Control-Allow-Methods', 'POST, OPTIONS');
    if (req.method === 'OPTIONS') {
      res.writeHead(204).end();
      return;
    }
    if (
      req.method !== 'POST' ||
      req.url !== '/report' ||
      req.headers.origin !== 'http://127.0.0.1:5189' ||
      req.headers['x-qa-report'] !== 'media'
    ) {
      res.writeHead(403).end();
      return;
    }
    try {
      const chunks = [];
      let length = 0;
      for await (const chunk of req) {
        length += chunk.length;
        if (length > 2 * 1024 * 1024) throw Error('Report too large');
        chunks.push(chunk);
      }
      const report = JSON.parse(Buffer.concat(chunks));
      const serialized = JSON.stringify(report, null, 2);
      if (report.phase === 'complete' || report.phase === 'failed')
        await writeFile(resolve(root, `report-${Date.now()}-${report.phase}.json`), serialized);
      await writeFile(resolve(root, 'browser-report.json'), serialized);
      if (report.phase === 'complete') {
        const name =
          report.suite === 'long-play'
            ? 'long-play-validation.json'
            : report.results?.some(
                  (row) => row.name === 'portrait_rotation_PAR_Bframes_VFR_DPR1_2_same_pixels'
                )
              ? 'cache-geometry-validation.json'
              : report.results?.some((row) => row.name === 'complete_one_hour_stereo_waveform')
                ? 'full-waveform-validation.json'
                : null;
        if (name) await writeFile(resolve(root, name), serialized);
      }
      res.writeHead(200).end('ok');
    } catch (error) {
      res.writeHead(400).end(error.message);
    }
  })
  .listen(4333, '127.0.0.1', () => console.log(`Media QA reports: ${root}`));
