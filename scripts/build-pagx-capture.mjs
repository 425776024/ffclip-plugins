// Regenerate the pinned upstream DOM-to-vector capture payloads. No browser or raster capture.
import { build } from 'esbuild';
import { mkdtemp, mkdir, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
const revision = 'ec175d317dc70d171beed2d34af0eaef205d7710';
const dir = await mkdtemp(join(tmpdir(), 'videocut-pagx-capture-'));
try {
  const hashes = {};
  for (const name of ['animation-capture', 'browser-snapshot', 'common', 'dom-tags']) {
    const source = execFileSync('curl', [
      '-fLsS',
      '--max-time',
      '60',
      `https://raw.githubusercontent.com/Tencent/libpag/${revision}/tools/html-snapshot/lib/${name}.ts`
    ]);
    hashes[name] = createHash('sha256').update(source).digest('hex');
    await writeFile(join(dir, name + '.ts'), source);
  }
  async function load(name) {
    const result = await build({
      entryPoints: [join(dir, name + '.ts')],
      bundle: true,
      format: 'esm',
      platform: 'node',
      write: false,
      minify: false,
      target: 'es2022'
    });
    return import(
      'data:text/javascript;base64,' + Buffer.from(result.outputFiles[0].text).toString('base64')
    );
  }
  const a = await load('animation-capture'),
    b = await load('browser-snapshot');
  // VideoCut has one finite absolute clock. Bake all declarative and JS animation
  // sources onto that clock; per-element looping would create multiple timelines.
  const capture = a.buildAnimationCapturePayload({ sampleCount: 24, maxElements: 500 });
  const marker = 'return pagxAnimMain(';
  if (!capture.includes(marker)) throw Error('Upstream capture entry changed');
  const adapter = `pagxMeasureGlobalDurationMs = () => window.__videocutDurationMs;
    pagxCollectWAAPI = () => {}; pagxCollectCSS = () => {}; pagxCollectTransitions = () => {};
    const originalSeek = pagxSeekAllToTime;
    pagxSeekAllToTime = t => { originalSeek(t); window.__videocutSampleTime(t); };
    pagxGlobalSampleCount = () => Math.ceil(window.__videocutDurationMs * 60 / 1000) + 1;
    `;
  const value = {
    revision,
    hashes,
    init: a.PAGX_ANIM_PAUSE_INIT_SCRIPT + '\n' + a.PAGX_VIRTUAL_CLOCK_INIT_SCRIPT,
    snapshot: b.takeSnapshot,
    pseudos: '(' + b.materializeDecorativePseudoElements.toString() + ')()',
    capture: capture.replace(marker, adapter + marker)
  };
  await mkdir('packages/pagx', { recursive: true });
  await writeFile('packages/pagx/capture.json', JSON.stringify(value) + '\n');
  console.log('PAGX capture payload pinned to ' + revision);
} finally {
  await rm(dir, { recursive: true, force: true });
}
