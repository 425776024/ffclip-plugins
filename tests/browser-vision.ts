import { createApp, nextTick } from 'vue';
import VisionSetupDialog from '../src/editor/VisionSetupDialog.vue';
import { VisionWorkerClient } from '../packages/vision/client';
import { planVideoSegments } from '../packages/vision/runtime.mjs';
import type { VisionModelStatus } from '../packages/client/types';
import type { VideoCutClient } from '../packages/client/index.mjs';
import '../src/editor/editor.css';

const report: { complete: boolean; rows: string[]; error?: string } = { complete: false, rows: [] };
(window as unknown as { visionReport: typeof report }).visionReport = report;
const check = (condition: unknown, name: string) => {
  if (!condition) throw new Error(name);
  report.rows.push(name);
};
async function waitFor(predicate: () => unknown) {
  for (let i = 0; i < 100; i++) {
    if (predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 30));
  }
  throw new Error('Browser fixture timed out');
}
const model: VisionModelStatus = {
  model: 'fastvlm-0.5b',
  repository: 'fixture',
  revision: 'fixture',
  license: 'apple-amlr',
  modelBaseUrl: '/vision-models/fastvlm-0.5b/',
  consent: 'unasked',
  promptRequested: false,
  installed: false,
  size: 1000,
  install: { state: 'idle', progress: 0, files: [] }
};
const calls: string[] = [];
const client = {
  async visionModelStatus() {
    calls.push('status');
    return structuredClone(model);
  },
  async request(path: string, options: RequestInit) {
    calls.push(path);
    if (path === '/vision/setup') {
      const { enabled } = JSON.parse(String(options.body));
      model.consent = enabled ? 'enabled' : 'declined';
      model.promptRequested = false;
      if (enabled)
        model.install = {
          state: 'downloading',
          progress: 0.4,
          files: [{ path: 'onnx/vision_encoder_q4.onnx', size: 1000, downloaded: 400 }]
        };
    } else if (path === '/vision/model/install') model.install.state = 'cancelled';
    return structuredClone(model);
  }
} as unknown as VideoCutClient;
const button = (label: string) =>
  Array.from(document.querySelectorAll('button')).find((b) => b.textContent?.trim() === label)!;
const dialog = () => document.querySelector('[role=dialog]');

async function run() {
  let app = createApp(VisionSetupDialog, { client });
  app.mount('#app');
  await waitFor(dialog);
  check(
    calls.every((c) => c === 'status'),
    'initialization only queries status before consent'
  );
  button('暂不启用').click();
  await waitFor(() => !dialog());
  check(
    model.consent === 'declined' && model.install.state === 'idle',
    'decline closes dialog without download'
  );
  app.unmount();
  app = createApp(VisionSetupDialog, { client });
  app.mount('#app');
  await nextTick();
  await new Promise((resolve) => setTimeout(resolve, 80));
  check(!dialog(), 'saved decline does not prompt again on initialization');
  model.promptRequested = true;
  window.dispatchEvent(new Event('videocut-vision-setup'));
  await waitFor(dialog);
  button('同意并下载').click();
  await waitFor(() => document.querySelector('progress'));
  check(
    document.querySelector('progress')?.value === 0.4 && dialog()?.textContent?.includes('40%'),
    'consent displays byte download progress'
  );
  button('取消下载').click();
  await waitFor(() => dialog()?.textContent?.includes('下载已取消'));
  check(model.consent === 'enabled', 'cancel preserves saved consent');
  button('重试下载').click();
  await waitFor(() => document.querySelector('progress'));
  model.installed = true;
  model.install = { state: 'ready', progress: 1, files: [] };
  await waitFor(() => button('完成'));
  button('完成').click();
  await waitFor(() => !dialog());
  app.unmount();

  const runtime = new VisionWorkerClient();
  const request = {
    modelBaseUrl: `${location.origin}/vision-models/fastvlm-0.5b/`,
    backend: 'wasm' as const,
    prompt: 'Describe the visible pixels.',
    maxNewTokens: 32
  };
  const image = await runtime.analyze({
    ...request,
    kind: 'image',
    sampleTimes: [0],
    mediaUrl: `${location.origin}/api/sessions/abc/vision-media?kind=image`
  });
  check(
    image.frames.length === 1 && image.frames[0].text === 'red pixels',
    'image Worker decodes actual local pixels and returns generated text'
  );
  const video = await runtime.analyze({
    ...request,
    kind: 'video',
    sampleTimes: [0.5, 1.5],
    mediaUrl: `${location.origin}/api/sessions/abc/vision-media?kind=video`
  });
  check(
    video.frames.map((f) => f.text).join(',') === 'red pixels,blue pixels',
    'video Worker samples both real decoded scenes'
  );
  check(
    video.frames.every(
      (f) =>
        f.sourceSeconds <= f.requestedSeconds &&
        f.sourceSeconds + f.durationSeconds >= f.requestedSeconds - 0.002
    ),
    'video result reports decoded PTS covering each requested time'
  );
  const plan = planVideoSegments(0, 2, { segmentSeconds: 2, framesPerSegment: 4 });
  const interval = await runtime.analyze({
    ...request,
    kind: 'video',
    resultFormat: 'segments',
    segmentPlan: plan.segments,
    sampleTimes: plan.segments.flatMap((s) => s.sampleTimes),
    prompt: 'Return JSON for the interval.',
    mediaUrl: `${location.origin}/api/sessions/abc/vision-media?kind=video`
  });
  check(
    interval.segments?.length === 1 &&
      interval.segments[0].startSeconds === 0 &&
      interval.segments[0].endSeconds === 2 &&
      interval.segments[0].text === '{"colors":["red","blue"]}',
    'interval storyboard includes decoded pixels from both scenes and preserves requested format'
  );
  check(
    interval.segments?.[0].samples.every(
      (s, i) =>
        s.requestedSeconds === plan.segments[0].sampleTimes[i] &&
        s.sourceSeconds <= s.requestedSeconds &&
        s.sourceSeconds + s.durationSeconds >= s.requestedSeconds - 0.002
    ),
    'interval samples expose requested times and covering actual decoded PTS'
  );
  const split = planVideoSegments(0, 2, { segmentSeconds: 1 });
  const intervals = await runtime.analyze({
    ...request,
    kind: 'video',
    resultFormat: 'segments',
    segmentPlan: split.segments,
    sampleTimes: split.segments.flatMap((s) => s.sampleTimes),
    mediaUrl: `${location.origin}/api/sessions/abc/vision-media?kind=video`
  });
  check(
    intervals.segments?.map((s) => s.text).join(',') === 'red pixels,blue pixels',
    'separate interval descriptions use their own decoded scenes'
  );
  let badPlanRejected = false;
  try {
    await runtime.analyze({
      ...request,
      kind: 'video',
      resultFormat: 'segments',
      segmentPlan: [{ ...split.segments[0], sampleTimes: [3] }],
      sampleTimes: [3],
      mediaUrl: `${location.origin}/api/sessions/abc/vision-media?kind=video`
    });
  } catch {
    badPlanRejected = true;
  }
  check(badPlanRejected, 'worker rejects a sample outside its claimed source interval');
  let rejected = false;
  try {
    await runtime.analyze({
      ...request,
      kind: 'image',
      sampleTimes: [0],
      mediaUrl: 'https://example.com/image.png'
    });
  } catch {
    rejected = true;
  }
  check(rejected, 'worker rejects remote media');
  const cancel = new AbortController();
  const work = runtime.analyze(
    {
      ...request,
      kind: 'image',
      sampleTimes: [0],
      mediaUrl: `${location.origin}/api/sessions/abc/vision-media?kind=image`
    },
    { signal: cancel.signal }
  );
  cancel.abort();
  try {
    await work;
    check(false, 'cancel should reject');
  } catch (error) {
    check((error as Error).name === 'AbortError', 'cancel terminates the worker');
  }
  runtime.dispose();
  report.complete = true;
}
run().catch((error) => {
  report.error = error instanceof Error ? error.stack : String(error);
  report.complete = true;
});
