<script setup lang="ts">
import { useI18n } from './i18n';
const { tr } = useI18n();
import { watch, onMounted, onBeforeUnmount, ref, shallowRef, computed, toRaw } from 'vue';
import {
  evaluateVisual,
  documentPatches,
  type Project,
  type Item
} from '../../packages/core/project.mjs';
import { previewExtent, previewSampleTime, layerGeometry } from '../../packages/render/plan.mjs';
import type { PresentationOverride } from '../../packages/render/projection';
import {
  cornerResize,
  textWidthResize,
  type ResizeCorner,
  type TextWidthEdge,
  type PreviewTransform
} from './preview-resize';
import type { RenderReport } from '../../packages/render/renderer';
import { sharedMediaEngine, PreviewAudioPlayer } from '../../packages/media/browser';
import Icon from './Icon.vue';
import { previewAxisSnap } from './snapping';
const props = defineProps<{
  project: Project;
  time: number;
  playing: boolean;
  mediaUrl: (id: string) => string;
  selected: string;
  disabled: boolean;
  snap?: boolean;
  quality?: 'auto' | 'full' | 'half';
  textWorkers?: boolean;
}>();
const emit = defineEmits<{
  error: [message: string];
  select: [id: string];
  transform: [id: string, changes: PreviewTransform, settle: () => void];
}>();
const stage = ref<HTMLElement>(),
  canvas = ref<HTMLCanvasElement>(),
  report = ref<RenderReport>(),
  status = ref('正在准备画布…');
const moving = ref<PresentationOverride | null>(null);
const corners: Record<ResizeCorner, string> = { tl: '左上', tr: '右上', bl: '左下', br: '右下' };
const widthEdges: Record<TextWidthEdge, string> = { left: '左', right: '右' };
const snapGuides = ref<{ x?: number; y?: number }>({});
const presentedProject = shallowRef<Project>();
const frameCurrent = computed(
  () =>
    presentedProject.value === toRaw(props.project) &&
    report.value?.time === previewSampleTime(props.time, props.project.frameRate, props.playing)
);
const scrubbing = ref(false);
let worker: Worker | undefined,
  observer: ResizeObserver | undefined,
  ready = false,
  running = false,
  pending = false,
  disposed = false,
  sequence = 0,
  generation = 0,
  cleanup = () => {};
let latestProject: Project,
  lastSentProject: Project | undefined,
  documentRevision = 0,
  lastAssets: Project['assets'] | undefined,
  lastMediaUrl: typeof props.mediaUrl | undefined;
let seekSettleTimer: ReturnType<typeof setTimeout> | undefined;
let committing = false;
let heldSeekPointer: number | undefined;
let mediaUrls: Record<string, string> = {};
function assetMediaUrl(a: Project['assets'][number]) {
  const url = new URL(props.mediaUrl(a.id), location.href),
    version = a as typeof a & { fingerprint?: unknown; mtimeMs?: number; sourceIdentity?: string };
  url.searchParams.set(
    'source',
    version.sourceIdentity ||
      JSON.stringify([a.id, a.path, a.size, version.fingerprint ?? null, version.mtimeMs ?? null])
  );
  return url.href;
}
function reportAudioFailure(error: unknown) {
  // A newer seek/play request cancels the previous decode. Its rejection must
  // not stop the current transport or surface as a playback failure.
  if (error instanceof Error && error.name === 'AbortError') return;
  emit('error', error instanceof Error ? error.message : String(error));
}
const audio = new PreviewAudioPlayer(
  sharedMediaEngine,
  (id) => {
    const asset = props.project.assets.find((a) => a.id === id);
    return asset ? assetMediaUrl(asset) : props.mediaUrl(id);
  },
  reportAudioFailure
);
function snapshot() {
  cleanup();
  clearTimeout(seekSettleTimer);
  scrubbing.value = false;
  moving.value = null;
  committing = false;
  latestProject = toRaw(props.project);
  invalidate();
  request();
}
function invalidate() {
  generation++;
  worker?.postMessage({ type: 'invalidate', generation });
}
function request() {
  pending = true;
  if (ready && !running) send();
}
function settleSeek() {
  if (disposed) return;
  clearTimeout(seekSettleTimer);
  seekSettleTimer = undefined;
  if (!scrubbing.value) return;
  scrubbing.value = false;
  // Once input stops, obsolete progressive frames cannot replace the final seek.
  invalidate();
  request();
}
function progressiveSeek() {
  if (!scrubbing.value) {
    scrubbing.value = true;
    invalidate();
  }
  // Keep one running frame and one latest request. Cancelling on every pointer
  // update would starve any HTML/native text render longer than an input event.
  clearTimeout(seekSettleTimer);
  if (heldSeekPointer === undefined)
    seekSettleTimer = setTimeout(settleSeek, Math.max(180, (report.value?.frameMs || 0) * 2));
}
function holdSeek(e: PointerEvent) {
  if (e.isPrimary && e.button === 0) {
    heldSeekPointer = e.pointerId;
    clearTimeout(seekSettleTimer);
  }
}
function releaseSeek(e: PointerEvent) {
  if (heldSeekPointer !== undefined && e.pointerId !== heldSeekPointer) return;
  heldSeekPointer = undefined;
  // Timeline/range handlers publish their pointer-up position synchronously;
  // settle after Vue applies that final prop so its exact time is requested.
  clearTimeout(seekSettleTimer);
  seekSettleTimer = setTimeout(settleSeek, 0);
}
function blurSeek() {
  heldSeekPointer = undefined;
  settleSeek();
}
function send() {
  if (!worker || !stage.value || disposed) return;
  running = true;
  pending = false;
  const rect = stage.value.getBoundingClientRect();
  const size =
    props.quality === 'full'
      ? { ...props.project.canvas }
      : props.quality === 'half'
        ? {
            width: Math.max(2, Math.round(props.project.canvas.width / 2)),
            height: Math.max(2, Math.round(props.project.canvas.height / 2))
          }
        : previewExtent(props.project.canvas, rect.width, rect.height, devicePixelRatio || 1);
  // Initial/resync snapshots cross postMessage's structured-clone boundary once.
  // Immutable edits carry stable-ID deltas; pointer motion carries only position and scale.
  let update: Record<string, unknown> = { documentRevision };
  if (!lastSentProject) update = { project: latestProject, documentRevision: ++documentRevision };
  else if (lastSentProject !== latestProject) {
    const patches = documentPatches(lastSentProject, latestProject);
    update = { patches, baseRevision: documentRevision, documentRevision: ++documentRevision };
  }
  const refreshUrls = lastAssets !== latestProject.assets || lastMediaUrl !== props.mediaUrl;
  if (refreshUrls) {
    mediaUrls = Object.fromEntries(latestProject.assets.map((a) => [a.id, assetMediaUrl(a)]));
    lastAssets = latestProject.assets;
    lastMediaUrl = props.mediaUrl;
  }
  worker.postMessage({
    type: 'render',
    id: ++sequence,
    generation,
    ...update,
    presentation: moving.value ? { ...moving.value } : undefined,
    time: previewSampleTime(props.time, props.project.frameRate, props.playing),
    prefetch: props.playing || !scrubbing.value,
    ...size,
    urls: refreshUrls ? mediaUrls : undefined
  });
  lastSentProject = latestProject;
}
watch(() => props.project, snapshot, { immediate: true });
watch(
  () => props.quality,
  () => {
    invalidate();
    request();
  }
);
watch(
  () => [props.time, props.playing],
  (values, previous) => {
    if (committing && previous && values[0] !== previous[0]) {
      moving.value = null;
      committing = false;
      invalidate();
    }
    if (previous && values[0] !== previous[0] && !props.playing) progressiveSeek();
    else if (previous && props.playing && Math.abs(Number(values[0]) - Number(previous[0])) > 36000)
      invalidate();
    request();
    if (props.playing && Math.abs(audio.currentTimeTicks() - props.time) > 36000)
      void audio.seek(props.time).catch(reportAudioFailure);
  }
);
watch(
  () => props.playing,
  (playing) => {
    if (playing) void audio.play(props.project, props.time).catch(reportAudioFailure);
    else {
      audio.stop();
      invalidate();
      request();
    }
    if (playing) settleSeek();
  }
);
watch(
  () => props.project,
  () => {
    if (props.playing) void audio.play(props.project, props.time).catch(reportAudioFailure);
  }
);
onMounted(() => {
  if (!canvas.value || !('transferControlToOffscreen' in canvas.value)) {
    status.value = '此浏览器不支持独立画布工作线程，请使用新版 Chrome、Edge 或 Safari。';
    emit('error', status.value);
    return;
  }
  worker = new Worker(new URL('../../packages/render/worker.ts', import.meta.url), {
    type: 'module'
  });
  worker.onmessage = ({ data }) => {
    if (data.type === 'ready') {
      ready = true;
      request();
    } else if (data.type === 'frame') {
      if (data.generation === generation) {
        report.value = data.report;
        presentedProject.value = lastSentProject;
        status.value = '';
      }
      running = false;
      if (pending) send();
    } else if (data.type === 'resync') {
      running = false;
      lastSentProject = undefined;
      lastAssets = undefined;
      invalidate();
      request();
    } else if (data.type === 'cancelled') {
      running = false;
      if (pending) send();
    } else if (data.type === 'error') {
      running = false;
      if (data.generation === undefined || data.generation === generation) {
        status.value = data.error;
        emit('error', data.error);
      }
      if (pending) send();
    }
  };
  worker.onerror = (e) => {
    status.value = e.message || '画布工作线程失败';
    running = false;
    emit('error', status.value);
  };
  const offscreen = canvas.value.transferControlToOffscreen();
  worker.postMessage(
    {
      type: 'init',
      canvas: offscreen,
      urls: {},
      options: { textWorkers: props.textWorkers !== false }
    },
    [offscreen]
  );
  observer = new ResizeObserver(request);
  observer.observe(stage.value!);
  window.addEventListener('pointerdown', holdSeek, true);
  window.addEventListener('blur', blurSeek);
  window.addEventListener('pointerup', releaseSeek);
  window.addEventListener('pointercancel', releaseSeek);
});
const hitBounds = computed(() => report.value?.bounds || []);
function hitStyle(b: { x: number; y: number; width: number; height: number }) {
  return {
    left: `${(100 * b.x) / props.project.canvas.width}%`,
    top: `${(100 * b.y) / props.project.canvas.height}%`,
    width: `${(100 * b.width) / props.project.canvas.width}%`,
    height: `${(100 * b.height) / props.project.canvas.height}%`
  };
}
function widthHandleStyle(b: RenderReport['bounds'][number], edge: TextWidthEdge) {
  const p = b.textLayout![edge];
  const axis = b.textLayout!;
  const rotation = Math.atan2(axis.right.y - axis.left.y, axis.right.x - axis.left.x);
  return {
    ...handlePointStyle(b, p, 14),
    transform: `translate(-50%, -50%) rotate(${rotation}rad)`
  };
}
function handlePointStyle(
  b: RenderReport['bounds'][number],
  point: { x: number; y: number },
  inset = 9
) {
  const display = stage.value?.getBoundingClientRect();
  const marginX =
    (inset * props.project.canvas.width) / (display?.width || props.project.canvas.width);
  const marginY =
    (inset * props.project.canvas.height) / (display?.height || props.project.canvas.height);
  const x = Math.max(marginX, Math.min(props.project.canvas.width - marginX, point.x));
  const y = Math.max(marginY, Math.min(props.project.canvas.height - marginY, point.y));
  return {
    left: `${(100 * (x - b.x)) / b.width}%`,
    top: `${(100 * (y - b.y)) / b.height}%`,
    right: 'auto',
    bottom: 'auto',
    transform: 'translate(-50%, -50%)'
  };
}
function cornerHandleStyle(b: RenderReport['bounds'][number], corner: ResizeCorner) {
  return handlePointStyle(b, {
    x: b.x + (corner.endsWith('r') ? b.width : 0),
    y: b.y + (corner.startsWith('b') ? b.height : 0)
  });
}
function drag(
  e: PointerEvent,
  id: string,
  locked: boolean,
  corner?: ResizeCorner,
  edge?: TextWidthEdge
) {
  if (e.button !== 0 || (e.altKey && !edge)) return;
  e.stopPropagation();
  emit('select', id);
  if (props.disabled || locked || committing || !stage.value) return;
  const item = props.project.timeline.tracks.flatMap((t) => t.items).find((i) => i.id === id);
  if (!item) return;
  const presented = presentedProject.value;
  const presentedItem = presented?.timeline.tracks.flatMap((t) => t.items).find((i) => i.id === id);
  // A rename can reuse the same geometry immediately. A changed clip, canvas,
  // media source or clock must wait for its own presented bounds.
  if (
    !frameCurrent.value &&
    !(
      presentedItem === toRaw(item) &&
      presented?.assets === toRaw(props.project.assets) &&
      presented?.canvas.width === props.project.canvas.width &&
      presented?.canvas.height === props.project.canvas.height &&
      report.value?.time === previewSampleTime(props.time, props.project.frameRate, props.playing)
    )
  )
    return;
  const evaluated = evaluateVisual(item, props.time);
  const bounds = hitBounds.value.find((b) => b.id === id);
  if (!bounds) return;
  cleanup();
  const rect = stage.value.getBoundingClientRect(),
    x = e.clientX,
    y = e.clientY;
  const asset = props.project.assets.find((entry) => entry.id === item.clip.assetId);
  const geometry = layerGeometry(
    props.project,
    evaluated,
    item.clip.html?.width || asset?.width || props.project.canvas.width,
    item.clip.html?.height || asset?.height || props.project.canvas.height,
    props.project.canvas.width,
    props.project.canvas.height,
    !!item.clip.text
  );
  const [a, b, c, d, tx, ty] = geometry.matrix;
  const pivot = {
    x: tx + a * (evaluated.anchorX ?? 0.5) + c * (evaluated.anchorY ?? 0.5),
    y: ty + b * (evaluated.anchorX ?? 0.5) + d * (evaluated.anchorY ?? 0.5)
  };
  const resize = corner ? cornerResize(bounds, evaluated, pivot, corner) : undefined;
  const reflow =
    edge && bounds.textLayout ? textWidthResize(bounds.textLayout, evaluated, edge) : undefined;
  const snapX = previewAxisSnap(
    bounds.x,
    bounds.width,
    evaluated.positionX,
    props.project.canvas.width,
    evaluated.rotationDegrees
  );
  const snapY = previewAxisSnap(
    bounds.y,
    bounds.height,
    evaluated.positionY,
    props.project.canvas.height,
    evaluated.rotationDegrees
  );
  let lastPointer: PointerEvent | undefined;
  const move = (event: PointerEvent, bypassSnap = event.ctrlKey, fromCenter = event.altKey) => {
    if (event.pointerId !== e.pointerId) return;
    lastPointer = event;
    if (reflow) {
      const changes = reflow(
        ((event.clientX - x) * props.project.canvas.width) / rect.width,
        ((event.clientY - y) * props.project.canvas.height) / rect.height,
        fromCenter
      );
      moving.value = {
        id,
        x: changes.positionX,
        y: changes.positionY,
        layoutWidth: changes.layoutWidth
      };
      request();
      return;
    }
    if (resize) {
      const changes = resize(
        ((event.clientX - x) * props.project.canvas.width) / rect.width,
        ((event.clientY - y) * props.project.canvas.height) / rect.height
      );
      moving.value = {
        id,
        x: changes.positionX,
        y: changes.positionY,
        scaleX: changes.scaleX,
        scaleY: changes.scaleY
      };
      request();
      return;
    }
    const enabled = props.snap !== false && !bypassSnap;
    const horizontal = snapX.snap(
      evaluated.positionX + ((event.clientX - x) * props.project.canvas.width) / rect.width,
      rect.width / props.project.canvas.width,
      enabled
    );
    const vertical = snapY.snap(
      evaluated.positionY + ((event.clientY - y) * props.project.canvas.height) / rect.height,
      rect.height / props.project.canvas.height,
      enabled
    );
    moving.value = {
      id,
      x: horizontal.value,
      y: vertical.value
    };
    snapGuides.value = { x: horizontal.guide, y: vertical.guide };
    request();
  };
  const end = (event: PointerEvent) => {
    if (event.pointerId !== e.pointerId) return;
    if (moving.value) move(event);
    const m = moving.value;
    cleanup();
    if (!m) return;
    if (
      m.x === evaluated.positionX &&
      m.y === evaluated.positionY &&
      (m.scaleX === undefined || m.scaleX === evaluated.scaleX) &&
      (m.scaleY === undefined || m.scaleY === evaluated.scaleY) &&
      (m.layoutWidth === undefined || m.layoutWidth === bounds.textLayout?.width)
    ) {
      cancel();
      return;
    }
    committing = true;
    invalidate();
    // Keep the final presentation until the authored snapshot arrives or the
    // command fails. Rendering the old document on release causes a visible jump.
    emit(
      'transform',
      id,
      {
        positionX: m.x,
        positionY: m.y,
        ...(m.scaleX === undefined ? {} : { scaleX: m.scaleX }),
        ...(m.scaleY === undefined ? {} : { scaleY: m.scaleY }),
        ...(m.layoutWidth === undefined ? {} : { layoutWidth: m.layoutWidth })
      },
      () => {
        if (moving.value !== m) return;
        committing = false;
        moving.value = null;
        invalidate();
        request();
      }
    );
    request();
  };
  const cancel = () => {
    cleanup();
    committing = false;
    moving.value = null;
    invalidate();
    request();
  };
  const pointerCancel = (event: PointerEvent) => {
    if (event.pointerId === e.pointerId) cancel();
  };
  const key = (event: KeyboardEvent) => {
    if (event.key === 'Escape') {
      event.preventDefault();
      cancel();
    } else if (event.key === 'Alt' && lastPointer) {
      move(lastPointer, lastPointer.ctrlKey, event.type === 'keydown');
    } else if (event.key === 'Control' && lastPointer) {
      move(lastPointer, event.type === 'keydown');
    }
  };
  window.addEventListener('pointermove', move);
  window.addEventListener('pointerup', end);
  window.addEventListener('pointercancel', pointerCancel);
  window.addEventListener('blur', cancel);
  window.addEventListener('keydown', key);
  window.addEventListener('keyup', key);
  cleanup = () => {
    window.removeEventListener('pointermove', move);
    window.removeEventListener('pointerup', end);
    window.removeEventListener('pointercancel', pointerCancel);
    window.removeEventListener('blur', cancel);
    window.removeEventListener('keydown', key);
    window.removeEventListener('keyup', key);
    snapGuides.value = {};
  };
  e.preventDefault();
}
defineExpose({
  mediaStatus: () =>
    (report.value?.media || []).map((m) => ({
      ...m,
      paused: !props.playing,
      seeking: running,
      ended: false
    })),
  renderStatus: () => report.value,
  clockTime: () => audio.currentTimeTicks()
});
onBeforeUnmount(() => {
  disposed = true;
  clearTimeout(seekSettleTimer);
  cleanup();
  window.removeEventListener('pointerdown', holdSeek, true);
  window.removeEventListener('blur', blurSeek);
  window.removeEventListener('pointerup', releaseSeek);
  window.removeEventListener('pointercancel', releaseSeek);
  observer?.disconnect();
  audio.dispose();
  worker?.postMessage({ type: 'dispose' });
  worker?.terminate();
});
</script>
<template>
  <div
    ref="stage"
    class="preview-stage"
    :style="{
      aspectRatio: `${project.canvas.width}/${project.canvas.height}`,
      '--preview-ratio': project.canvas.width / project.canvas.height
    }"
    :aria-label="tr('作品实时预览')"
    :data-render-backend="report?.backend"
    :data-render-time="report?.time"
    :data-frame-ms="report?.frameMs.toFixed(1)"
    :data-render-width="report?.width"
    :data-render-height="report?.height"
    data-preview-quality-scale="1"
    :data-preview-scrubbing="scrubbing"
    :data-preview-current="frameCurrent"
  >
    <canvas ref="canvas" class="unified-preview-canvas" :aria-label="tr('作品合成画面')" />
    <div
      v-for="b in hitBounds"
      :key="b.id"
      class="unified-preview-hit"
      :class="{
        selected: selected === b.id,
        snapped: selected === b.id && (snapGuides.x !== undefined || snapGuides.y !== undefined)
      }"
      :style="hitStyle(b)"
      @pointerdown="drag($event, b.id, b.locked)"
    >
      <template v-if="selected === b.id && !playing"
        ><button
          v-for="(label, corner) in corners"
          :key="corner"
          type="button"
          :class="['selection-handle', corner]"
          :style="cornerHandleStyle(b, corner)"
          :aria-label="tr('拖动{corner}角缩放', { corner: tr(label) })"
          :title="tr('拖动{corner}角缩放', { corner: tr(label) })"
          @pointerdown.stop="drag($event, b.id, b.locked, corner)"
        />
        <template v-if="b.textLayout">
          <button
            v-for="(label, edge) in widthEdges"
            :key="edge"
            type="button"
            :class="['text-width-handle', edge]"
            :style="widthHandleStyle(b, edge)"
            :aria-label="tr('拖动{edge}边调节文字框宽度', { edge: tr(label) })"
            :title="tr('拖动{edge}边调节文字框宽度', { edge: tr(label) })"
            @pointerdown.stop="drag($event, b.id, b.locked, undefined, edge)"
          />
        </template>
      </template>
    </div>
    <div
      v-if="snapGuides.x !== undefined"
      class="preview-snap-guide vertical"
      :data-snap-x="snapGuides.x"
      :style="{ left: `min(${(100 * snapGuides.x) / project.canvas.width}%, calc(100% - 1px))` }"
    />
    <div
      v-if="snapGuides.y !== undefined"
      class="preview-snap-guide horizontal"
      :data-snap-y="snapGuides.y"
      :style="{ top: `min(${(100 * snapGuides.y) / project.canvas.height}%, calc(100% - 1px))` }"
    />
    <div v-if="status" class="template-status">{{ tr(status) }}</div>
    <div v-else-if="!project.timeline.tracks.length" class="preview-empty">
      <Icon name="monitor" :size="38" />
      <p>{{ tr('预览画面') }}</p>
      <small>{{ tr('添加素材或文字，开始剪辑') }}</small>
    </div>
  </div>
</template>
<style scoped>
.unified-preview-canvas {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  display: block;
}
.unified-preview-hit {
  position: absolute;
  cursor: move;
  box-sizing: border-box;
}
.unified-preview-hit.selected {
  z-index: 2;
  outline: 1px solid var(--accent, #5ad2c1);
}
.unified-preview-hit.snapped {
  outline-color: #ffcd59;
}
.selection-handle {
  width: 18px;
  height: 18px;
  min-height: 0;
  padding: 0;
  border: 0;
  border-radius: 0;
  background: transparent;
  pointer-events: auto;
  touch-action: none;
  z-index: 1;
}
.selection-handle::after {
  content: '';
  position: absolute;
  inset: 5px;
  border: 1px solid #aaa;
  background: #fff;
  border-radius: 50%;
}
.selection-handle.tl {
  top: -9px;
  left: -9px;
  cursor: nwse-resize;
}
.selection-handle.tr {
  top: -9px;
  right: -9px;
  cursor: nesw-resize;
}
.selection-handle.bl {
  bottom: -9px;
  left: -9px;
  cursor: nesw-resize;
}
.selection-handle.br {
  bottom: -9px;
  right: -9px;
  cursor: nwse-resize;
}
.text-width-handle {
  position: absolute;
  width: 20px;
  height: 28px;
  min-height: 0;
  padding: 0;
  border: 0;
  background: transparent;
  cursor: ew-resize;
  touch-action: none;
  z-index: 2;
}
.text-width-handle::after {
  content: '';
  position: absolute;
  inset: 6px 8px;
  border: 1px solid #777;
  border-radius: 3px;
  background: #fff;
}
.preview-snap-guide {
  position: absolute;
  z-index: 3;
  background: #ffcd59;
  box-shadow: 0 0 0 1px #0003;
  pointer-events: none;
}
.preview-snap-guide.vertical {
  top: 0;
  bottom: 0;
  width: 1px;
}
.preview-snap-guide.horizontal {
  left: 0;
  right: 0;
  height: 1px;
}
.template-status {
  pointer-events: none;
}
</style>
