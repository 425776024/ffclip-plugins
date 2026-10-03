<script setup lang="ts">
import { ref, watch, onMounted, onBeforeUnmount } from 'vue';
import { sharedMediaClient } from '../../packages/media/client';
const props = withDefaults(
  defineProps<{ url: string; kind: string; time?: number; label?: string }>(),
  { time: 0, label: '' }
);
const canvas = ref<HTMLCanvasElement>(),
  visible = ref(false),
  error = ref('');
let observer: IntersectionObserver | undefined,
  resize: ResizeObserver | undefined,
  controller: AbortController | undefined;
async function draw() {
  controller?.abort();
  if (!visible.value || !canvas.value || props.kind === 'audio') return;
  const task = (controller = new AbortController());
  try {
    const rect = canvas.value.getBoundingClientRect(),
      dpr = Math.max(1, Math.min(4, window.devicePixelRatio || 1));
    const frame =
      props.kind === 'image'
        ? await sharedMediaClient.image(props.url, {
            signal: task.signal,
            priority: 8,
            width: Math.ceil(rect.width * dpr),
            height: Math.ceil(rect.height * dpr)
          })
        : (
            await sharedMediaClient.thumbnails(props.url, [props.time], {
              signal: task.signal,
              displayWidth: Math.max(1, rect.width),
              displayHeight: Math.max(1, rect.height),
              dpr,
              fit: 'contain',
              quality: 'final',
              priority: 8
            })
          )[0];
    try {
      if (task.signal.aborted || !canvas.value) return;
      canvas.value.width = frame.width;
      canvas.value.height = frame.height;
      canvas.value.getContext('2d')?.drawImage(frame.frame, 0, 0);
      canvas.value.dataset.pts = String(frame.timestamp);
      error.value = '';
    } finally {
      frame.close();
    }
  } catch (failure) {
    if (!task.signal.aborted)
      error.value = failure instanceof Error ? failure.message : String(failure);
  }
}
watch(
  () => [props.url, props.kind, props.time, visible.value],
  () => void draw(),
  { flush: 'post' }
);
onMounted(() => {
  observer = new IntersectionObserver(
    ([entry]) => {
      visible.value = entry.isIntersecting;
      if (!entry.isIntersecting) controller?.abort();
    },
    { rootMargin: '120px' }
  );
  if (canvas.value) observer.observe(canvas.value);
  resize = new ResizeObserver(() => {
    if (visible.value) void draw();
  });
  if (canvas.value) resize.observe(canvas.value);
});
onBeforeUnmount(() => {
  controller?.abort();
  observer?.disconnect();
  resize?.disconnect();
});
</script>
<template>
  <canvas
    ref="canvas"
    class="media-poster"
    :aria-label="label"
    :title="error || label"
    :data-media-error="error || undefined"
  />
</template>
<style scoped>
.media-poster {
  display: block;
  width: 100%;
  height: 100%;
  object-fit: contain;
  background: #141b22;
}
</style>
