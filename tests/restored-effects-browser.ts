import {
  createProject,
  addAsset,
  editTimeline,
  ticks,
  EFFECT_TEMPLATES,
  TRANSITION_TEMPLATES,
  type Project,
  type EditorCommand
} from '../packages/core/project.mjs';
import { IDENTITY_CUBE, LOOK_NAMES } from '../packages/core/color.mjs';
import { SceneRenderer } from '../packages/render/renderer';
import { exportProject } from '../packages/render/export';
import { sharedMediaEngine } from '../packages/media/browser';
const status = document.querySelector('#status')!,
  canvas = document.querySelector('#preview') as HTMLCanvasElement;
const result: any = { phase: 'running', cases: [] };
(window as any).verification = result;
const urls: Record<string, string> = {};
const renderer = new SceneRenderer(canvas, (id) => urls[id]);
const signal = new AbortController().signal;
function assert(ok: unknown, message: string) {
  if (!ok) throw new Error(message);
}
function fixture(): { project: Project; from: string; to: string } {
  const project = createProject();
  project.canvas = { width: 160, height: 90 };
  const a = addAsset(project, {
    id: 'a',
    name: 'a',
    kind: 'image',
    path: '/a.png',
    size: 1,
    width: 160,
    height: 90,
    duration: ticks(5),
    hasAudio: false
  });
  a.placement.end = ticks(0.4);
  a.clip.source.end = ticks(0.4);
  const b = addAsset(
    project,
    {
      id: 'b',
      name: 'b',
      kind: 'image',
      path: '/b.png',
      size: 1,
      width: 160,
      height: 90,
      duration: ticks(5),
      hasAudio: false
    },
    { trackId: project.timeline.tracks[0].id }
  );
  b.placement.end = ticks(0.8);
  b.clip.source.end = ticks(0.4);
  return { project, from: a.id, to: b.id };
}
async function pixels(project: Project, time: number) {
  const report = await renderer.render(project, ticks(time), 160, 90, signal);
  assert(report.backend === 'webgpu', 'WebGPU is required');
  return renderer.readPixels();
}
function difference(a: Uint8Array, b: Uint8Array | Uint8ClampedArray) {
  let n = 0;
  for (let i = 0; i < a.length; i += 4)
    n += Math.abs(a[i] - b[i]) + Math.abs(a[i + 1] - b[i + 1]) + Math.abs(a[i + 2] - b[i + 2]);
  return n / ((a.length / 4) * 3);
}
async function encoded(project: Project, time: number, expected: Uint8Array) {
  const chunks: { position: number; data: Uint8Array }[] = [];
  const output = await exportProject(
    project,
    'mp4',
    urls,
    {
      configure: async (p) => assert(p.encoding === 'browser', 'Expected browser encoding'),
      write: async (position, data) => {
        chunks.push({ position, data: data.slice() });
      },
      audio: async () => {},
      frame: async () => {},
      progress: () => {}
    },
    signal
  );
  const length = Math.max(...chunks.map((c) => c.position + c.data.length)),
    bytes = new Uint8Array(length);
  for (const c of chunks) bytes.set(c.data, c.position);
  const blob = new Blob([bytes], { type: 'video/mp4' }),
    url = URL.createObjectURL(blob);
  try {
    const frame = await sharedMediaEngine.videoFrame(url, time);
    const decoded = new OffscreenCanvas(160, 90),
      ctx = decoded.getContext('2d')!;
    ctx.drawImage(frame.frame, 0, 0);
    frame.close();
    const error = difference(expected, ctx.getImageData(0, 0, 160, 90).data);
    assert(error < 14, `Encoded pixel error ${error}`);
    return { bytes: length, error, output };
  } finally {
    URL.revokeObjectURL(url);
  }
}
async function run() {
  for (const id of ['a', 'b']) {
    const c = document.createElement('canvas');
    c.width = 160;
    c.height = 90;
    const ctx = c.getContext('2d')!;
    const gradient = ctx.createLinearGradient(0, 0, 160, 90);
    gradient.addColorStop(0, id === 'a' ? '#17466e' : '#c93f66');
    gradient.addColorStop(1, id === 'a' ? '#f8d36a' : '#3acd9e');
    ctx.fillStyle = gradient;
    ctx.fillRect(0, 0, 160, 90);
    ctx.fillStyle = id === 'a' ? '#eeeeff' : '#211c44';
    ctx.fillRect(15, 15, 36, 40);
    urls[id] = c.toDataURL();
  }
  const sample = fixture(),
    baseline = await pixels(sample.project, 0.4);
  for (const template of TRANSITION_TEMPLATES) {
    const { project, from, to } = fixture();
    const p = editTimeline(project, [
      {
        action: 'add_transition',
        fromItemId: from,
        toItemId: to,
        templateId: template.id,
        durationSeconds: 0.4
      } as EditorCommand
    ]).project;
    const mid = await pixels(p, 0.4),
      beginning = await pixels(p, 0.2),
      ending = await pixels(p, 0.6);
    const first = await pixels(project, 0),
      last = await pixels(project, 0.7);
    assert(difference(beginning, first) < 0.02, `${template.id}: start endpoint`);
    assert(difference(ending, last) < 0.02, `${template.id}: end endpoint`);
    assert(difference(mid, baseline) > 0.1, `${template.id}: visible transition`);
    const exportResult = await encoded(p, 0.4, mid);
    result.cases.push({ kind: 'transition', id: template.id, ...exportResult });
    status.textContent = JSON.stringify(result, null, 2);
  }
  for (const template of EFFECT_TEMPLATES) {
    const { project, from } = fixture();
    const parameters: Record<string, number | string> =
      template.id === 'color-grade'
        ? { exposure: 0.8, curvered: '[[0,0],[0.5,0.8],[1,1]]' }
        : template.id === 'detail'
          ? { sharpness: 65, noise: 25, vignette: 70 }
          : template.id === 'custom-lut'
            ? {
                cube: IDENTITY_CUBE.split('\n')
                  .map((line, i) =>
                    i
                      ? line
                          .split(' ')
                          .map((v) => 1 - Number(v))
                          .join(' ')
                      : line
                  )
                  .join('\n')
              }
            : {};
    const p = editTimeline(project, [
      { action: 'add_effect', itemId: from, templateId: template.id, parameters } as EditorCommand
    ]).project;
    const actual = await pixels(p, 0.2);
    const output = await encoded(p, 0.2, actual);
    result.cases.push({ kind: 'effect', id: template.id, ...output });
    status.textContent = JSON.stringify(result, null, 2);
  }
  for (const preset of Object.keys(LOOK_NAMES).filter((id) => id !== 'none')) {
    const { project, from } = fixture();
    const p = editTimeline(project, [
      { action: 'add_effect', itemId: from, templateId: 'looks', parameters: { preset } }
    ]).project;
    const actual = await pixels(p, 0.2),
      output = await encoded(p, 0.2, actual);
    assert(
      difference(actual, await pixels(project, 0.2)) > 0.1,
      `Filter ${preset} should change the image`
    );
    result.cases.push({ kind: 'look', id: preset, ...output });
    status.textContent = JSON.stringify(result, null, 2);
  }
  result.phase = 'complete';
}
run()
  .catch((error) => {
    result.phase = 'failed';
    result.error = error.stack || String(error);
  })
  .finally(() => {
    renderer.dispose();
    status.textContent = JSON.stringify(result, null, 2);
  });
