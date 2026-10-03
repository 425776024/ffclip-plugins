import { createTextEngine, loadTextTemplate } from '../dist/index.mjs';
import { createTextGpuEffects } from '../dist/webgpu.mjs';
const $ = (id) => document.getElementById(id);
const catalog = await fetch('./catalog.json').then((r) => r.json());
let engine,
  renderer,
  gpu,
  busy = false,
  playing = false,
  raf = 0;
const report = (data) => {
  $('diagnostics').textContent = JSON.stringify(data, null, 2);
};
function status(text, error = false) {
  $('status').textContent = text;
  $('status').className = error ? 'error' : '';
}
function options() {
  const n = Number($('strength').value);
  return {
    effect: $('effect').value,
    radius: n,
    sigma: Math.max(n / 2, 0.1),
    exposure: 1 + n / 12,
    strokeWidth: $('effect').value === 'sdf' ? n : 18
  };
}
function stop() {
  playing = false;
  cancelAnimationFrame(raf);
  $('animate').textContent = '播放强度动画';
}
function draw() {
  $('strength-value').textContent = $('strength').value;
  const r = gpu.render(options());
  $('timing').textContent = `CPU 提交 ${r.submissionMs.toFixed(2)} ms · GPU 绘制`;
  return r;
}
async function update(showStatus = true) {
  stop();
  try {
    renderer.setText($('text').value);
    const start = performance.now();
    gpu.setMesh(renderer.prepareSdfMesh({ width: 640, height: 360, range: 30 }));
    draw();
    await gpu.completed();
    if (showStatus) status(`字形重建与首次 GPU 绘制 ${(performance.now() - start).toFixed(1)} ms`);
  } catch (e) {
    status(e.message, true);
  }
}
function disable(value) {
  for (const id of ['apply', 'animate', 'benchmark', 'verify', 'effect', 'strength', 'text'])
    $(id).disabled = value;
}
async function benchmark() {
  if (busy) return;
  busy = true;
  stop();
  disable(true);
  try {
    const times = [];
    const total = performance.now();
    for (let i = 0; i < 60; i++) {
      const start = performance.now();
      gpu.render({
        ...options(),
        radius: 8 + 8 * Math.sin((i / 60) * Math.PI) ** 2,
        strokeWidth: 8 + 8 * Math.sin((i / 60) * Math.PI) ** 2
      });
      await gpu.completed();
      times.push(performance.now() - start);
    }
    times.sort((a, b) => a - b);
    const result = {
      profile: gpu.profile,
      adapter: gpu.adapterInfo,
      effect: $('effect').value,
      width: 640,
      height: 360,
      frames: 60,
      medianCompletedMs: times[30],
      p95CompletedMs: times[56],
      totalMs: performance.now() - total,
      note: 'Wall time from submission to GPU queue completion; not physical presentation latency; warmed SDF cached.'
    };
    report(result);
    status(
      `60 帧完成 · GPU 队列完成中位 ${times[30].toFixed(2)} ms · P95 ${times[56].toFixed(2)} ms`
    );
  } catch (e) {
    status(e.message, true);
  } finally {
    busy = false;
    disable(false);
  }
}
async function gallery() {
  const cards = [];
  for (const [effect, label] of [
    ['sdf', 'SDF 描边'],
    ['gaussian', '高斯模糊'],
    ['soft-glow', '柔光']
  ]) {
    gpu.render({ effect, strokeWidth: 18, radius: 12, sigma: 6, exposure: 1.5 });
    const f = await gpu.readPixels();
    const data = new Uint8ClampedArray(f.data);
    for (let i = 0; i < data.length; i += 4) {
      const a = data[i + 3];
      if (a) {
        for (let c = 0; c < 3; c++)
          data[i + c] = Math.min(255, Math.round((data[i + c] * 255) / a));
      }
    }
    const card = document.createElement('div'),
      title = document.createElement('p'),
      canvas = document.createElement('canvas');
    title.textContent = label;
    canvas.width = f.width;
    canvas.height = f.height;
    canvas.setAttribute('aria-label', label + ' GPU 结果');
    canvas.getContext('2d').putImageData(new ImageData(data, f.width, f.height), 0, 0);
    card.append(title, canvas);
    cards.push(card);
  }
  $('comparisons').replaceChildren(...cards);
  draw();
}
$('apply').addEventListener('click', update);
$('effect').addEventListener('change', () => {
  stop();
  try {
    draw();
    status('效果已更新');
  } catch (e) {
    status(e.message, true);
  }
});
$('strength').addEventListener('input', () => {
  try {
    draw();
  } catch (e) {
    status(e.message, true);
  }
});
$('benchmark').addEventListener('click', benchmark);
$('animate').addEventListener('click', () => {
  if (playing) {
    stop();
    return;
  }
  playing = true;
  $('animate').textContent = '暂停';
  const start = performance.now();
  let last = start,
    count = 0;
  const tick = () => {
    if (!playing) return;
    try {
      const now = performance.now();
      $('strength').value = String(Math.round(12 + 10 * Math.sin((now - start) / 700)));
      draw();
      count++;
      if (now - last >= 1000) {
        status(`GPU 效果强度动画 · ${((count * 1000) / (now - last)).toFixed(1)} FPS`);
        count = 0;
        last = now;
      }
      raf = requestAnimationFrame(tick);
    } catch (e) {
      stop();
      status(e.message, true);
    }
  };
  raf = requestAnimationFrame(tick);
});
$('verify').addEventListener('click', async () => {
  if (busy) return;
  busy = true;
  stop();
  disable(true);
  try {
    const { verifyGpu } = await import('./gpu-verify.mjs');
    const result = await verifyGpu(gpu);
    report(result);
    status(result.passed ? 'Metal / WebGPU 对比通过' : '对比有差异，请查看记录', !result.passed);
  } catch (e) {
    status(e.message, true);
    report({ error: e.stack });
  } finally {
    busy = false;
    disable(false);
    await update(false);
  }
});
try {
  disable(true);
  [engine, gpu] = await Promise.all([createTextEngine(), createTextGpuEffects($('gpu'))]);
  renderer = engine.createRenderer();
  for (const f of catalog.fonts)
    renderer.registerAsset(
      f.id,
      f.mediaType,
      new Uint8Array(await (await fetch(f.url)).arrayBuffer())
    );
  await renderer.loadTemplate(
    await loadTextTemplate(
      new URL(
        '../fixtures/templates/com.videocut.text.qt-type.flower-style-09/manifest.json',
        location.href
      )
    ),
    {
      allowRasterFallback: true,
      bindings: { content: $('text').value },
      fallbackFonts: [{ id: catalog.fonts[1].id, family: 'Source Han Sans SC' }]
    }
  );
  gpu.setMesh(renderer.prepareSdfMesh({ width: 640, height: 360, range: 30 }));
  draw();
  await gpu.completed();
  await gallery();
  $('engine-state').textContent = 'WebGPU 已就绪';
  report({ profile: gpu.profile, adapter: gpu.adapterInfo });
  status('SDF 网格、距离场与材质由 GPU 实际绘制');
  disable(false);
} catch (e) {
  status(e.message, true);
  $('engine-state').textContent = 'GPU 初始化失败';
  report({ error: e.stack });
}
window.addEventListener('pagehide', () => {
  stop();
  gpu?.dispose();
  engine?.dispose();
});
