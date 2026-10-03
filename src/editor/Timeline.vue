<script setup lang="ts">
import { useI18n } from './i18n';
const { tr } = useI18n();
import Icon from './Icon.vue';
import ClipVisual from './ClipVisual.vue';
import { MagneticSnap, type SnapTarget } from './snapping';
import { computed, ref, onMounted, watch, onBeforeUnmount } from 'vue';
import {
  duration,
  quantizeTime,
  selectionClosure,
  seconds,
  ticks,
  type Project,
  type Item,
  type Track,
  type EditorCommand
} from '../../packages/core/project.mjs';
const props = defineProps<{
  project: Project;
  time: number;
  selected: string;
  selection: string[];
  zoom: number;
  snap: boolean;
  busy: boolean;
  mediaUrl: (id: string) => string;
}>();
const emit = defineEmits<{
  draft: [commands: EditorCommand[]];
  cancel: [];
  seek: [value: number];
  select: [id: string, additive?: boolean];
  commit: [settle: () => void];
  track: [id: string, key: 'visible' | 'muted' | 'locked' | 'syncLocked'];
  add: [assetId: string, trackId: string, start: number];
}>();
const width = computed(() => Math.max(900, (seconds(duration(props.project)) + 10) * props.zoom));
const interval = computed(() => (props.zoom < 30 ? 5 : props.zoom < 65 ? 2 : 1));
const viewport = ref<HTMLDivElement>();
const view = ref({ left: 0, top: 0, width: 1000, height: 400 });
function updateViewport() {
  const el = viewport.value;
  if (el)
    view.value = {
      left: el.scrollLeft,
      top: el.scrollTop,
      width: el.clientWidth,
      height: el.clientHeight
    };
}
const startSeconds = computed(() => Math.max(0, (view.value.left - 180) / props.zoom));
const endSeconds = computed(() => (view.value.left + view.value.width + 180) / props.zoom);
const marks = computed(() => {
  const first = Math.floor(startSeconds.value / interval.value),
    last = Math.ceil(endSeconds.value / interval.value);
  return Array.from({ length: last - first + 1 }, (_, i) => (i + first) * interval.value);
});
const trackHeaderWidth = 136;
const rowHeight = (track: Track) => (track.type === 'audio' ? 40 : track.type === 'text' ? 28 : 56);
const rows = computed(() => {
  let y = 0;
  return props.project.timeline.tracks.map((track, index) => {
    const row = { track, index, top: y, height: rowHeight(track) };
    y += row.height + 4;
    return row;
  });
});
const totalHeight = computed(() => rows.value.reduce((sum, r) => sum + r.height + 4, 0));
const visibleRows = computed(() =>
  rows.value.filter(
    (r) =>
      r.top + r.height >= view.value.top - 250 && r.top < view.value.top + view.value.height + 120
  )
);
function visibleItems(track: Track) {
  return track.items.filter(
    (i) =>
      i.placement.end >= ticks(startSeconds.value) && i.placement.begin <= ticks(endSeconds.value)
  );
}
let resize: ResizeObserver | undefined;
onMounted(() => {
  resize = new ResizeObserver(updateViewport);
  if (viewport.value) resize.observe(viewport.value);
  updateViewport();
});
const ghost = ref<{ id: string; begin: number; end: number; delta?: number } | null>(null);
const movingIds = ref<string[]>([]);
const snapGuide = ref<number | null>(null);
const seeking = ref(false);
const edgeTargets = computed<SnapTarget[]>(() => [
  { value: 0 },
  ...props.project.timeline.tracks.flatMap((track) =>
    track.items.flatMap((item) => [{ value: item.placement.begin }, { value: item.placement.end }])
  )
]);
const frame = computed(
  () => (120000 * props.project.frameRate.denominator) / props.project.frameRate.numerator
);
let cleanup = () => {};
const quantize = (value: number) => Math.max(0, quantizeTime(value, props.project.frameRate));
function label(s: number) {
  return `${Math.floor(s / 60)
    .toString()
    .padStart(2, '0')}:${Math.floor(s % 60)
    .toString()
    .padStart(2, '0')}`;
}
function geometry(item: Item) {
  const range =
    ghost.value?.id === item.id
      ? ghost.value
      : ghost.value?.delta !== undefined && movingIds.value.includes(item.id)
        ? {
            begin: item.placement.begin + ghost.value.delta,
            end: item.placement.end + ghost.value.delta
          }
        : item.placement;
  return {
    left: `${seconds(range.begin) * props.zoom}px`,
    width: `${Math.max(4, seconds(range.end - range.begin) * props.zoom)}px`
  };
}
function listen(
  event: PointerEvent,
  move: (e: PointerEvent, bypassSnap?: boolean) => void,
  end: (e: PointerEvent) => void
) {
  cleanup();
  let lastPointer = event;
  const motion = (e: PointerEvent) => {
    if (e.pointerId === event.pointerId) {
      lastPointer = e;
      move(e);
    }
  };
  const done = (e: PointerEvent) => {
    if (e.pointerId !== event.pointerId) return;
    end(e);
    cleanup();
  };
  const cancel = () => {
    cleanup();
    ghost.value = null;
    movingIds.value = [];
    emit('cancel');
  };
  const pointerCancel = (e: PointerEvent) => {
    if (e.pointerId === event.pointerId) cancel();
  };
  const key = (e: KeyboardEvent) => {
    if (e.key === 'Escape') {
      e.preventDefault();
      cancel();
    } else if (e.key === 'Control' && lastPointer) {
      move(lastPointer, e.type === 'keydown');
    }
  };
  window.addEventListener('keydown', key);
  window.addEventListener('keyup', key);
  window.addEventListener('pointermove', motion);
  window.addEventListener('pointerup', done);
  window.addEventListener('pointercancel', pointerCancel);
  window.addEventListener('blur', cancel);
  cleanup = () => {
    window.removeEventListener('keydown', key);
    window.removeEventListener('keyup', key);
    window.removeEventListener('pointermove', motion);
    window.removeEventListener('pointerup', done);
    window.removeEventListener('pointercancel', pointerCancel);
    window.removeEventListener('blur', cancel);
    snapGuide.value = null;
    seeking.value = false;
  };
  event.preventDefault();
}
function seek(event: PointerEvent) {
  if (event.button !== 0 || !viewport.value) return;
  viewport.value.focus({ preventScroll: true });
  // Re-read scrollLeft during motion; a frozen DOM rect drifts when scrolling.
  const snapper = new MagneticSnap([{ offset: 0, targets: edgeTargets.value }]);
  const update = (e: PointerEvent, bypassSnap = e.ctrlKey) => {
    const el = viewport.value!;
    const raw = ticks(
      (e.clientX - el.getBoundingClientRect().left + el.scrollLeft - trackHeaderWidth) / props.zoom
    );
    const result = snapper.snap(
      raw,
      props.zoom / 120000,
      props.snap && !bypassSnap,
      0,
      Math.max(duration(props.project), ticks(10))
    );
    seeking.value = true;
    snapGuide.value = result.guide ?? null;
    emit('seek', result.guide === undefined ? quantize(result.value) : result.value);
  };
  listen(event, update, update);
  update(event);
}
function drag(event: PointerEvent, item: Item, track: Track, mode: 'move' | 'left' | 'right') {
  // Dragging prevents the browser's default focus change, so take focus explicitly.
  if (event.button === 0) viewport.value?.focus({ preventScroll: true });
  const additive = event.metaKey || event.ctrlKey || event.shiftKey;
  emit('select', item.id, additive);
  if (additive) return;
  if (track.locked || props.busy || event.button !== 0) return;
  const x = event.clientX,
    begin = item.placement.begin,
    end = item.placement.end;
  const rate = (item.clip.retime?.constantRatePpm ?? 1000000) / 1000000;
  const ids = selectionClosure(
    props.project,
    props.selection.includes(item.id) ? props.selection : [item.id]
  );
  movingIds.value = ids;
  const minimum = Math.min(
    ...props.project.timeline.tracks.flatMap((t) =>
      t.items.filter((i) => ids.includes(i.id)).map((i) => i.placement.begin)
    )
  );
  const anchors: SnapTarget[] = [
    { value: props.time, multiplier: 1.25 },
    { value: 0 },
    ...props.project.timeline.tracks.flatMap((t) =>
      t.items
        .filter((i) => !ids.includes(i.id))
        .flatMap((i) => [{ value: i.placement.begin }, { value: i.placement.end }])
    )
  ];
  const offsets =
    mode === 'move'
      ? props.project.timeline.tracks.flatMap((t) =>
          t.items
            .filter((i) => ids.includes(i.id))
            .flatMap((i) => [i.placement.begin - begin, i.placement.end - begin])
        )
      : [0];
  const snapper = new MagneticSnap(
    [...new Set(offsets)].map((offset) => ({ offset, targets: anchors }))
  );
  const scrollStart = viewport.value?.scrollLeft ?? 0;
  const minimumEdge =
    mode === 'move'
      ? begin - minimum
      : mode === 'left'
        ? item.clip.type === 'text'
          ? 0
          : Math.max(0, begin - item.clip.source.begin / rate)
        : begin + frame.value;
  let maximumEdge = mode === 'left' ? end - frame.value : Infinity;
  if (mode === 'right') {
    const asset = props.project.assets.find((a) => a.id === item.clip.assetId);
    if (asset && asset.kind !== 'image')
      maximumEdge = Math.min(maximumEdge, end + (asset.duration - item.clip.source.end) / rate);
    if (item.clip.html)
      maximumEdge = Math.min(
        maximumEdge,
        end + (item.clip.html.duration - item.clip.source.end) / rate
      );
  }
  let moved = false;
  const update = (e: PointerEvent, bypassSnap = e.ctrlKey) => {
    if (e.pointerId !== event.pointerId) return;
    const delta = ticks(
      (e.clientX - x + (viewport.value?.scrollLeft ?? 0) - scrollStart) / props.zoom
    );
    if (Math.abs(e.clientX - x) > 2) moved = true;
    if (!moved) return;
    const result = snapper.snap(
      (mode === 'right' ? end : begin) + delta,
      props.zoom / 120000,
      props.snap && !bypassSnap,
      minimumEdge,
      maximumEdge
    );
    const edge =
      result.guide === undefined
        ? Math.max(minimumEdge, Math.min(maximumEdge, quantize(result.value)))
        : result.value;
    const a = mode === 'right' ? begin : edge;
    const b = mode === 'move' ? a + end - begin : mode === 'left' ? end : edge;
    snapGuide.value = result.guide ?? null;
    ghost.value = {
      id: item.id,
      begin: Math.round(a),
      end: Math.round(b),
      ...(mode === 'move' ? { delta: Math.round(a) - begin } : {})
    };
    const targetTrack = document
      .elementFromPoint(e.clientX, e.clientY)
      ?.closest<HTMLElement>('[data-track]')?.dataset.track;
    emit(
      'draft',
      mode === 'move'
        ? ids.length === 1
          ? [
              {
                action: 'move_clip',
                itemId: item.id,
                startSeconds: seconds(Math.round(a)),
                trackId: targetTrack
              }
            ]
          : [{ action: 'move_clips', itemIds: ids, deltaSeconds: seconds(Math.round(a) - begin) }]
        : [
            {
              action: 'trim_range',
              itemId: item.id,
              beginSeconds: seconds(Math.round(a)),
              endSeconds: seconds(Math.round(b))
            }
          ]
    );
  };
  listen(event, update, (e) => {
    update(e);
    const g = ghost.value;
    if (!g || !moved) {
      ghost.value = null;
      movingIds.value = [];
      emit('cancel');
    } else {
      // Hold the released geometry until the authored snapshot arrives or the
      // command settles without a change. Clearing it here flashes the old range.
      emit('commit', () => {
        if (ghost.value !== g) return;
        ghost.value = null;
        movingIds.value = [];
      });
    }
  });
}
function drop(event: DragEvent, track: Track) {
  const id = event.dataTransfer?.getData('application/x-videocut-asset');
  if (!id) return;
  const rect = (event.currentTarget as HTMLElement).getBoundingClientRect();
  const snapper = new MagneticSnap([
    { offset: 0, targets: [{ value: props.time, multiplier: 1.25 }, ...edgeTargets.value] }
  ]);
  const result = snapper.snap(
    ticks((event.clientX - rect.left) / props.zoom),
    props.zoom / 120000,
    props.snap && !event.ctrlKey,
    0
  );
  emit(
    'add',
    id,
    track.id,
    Math.round(result.guide === undefined ? quantize(result.value) : result.value)
  );
}
watch(
  () => props.project,
  () => {
    cleanup();
    ghost.value = null;
    movingIds.value = [];
    emit('cancel');
  }
);
onBeforeUnmount(() => {
  cleanup();
  resize?.disconnect();
});
</script>

<template>
  <div
    ref="viewport"
    class="timeline-scroll"
    tabindex="0"
    role="region"
    :aria-label="tr('时间轴滚动区域')"
    @scroll.passive="updateViewport"
  >
    <div
      class="timeline-content"
      :style="{
        minWidth: `${width + trackHeaderWidth}px`,
        '--track-header-width': `${trackHeaderWidth}px`
      }"
    >
      <div class="ruler-row">
        <div class="track-label ruler-label">
          <Icon name="list" :size="16" /><span>{{
            tr('{count} 轨道', { count: project.timeline.tracks.length })
          }}</span>
        </div>
        <div
          class="ruler"
          :style="{ width: `${width}px`, backgroundSize: `${(zoom * interval) / 5}px 7px` }"
          @pointerdown="seek"
        >
          <span v-for="mark in marks" :key="mark" :style="{ left: `${mark * zoom}px` }">{{
            label(mark)
          }}</span>
          <div
            class="ruler-playhead"
            :class="{ snapped: seeking && snapGuide !== null }"
            :style="{ left: `${seconds(time) * zoom}px` }"
          >
            <span />
          </div>
        </div>
      </div>
      <div class="timeline-top-space" @pointerdown.self="emit('select', '')" />
      <div :style="{ height: `${totalHeight}px`, position: 'relative' }">
        <div
          v-for="{ track, index, top, height } in visibleRows"
          :key="track.id"
          class="track-row"
          :style="{ position: 'absolute', top: `${top}px`, height: `${height}px` }"
          :class="[track.type + '-track', { locked: track.locked, alternate: index % 2 === 1 }]"
          :data-track="track.id"
        >
          <div class="track-label">
            <span class="track-badge" :title="track.name"
              >{{ track.type === 'audio' ? 'A' : track.type === 'text' ? 'T' : 'V'
              }}{{ index + 1 }}</span
            >
            <div class="track-actions">
              <button
                :class="{ active: track.syncLocked }"
                :title="tr(track.syncLocked ? '取消波纹同步锁定' : '参与同步轨道波纹编辑')"
                @click="emit('track', track.id, 'syncLocked')"
              >
                ⇄
              </button>
              <button
                :class="{ active: track.locked }"
                :title="tr(track.locked ? '解锁轨道' : '锁定轨道')"
                @click="emit('track', track.id, 'locked')"
              >
                <Icon :name="track.locked ? 'lock' : 'unlock'" :size="12" /></button
              ><button
                :class="{ active: !track.visible }"
                :title="tr(track.visible ? '隐藏轨道' : '显示轨道')"
                @click="emit('track', track.id, 'visible')"
              >
                <Icon :name="track.visible ? 'eye' : 'eyeOff'" :size="14" /></button
              ><button
                v-if="track.type !== 'text'"
                :class="{ active: track.muted }"
                :title="tr(track.muted ? '取消静音' : '静音轨道')"
                @click="emit('track', track.id, 'muted')"
              >
                <Icon :name="track.muted ? 'mute' : 'volume'" :size="13" />
              </button>
            </div>
          </div>
          <div
            class="track-lane"
            :style="{ width: `${width}px` }"
            @dragover.prevent
            @drop.prevent="drop($event, track)"
            @pointerdown.self="seek"
          >
            <div
              v-for="item in visibleItems(track)"
              :key="item.id"
              class="timeline-clip"
              :class="[
                item.clip.type,
                { selected: selection.includes(item.id), disabled: !item.enabled }
              ]"
              :style="geometry(item)"
              @pointerdown.stop="drag($event, item, track, 'move')"
              :title="
                tr(
                  `${item.name} · ${seconds(item.placement.end - item.placement.begin).toFixed(2)} 秒`
                )
              "
            >
              <span
                class="trim-handle left"
                @pointerdown.stop="drag($event, item, track, 'left')"
              /><span class="clip-title"
                ><Icon
                  v-if="['text', 'audio', 'html-clip'].includes(item.clip.type)"
                  :name="
                    item.clip.type === 'text'
                      ? 'text'
                      : item.clip.type === 'html-clip'
                        ? 'layers'
                        : 'music'
                  "
                  :size="12"
                /><span>{{ item.name }}</span
                ><small v-if="item.clip.type !== 'text'"
                  >{{ seconds(item.placement.end - item.placement.begin).toFixed(2) }}s</small
                ></span
              >
              <ClipVisual
                v-if="!['text', 'html-clip'].includes(item.clip.type)"
                :project="project"
                :item="item"
                :media-url="mediaUrl"
                :zoom="zoom"
                :viewport-start-seconds="startSeconds"
                :viewport-end-seconds="endSeconds"
                :height="height - 20"
              />
              <div
                v-else-if="item.clip.html"
                class="html-clip-body"
                :class="{ transparent: item.clip.html.transparent }"
              />
              <span
                class="trim-handle right"
                @pointerdown.stop="drag($event, item, track, 'right')"
              />
            </div>
          </div>
        </div>
      </div>
      <div v-if="!project.timeline.tracks.length" class="empty-timeline">
        <Icon name="layers" :size="30" />
        <p>{{ tr('将素材添加到时间轴') }}</p>
        <small>{{ tr('从左侧导入素材，或添加文字与 HTML 动画') }}</small>
      </div>
      <div
        v-if="snapGuide !== null"
        class="timeline-snap-guide"
        :data-snap-time="snapGuide"
        :style="{ left: `${trackHeaderWidth + seconds(snapGuide) * zoom}px` }"
      />
      <div
        class="playhead"
        :class="{ snapped: seeking && snapGuide !== null }"
        :style="{ left: `${trackHeaderWidth + seconds(time) * zoom}px` }"
      />
    </div>
  </div>
</template>
