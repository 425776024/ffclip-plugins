import { createApp, h, reactive } from 'vue';
import '../src/editor/editor.css';
import { VideoCutClient } from '../packages/client/index.mjs';
import {
  createProject,
  addAsset,
  ticks,
  seconds,
  mapTimelineToSource
} from '../packages/core/project.mjs';
import { editTimeline } from '../packages/core/operations.mjs';
import { MediaEngine, setMediaForegroundActivity } from '../packages/media/browser';
import { MediaWorkerClient, sharedMediaClient } from '../packages/media/client';
import { MediaArtifactStore } from '../packages/media/artifacts';
import { visualTileStats } from '../packages/media/tiles';
import ClipVisual from '../src/editor/ClipVisual.vue';
import Timeline from '../src/editor/Timeline.vue';

const query = new URLSearchParams(location.search),
  fixtures =
    query.get('fixtures') || '/Users/jxinfa/WebstormProjects/videocut/.local/architecture-qa';
if (document.documentElement.dataset.suite === 'long-play') query.set('long', '1');
const client = new VideoCutClient(location.origin),
  status = document.querySelector('#status')!;
const report: any = {
  suite: query.has('long') ? 'long-play' : 'cache-geometry',
  phase: 'idle',
  userAgent: navigator.userAgent,
  results: []
};
const check = (value: unknown, message: string) => {
  if (!value) throw Error(message);
};
const delay = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));
const summary = (values: number[]) => {
  const sorted = [...values].sort((a, b) => a - b);
  return {
    n: values.length,
    median: sorted[Math.floor(sorted.length * 0.5)],
    p95: sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * 0.95))],
    max: sorted.at(-1)
  };
};
async function publish() {
  status.textContent = JSON.stringify(report, null, 2);
  await fetch('http://127.0.0.1:4333/report', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'X-QA-Report': 'media' },
    body: JSON.stringify(report)
  }).catch(() => {});
}
async function one(name: string, work: () => Promise<unknown>) {
  report.phase = name;
  await publish();
  const start = performance.now();
  const value = await work();
  report.results.push({ name, milliseconds: performance.now() - start, value });
  await publish();
}
async function until(work: () => boolean, message: string) {
  for (let i = 0; i < 400; i++) {
    if (work()) return;
    await delay(25);
  }
  throw Error(message);
}
function pixels(frame: ImageBitmap) {
  const c = new OffscreenCanvas(frame.width, frame.height);
  const x = c.getContext('2d')!;
  x.drawImage(frame, 0, 0);
  return x.getImageData(0, 0, c.width, c.height).data;
}
async function run() {
  report.phase = 'importing';
  report.started = new Date().toISOString();
  report.results = [];
  report.freshThumbnailIdentity = query.has('cold');
  delete report.error;
  await publish();
  const engine = new MediaEngine(),
    worker = new MediaWorkerClient();
  let session = '',
    clipApp: any,
    timelineApp: any;
  try {
    await client.connect();
    const names = [
      'timecode.mp4',
      'vfr.mp4',
      'portrait.mp4',
      'anamorphic.mp4',
      'probe/rotated.mov',
      ...(query.has('long') ? ['long-play.mp4'] : [])
    ];
    const assets = await Promise.all(
      names.map((name) =>
        client.importMedia(`${fixtures}/${name}`).then((value) => value.asset || value)
      )
    );
    const project = createProject('Media cache acceptance');
    assets.forEach((asset) => addAsset(project, asset));
    session = (await client.createSession(project)).id;
    const urls = assets.map(
      (asset) =>
        client.mediaUrl(session, asset.id) +
        (query.has('cold') ? '' : '&source=' + encodeURIComponent(asset.sourceIdentity))
    );
    const reference = await (await fetch(`/@fs${fixtures}/media-pts.json`)).json();
    await one('portrait_rotation_PAR_Bframes_VFR_DPR1_2_same_pixels', async () => {
      const results = [];
      const crop = { left: 0.13, right: 0.09, top: 0.07, bottom: 0.11 };
      for (let index = 0; index < 5; index++)
        for (const dpr of [1, 2]) {
          const times = [0.001, 0.371, 1.001, 2.101, 3.4];
          setMediaForegroundActivity(false);
          const before = engine.stats(),
            start = performance.now();
          const frames = await engine.thumbnails(urls[index], times, {
            displayWidth: 80,
            displayHeight: 40,
            dpr,
            crop
          });
          const coldMs = performance.now() - start,
            metrics = [];
          for (let i = 0; i < frames.length; i++) {
            const f = frames[i],
              full = await engine.videoFrame(urls[index], times[i]);
            try {
              const pts = reference[names[index]]
                .filter((p: number) => p <= times[i] + 1e-8)
                .at(-1);
              check(Math.abs(f.timestamp - pts) < 2e-6, `PTS mismatch ${names[index]} ${times[i]}`);
              check(
                f.width === 80 * dpr && f.height === 40 * dpr,
                'Thumbnail raster did not meet both slot dimensions'
              );
              const c = new OffscreenCanvas(f.width, f.height),
                ctx = c.getContext('2d')!;
              const sx = full.width * crop.left,
                sy = full.height * crop.top,
                sw = full.width * (1 - crop.left - crop.right),
                sh = full.height * (1 - crop.top - crop.bottom);
              const scale = Math.max(c.width / sw, c.height / sh),
                dw = sw * scale,
                dh = sh * scale;
              ctx.drawImage(
                full.frame,
                sx,
                sy,
                sw,
                sh,
                (c.width - dw) / 2,
                (c.height - dh) / 2,
                dw,
                dh
              );
              const expected = ctx.getImageData(0, 0, c.width, c.height).data,
                actual = pixels(f.frame);
              let sum = 0;
              for (let p = 0; p < actual.length; p++) sum += Math.abs(actual[p] - expected[p]);
              const mae = sum / actual.length;
              check(
                mae < 2,
                `Raster differs from full-resolution reference: ${names[index]} ${mae}`
              );
              metrics.push({
                requested: times[i],
                actualPTS: f.timestamp,
                mae,
                width: f.width,
                height: f.height
              });
            } finally {
              f.close();
              full.close();
            }
          }
          const startWarm = performance.now(),
            warm = await engine.thumbnails(urls[index], times, {
              displayWidth: 80,
              displayHeight: 40,
              dpr,
              crop
            });
          warm.forEach((frame) => frame.close());
          results.push({
            fixture: names[index],
            dpr,
            coldMs,
            warmMs: performance.now() - startWarm,
            newDecodeJobs: engine.stats().thumbnailDecodes - before.thumbnailDecodes,
            metrics
          });
        }
      return results;
    });
    await one('image_preview_size_and_identity', async () => {
      const imported = await client.importMedia(`${fixtures}/still.png`),
        asset = imported.asset || imported;
      const imageProject = createProject('Image size acceptance');
      addAsset(imageProject, asset);
      const imageSession = (await client.createSession(imageProject)).id;
      try {
        const url =
          client.mediaUrl(imageSession, asset.id) +
          '&source=' +
          encodeURIComponent(asset.sourceIdentity);
        const original = await engine.image(url),
          low = await engine.image(url, { width: 160, height: 90 }),
          high = await engine.image(url, { width: 320, height: 180 });
        try {
          check(
            low.width <= 160 && low.height <= 90 && high.width <= 320 && high.height <= 180,
            'Image preview dimensions exceeded target'
          );
          check(
            original.width > low.width && high.width > low.width,
            'Image cache reused the wrong resolution'
          );
          const before = engine.stats(),
            warm = await engine.image(url, { width: 160, height: 90 });
          warm.close();
          check(engine.stats().completed === before.completed, 'Sized image cache missed');
          const imageItem = imageProject.timeline.tracks
            .flatMap((track) => track.items)
            .find((item) => item.clip.assetId === asset.id)!;
          imageItem.clip.visual.crop = { left: 0.13, right: 0.09, top: 0.07, bottom: 0.11 };
          clipApp = createApp({
            render: () =>
              h(ClipVisual, {
                project: imageProject,
                item: imageItem,
                mediaUrl: () => url,
                zoom: 180,
                viewportStartSeconds: 0,
                viewportEndSeconds: 4,
                height: 54
              })
          });
          clipApp.mount('#clip');
          await until(
            () =>
              document
                .querySelector('#clip [data-visual-quality]')
                ?.getAttribute('data-visual-quality') === 'final',
            'Image filmstrip did not settle'
          );
          const canvas = document.querySelector('#clip canvas') as HTMLCanvasElement,
            cellWidth = Math.round(80 * devicePixelRatio),
            crop = imageItem.clip.visual.crop;
          const expectedCanvas = new OffscreenCanvas(cellWidth, canvas.height),
            referenceContext = expectedCanvas.getContext('2d')!;
          const sourceWidth = original.width * (1 - crop.left - crop.right),
            sourceHeight = original.height * (1 - crop.top - crop.bottom),
            scale = Math.max(cellWidth / sourceWidth, canvas.height / sourceHeight);
          referenceContext.drawImage(
            original.frame,
            original.width * crop.left,
            original.height * crop.top,
            sourceWidth,
            sourceHeight,
            (cellWidth - sourceWidth * scale) / 2,
            (canvas.height - sourceHeight * scale) / 2,
            sourceWidth * scale,
            sourceHeight * scale
          );
          const actualPixels = canvas
              .getContext('2d')!
              .getImageData(0, 0, cellWidth, canvas.height).data,
            referencePixels = referenceContext.getImageData(0, 0, cellWidth, canvas.height).data;
          let delta = 0;
          for (let i = 0; i < actualPixels.length; i++)
            delta += Math.abs(actualPixels[i] - referencePixels[i]);
          const filmstripMAE = delta / actualPixels.length;
          check(
            filmstripMAE < 2,
            `Image filmstrip crop differs from full resolution reference: ${filmstripMAE}`
          );
          clipApp.unmount();
          clipApp = undefined;
          return {
            filmstripMAE,
            original: [original.width, original.height],
            low: [low.width, low.height],
            high: [high.width, high.height],
            warmCacheHits: engine.stats().frameHits - before.frameHits
          };
        } finally {
          original.close();
          low.close();
          high.close();
        }
      } finally {
        await client.closeSession(imageSession);
      }
    });
    await one('persistent_thumbnail_index_cross_engine_and_worker', async () => {
      const other = new MediaEngine(),
        options = {
          displayWidth: 80,
          displayHeight: 40,
          dpr: 2,
          crop: { left: 0.13, right: 0.09, top: 0.07, bottom: 0.11 }
        };
      try {
        const start = performance.now(),
          frames = await other.thumbnails(urls[2], [0.001, 0.371, 1.001], options);
        frames.forEach((frame) => frame.close());
        const secondEngine = { ms: performance.now() - start, ...other.stats() };
        check(
          secondEngine.thumbnailDecodes === 0 &&
            secondEngine.persistentThumbnailHits === 3 &&
            secondEngine.sources === 0,
          'New engine did not reuse immutable index and PNG artifacts'
        );
        const fresh = await worker.thumbnails(urls[2], [0.001, 0.371, 1.001], options);
        fresh.forEach((frame) => frame.close());
        const stats = await worker.stats();
        check(
          stats.thumbnailDecodes === 0 && stats.persistentThumbnailHits === 3,
          'Worker decoded a persisted thumbnail again'
        );
        other.invalidate(urls[2]);
        const invalidated = await other.thumbnails(urls[2], [0.001, 0.371, 1.001], options);
        invalidated.forEach((frame) => frame.close());
        check(
          other.stats().thumbnailDecodes === 3,
          'Invalidation revived persisted thumbnails without opening a decoder'
        );
        return {
          secondEngine,
          worker: stats,
          afterInvalidation: other.stats(),
          persistent: await other.persistentUsage()
        };
      } finally {
        other.dispose();
      }
    });
    await one('IDB_budget_LRU_and_hashed_private_keys', async () => {
      const name = 'videocut-media-qa-' + crypto.randomUUID(),
        store = new MediaArtifactStore(800000, name),
        bytes = new Uint8Array(300000);
      try {
        await store.put(
          'thumbnail-v3:/private/user/path?token=secret-one',
          bytes,
          bytes.byteLength
        );
        await delay(2);
        await store.put(
          'thumbnail-v3:/private/user/path?token=secret-two',
          bytes,
          bytes.byteLength
        );
        await delay(2);
        check(
          !!(await store.get('thumbnail-v3:/private/user/path?token=secret-one')),
          'Expected first entry'
        );
        await delay(2);
        await store.put(
          'thumbnail-v3:/private/user/path?token=secret-three',
          bytes,
          bytes.byteLength
        );
        check(
          !(await store.get('thumbnail-v3:/private/user/path?token=secret-two')),
          'LRU did not evict least recently read entry'
        );
        const usage = await store.usage();
        check(usage.bytes === 600000 && usage.entries === 2, 'Persistent byte budget failed');
        const keys = await new Promise<string[]>((resolve, reject) => {
          const request = indexedDB.open(name);
          request.onsuccess = () => {
            const db = request.result,
              r = db.transaction('artifacts').objectStore('artifacts').getAllKeys();
            r.onsuccess = () => {
              resolve(r.result.map(String));
              db.close();
            };
            r.onerror = () => reject(r.error);
          };
          request.onerror = () => reject(request.error);
        });
        check(
          keys.every((key) => /^thumbnail:[a-f0-9]{64}$/.test(key)),
          'Persistent keys exposed private source information'
        );
        return { usage, keys, counters: store.counters };
      } finally {
        await store.close();
        indexedDB.deleteDatabase(name);
      }
    });
    await one('trim_split_speed_and_settled_HD_component', async () => {
      const base = createProject('Mapping');
      addAsset(base, assets[2]);
      const original = base.timeline.tracks.flatMap((t) => t.items)[0];
      let edited = editTimeline(base, [
        { action: 'trim_clip', itemId: original.id, sourceInSeconds: 0.5, durationSeconds: 3 },
        { action: 'set_speed', itemId: original.id, rate: 2, ripple: false },
        { action: 'split_clip', itemId: original.id, atSeconds: 0.75 }
      ]).project;
      const item = edited.timeline.tracks.flatMap((t) => t.items).at(-1)!;
      item.clip.visual.crop = { left: 0.1, right: 0.1, top: 0, bottom: 0 };
      const state = reactive({ start: 0, end: 4 });
      clipApp = createApp({
        render: () =>
          h(ClipVisual, {
            project: edited,
            item,
            mediaUrl: () => urls[2],
            zoom: 180,
            viewportStartSeconds: state.start,
            viewportEndSeconds: state.end
          })
      });
      clipApp.mount('#clip');
      await until(
        () =>
          document
            .querySelector('#clip [data-visual-quality]')
            ?.getAttribute('data-visual-quality') === 'final',
        'ClipVisual never produced settled HD'
      );
      const canvas = document.querySelector('#clip canvas') as HTMLCanvasElement;
      check(
        Number(canvas.dataset.tileWidth) >= 80 * Math.min(4, devicePixelRatio),
        'Portrait component is magnifying undersized thumbnails'
      );
      const sampleTimes = JSON.parse(canvas.dataset.sampleTimes!);
      check(
        sampleTimes.every(
          (t: number) => t >= item.clip.source.begin / 120000 && t < item.clip.source.end / 120000
        ),
        'Trim/split/speed escaped source range'
      );
      const firstPTS = Number(canvas.dataset.firstPts);
      check(
        Math.abs(
          firstPTS -
            reference['portrait.mp4'].filter((t: number) => t <= sampleTimes[0] + 1e-8).at(-1)
        ) < 2e-6,
        'Component does not use exact remapped PTS'
      );
      state.start = 0.5;
      state.end = 3.5;
      await delay(10);
      const reused = Number(canvas.dataset.reusedTiles);
      check(reused > 0, 'Scroll did not immediately reuse raster tiles');
      await until(
        () =>
          document
            .querySelector('#clip [data-visual-quality]')
            ?.getAttribute('data-visual-quality') === 'final',
        'Scrolled clip never settled'
      );
      return {
        source: item.clip.source,
        placement: item.placement,
        retime: item.clip.retime,
        sampleTimes,
        firstPTS,
        reused,
        tileWidth: canvas.dataset.tileWidth,
        tileHeight: canvas.dataset.tileHeight,
        dpr: devicePixelRatio
      };
    });
    clipApp.unmount();
    clipApp = undefined;
    await one('1000_clips_scroll_with_continuous_cursor', async () => {
      const many = createProject('1000 clips');
      const mediaIndex = query.has('long') ? 5 : 0;
      addAsset(many, assets[mediaIndex]);
      const template = many.timeline.tracks.find((t) => t.type === 'video')!.items[0];
      many.timeline.tracks = Array.from({ length: 50 }, (_, row) => ({
        id: 'qa-track-' + row,
        name: 'Track ' + row,
        type: 'video',
        visible: true,
        muted: true,
        locked: false,
        items: Array.from({ length: 20 }, (_, column) => {
          const item = structuredClone(template);
          item.id = `qa-item-${row}-${column}`;
          item.clip.id = `qa-clip-${row}-${column}`;
          item.placement = { begin: ticks(column * 2), end: ticks(column * 2 + 2) };
          const sourceBegin = query.has('long') ? (row * 20 + column) * 0.1 : 0;
          item.clip.source = { begin: ticks(sourceBegin), end: ticks(sourceBegin + 2) };
          return item;
        })
      }));
      const state = reactive({ time: 0 });
      timelineApp = createApp({
        render: () =>
          h(Timeline, {
            project: many,
            time: state.time,
            selected: '',
            selection: [],
            zoom: 90,
            snap: true,
            busy: false,
            mediaUrl: () => urls[mediaIndex]
          })
      });
      timelineApp.mount('#timeline');
      await delay(300);
      const viewport = document.querySelector('#timeline .timeline-scroll') as HTMLElement;
      const reader = await engine.createVideoReader(urls[mediaIndex], { width: 640, height: 360 }),
        latency: number[] = [],
        heap: number[] = [],
        dom: number[] = [],
        workerMax: Record<string, number> = {},
        intervals: unknown[] = [];
      const workerBefore = await sharedMediaClient.stats();
      const began = performance.now(),
        frames = query.has('short') ? 120 : query.has('long') ? 3596 : 600;
      try {
        for (let i = 0; i < frames; i++) {
          const at = ((query.has('long') ? i : i % 120) * 1001) / 30000,
            start = performance.now(),
            frame = await reader.frameAt(at);
          latency.push(performance.now() - start);
          if (query.has('long'))
            check(
              Math.abs(frame.timestamp - at) < 2e-6,
              `Long cursor PTS mismatch at ${at}: ${frame.timestamp}`
            );
          frame.close();
          state.time = ticks(at);
          if (i % 3 === 0) {
            viewport.scrollLeft = (i * 29) % 2500;
            viewport.scrollTop = (i * 41) % 3500;
            viewport.dispatchEvent(new Event('scroll'));
          }
          dom.push(document.querySelectorAll('#timeline .timeline-clip').length);
          heap.push((performance as any).memory?.usedJSHeapSize || 0);
          if (i % 30 === 0) {
            const stats = await sharedMediaClient.stats();
            for (const key of ['activeJobs', 'frameBytes', 'pcmBytes', 'waveformBytes', 'sources'])
              workerMax[key] = Math.max(workerMax[key] || 0, stats[key]);
          }
          if (i % 900 === 899) {
            intervals.push({
              frame: i + 1,
              milliseconds: performance.now() - began,
              jsHeapBytes: heap.at(-1),
              main: engine.stats(),
              worker: await sharedMediaClient.stats()
            });
            report.longPlaybackFrames = i + 1;
            await publish();
          }
          await delay(Math.max(0, ((i + 1) * 1001) / 30 - (performance.now() - began)));
        }
      } finally {
        await reader.close();
        setMediaForegroundActivity(false);
      }
      check(Math.max(...dom) < 150, 'Timeline mounted too many of its 1000 clips');
      const settledStart = performance.now();
      const visibleFinalCount = () => {
        const bounds = viewport.getBoundingClientRect();
        return [...document.querySelectorAll('#timeline [data-visual-quality="final"]')].filter(
          (element) => {
            const rect = element.getBoundingClientRect();
            return (
              rect.width > 0 &&
              rect.height > 0 &&
              rect.left < bounds.right &&
              rect.right > bounds.left &&
              rect.top < bounds.bottom &&
              rect.bottom > bounds.top
            );
          }
        ).length;
      };
      await until(
        () => visibleFinalCount() > 0,
        'Visible thumbnails did not resume after playback'
      );
      const settledMs = performance.now() - settledStart;
      const actual = visibleFinalCount();
      check(actual > 0, 'Visible thumbnails did not resume after playback');
      return {
        sourceDurationSeconds: seconds(assets[mediaIndex].duration),
        continuousSource: query.has('long'),
        samples: intervals,
        authoredClips: 1000,
        frames,
        wallMs: performance.now() - began,
        decodeLatency: summary(latency),
        maximumClipDOM: Math.max(...dom),
        settledVisibleClipCount: actual,
        settledMs,
        maximumJSHeapBytes: Math.max(...heap),
        engine: engine.stats(),
        backgroundWorkerMaximum: workerMax,
        backgroundWorkerBefore: workerBefore,
        backgroundWorkerAfter: await sharedMediaClient.stats(),
        uiTiles: visualTileStats()
      };
    });
    timelineApp.unmount();
    timelineApp = undefined;
    report.phase = 'complete';
    report.completed = new Date().toISOString();
  } catch (error) {
    report.phase = 'failed';
    report.error = error instanceof Error ? error.stack : String(error);
  } finally {
    clipApp?.unmount();
    timelineApp?.unmount();
    worker.dispose();
    engine.dispose();
    if (session) await client.closeSession(session).catch(() => {});
    await publish();
  }
}
document.querySelector('#run')!.addEventListener('click', () => void run());
if (query.has('autorun')) void run();
