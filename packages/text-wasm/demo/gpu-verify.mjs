const binary = async (name) => {
  const r = await fetch(`/gpu-reference/${name}`);
  if (!r.ok)
    throw new Error(
      `Missing Metal fixture: ${name}. Run npm run test:gpu:reference on macOS first.`
    );
  return new Uint8Array(await r.arrayBuffer());
};
const json = async (name) => {
  const r = await fetch(`/gpu-reference/${name}`);
  if (!r.ok) throw new Error('Metal reference has not been generated');
  return r.json();
};
function compare(a, b, channels = [0, 1, 2, 3]) {
  if (a.length !== b.length) throw new Error('Reference size mismatch');
  let total = 0,
    max = 0,
    changed = 0,
    above2 = 0,
    n = 0;
  for (let i = 0; i < a.length; i += 4)
    for (const c of channels) {
      const d = Math.abs(a[i + c] - b[i + c]);
      total += d;
      max = Math.max(max, d);
      if (d) changed++;
      if (d > 2) above2++;
      n++;
    }
  return {
    meanAbsByteError: total / n,
    maxByteError: max,
    differentChannelFraction: changed / n,
    above2ByteFraction: above2 / n
  };
}
function distanceCompare(a, b) {
  let total = 0,
    max = 0,
    bad = 0;
  const n = a.length / 4;
  for (let i = 0; i < a.length; i += 4) {
    const d = Math.abs(a[i] / 256 + a[i + 1] - (b[i] / 256 + b[i + 1])) / 255;
    total += d;
    max = Math.max(max, d);
    if (d > 0.01) bad++;
  }
  return {
    meanNormalizedDistanceError: total / n,
    maxNormalizedDistanceError: max,
    aboveOnePercentFraction: bad / n
  };
}
const hash = async (data) =>
  Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', data)), (b) =>
    b.toString(16).padStart(2, '0')
  ).join('');
export async function verifyGpu(gpu) {
  const reference = await json('reference.json');
  const info = await json('mesh.json');
  const mesh = {
    ...info,
    distanceVertices: new Float32Array((await binary('distance.bin')).buffer),
    shapeVertices: new Float32Array((await binary('shape.bin')).buffer)
  };
  const cases = [];
  gpu.setMesh(mesh);
  gpu.render({ effect: 'sdf' });
  await gpu.completed();
  const field = await gpu.readPixels({ distanceField: true });
  const metalField = await binary('sdf.rgba');
  const d = distanceCompare(field.data, metalField);
  const visible = field.data.some((v, i) => i % 4 === 1 && v > 128);
  cases.push({
    effect: 'sdf-distance-field',
    ...d,
    visible,
    passed: visible && d.meanNormalizedDistanceError < 0.0001 && d.aboveOnePercentFraction < 0.001,
    scope:
      'Native MetalTextSdf algorithm on the same SDK mesh; decoded RG distances compared, not full native atlas/material/layout parity.'
  });
  const input = await binary('input.rgba');
  for (const effect of ['gaussian', 'soft-glow']) {
    gpu.setSource({ width: 640, height: 360, data: input });
    gpu.render({
      effect,
      radius: 12,
      sigma: 6,
      exposure: 1.5,
      threshold: 0.15,
      glowColor: [1, 0.65, 0.3]
    });
    await gpu.completed();
    const frame = await gpu.readPixels();
    const originalHash = await hash(frame.data);
    const baseline = await binary(`${effect}.rgba`);
    const metrics = compare(frame.data, baseline);
    gpu.render({ effect, radius: 0, sigma: 6, exposure: 1.5 });
    const identity = await gpu.readPixels();
    const change = compare(frame.data, identity.data).meanAbsByteError;
    gpu.render({
      effect,
      radius: 12,
      sigma: 6,
      exposure: 1.5,
      threshold: 0.15,
      glowColor: [1, 0.65, 0.3]
    });
    const repeat = await gpu.readPixels();
    const deterministic = (await hash(repeat.data)) === originalHash;
    cases.push({
      effect,
      ...metrics,
      parameterChangeMeanError: change,
      deterministic,
      passed:
        metrics.meanAbsByteError < 0.1 &&
        metrics.above2ByteFraction < 0.001 &&
        change > 0.05 &&
        deterministic
    });
  }
  gpu.setMesh(mesh);
  let outline = false;
  gpu.render({ effect: 'sdf' });
  const before = await gpu.readPixels();
  gpu.render({ effect: 'sdf', strokeWidth: 0 });
  const after = await gpu.readPixels();
  outline = compare(before.data, after.data).meanAbsByteError > 0.1;
  cases.push({ effect: 'sdf-stroke-control', passed: outline });
  const timings = [];
  for (const effect of ['sdf', 'gaussian', 'soft-glow']) {
    gpu.render({ effect });
    await gpu.completed();
    const values = [];
    const start = performance.now();
    for (let i = 0; i < 60; i++) {
      const frameStart = performance.now();
      gpu.render({ effect, radius: 8 + (i % 9), strokeWidth: 8 + (i % 9), sigma: 6 });
      await gpu.completed();
      values.push(performance.now() - frameStart);
    }
    const total = performance.now() - start;
    values.sort((a, b) => a - b);
    timings.push({
      effect,
      frames: 60,
      width: 640,
      height: 360,
      medianCompletedMs: values[30],
      p95CompletedMs: values[56],
      totalMs: total
    });
  }
  const portSha256 = {};
  for (const file of ['webgpu.mjs', 'gpu-shaders.mjs']) {
    const response = await fetch(`../dist/${file}`);
    if (!response.ok) throw new Error(`Cannot hash GPU port: ${file}`);
    portSha256[file] = await hash(await response.arrayBuffer());
  }
  const report = {
    profile: gpu.profile,
    timestamp: new Date().toISOString(),
    adapter: gpu.adapterInfo,
    reference,
    portSha256,
    cases,
    timings,
    passed: cases.every((c) => c.passed),
    notes: [
      'GPU readback comparisons against unmodified native Metal shaders.',
      'SDF uses the same WASM-generated base outline for both backends. This does not validate full desktop template parity.',
      'Timings include JS submission and GPU queue completion, exclude first shader compilation and cached SDF mesh generation; no claim about screen presentation latency.'
    ]
  };
  const response = await fetch('/gpu-test-report', {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify(report)
  });
  if (!response.ok) throw new Error(`Cannot save GPU report: ${response.status}`);
  return report;
}
