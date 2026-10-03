import { createTextEngine } from '../dist/index.mjs';
import { createBrowserTextComposition } from '../dist/browser-composition.mjs';
import { recipes, loadRecipe } from './complex-recipes.mjs';
const $ = (id) => document.getElementById(id);
let engine,
  fonts,
  player,
  selected = recipes[0],
  playing = false,
  raf = 0,
  pending = Promise.resolve(),
  busy = false;
const reports = [];
const status = (message, error = false) => {
  $('status').textContent = message;
  $('status').className = error ? 'error' : '';
};
const diagnostics = (value) => ($('diagnostics').textContent = JSON.stringify(value, null, 2));
function disabled(value) {
  for (const element of document.querySelectorAll('button,input')) element.disabled = value;
  if (!value) for (const id of ['background', 'post']) $(id).disabled = !selected.external;
}
function options(timeUs) {
  return { timeUs, background: $('background').checked, postEffects: $('post').checked };
}
async function draw(timeUs) {
  $('time').value = String(timeUs / 1000);
  $('time-label').textContent = (timeUs / 1e6).toFixed(2) + ' s';
  pending = player.render(options(timeUs));
  const r = await pending;
  status(
    `WASM 文字 ${r.textMs.toFixed(1)} ms · 解码与 GPU 完成合计 ${r.totalMs.toFixed(1)} ms${r.mediaFrames.length ? ' · 装饰源帧 ' + r.mediaFrames.join(',') : ''}`
  );
  return r;
}
async function stop() {
  playing = false;
  cancelAnimationFrame(raf);
  $('play').textContent = '播放动画';
  await pending.catch(() => {});
}
async function action(fn) {
  if (busy) return;
  busy = true;
  await stop();
  disabled(true);
  try {
    await fn();
  } catch (e) {
    status(e.message, true);
    diagnostics({ error: e.stack });
  } finally {
    busy = false;
    disabled(false);
  }
}
function frameCanvas(pixels) {
  const canvas = document.createElement('canvas');
  canvas.width = pixels.width;
  canvas.height = pixels.height;
  const data = new Uint8ClampedArray(pixels.data);
  for (let i = 0; i < data.length; i += 4) {
    const a = data[i + 3];
    if (a)
      for (let c = 0; c < 3; c++) data[i + c] = Math.min(255, Math.round((data[i + c] * 255) / a));
  }
  canvas.getContext('2d').putImageData(new ImageData(data, pixels.width, pixels.height), 0, 0);
  return canvas;
}
async function filmstrip() {
  const cards = [];
  for (const t of [350000, 1000000, 1800000, 2600000]) {
    status(`生成动态片段 ${cards.length + 1}/4…`);
    await player.render(options(t));
    const pixels = await player.readPixels();
    const card = document.createElement('div');
    card.className = 'film-frame';
    const label = document.createElement('p');
    label.textContent = (t / 1e6).toFixed(2) + ' s';
    const canvas = frameCanvas(pixels);
    canvas.setAttribute('aria-label', `${selected.name} ${(t / 1e6).toFixed(2)} 秒`);
    card.append(canvas, label);
    cards.push(card);
  }
  $('frames').replaceChildren(...cards);
}
async function select(recipe) {
  selected = recipe;
  status('加载模板、字体与原始资源…');
  player?.dispose();
  player = null;
  // Each WebGPU canvas has exactly one owned device/context.
  const canvas = document.createElement('canvas');
  canvas.id = 'preview';
  canvas.setAttribute('aria-label', '复杂模板实时预览');
  $('stage').replaceChildren(canvas);
  const { bundle, sources, adapted } = await loadRecipe(recipe);
  $('title').textContent = recipe.name;
  $('subtitle').textContent = recipe.tag;
  $('text').value = recipe.text;
  $('background').checked = true;
  $('post').checked = true;
  for (const button of document.querySelectorAll('.template'))
    button.setAttribute('aria-pressed', String(button.dataset.id === recipe.id));
  player = await createBrowserTextComposition(engine, canvas, bundle, {
    fonts,
    bindings: { content: recipe.text },
    experimentalComposition: !!recipe.external,
    fallbackFonts: [{ id: fonts[1].id, family: 'Source Han Sans SC' }]
  });
  $('state').textContent = 'WASM + WebGPU';
  $('time').max = String(player.info.durationUs / 1000 - 1);
  diagnostics({
    sources,
    adapted,
    profile: player.profile,
    adapter: player.adapter,
    warnings: player.info.warnings
  });
  await filmstrip();
  await draw(recipe.timeUs);
}
function play() {
  if (busy || !player) return;
  if (playing) {
    stop();
    return;
  }
  playing = true;
  $('play').textContent = '暂停';
  const start = performance.now(),
    offset = Number($('time').value) * 1000;
  let frames = 0,
    windowStart = start;
  const tick = async () => {
    if (!playing) return;
    try {
      const t = Math.floor((offset + (performance.now() - start) * 1000) % player.info.durationUs);
      const r = await draw(t);
      frames++;
      if (performance.now() - windowStart > 1000) {
        status(
          `播放 ${Math.round((frames * 1000) / (performance.now() - windowStart))} FPS · 本帧总计 ${r.totalMs.toFixed(1)} ms`
        );
        frames = 0;
        windowStart = performance.now();
      }
      if (playing) raf = requestAnimationFrame(tick);
    } catch (e) {
      playing = false;
      $('play').textContent = '播放动画';
      status(e.message, true);
    }
  };
  raf = requestAnimationFrame(tick);
}
const hash = async (data) =>
  Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', data)), (b) =>
    b.toString(16).padStart(2, '0')
  ).join('');
const difference = (a, b) => {
  let sum = 0,
    changed = 0;
  for (let i = 0; i < a.length; i++) {
    const d = Math.abs(a[i] - b[i]);
    sum += d;
    if (d > 2) changed++;
  }
  return { meanByteDifference: sum / a.length, changedFraction: changed / a.length };
};
async function verify() {
  const samples = [];
  const hashes = [];
  for (const timeUs of [350000, 1000000, 1800000, 2600000]) {
    const r = await player.render({ timeUs });
    const p = await player.readPixels();
    const sha256 = await hash(p.data);
    hashes.push(sha256);
    samples.push({
      timeUs,
      totalMs: r.totalMs,
      textMs: r.textMs,
      mediaFrames: r.mediaFrames,
      sha256,
      nonzeroAlpha: p.data.filter((v, i) => i % 4 === 3 && v > 0).length
    });
  }
  await player.render({ timeUs: 1800000 });
  const baseline = await player.readPixels();
  const repeat = await hash(baseline.data);
  const result = {
    id: selected.id,
    sources: [selected.base, selected.backdrop, selected.animation].filter(Boolean),
    samples,
    deterministic: repeat === hashes[2],
    animated: new Set(hashes).size > 1,
    background: null,
    postEffects: null,
    edited: false
  };
  if (selected.external) {
    await player.render({ timeUs: 1800000, background: false });
    result.background = difference(baseline.data, (await player.readPixels()).data);
    await player.render({ timeUs: 1800000, postEffects: false });
    result.postEffects = difference(baseline.data, (await player.readPixels()).data);
  }
  const original = $('text').value;
  try {
    player.setText(original === '你好' ? '文字' : '你好');
    await player.render({ timeUs: 1800000 });
    result.edited = repeat !== (await hash((await player.readPixels()).data));
  } finally {
    player.setText(original);
  }
  result.passed =
    result.deterministic &&
    result.animated &&
    result.edited &&
    samples.some((s) => s.nonzeroAlpha > 100) &&
    (!selected.external ||
      (result.background.meanByteDifference > 0.01 &&
        result.postEffects.meanByteDifference > 0.01));
  const index = reports.findIndex((x) => x.id === selected.id);
  if (index < 0) reports.push(result);
  else reports[index] = result;
  const report = {
    profile: player.profile,
    timestamp: new Date().toISOString(),
    adapter: player.adapter,
    cases: reports,
    scope:
      'Real browser pixels, animation, background/post ablation, repeat seek and Chinese edit. Not native full-template pixel parity.'
  };
  const response = await fetch('/complex-test-report', {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify(report)
  });
  if (!response.ok) throw new Error('Cannot save complex template report');
  diagnostics(report);
  await draw(selected.timeUs);
  status(
    result.passed
      ? selected.external
        ? '检查通过：时间变化、重复定位、中文改字和图层贡献均有实际像素验证。'
        : '检查通过：时间变化、重复定位和中文改字均有实际像素验证。'
      : '检查未通过，展开记录查看差异。',
    !result.passed
  );
}
for (const recipe of recipes) {
  const button = document.createElement('button');
  button.className = 'template';
  button.dataset.id = recipe.id;
  button.setAttribute('aria-pressed', 'false');
  button.textContent = recipe.name;
  const label = document.createElement('small');
  label.textContent = recipe.tag;
  button.append(label);
  button.addEventListener('click', () => action(() => select(recipe)));
  $('templates').append(button);
}
$('play').addEventListener('click', play);
$('restart').addEventListener('click', async () => {
  await stop();
  $('time').value = '0';
  play();
});
$('time').addEventListener('input', () => {
  $('time-label').textContent = (Number($('time').value) / 1000).toFixed(2) + ' s';
});
$('time').addEventListener('pointerdown', () => {
  stop();
});
$('time').addEventListener('change', () =>
  action(() => draw(Math.round(Number($('time').value) * 1000)))
);
$('apply').addEventListener('click', () =>
  action(async () => {
    player.setText($('text').value);
    await filmstrip();
    await draw(selected.timeUs);
  })
);
for (const id of ['background', 'post'])
  $(id).addEventListener('change', () =>
    action(() => draw(Math.round(Number($('time').value) * 1000)))
  );
$('verify').addEventListener('click', () => action(verify));
disabled(true);
try {
  const catalog = await fetch('./catalog.json').then((r) => r.json());
  [engine, fonts] = await Promise.all([
    createTextEngine(),
    Promise.all(
      catalog.fonts.map(async (f) => ({
        ...f,
        bytes: new Uint8Array(await (await fetch(f.url)).arrayBuffer())
      }))
    )
  ]);
  await action(() => select(recipes[0]));
} catch (e) {
  status(e.message, true);
  diagnostics({ error: e.stack });
}
window.addEventListener('pagehide', () => {
  playing = false;
  cancelAnimationFrame(raf);
  pending.finally(() => {
    player?.dispose();
    engine?.dispose();
  });
});
