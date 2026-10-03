import { createApp, defineComponent, h, nextTick, ref, shallowRef } from 'vue';
import { VideoCutClient } from '../packages/client/index.mjs';
import {
  createProject,
  addAsset,
  addText,
  addHtmlClip,
  editTimeline,
  findItem,
  ticks,
  seconds,
  type Project,
  type Asset,
  type Item
} from '../packages/core/project.mjs';
import type { HtmlContent } from '../packages/core/types';
import { HtmlFrameClient } from '../packages/render/html';
import { SceneRenderer } from '../packages/render/renderer';
import { sharedMediaEngine } from '../packages/media/browser';
import { renderTemplateExport } from '../src/editor/template-export';
import type { EncodingMode } from '../packages/render/export';
import Preview from '../src/editor/Preview.vue';
import Timeline from '../src/editor/Timeline.vue';
import '../src/editor/editor.css';

const query = new URLSearchParams(location.search);
const fixtures =
  query.get('fixtures') || '/Users/jxinfa/WebstormProjects/videocut/.local/html-evidence';
const destination = query.get('output') || `${fixtures}/render-results`;
const reportUrl = query.get('report');
const preview = document.querySelector<HTMLCanvasElement>('#preview')!;
const decoded = document.querySelector<HTMLCanvasElement>('#decoded')!;
const button = document.querySelector<HTMLButtonElement>('#run')!;
const status = document.querySelector<HTMLElement>('#status')!;
const client = new VideoCutClient(location.origin);
const W = 640,
  H = 360,
  LENGTH = 1.2;
const gsapHtml: HtmlContent = {
  width: W,
  height: H,
  duration: ticks(LENGTH),
  transparent: true,
  variables: { color: 'rgba(255,0,0,0.5)' },
  html: `<!doctype html><html><head><style>
html,body { margin:0; width:100%; height:100%; overflow:hidden; background:transparent; }
#shape { position:absolute; left:100px; top:80px; width:100px; height:60px; background:rgba(255,0,0,0.5); }
</style></head><body><div id="shape"></div><script>
document.querySelector('#shape').style.background = (window.variables || window.__videocutVariables).color;
window.__timelines = { main: gsap.timeline({ paused:true }).fromTo('#shape', {x:0}, {x:288,duration:1.2,ease:'none'}, 0) };
</script></body></html>`
};
const tickHtml: HtmlContent = {
  width: W,
  height: H,
  duration: ticks(LENGTH),
  transparent: true,
  html: `<!doctype html><html><head><style>html,body{margin:0;overflow:hidden;background:transparent}#shape{position:absolute;left:60px;top:220px;width:80px;height:50px;background:#00cc66}</style></head><body><div id="shape"></div><script>
window.tick = function(t) { document.querySelector('#shape').style.transform = 'translateX(' + (t * 180) + 'px)'; };
</script></body></html>`
};
type Bytes = Uint8Array | Uint8ClampedArray;
let controller = new AbortController();
let app: ReturnType<typeof createApp> | undefined;
let activeSession = '';
const report: any = {
  kind: 'html-clip-browser',
  phase: 'idle',
  complete: false,
  fixtures,
  results: [],
  logs: []
};
async function publish() {
  status.textContent = JSON.stringify(report, null, 2);
  if (reportUrl)
    await fetch(`${reportUrl}/report`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-QA-Report': 'render' },
      body: JSON.stringify(report)
    }).catch(() => {});
}
function check(value: unknown, message: string): asserts value {
  if (!value) throw new Error(message);
}
async function record(name: string, details: unknown, passed: boolean) {
  report.results.push({ name, passed, details });
  await publish();
  check(passed, `${name} failed`);
}
function delta(a: Bytes, b: Bytes) {
  check(a.length === b.length, 'Pixel dimensions changed');
  let changed = 0,
    max = 0,
    total = 0;
  const histogram = new Uint32Array(256);
  for (let i = 0; i < a.length; i++) {
    const d = Math.abs(a[i] - b[i]);
    if (d) changed++;
    max = Math.max(max, d);
    if (i % 4 !== 3) {
      total += d;
      histogram[d]++;
    }
  }
  const count = (a.length / 4) * 3;
  let accumulated = 0,
    p99 = 0;
  for (let i = 0; i < 256; i++) {
    accumulated += histogram[i];
    if (accumulated >= count * 0.99) {
      p99 = i;
      break;
    }
  }
  return { changedBytes: changed, maxByteError: max, mae: total / count, p99 };
}
const pixel = (bytes: Bytes, x: number, y: number) =>
  Array.from(bytes.slice((y * W + x) * 4, (y * W + x) * 4 + 4));
function near(actual: number[], expected: number[], tolerance = 2) {
  return actual.every((v, i) => Math.abs(v - expected[i]) <= tolerance);
}
function blended(background: number[], alpha: number, foreground = [255, 0, 0]) {
  return [
    Math.round(foreground[0] * alpha + background[0] * (1 - alpha)),
    Math.round(foreground[1] * alpha + background[1] * (1 - alpha)),
    Math.round(foreground[2] * alpha + background[2] * (1 - alpha)),
    255
  ];
}
async function wait(predicate: () => boolean, message: string, timeout = 30000) {
  const start = performance.now();
  while (!predicate()) {
    controller.signal.throwIfAborted();
    if (performance.now() - start > timeout) throw new Error(message);
    await new Promise((resolve) => setTimeout(resolve, 25));
  }
}
async function bitmapPixels(frame: { frame: CanvasImageSource; close: () => void }) {
  try {
    const canvas = new OffscreenCanvas(W, H),
      ctx = canvas.getContext('2d')!;
    ctx.drawImage(frame.frame, 0, 0);
    return ctx.getImageData(0, 0, W, H).data;
  } finally {
    frame.close();
  }
}
function mediaClip(p: Project, asset: Asset) {
  const item = addAsset(p, asset, { start: 0, validate: false });
  item.placement.end = ticks(LENGTH);
  item.clip.source.end = ticks(LENGTH);
  return item;
}
async function capture(name: string, canvas: HTMLCanvasElement) {
  if (!reportUrl) return;
  const blob = await new Promise<Blob | null>((resolve) => canvas.toBlob(resolve));
  if (blob)
    await fetch(`${reportUrl}/image?name=${encodeURIComponent(name)}`, {
      method: 'POST',
      headers: { 'X-QA-Report': 'render' },
      body: blob
    }).catch(() => {});
}
function pointer(target: EventTarget, type: string, x: number, y: number) {
  target.dispatchEvent(
    new PointerEvent(type, {
      bubbles: true,
      cancelable: true,
      pointerId: 1,
      pointerType: 'mouse',
      isPrimary: true,
      button: 0,
      buttons: type === 'pointerup' ? 0 : 1,
      clientX: x,
      clientY: y,
      view: window
    })
  );
}
async function componentCases(project: Project, url: (id: string) => string, itemId: string) {
  const value = shallowRef(project),
    time = ref(ticks(0.25)),
    playing = ref(false),
    selected = ref(itemId);
  const exposed = ref<InstanceType<typeof Preview>>();
  const errors: string[] = [],
    seeks: number[] = [];
  const shell = defineComponent({
    setup: () => () =>
      h('div', [
        h(
          'div',
          { class: 'qa-component-time' },
          `播放头 ${seconds(time.value).toFixed(3)}s · ${playing.value ? '播放' : '暂停'}`
        ),
        h(Preview, {
          ref: exposed,
          project: value.value,
          time: time.value,
          playing: playing.value,
          mediaUrl: url,
          selected: selected.value,
          disabled: false,
          quality: 'full',
          onError: (message: string) => errors.push(message),
          onSelect: (id: string) => {
            selected.value = id;
          }
        }),
        h(Timeline, {
          project: value.value,
          time: time.value,
          selected: selected.value,
          selection: [selected.value],
          zoom: 240,
          snap: false,
          busy: false,
          mediaUrl: url,
          onSelect: (id: string) => {
            selected.value = id;
          },
          onSeek: (next: number) => {
            time.value = next;
            seeks.push(next);
            report.lastSeekSeconds = seconds(next);
            void publish();
          }
        })
      ])
  });
  app = createApp(shell);
  app.mount('#component-fixture');
  await wait(
    () => exposed.value?.renderStatus()?.time === time.value,
    `Preview initial render failed: ${errors.join(';')}`
  );
  const ruler = document.querySelector<HTMLElement>('#component-fixture .ruler')!;
  check(ruler, 'Timeline ruler missing');
  const rect = ruler.getBoundingClientRect(),
    y = rect.top + rect.height / 2;
  const target = (seconds: number) => rect.left + seconds * 240;
  pointer(ruler, 'pointerdown', target(0.2), y);
  await nextTick();
  await wait(
    () => exposed.value?.renderStatus()?.time === ticks(0.2),
    'Forward seek did not present frame .2'
  );
  const samples = [exposed.value!.renderStatus()!];
  pointer(window, 'pointermove', target(0.8), y);
  await nextTick();
  await wait(
    () => exposed.value?.renderStatus()?.time === ticks(0.8),
    'Dragged playhead did not present frame .8'
  );
  samples.push(exposed.value!.renderStatus()!);
  pointer(window, 'pointermove', target(0.2), y);
  pointer(window, 'pointerup', target(0.2), y);
  await nextTick();
  await wait(
    () => exposed.value?.renderStatus()?.time === ticks(0.2),
    'Backward seek did not restore frame .2'
  );
  samples.push(exposed.value!.renderStatus()!);
  await record(
    'Preview + Timeline playhead forward/backward drag',
    {
      eventSource: 'Synthetic PointerEvent, isTrusted=false',
      seeks: seeks.map(seconds),
      presentedTimes: samples.map((sample) => seconds(sample.time)),
      backends: samples.map((sample) => sample.backend),
      htmlSources: samples.map((sample) =>
        sample.media.filter((media) => media.kind === 'html-clip' || media.type === 'html-clip')
      ),
      errors
    },
    errors.length === 0 &&
      samples.length === 3 &&
      samples.every((sample) => sample.width === W && sample.height === H)
  );
  playing.value = true;
  await nextTick();
  const playbackFrames = [];
  for (const next of [0.3, 0.4, 0.5]) {
    time.value = ticks(next);
    await nextTick();
    await wait(
      () => exposed.value?.renderStatus()?.time === time.value,
      'Playing Preview frame failed'
    );
    playbackFrames.push(exposed.value!.renderStatus()!);
  }
  playing.value = false;
  await nextTick();
  await record(
    'Preview playing updates HTML and video together',
    {
      clock: 'Authored time advanced by harness with playing=true',
      times: playbackFrames.map((frame) => seconds(frame.time)),
      sourceTypes: playbackFrames.map((frame) => frame.media.map((entry) => entry.kind)),
      errors
    },
    errors.length === 0 && playbackFrames.every((frame) => frame.bounds.length === 3)
  );
}
async function exportCase(
  project: Project,
  session: { id: string; version: number },
  url: (id: string) => string
) {
  const events = new EventSource(client.eventsUrl(session.id));
  const scene = new SceneRenderer(new OffscreenCanvas(W, H), url);
  let outputSession = '';
  try {
    await new Promise<void>((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error('Export SSE connection timeout')), 20000);
      events.onopen = () => {
        clearTimeout(timeout);
        resolve();
      };
      events.onerror = () => {
        clearTimeout(timeout);
        reject(new Error('Export SSE failed'));
      };
    });
    events.addEventListener('render-progress', (event) => {
      report.exportProgress = JSON.parse((event as MessageEvent).data);
      void publish();
    });
    const pending = client.renderVideo(session.id, session.version, destination, 'mp4');
    pending.catch(() => {});
    let jobId = '';
    for (let attempt = 0; attempt < 200; attempt++) {
      controller.signal.throwIfAborted();
      const job = await client.renderStatus(session.id);
      if (job.id || job.jobId) {
        jobId = String(job.id || job.jobId);
        break;
      }
      await new Promise((resolve) => setTimeout(resolve, 50));
    }
    check(jobId, 'Export job did not become available');
    const encoding = query.get('encoding');
    const preferredEncoding: EncodingMode | 'auto' = [
      'browser',
      'video-with-pcm',
      'frames-with-pcm'
    ].includes(encoding || '')
      ? (encoding as EncodingMode)
      : 'auto';
    const claimed = await renderTemplateExport(
      client,
      session.id,
      jobId,
      controller.signal,
      () => {},
      { preferredEncoding }
    );
    check(claimed, 'Export render job was not claimed');
    const receipt = await pending;
    report.export = receipt;
    const output = await client.importMedia(receipt.path);
    const playback = createProject('Reimported HTML MP4 evidence');
    addAsset(playback, output);
    outputSession = (await client.createSession(playback)).id;
    const outputUrl = client.mediaUrl(outputSession, output.id);
    const frames = [];
    for (const time of [0.3, 0.8, 1.1]) {
      await scene.render(project, ticks(time), W, H, controller.signal);
      const reference = await scene.readPixels();
      const frame = await sharedMediaEngine.videoFrame(outputUrl, time, {
        signal: controller.signal
      });
      const timestamp = frame.timestamp;
      let actual: Uint8ClampedArray;
      try {
        const context = decoded.getContext('2d')!;
        context.clearRect(0, 0, W, H);
        context.drawImage(frame.frame, 0, 0, W, H);
        actual = context.getImageData(0, 0, W, H).data;
      } finally {
        frame.close();
      }
      const difference = delta(reference, actual);
      frames.push({
        time,
        timestamp,
        difference,
        passed: difference.mae < 6 && difference.p99 < 70 && Math.abs(timestamp - time) < 0.034
      });
    }
    const metadata = await sharedMediaEngine.probe(outputUrl);
    const blocks = await sharedMediaEngine.audioWindow(outputUrl, 0, 0.8, {
      signal: controller.signal
    });
    let leftEnergy = 0,
      rightEnergy = 0,
      dot = 0,
      samples = 0;
    for (const block of blocks) {
      const left = block.data[0],
        right = block.data[1];
      if (!left || !right) continue;
      const n = Math.min(left.length, right.length);
      for (let i = 0; i < n; i++) {
        leftEnergy += left[i] * left[i];
        rightEnergy += right[i] * right[i];
        dot += left[i] * right[i];
      }
      samples += n;
    }
    const sound = {
      channels: metadata.channels,
      sampleRate: metadata.sampleRate,
      duration: metadata.duration,
      samples,
      leftRms: Math.sqrt(leftEnergy / Math.max(1, samples)),
      rightRms: Math.sqrt(rightEnergy / Math.max(1, samples)),
      correlation: dot / Math.sqrt(leftEnergy * rightEnergy)
    };
    await record(
      'MP4 export reimported video frame parity and stereo audio',
      {
        receipt,
        imported: {
          width: output.width,
          height: output.height,
          hasAudio: output.hasAudio,
          duration: seconds(output.duration),
          size: output.size
        },
        frameSamples: frames,
        audio: sound,
        projectSeconds: LENGTH,
        preferredEncoding
      },
      output.width === W &&
        output.height === H &&
        output.hasAudio &&
        output.size > 1000 &&
        frames.every((frame) => frame.passed) &&
        sound.channels === 2 &&
        sound.sampleRate === 48000 &&
        Math.abs(sound.duration - LENGTH) < 0.06 &&
        samples > 20000 &&
        sound.leftRms > 0.05 &&
        sound.rightRms > 0.05 &&
        sound.correlation < -0.95
    );
    await capture('html-export-decoded', decoded);
  } finally {
    events.close();
    scene.dispose();
    if (outputSession) await client.closeSession(outputSession).catch(() => {});
  }
}
async function run() {
  if (button.disabled) return;
  button.disabled = true;
  controller = new AbortController();
  app?.unmount();
  app = undefined;
  if (activeSession) await client.closeSession(activeSession).catch(() => {});
  report.phase = 'importing';
  report.complete = false;
  report.results = [];
  report.logs = [];
  report.started = new Date().toISOString();
  report.userAgent = navigator.userAgent;
  delete report.error;
  await publish();
  const htmlClient = new HtmlFrameClient();
  let scene: SceneRenderer | undefined;
  try {
    const info = await client.connect();
    report.capabilities = info.htmlClips;
    const [video, audio, still] = await Promise.all(
      ['timecode.mp4', 'stereo.wav', 'still.png'].map((name) =>
        client.importMedia(`${fixtures}/${name}`)
      )
    );
    report.phase = 'transparent-dom-frames';
    await publish();
    const rawA = await bitmapPixels(
      await htmlClient.frame('gsap-fixture', gsapHtml, ticks(0.25), controller.signal)
    );
    const rawB = await bitmapPixels(
      await htmlClient.frame('gsap-fixture', gsapHtml, ticks(0.75), controller.signal)
    );
    const rawBack = await bitmapPixels(
      await htmlClient.frame('gsap-fixture', gsapHtml, ticks(0.25), controller.signal)
    );
    const rawAlpha = pixel(rawA, 210, 110),
      outside = pixel(rawA, 20, 20);
    await record(
      'GSAP seek is reversible and preserves PNG alpha',
      {
        inside: rawAlpha,
        outside,
        forward: delta(rawA, rawB),
        reverse: delta(rawA, rawBack)
      },
      near(rawAlpha, [255, 0, 0, 128], 1) &&
        outside[3] === 0 &&
        delta(rawA, rawB).changedBytes > 5000 &&
        delta(rawA, rawBack).maxByteError === 0
    );
    const tickA = await bitmapPixels(
      await htmlClient.frame('tick-fixture', tickHtml, ticks(0.2), controller.signal)
    );
    const tickB = await bitmapPixels(
      await htmlClient.frame('tick-fixture', tickHtml, ticks(0.8), controller.signal)
    );
    const tickBack = await bitmapPixels(
      await htmlClient.frame('tick-fixture', tickHtml, ticks(0.2), controller.signal)
    );
    await record(
      'Custom tick(t) forward/backward exact frame parity',
      { forward: delta(tickA, tickB), reverse: delta(tickA, tickBack) },
      delta(tickA, tickB).changedBytes > 5000 && delta(tickA, tickBack).maxByteError === 0
    );
    const project = createProject('HTML clip browser evidence');
    project.canvas = { width: W, height: H };
    project.frameRate = { numerator: 30, denominator: 1 };
    const image = mediaClip(project, still);
    Object.assign(image.clip.visual, {
      scaleX: 0.25,
      scaleY: 0.25,
      positionX: 220,
      positionY: 110
    });
    const videoItem = mediaClip(project, video);
    if (video.hasAudio) videoItem.clip.audio.muted = true;
    const audioItem = mediaClip(project, audio);
    audioItem.clip.audio.gainLinear = 0.4;
    const animation = addHtmlClip(project, {
      html: gsapHtml,
      name: 'Transparent GSAP alpha fixture'
    });
    const session = await client.createSession(project);
    activeSession = session.id;
    report.session = session.id;
    const url = (id: string) => client.mediaUrl(session.id, id);
    scene = new SceneRenderer(preview, url);
    const render = async (p: Project, time: number) => {
      const metadata = await scene!.render(p, ticks(time), W, H, controller.signal);
      return { metadata, pixels: await scene!.readPixels() };
    };
    report.phase = 'mixed-compositing';
    await publish();
    const base = structuredClone(project);
    findItem(base, animation.id).item.enabled = false;
    const background = await render(base, 0.25),
      mixed = await render(project, 0.25);
    const sample = pixel(mixed.pixels, 210, 110),
      expected = blended(pixel(background.pixels, 210, 110), rawAlpha[3] / 255);
    await record(
      'Transparent HTML alpha composites over video and image',
      {
        backend: mixed.metadata.backend,
        layers: mixed.metadata.bounds.length,
        sample,
        expected,
        outside: pixel(mixed.pixels, 20, 20),
        backgroundOutside: pixel(background.pixels, 20, 20)
      },
      mixed.metadata.bounds.length === 3 &&
        near(sample, expected) &&
        near(pixel(mixed.pixels, 20, 20), pixel(background.pixels, 20, 20), 0)
    );
    const variablesProject = editTimeline(project, [
      {
        action: 'set_html_clip',
        itemId: animation.id,
        html: { ...gsapHtml, variables: { color: 'rgba(0,0,255,0.5)' } }
      }
    ]).project;
    const variableFrame = await render(variablesProject, 0.25);
    const variablePixel = pixel(variableFrame.pixels, 210, 110);
    const variableExpected = blended(
      pixel(background.pixels, 210, 110),
      rawAlpha[3] / 255,
      [0, 0, 255]
    );
    await record(
      'Editing HTML variables invalidates the same-time frame',
      { sample: variablePixel, expected: variableExpected },
      near(variablePixel, variableExpected)
    );
    const half = editTimeline(project, [
      { action: 'set_transform', itemId: animation.id, opacity: 0.5 }
    ]).project;
    const opacity = await render(half, 0.25),
      opacityPixel = pixel(opacity.pixels, 210, 110),
      opacityExpected = blended(pixel(background.pixels, 210, 110), (rawAlpha[3] / 255) * 0.5);
    await record(
      'Timeline opacity multiplies DOM alpha',
      { sample: opacityPixel, expected: opacityExpected },
      near(opacityPixel, opacityExpected)
    );
    const transformed = editTimeline(project, [
      { action: 'set_transform', itemId: animation.id, positionX: 60, positionY: 40 }
    ]).project;
    const moved = await render(transformed, 0.25),
      movedPixel = pixel(moved.pixels, 270, 150),
      movedExpected = blended(pixel(background.pixels, 270, 150), rawAlpha[3] / 255);
    await record(
      'Timeline transform moves HTML pixels',
      {
        sample: movedPixel,
        expected: movedExpected,
        oldLeft: pixel(moved.pixels, 165, 90),
        backgroundOldLeft: pixel(background.pixels, 165, 90)
      },
      near(movedPixel, movedExpected) &&
        near(pixel(moved.pixels, 165, 90), pixel(background.pixels, 165, 90), 0)
    );
    if (mixed.metadata.backend === 'webgpu') {
      const effectProject = editTimeline(transformed, [
        {
          action: 'add_effect',
          itemId: animation.id,
          templateId: 'blur',
          parameters: { radius: 8 }
        }
      ]).project;
      const blurred = await render(effectProject, 0.25),
        difference = delta(moved.pixels, blurred.pixels);
      await record(
        'HTML uses shared WebGPU blur effect pipeline',
        { difference, gpuPasses: blurred.metadata.gpuPasses },
        difference.changedBytes > 1000 && difference.mae > 0.1 && blurred.metadata.gpuPasses > 0
      );
    } else
      await record(
        'WebGPU effects unavailable',
        { backend: mixed.metadata.backend, scope: 'Effect pipeline requires WebGPU' },
        false
      );
    const original = await render(project, 0.75),
      splitProject = editTimeline(project, [
        { action: 'split_clip', itemId: animation.id, atSeconds: 0.6 }
      ]).project;
    const split = await render(splitProject, 0.75),
      parts = splitProject.timeline.tracks
        .flatMap((track) => track.items)
        .filter((item) => item.clip.html);
    await record(
      'Split preserves HTML animation source continuity',
      {
        parts: parts.map((item) => ({ placement: item.placement, source: item.clip.source })),
        difference: delta(original.pixels, split.pixels)
      },
      parts.length === 2 &&
        parts[1].clip.source.begin === ticks(0.6) &&
        delta(original.pixels, split.pixels).maxByteError === 0
    );
    const flowerProject = structuredClone(project);
    const flower = addText(flowerProject, {
      content: '花字创作',
      length: ticks(LENGTH),
      template: {
        id: 'skill-flower',
        version: 1,
        recipe: {
          base: 'flower-style-03',
          backdrop: 'bubble-tile',
          animation: 'anim-lua-letter-transform'
        }
      }
    });
    flower.clip.visual.scaleX = flower.clip.visual.scaleY = 0.35;
    const flowerFirst = await render(flowerProject, 0.3);
    const flowerMiddle = await render(flowerProject, 0.8);
    const flowerBack = await render(flowerProject, 0.3);
    const recipeChanged = structuredClone(flowerProject);
    findItem(recipeChanged, flower.id).item.clip.text!.template!.recipe!.backdrop =
      'bubble-nine-slice';
    const changedFlower = await render(recipeChanged, 0.3);
    const withoutFlower = await render(project, 0.3);
    await record(
      'SKILL native flower recipe renders and rewinds alongside HTML/media',
      {
        visibleDifference: delta(flowerFirst.pixels, withoutFlower.pixels),
        reverse: delta(flowerFirst.pixels, flowerBack.pixels),
        forward: delta(flowerFirst.pixels, flowerMiddle.pixels),
        changedRecipe: delta(flowerFirst.pixels, changedFlower.pixels)
      },
      delta(flowerFirst.pixels, withoutFlower.pixels).changedBytes > 500 &&
        delta(flowerFirst.pixels, flowerBack.pixels).maxByteError === 0 &&
        delta(flowerFirst.pixels, changedFlower.pixels).changedBytes > 500
    );
    await render(project, 0.3);
    await capture('html-mixed-preview', preview);
    report.phase = 'component-seek-playback';
    await publish();
    await componentCases(project, url, animation.id);
    report.phase = 'mp4-export';
    await publish();
    const flowerSession = await client.updateSession(session.id, flowerProject, session.version);
    await exportCase(flowerProject, flowerSession, url);
    report.phase = 'complete';
    report.complete = true;
    report.completed = new Date().toISOString();
  } catch (error) {
    report.phase = controller.signal.aborted ? 'cancelled' : 'failed';
    report.error = error instanceof Error ? error.stack : String(error);
  } finally {
    htmlClient.dispose();
    scene?.dispose();
    button.disabled = false;
    await publish();
  }
}
button.addEventListener('click', () => void run());
document.querySelector('#stop')!.addEventListener('click', () => controller.abort());
void publish();
