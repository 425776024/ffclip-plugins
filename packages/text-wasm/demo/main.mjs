import { createTextEngine, loadTextTemplate, sha256 } from '../dist/index.mjs';
const $ = (id) => document.getElementById(id);
const catalog = await fetch('./catalog.json').then((r) => r.json());
let engine, renderer, fontData, generation = 0, playing = false, animationId = 0;
function status(message, error = false) { $('status').textContent = message; $('status').className = error ? 'error' : ''; }
function stop() { playing = false; cancelAnimationFrame(animationId); $('play').textContent = '播放'; }
function draw() {
  if (!renderer?.info) return;
  const start = performance.now();
  const frame = renderer.draw($('canvas'), { timeUs: Number($('time').value) * 1000 });
  $('render-ms').textContent = `${(performance.now() - start).toFixed(1)} ms`;
  $('time-label').textContent = `${(Number($('time').value) / 1000).toFixed(3)} s`;
  $('empty').style.display = 'none';
  return frame;
}
function fillTemplates() {
  const items = catalog.templates.filter((t) => t.kind === $('kind').value);
  $('template').replaceChildren(...items.map((item) => {
    const unavailable = item.sampleStatus !== 'rendered';
    const label = item.name.replace(/^Qt Flower /, '').replace(/^ANIM-/, '');
    const option = new Option(unavailable ? `${label} · ${item.sampleStatus === 'timeout' ? '计算超时' : '暂不支持'}` : label, item.id);
    option.disabled = unavailable; return option;
  }));
  if ($('kind').value === 'text_flower') $('template').value = 'com.videocut.text.qt-type.flower-style-09';
  if ($('kind').value === 'text_animation') $('template').value = 'com.videocut.text.qt-type.anim-lua-opacity';
}
async function load() {
  const current = ++generation; stop();
  renderer?.dispose(); renderer = null;
  $('apply').disabled = $('play').disabled = $('benchmark').disabled = true;
  $('empty').textContent = '正在加载模板…'; $('empty').style.display = 'grid';
  $('canvas').getContext('2d').clearRect(0, 0, $('canvas').width, $('canvas').height);
  const local = engine.createRenderer();
  try {
    const entry = catalog.templates.find((t) => t.id === $('template').value);
    const bundle = await loadTextTemplate(new URL(entry.url, location.href));
    if (current !== generation) { local.dispose(); return; }
    for (const font of fontData) local.registerAsset(font.id, font.mediaType, font.bytes);
    const info = await local.loadTemplate(bundle, { bindings: { content: $('text').value }, allowRasterFallback: $('fallback').checked,
      fallbackFonts: [{ id: catalog.fonts[1].id, family: 'Source Han Sans SC' }] });
    if (current !== generation) { local.dispose(); return; }
    renderer = local;
    $('time').max = String(Math.max(1, Math.floor(info.durationUs / 1000) - 1));
    $('time').value = String(Math.min(Number($('time').value), Number($('time').max)));
    draw();
    status(info.warnings.length ? '已绘制 · 使用跨平台栅格路径，未验证与桌面 Metal 逐像素一致。' : '已绘制');
    $('diagnostics').textContent = JSON.stringify(info, null, 2);
    $('apply').disabled = $('play').disabled = $('benchmark').disabled = false;
  } catch (error) {
    local.dispose(); if (renderer === local) renderer = null;
    if (current === generation) { status(error.message, true); $('empty').textContent = '此模板当前无法绘制'; $('diagnostics').textContent = error.stack; }
  }
}
function safeDraw() { try { draw(); } catch (e) { stop(); status(e.message, true); } }
$('kind').addEventListener('change', () => { fillTemplates(); load(); });
$('template').addEventListener('change', load);
$('fallback').addEventListener('change', load);
$('time').addEventListener('input', () => { stop(); safeDraw(); });
$('resolution').addEventListener('change', () => { $('canvas').width = Number($('resolution').value); $('canvas').height = $('canvas').width * 9 / 16; safeDraw(); });
$('apply').addEventListener('click', () => { stop(); try { renderer.setText($('text').value); draw(); status('文字已更新'); } catch (e) { status(e.message, true); } });
$('play').addEventListener('click', () => {
  if (playing) { stop(); return; }
  playing = true; $('play').textContent = '暂停';
  const start = performance.now() - Number($('time').value); let previous = performance.now(), count = 0;
  const tick = () => {
    if (!playing) return;
    const now = performance.now(); $('time').value = String(Math.floor((now - start) % Number($('time').max)));
    safeDraw(); count++;
    if (now - previous > 1000) { $('fps').textContent = `${(count * 1000 / (now - previous)).toFixed(1)} FPS`; previous = now; count = 0; }
    if (playing) animationId = requestAnimationFrame(tick);
  }; animationId = requestAnimationFrame(tick);
});
$('benchmark').addEventListener('click', async () => {
  stop(); $('benchmark').disabled = true; const current = generation;
  try {
    const times = []; const elapsedStart = performance.now();
    for (let i = 0; i < 60; i++) {
      if (current !== generation) return;
      $('time').value = String(Math.floor((i + .5) / 60 * Number($('time').max)));
      const start = performance.now(); draw(); times.push(performance.now() - start);
      await new Promise(requestAnimationFrame);
    }
    times.sort((a, b) => a - b);
    const report = { frames: 60, width: $('canvas').width, height: $('canvas').height,
      medianDrawMs: times[30], p95DrawMs: times[56], elapsedMs: performance.now() - elapsedStart,
      note: 'WASM render + RGBA copy + putImageData submission. Does not measure compositor presentation latency or prove native fidelity.' };
    $('diagnostics').textContent = JSON.stringify(report, null, 2);
    status(`60 帧完成 · 中位 ${report.medianDrawMs.toFixed(1)} ms · P95 ${report.p95DrawMs.toFixed(1)} ms`);
  } catch (e) { status(e.message, true); }
  finally { if (current === generation) $('benchmark').disabled = !renderer?.info; }
});
fillTemplates();
try {
  [engine, fontData] = await Promise.all([createTextEngine(), Promise.all(catalog.fonts.map(async (font) => {
    const response = await fetch(font.url); if (!response.ok) throw new Error('字体加载失败');
    const bytes = new Uint8Array(await response.arrayBuffer());
    if (await sha256(bytes) !== font.sha256) throw new Error('字体校验失败');
    return { ...font, bytes };
  }))]);
  $('engine-state').textContent = 'WASM 已就绪';
  await load();
} catch (e) { status(e.message, true); $('engine-state').textContent = '加载失败'; $('diagnostics').textContent = e.stack; }
window.addEventListener('pagehide', () => { stop(); engine?.dispose(); });
