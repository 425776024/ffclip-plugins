import { writeFile, mkdir } from 'node:fs/promises';
import { join } from 'node:path';
import { Worker } from 'node:worker_threads';
import { catalog } from '../tests/helpers.mjs';
import { root } from './common.mjs';
const records = [];
const renderBudgetMs = 3000;
function sample(item) {
  return new Promise((resolve) => {
    const worker = new Worker(new URL('./audit-worker.mjs', import.meta.url), { workerData: item });
    let settled = false;
    let timer = setTimeout(() => done({ status: 'timeout', error: 'Initialization exceeded 15 s' }), 15000);
    function done(result) {
      if (settled) return;
      settled = true; clearTimeout(timer); worker.terminate();
      resolve({ id: item.id, kind: item.kind, ...result });
    }
    worker.on('message', (message) => {
      if (message === 'rendering') {
        clearTimeout(timer);
        timer = setTimeout(() => done({ status: 'timeout', error: `Render exceeded ${renderBudgetMs} ms CPU budget` }), renderBudgetMs);
      } else done(message);
    });
    worker.on('error', (error) => done({ status: 'error', error: error.message }));
    worker.on('exit', (code) => { if (!settled) done({ status: 'error', error: `Worker exited: ${code}` }); });
  });
}
await mkdir(join(root, '.cache'), { recursive: true });
for (const item of catalog.templates) {
  const record = await sample(item);
  records.push(record);
  console.log(`${records.length}/${catalog.templates.length} ${record.status} ${item.id} ${record.error || record.renderMs.toFixed(1) + 'ms'}`);
  await writeFile(join(root, '.cache/template-audit.json'), JSON.stringify({
    profile: 'wasm-raster-v1', renderBudgetMs,
    note: 'One 640x360 midpoint sample, explicit raster fallback. Coverage only; no native fidelity, animation completeness or browser FPS acceptance. Timeouts are not proof of unsupported templates.',
    counts: records.reduce((out, r) => { out[r.status] = (out[r.status] || 0) + 1; return out; }, {}),
    tested: records.length, total: catalog.templates.length, records
  }, null, 2) + '\n');
}
