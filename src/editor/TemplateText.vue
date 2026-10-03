<script setup lang="ts">
import { useI18n } from './i18n';
const { tr } = useI18n();
import { ref, watch, onBeforeUnmount } from 'vue';
import { createTemplatePlayer, type TemplatePlayer } from './text-renderer';
const props = defineProps<{
  templateId: string;
  text: string;
  timeUs: number;
  width: number;
  height: number;
  playing: boolean;
  selected: boolean;
}>();
const emit = defineEmits<{ error: [message: string]; drag: [event: PointerEvent] }>();
const host = ref<HTMLElement>(),
  status = ref('正在加载文字模板…'),
  frameMs = ref(0),
  renderedTimeUs = ref(-1),
  renderedFrames = ref(0);
const bounds = ref({ left: '25%', top: '25%', width: '50%', height: '50%' });
let failed = '',
  disposed = false,
  pending = false,
  running = false,
  player: TemplatePlayer | undefined,
  key = '',
  text = '';
function request() {
  pending = true;
  if (!running) void draw();
}
async function draw() {
  if (!host.value || disposed) return;
  running = true;
  try {
    while (pending && !disposed) {
      pending = false;
      const state = { ...props };
      const attempt = `${state.templateId}:${state.width}:${state.height}:${state.text}`;
      if (failed === attempt) continue;
      const nextKey = `${state.templateId}:${state.width}:${state.height}`;
      if (nextKey !== key) {
        player?.dispose();
        player = undefined;
        key = '';
        status.value = '正在加载文字模板…';
        const canvas = document.createElement('canvas');
        canvas.setAttribute('aria-label', '文字模板渲染画面');
        host.value.replaceChildren(canvas);
        const rect = host.value.getBoundingClientRect();
        const dpr = globalThis.devicePixelRatio || 1;
        const scale = Math.min(
          1,
          (state.playing ? 1280 : 4096) / Math.max(state.width, state.height),
          Math.max(
            (rect.width * dpr) / state.width,
            (rect.height * dpr) / state.height,
            1 / Math.max(state.width, state.height)
          )
        );
        player = await createTemplatePlayer(
          canvas,
          state.templateId,
          state.text,
          Math.max(1, Math.round(state.width * scale)),
          Math.max(1, Math.round(state.height * scale))
        );
        key = nextKey;
        text = state.text;
      }
      if (disposed) break;
      if (!player) continue;
      if (text !== state.text) {
        player.setText(state.text);
        text = state.text;
      }
      const frame = await player.render(
        Math.max(0, Math.min(state.timeUs, Math.max(0, player.durationUs - 1)))
      );
      if (disposed) break;
      status.value = '';
      frameMs.value = frame.ms;
      renderedTimeUs.value = Math.max(
        0,
        Math.min(state.timeUs, Math.max(0, player.durationUs - 1))
      );
      renderedFrames.value++;
      // Native control geometry is stable authoring state. It must never size
      // or clip the independent full-canvas rendering surface.
      const b = frame.controlBounds;
      const canvas = host.value.querySelector('canvas')!;
      if (b && b.width > 0 && b.height > 0)
        bounds.value = {
          left: `${(b.x / canvas.width) * 100}%`,
          top: `${(b.y / canvas.height) * 100}%`,
          width: `${(b.width / canvas.width) * 100}%`,
          height: `${(b.height / canvas.height) * 100}%`
        };
      // Yield between frames so slow templates never build an unbounded render backlog.
      if (pending) await new Promise<void>((resolve) => requestAnimationFrame(() => resolve()));
    }
  } catch (error) {
    if (!disposed) {
      status.value = error instanceof Error ? error.message : '文字模板渲染失败';
      emit('error', status.value);
    }
    failed = `${props.templateId}:${props.width}:${props.height}:${props.text}`;
    player?.dispose();
    player = undefined;
    key = '';
    pending = false;
  } finally {
    running = false;
    if (disposed) {
      player?.dispose();
      player = undefined;
    }
  }
}
watch(
  () => [
    props.templateId,
    props.text,
    props.timeUs,
    props.width,
    props.height,
    props.playing,
    host.value
  ],
  request,
  { flush: 'post' }
);
onBeforeUnmount(() => {
  disposed = true;
  if (!running) player?.dispose();
});
</script>
<template>
  <div
    class="template-layer"
    :data-template="templateId"
    :data-frame-ms="frameMs.toFixed(1)"
    :data-time-us="renderedTimeUs"
    :data-rendered-frames="renderedFrames"
    :data-render-state="status ? 'loading-or-error' : 'ready'"
  >
    <div ref="host" class="template-canvas" />
    <div v-if="status" class="template-status">{{ tr(status) }}</div>
    <div
      v-else
      class="template-hit"
      :class="{ selected }"
      :style="bounds"
      @pointerdown.stop="emit('drag', $event)"
    >
      <template v-if="selected && !playing"
        ><i
          v-for="corner in ['tl', 'tr', 'bl', 'br']"
          :key="corner"
          :class="['selection-handle', corner]"
      /></template>
    </div>
  </div>
</template>
