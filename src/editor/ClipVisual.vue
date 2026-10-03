<script setup lang="ts">
import { useI18n } from './i18n';
const { tr } = useI18n();
import { computed, ref, watch, onMounted, onBeforeUnmount } from 'vue';
import {
  seconds,
  ticks,
  mapTimelineToSource,
  type Project,
  type Item
} from '../../packages/core/project.mjs';
import { sharedMediaClient } from '../../packages/media/client';
import { sourceCacheIdentity } from '../../packages/media/runtime.mjs';
import { keepVisualTile, visualTile } from '../../packages/media/tiles';
import type { MediaFrame } from '../../packages/media/browser';

const props = withDefaults(
  defineProps<{
    project: Project;
    item: Item;
    mediaUrl: (id: string) => string;
    zoom: number;
    viewportStartSeconds: number;
    viewportEndSeconds: number;
    height?: number;
  }>(),
  { height: 42 }
);
const host = ref<HTMLElement>(),
  canvas = ref<HTMLCanvasElement>();
const visible = ref(false),
  measuredHeight = ref(0),
  message = ref('');
const paintedLeft = ref(0),
  paintedWidth = ref(0),
  revision = ref(0),
  quality = ref('idle');
const dpr = ref(Math.max(1, Math.min(4, window.devicePixelRatio || 1)));
const asset = computed(() =>
  props.project.assets.find((candidate) => candidate.id === props.item.clip.assetId)
);
let observer: IntersectionObserver | undefined, resize: ResizeObserver | undefined;
let request: AbortController | undefined,
  previewTimer: ReturnType<typeof setTimeout> | undefined,
  settleTimer: ReturnType<typeof setTimeout> | undefined;
let generation = 0;
const tile = 80;
function layout() {
  if (!visible.value || !canvas.value || !asset.value || props.zoom <= 0) return;
  const item = props.item,
    media = asset.value;
  const clipBegin = seconds(item.placement.begin),
    clipEnd = seconds(item.placement.end);
  // Overscan is measured in pixels, so zooming out never schedules minutes of hidden media.
  const begin = Math.max(clipBegin, props.viewportStartSeconds - tile / props.zoom);
  const end = Math.min(clipEnd, props.viewportEndSeconds + tile / props.zoom);
  if (end <= begin) return;
  const height = Math.max(8, measuredHeight.value || props.height),
    film = media.kind !== 'audio';
  const waveHeight = media.hasAudio ? (film ? Math.min(16, height * 0.36) : height) : 0,
    filmHeight = height - waveHeight;
  const first = Math.floor(((begin - clipBegin) * props.zoom) / tile),
    last = Math.min(first + 256, Math.ceil(((end - clipBegin) * props.zoom) / tile));
  const left = film ? first * tile : (begin - clipBegin) * props.zoom;
  const width = Math.min(
    (clipEnd - clipBegin) * props.zoom - left,
    film ? (last - first) * tile : (end - begin) * props.zoom
  );
  if (width <= 0) return;
  const url = props.mediaUrl(media.id),
    source = sourceCacheIdentity(url),
    crop = item.clip.visual.crop ?? { left: 0, right: 0, top: 0, bottom: 0 };
  const times = Array.from({ length: last - first }, (_, i) =>
    seconds(
      mapTimelineToSource(
        item,
        Math.min(item.placement.end - 1, ticks(clipBegin + ((first + i + 0.5) * tile) / props.zoom))
      )
    )
  );
  const key = (time: number, level: 'interactive' | 'final') =>
    JSON.stringify([
      source,
      media.kind === 'image' ? 0 : Math.round(time * 1e6),
      tile,
      filmHeight,
      dpr.value,
      crop,
      level
    ]);
  return {
    item,
    media,
    clipBegin,
    url,
    crop,
    height,
    film,
    filmHeight,
    waveHeight,
    left,
    width,
    times,
    key
  };
}
type Layout = NonNullable<ReturnType<typeof layout>>;
function prepare(view: Layout) {
  const target = canvas.value!,
    ctx = target.getContext('2d');
  if (!ctx) return;
  target.width = Math.min(16384, Math.ceil(view.width * dpr.value));
  target.height = Math.ceil(view.height * dpr.value);
  ctx.scale(target.width / view.width, dpr.value);
  ctx.fillStyle = '#12323b';
  ctx.fillRect(0, 0, view.width, view.height);
  paintedLeft.value = view.left;
  paintedWidth.value = view.width;
  return ctx;
}
function paint(ctx: CanvasRenderingContext2D, view: Layout, frame: MediaFrame, index: number) {
  const crop = view.media.kind === 'image' ? view.crop : { left: 0, right: 0, top: 0, bottom: 0 };
  const availableWidth = frame.width * (1 - crop.left - crop.right),
    availableHeight = frame.height * (1 - crop.top - crop.bottom);
  const scale = Math.max(tile / availableWidth, view.filmHeight / availableHeight),
    sw = tile / scale,
    sh = view.filmHeight / scale;
  ctx.drawImage(
    frame.frame,
    frame.width * crop.left + (availableWidth - sw) / 2,
    frame.height * crop.top + (availableHeight - sh) / 2,
    sw,
    sh,
    index * tile,
    0,
    tile,
    view.filmHeight
  );
}
function drawCached() {
  const view = layout();
  if (!view) return;
  const ctx = prepare(view);
  if (!ctx) return;
  let reused = 0;
  if (view.film)
    view.times.forEach((time, index) => {
      const frame =
        visualTile(view.key(time, 'final')) || visualTile(view.key(time, 'interactive'));
      if (frame) {
        paint(ctx, view, frame, index);
        reused++;
      }
    });
  canvas.value!.dataset.reusedTiles = String(reused);
  quality.value = 'interactive';
}
async function draw(level: 'interactive' | 'final') {
  request?.abort();
  const view = layout();
  if (!view) return;
  const controller = (request = new AbortController()),
    epoch = ++generation;
  const current = () => !controller.signal.aborted && epoch === generation && Boolean(canvas.value);
  const ctx = canvas.value!.getContext('2d');
  if (!ctx) return;
  const failed = (error: unknown) => {
    if (current() && !(error instanceof Error && error.name === 'AbortError'))
      message.value = error instanceof Error ? error.message : String(error);
  };
  message.value = '';
  const film = view.film
    ? (async () => {
        const missing = view.times
          .map((time, index) => ({ time, index }))
          .filter(
            ({ time }) =>
              !visualTile(view.key(time, level)) &&
              !(level === 'interactive' && visualTile(view.key(time, 'final')))
          );
        const mediaWidth = view.media.width || 1,
          mediaHeight = view.media.height || 1;
        const imageScale = Math.min(
          1,
          Math.max(
            tile / (mediaWidth * (1 - view.crop.left - view.crop.right)),
            view.filmHeight / (mediaHeight * (1 - view.crop.top - view.crop.bottom))
          ) *
            dpr.value *
            (level === 'interactive' ? 0.5 : 1)
        );
        const frames =
          view.media.kind === 'image'
            ? missing.length
              ? [
                  await sharedMediaClient.image(view.url, {
                    signal: controller.signal,
                    priority: 8,
                    width: Math.ceil(mediaWidth * imageScale),
                    height: Math.ceil(mediaHeight * imageScale)
                  })
                ]
              : []
            : missing.length
              ? await sharedMediaClient.thumbnails(
                  view.url,
                  missing.map((value) => value.time),
                  {
                    signal: controller.signal,
                    displayWidth: tile,
                    displayHeight: view.filmHeight,
                    dpr: dpr.value,
                    fit: 'cover',
                    crop: view.crop,
                    quality: level,
                    priority: 8
                  }
                )
              : [];
        try {
          if (!current()) return;
          for (let i = 0; i < missing.length; i++) {
            const { time, index } = missing[i],
              frame = frames[view.media.kind === 'image' ? 0 : i];
            if (!frame) continue;
            paint(ctx, view, frame, index);
            if (view.media.kind !== 'image') {
              keepVisualTile(view.key(time, level), frame);
              frames[i] = undefined as unknown as MediaFrame;
            }
          }
          if (view.media.kind === 'image' && frames[0]) {
            keepVisualTile(view.key(0, level), frames[0]);
            frames[0] = undefined as unknown as MediaFrame;
          }
          const first = visualTile(view.key(view.times[0], level)),
            last = visualTile(view.key(view.times.at(-1)!, level));
          if (canvas.value && first) {
            canvas.value.dataset.firstPts = String(first.timestamp);
            canvas.value.dataset.lastPts = String(last?.timestamp);
            canvas.value.dataset.tileWidth = String(first.width);
            canvas.value.dataset.tileHeight = String(first.height);
            canvas.value.dataset.sampleTimes = JSON.stringify(view.times);
          }
          quality.value = level;
          revision.value++;
        } finally {
          frames.forEach((frame) => frame?.close());
        }
      })().catch(failed)
    : Promise.resolve();
  const wave =
    level === 'final' && view.media.hasAudio
      ? sharedMediaClient
          .waveform(
            view.url,
            seconds(mapTimelineToSource(view.item, ticks(view.clipBegin + view.left / props.zoom))),
            seconds(
              mapTimelineToSource(
                view.item,
                Math.min(
                  view.item.placement.end,
                  ticks(view.clipBegin + (view.left + view.width) / props.zoom)
                )
              )
            ),
            Math.min(8192, Math.max(1, Math.ceil(view.width * dpr.value))),
            { signal: controller.signal }
          )
          .then((value) => {
            if (!current()) return;
            ctx.fillStyle = view.film ? '#143d43' : '#1b3d4a';
            ctx.fillRect(0, view.filmHeight, view.width, view.waveHeight);
            ctx.strokeStyle = '#94dcc5';
            ctx.lineWidth = 1 / dpr.value;
            value.channels.forEach((channel, index) => {
              const band = view.waveHeight / value.channels.length,
                center = view.filmHeight + band * (index + 0.5),
                amplitude = band * 0.44;
              ctx.beginPath();
              for (let x = 0; x < channel.min.length; x++) {
                const at = ((x + 0.5) * view.width) / channel.min.length;
                ctx.moveTo(at, center - channel.max[x] * amplitude);
                ctx.lineTo(at, center - channel.min[x] * amplitude);
              }
              ctx.stroke();
            });
            revision.value++;
          })
          .catch(failed)
      : Promise.resolve();
  await Promise.all([film, wave]);
}
function schedule() {
  generation++;
  request?.abort();
  clearTimeout(previewTimer);
  clearTimeout(settleTimer);
  if (!visible.value) return;
  drawCached();
  previewTimer = setTimeout(() => void draw('interactive'), 35);
  settleTimer = setTimeout(() => void draw('final'), 140);
}
const updateDpr = () => {
  dpr.value = Math.max(1, Math.min(4, window.devicePixelRatio || 1));
};
watch(
  [
    () => visible.value,
    () => props.item.placement.begin,
    () => props.item.placement.end,
    () => props.item.clip.source.begin,
    () => props.item.clip.source.end,
    () => JSON.stringify(props.item.clip.retime),
    () => JSON.stringify(props.item.clip.visual.crop),
    () => asset.value,
    () => asset.value && props.mediaUrl(asset.value.id),
    () => props.zoom,
    () => props.viewportStartSeconds,
    () => props.viewportEndSeconds,
    () => measuredHeight.value,
    () => dpr.value
  ],
  schedule,
  { flush: 'post' }
);
onMounted(() => {
  observer = new IntersectionObserver(
    ([entry]) => {
      visible.value = entry.isIntersecting;
      if (!entry.isIntersecting) {
        generation++;
        request?.abort();
        clearTimeout(previewTimer);
        clearTimeout(settleTimer);
      }
    },
    { rootMargin: '80px' }
  );
  if (host.value) observer.observe(host.value);
  resize = new ResizeObserver(([entry]) => {
    measuredHeight.value = entry.contentRect.height;
    updateDpr();
  });
  if (host.value) resize.observe(host.value);
  window.addEventListener('resize', updateDpr);
});
onBeforeUnmount(() => {
  generation++;
  request?.abort();
  clearTimeout(previewTimer);
  clearTimeout(settleTimer);
  observer?.disconnect();
  resize?.disconnect();
  window.removeEventListener('resize', updateDpr);
});
</script>

<template>
  <div
    ref="host"
    class="clip-media-visual"
    :title="message"
    :data-visual-revision="revision"
    :data-visual-quality="quality"
    :data-visual-error="message || undefined"
  >
    <canvas ref="canvas" :style="{ left: `${paintedLeft}px`, width: `${paintedWidth}px` }" />
    <span v-if="message" class="clip-media-error">{{ tr('媒体预览不可用') }}</span>
  </div>
</template>
<style scoped>
.clip-media-visual {
  position: relative;
  height: calc(100% - 18px);
  overflow: hidden;
  pointer-events: none;
  background: #12323b;
}
canvas {
  position: absolute;
  top: 0;
  height: 100%;
}
.clip-media-error {
  position: absolute;
  left: 5px;
  top: 2px;
  font-size: 9px;
  color: #a8c4c9;
}
</style>
