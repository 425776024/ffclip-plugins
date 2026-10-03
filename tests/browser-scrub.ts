import { createApp, defineComponent, h, nextTick, ref, shallowRef, toRaw } from 'vue';
import { VideoCutClient } from '../packages/client/index.mjs';
import { ticks, seconds } from '../packages/core/project.mjs';
import Preview from '../src/editor/Preview.vue';
import Timeline from '../src/editor/Timeline.vue';
import '../src/editor/editor.css';
import type { Project } from '../packages/core/types';

const query = new URLSearchParams(location.search),
  client = new VideoCutClient(location.origin);
const status = document.querySelector('#status')!,
  run = document.querySelector<HTMLButtonElement>('#run')!,
  play = document.querySelector<HTMLButtonElement>('#play')!,
  playWarm = document.querySelector<HTMLButtonElement>('#play-warm')!,
  playReady = document.querySelector<HTMLButtonElement>('#play-ready')!;
const buttons = [run, play, playWarm, playReady];
buttons.forEach((button) => (button.disabled = true));
const exposed = ref<InstanceType<typeof Preview>>(),
  time = ref(ticks(2)),
  playing = ref(false),
  errors: string[] = [],
  previewGeneration = ref(0);
const quality = (query.get('quality') || 'full') as 'auto' | 'full' | 'half';
const textWorkers = query.get('textWorkers') !== 'off';
const projectUrl =
  query.get('project') ||
  '/@fs/Users/jxinfa/WebstormProjects/videocut/.local/html-evidence/performance-fixed-quality-session.json';
async function loadSnapshot() {
  if (query.has('session')) return client.getSession(query.get('session')!);
  const response = await fetch(projectUrl, { cache: 'no-store' });
  if (!response.ok) throw new Error(`QA project cannot be read: ${response.status}`);
  const baseline = await response.json();
  return client.createSession((baseline.project || baseline) as Project);
}
const snapshot = await loadSnapshot();
const project = shallowRef(snapshot.project);
const htmlItem = project.value.timeline.tracks.flatMap((t) => t.items).find((i) => i.clip.html);
const selected = htmlItem?.id || '';
const sourceInventory = project.value.timeline.tracks.flatMap((track) =>
  track.items.map((item) => {
    const asset = project.value.assets.find((asset) => asset.id === item.clip.assetId);
    return {
      itemId: item.id,
      name: item.name,
      kind: item.clip.type,
      assetId: asset?.id,
      originalDimensions: item.clip.html
        ? {
            width: item.clip.html.width,
            height: item.clip.html.height,
            from: 'authored-html-viewport'
          }
        : asset
          ? { width: asset.width, height: asset.height, from: 'original-media-asset-metadata' }
          : { ...project.value.canvas, from: 'native-text-render-target-at-full-quality' },
      placement: item.placement,
      source: item.clip.source,
      text: item.clip.text?.content,
      template: item.clip.text?.template
    };
  })
);
const report: any = {
  kind: 'mixed-timeline-scrub',
  session: snapshot.id,
  quality,
  textWorkers,
  projectUrl: query.has('session') ? undefined : projectUrl,
  projectCanvas: snapshot.project.canvas,
  projectFrameRate: snapshot.project.frameRate,
  sourceInventory,
  playbackWindowMs: 5000,
  playbackPreparationPolicy:
    'Fresh Preview key creates a new worker/SceneRenderer; complete initial frame and optional deterministic timeline scrub preparation are measured separately, before the exact 5000ms presentation window',
  complete: false,
  errors,
  measurements: []
};
const publish = () => {
  document.querySelector('#raw-report')!.textContent = JSON.stringify(report);
  status.textContent = JSON.stringify(
    {
      ...report,
      measurements: report.measurements.map(({ samples, ...measurement }: any) => measurement)
    },
    null,
    2
  );
};
const app = createApp(
  defineComponent({
    setup: () => () =>
      h('div', [
        h('p', `播放头 ${seconds(time.value).toFixed(3)} 秒`),
        h(Preview, {
          key: previewGeneration.value,
          ref: exposed,
          project: project.value,
          time: time.value,
          playing: playing.value,
          mediaUrl: (id: string) => client.mediaUrl(snapshot.id, id),
          selected,
          disabled: false,
          quality,
          textWorkers,
          onError: (message: string) => {
            errors.push(message);
            publish();
          }
        }),
        h(Timeline, {
          project: project.value,
          time: time.value,
          selected,
          selection: selected ? [selected] : [],
          zoom: 60,
          snap: false,
          busy: false,
          mediaUrl: (id: string) => client.mediaUrl(snapshot.id, id),
          onSeek: (next: number) => (time.value = next)
        })
      ])
  })
);
app.mount('#fixture');
const delay = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));
async function wait(check: () => boolean) {
  const end = performance.now() + 15000;
  while (!check()) {
    if (performance.now() > end) throw new Error('预览定位超时');
    await delay(20);
  }
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
function monitor() {
  const samples: any[] = [];
  let last: unknown = exposed.value?.renderStatus();
  const started = performance.now();
  const observer = new MutationObserver(() => {
    const frame = exposed.value?.renderStatus();
    if (frame && frame !== last) {
      last = frame;
      samples.push({ at: performance.now() - started, ...structuredClone(toRaw(frame)) });
    }
  });
  observer.observe(document.querySelector('.preview-stage')!, { attributes: true });
  return { samples, started, stop: () => observer.disconnect() };
}
function summarize(samples: any[], duration: number) {
  const sorted = samples.map((f) => f.frameMs).sort((a, b) => a - b);
  const mean = (arr: number[]) => arr.reduce((a, b) => a + b, 0) / Math.max(1, arr.length);
  return {
    durationMs: duration,
    frames: samples.length,
    fps: (samples.length * 1000) / duration,
    averageFrameMs: mean(sorted),
    p95FrameMs: sorted[Math.max(0, Math.ceil(sorted.length * 0.95) - 1)],
    maxGapMs: Math.max(0, ...samples.slice(1).map((f, i) => f.at - samples[i].at)),
    sources: Object.fromEntries(
      ['text', 'html-clip', 'video'].map((kind) => [
        kind,
        mean(
          samples.flatMap((f) =>
            f.media.filter((m: any) => m.kind === kind).map((m: any) => m.sourceMs)
          )
        )
      ])
    ),
    sourceCacheHits: Object.fromEntries(
      ['text', 'html-clip', 'video'].map((kind) => {
        const media = samples.flatMap((f) => f.media.filter((m: any) => m.kind === kind));
        return [
          kind,
          { hits: media.filter((m: any) => m.sourceCacheHit).length, total: media.length }
        ];
      })
    ),
    sameQuality: samples.every(
      (f) => f.width === report.initial.width && f.height === report.initial.height
    ),
    sizes: [...new Set(samples.map((f) => `${f.width}×${f.height}`))]
  };
}
function renderTimingSummary(samples: any[]) {
  const fields = new Map<string, number[]>();
  function visit(value: unknown, path: string) {
    if (typeof value === 'number' && Number.isFinite(value)) {
      if (!fields.has(path)) fields.set(path, []);
      fields.get(path)!.push(value);
    } else if (value && typeof value === 'object' && !Array.isArray(value)) {
      for (const [key, child] of Object.entries(value)) visit(child, path ? `${path}.${key}` : key);
    }
  }
  for (const sample of samples) visit(sample.timings, '');
  return {
    availableFrames: samples.filter((sample) => sample.timings).length,
    stages: Object.fromEntries(
      [...fields].map(([key, values]) => {
        const ordered = [...values].sort((a, b) => a - b);
        return [
          key,
          {
            count: values.length,
            mean: values.reduce((sum, value) => sum + value, 0) / values.length,
            median: ordered[Math.floor(ordered.length / 2)],
            p95: ordered[Math.ceil(ordered.length * 0.95) - 1],
            max: ordered.at(-1)
          }
        ];
      })
    )
  };
}
function uniquePresented(samples: any[]) {
  const seen = new Set<number>();
  return samples.filter((sample) => {
    if (seen.has(sample.time)) return false;
    seen.add(sample.time);
    return true;
  });
}
function actualSourceDimensions(samples: any[]) {
  const values = new Map<string, any>();
  for (const frame of samples)
    for (const media of frame.media) {
      const original = sourceInventory.find((source) => source.itemId === media.itemId);
      const decodedWidth =
        media.sourceWidth ||
        media.htmlWidth ||
        (media.kind === 'text' ? frame.width : original?.originalDimensions.width);
      const decodedHeight =
        media.sourceHeight ||
        media.htmlHeight ||
        (media.kind === 'text' ? frame.height : original?.originalDimensions.height);
      values.set(`${media.itemId}:${decodedWidth}x${decodedHeight}`, {
        itemId: media.itemId,
        kind: media.kind,
        originalDimensions: original?.originalDimensions,
        reportedOrRequestedSourceDimensions: { width: decodedWidth, height: decodedHeight },
        dimensionsEvidence:
          media.sourceWidth || media.htmlWidth
            ? 'actual-renderer-source-report'
            : media.kind === 'text'
              ? 'native-source-target-matches-frame-output'
              : 'original-asset-probe',
        authoredDimensions: media.authoredWidth
          ? { width: media.authoredWidth, height: media.authoredHeight }
          : undefined
      });
    }
  return [...values.values()];
}
async function sweep() {
  buttons.forEach((button) => (button.disabled = true));
  try {
    await wait(() => exposed.value?.renderStatus()?.time === time.value);
    const ruler = document.querySelector<HTMLElement>('.ruler')!,
      rect = ruler.getBoundingClientRect(),
      y = rect.top + rect.height / 2,
      x = (t: number) => rect.left + t * 60;
    const m = monitor();
    pointer(ruler, 'pointerdown', x(1.6), y);
    let inputs = 1;
    while (performance.now() - m.started < 8000) {
      const elapsed = performance.now() - m.started,
        phase = (elapsed % 1600) / 1600,
        t = phase < 0.5 ? 1.6 + phase * 5.6 : 4.4 - (phase - 0.5) * 5.6;
      pointer(window, 'pointermove', x(t), y);
      inputs++;
      await delay(1000 / 60);
    }
    const duration = performance.now() - m.started;
    const during = m.samples.filter((f) => f.at <= duration);
    pointer(window, 'pointermove', x(2.4), y);
    await nextTick();
    await wait(() => exposed.value?.renderStatus()?.time === ticks(2.4));
    await delay(500);
    const held = {
      time: exposed.value?.renderStatus()?.time,
      scrubbing: document.querySelector<HTMLElement>('.preview-stage')?.dataset.previewScrubbing
    };
    const released = performance.now();
    pointer(window, 'pointerup', x(2.2), y);
    await nextTick();
    await wait(
      () =>
        exposed.value?.renderStatus()?.time === ticks(2.2) &&
        exposed.value?.renderStatus()?.width === report.initial.width &&
        document.querySelector<HTMLElement>('.preview-stage')?.dataset.previewScrubbing === 'false'
    );
    await delay(150);
    const final = exposed.value!.renderStatus()!;
    m.stop();
    report.measurements.push({
      mode: 'scrub',
      eventSource: 'Synthetic PointerEvent, isTrusted=false',
      inputs,
      ...summarize(during, duration),
      last4Seconds: summarize(
        during.filter((f) => f.at > duration - 4000),
        4000
      ),
      final: {
        time: final.time,
        width: final.width,
        height: final.height,
        settleMs: performance.now() - released
      },
      stationaryHeld: held,
      samples: m.samples
    });
    report.complete =
      errors.length === 0 &&
      during.length > 10 &&
      during.every((f) => f.width === report.initial.width && f.height === report.initial.height) &&
      final.time === ticks(2.2) &&
      held.time === ticks(2.4) &&
      held.scrubbing === 'true';
    publish();
  } catch (e) {
    report.error = String(e);
    publish();
  } finally {
    buttons.forEach((button) => (button.disabled = false));
  }
}
async function freshPreview() {
  const previous = exposed.value?.renderStatus(),
    started = performance.now();
  playing.value = false;
  time.value = ticks(1);
  previewGeneration.value++;
  await nextTick();
  await wait(
    () =>
      !!exposed.value?.renderStatus() &&
      exposed.value.renderStatus() !== previous &&
      exposed.value.renderStatus()?.time === ticks(1)
  );
  const frame = structuredClone(toRaw(exposed.value!.renderStatus()!));
  report.initial = frame;
  return {
    rendererGeneration: previewGeneration.value,
    complete: true,
    initializationAndFirstFrameMs: performance.now() - started,
    firstFrame: frame,
    sourceDimensions: actualSourceDimensions([frame])
  };
}
async function scrubWarmup() {
  const times = [1.6, 2.4, 4.4, 2.2, 1.2, 1],
    started = performance.now();
  const ruler = document.querySelector<HTMLElement>('.ruler')!,
    rect = ruler.getBoundingClientRect();
  const x = (t: number) => rect.left + t * 60,
    y = rect.top + rect.height / 2;
  const samples: any[] = [];
  try {
    for (let i = 0; i < times.length; i++) {
      const t = times[i],
        start = performance.now();
      pointer(i === 0 ? ruler : window, i === 0 ? 'pointerdown' : 'pointermove', x(t), y);
      await nextTick();
      await wait(() => exposed.value?.renderStatus()?.time === ticks(t));
      samples.push({
        requestedTime: ticks(t),
        seekMs: performance.now() - start,
        ...structuredClone(toRaw(exposed.value!.renderStatus()!))
      });
    }
  } finally {
    pointer(window, 'pointerup', x(1), y);
  }
  await nextTick();
  await wait(
    () =>
      exposed.value?.renderStatus()?.time === ticks(1) &&
      document.querySelector<HTMLElement>('.preview-stage')?.dataset.previewScrubbing === 'false'
  );
  return {
    complete: true,
    eventSource: 'Synthetic PointerEvent, isTrusted=false',
    requestedTimes: times.map(ticks),
    preparationMs: performance.now() - started,
    samples,
    renderTimings: renderTimingSummary(samples),
    sourceDimensions: actualSourceDimensions(samples)
  };
}
async function playback(warm = false, readyDelay = false) {
  buttons.forEach((button) => (button.disabled = true));
  report.complete = false;
  let m: ReturnType<typeof monitor> | undefined;
  try {
    const rate = project.value.frameRate;
    if (rate.numerator / rate.denominator !== 30)
      throw new Error('This playback benchmark requires the original 30Hz project frame rate');
    const preparation = await freshPreview();
    const warmup = warm
      ? await scrubWarmup()
      : {
          complete: true,
          skipped: true,
          reason:
            'Fresh renderer playback retains only its separately reported complete initial-frame preparation'
        };
    const delayStarted = performance.now();
    if (readyDelay) await delay(600);
    const pausedLookahead = {
      enabled: readyDelay,
      requestedWaitMs: readyDelay ? 600 : 0,
      actualWaitMs: readyDelay ? performance.now() - delayStarted : 0,
      outsideFpsWindow: true,
      lastPresentedSnapshot: readyDelay
        ? structuredClone(toRaw(exposed.value!.renderStatus()!))
        : undefined,
      readinessEvidence: readyDelay
        ? 'Fixed paused wait; last presented report is a snapshot and does not prove background queue is empty'
        : undefined
    };
    const requests: any[] = [],
      durationMs = 5000,
      base = ticks(1);
    const frameTicks = (index: number) =>
      Math.round((index * 120000 * rate.denominator) / rate.numerator);
    m = monitor();
    const started = m.started;
    playing.value = true;
    let stopAt = started,
      nextIndex = 0;
    await new Promise<void>((resolve) => {
      let timer: ReturnType<typeof setTimeout>;
      const finish = () => {
        clearTimeout(timer);
        stopAt = performance.now();
        resolve();
      };
      const end = setTimeout(finish, durationMs);
      const step = () => {
        const now = performance.now(),
          elapsed = now - started;
        if (elapsed >= durationMs) {
          clearTimeout(end);
          finish();
          return;
        }
        const index = Math.floor((elapsed * 30) / 1000);
        if (index >= nextIndex) {
          const next = base + frameTicks(index);
          requests.push({ at: elapsed, index, time: next });
          time.value = next;
          nextIndex = index + 1;
        }
        timer = setTimeout(
          step,
          Math.max(1, started + (nextIndex * 1000) / 30 - performance.now())
        );
      };
      step();
    });
    playing.value = false;
    const during = m.samples.filter((f) => f.at >= 0 && f.at < durationMs);
    const unique = uniquePresented(during),
      requestTimes = new Set(requests.map((request) => request.time));
    await delay(350);
    m.stop();
    report.measurements.push({
      mode: warm
        ? 'playback-after-scrub'
        : readyDelay
          ? 'playback-after-paused-lookahead'
          : 'playback-fresh-renderer',
      eventSource:
        'Harness absolute wall-clock scheduled project-frame ticks at 30Hz, playing=true',
      rendererFresh: true,
      preparation,
      warmup,
      pausedLookahead,
      samplingWindow: {
        requestedMs: durationMs,
        durationMs,
        started,
        ended: started + durationMs,
        actualStopDispatchMs: stopAt - started,
        stopDispatchLatenessMs: Math.max(0, stopAt - started - durationMs),
        drainAfterWindowMs: 350,
        warmupIncludedInFps: false
      },
      requests,
      requestedUniqueTimes: [...requestTimes],
      requestedFrames: requests.length,
      expectedProjectFrames: 150,
      skippedClockFrames: 150 - requests.length,
      ...summarize(unique, durationMs),
      observedReportEvents: during.length,
      duplicateTimeReportEvents: during.length - unique.length,
      presentedUniqueTimes: unique.map((sample) => sample.time),
      unexpectedPresentedTimes: unique
        .filter((sample) => !requestTimes.has(sample.time))
        .map((sample) => sample.time),
      renderTimings: renderTimingSummary(unique),
      sourceDimensions: actualSourceDimensions(unique),
      samples: unique,
      reportEvents: during
    });
    report.complete =
      errors.length === 0 &&
      unique.length > 0 &&
      unique.every(
        (sample) => sample.width === report.initial.width && sample.height === report.initial.height
      );
    publish();
  } catch (e) {
    report.error = String(e);
    publish();
  } finally {
    playing.value = false;
    m?.stop();
    buttons.forEach((button) => (button.disabled = false));
  }
}
run.addEventListener('click', () => void sweep());
play.addEventListener('click', () => void playback(false));
playWarm.addEventListener('click', () => void playback(true));
playReady.addEventListener('click', () => void playback(false, true));
document.querySelector('#download')!.addEventListener('click', () => {
  const url = URL.createObjectURL(
      new Blob([JSON.stringify(report, null, 2)], { type: 'application/json' })
    ),
    link = document.createElement('a');
  link.href = url;
  link.download = 'mixed-timeline-scrub-and-playback.json';
  link.click();
  URL.revokeObjectURL(url);
});
await wait(() => exposed.value?.renderStatus()?.time === time.value);
report.initial = exposed.value?.renderStatus();
publish();
buttons.forEach((button) => (button.disabled = false));
