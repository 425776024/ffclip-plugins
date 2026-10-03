import { parentPort, workerData } from 'node:worker_threads';
import { createTextEngine } from '../dist/index.mjs';
import { fixture, registerFonts, nontransparent } from '../tests/helpers.mjs';
let engine;
try {
  engine = await createTextEngine();
  const renderer = engine.createRenderer();
  registerFonts(renderer);
  await renderer.loadTemplate(await fixture(workerData.id), { allowRasterFallback: true, bindings: { content: '文字 WASM' } });
  parentPort.postMessage('rendering');
  const start = performance.now();
  const frame = renderer.render({ timeUs: 1500000, width: 640, height: 360 });
  const renderMs = performance.now() - start;
  const visiblePixels = nontransparent(frame);
  parentPort.postMessage({ status: visiblePixels ? 'rendered' : 'empty', renderMs, visiblePixels, warnings: frame.warnings });
} catch (e) { parentPort.postMessage({ status: 'error', error: e.message }); }
finally { engine?.dispose(); }
