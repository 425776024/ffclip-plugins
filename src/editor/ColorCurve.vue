<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { parseCurve } from '../../packages/core/color.mjs';
import { useI18n } from './i18n';
const { tr } = useI18n();
const props = defineProps<{ value: string; channel: string }>();
const emit = defineEmits<{ change: [value: string] }>();
const points = ref<number[][]>([]),
  selected = ref(0);
let drag: number | null = null;
watch(
  () => props.value,
  (value) => {
    points.value = parseCurve(value);
    selected.value = Math.min(selected.value, points.value.length - 1);
  },
  { immediate: true }
);
const path = computed(() =>
  points.value.map(([x, y], i) => `${i ? 'L' : 'M'}${x * 240 + 8},${(1 - y) * 160 + 8}`).join(' ')
);
function coordinate(event: PointerEvent | MouseEvent) {
  const rect = (event.currentTarget as SVGSVGElement).getBoundingClientRect();
  return [
    Math.max(0, Math.min(1, (((event.clientX - rect.left) / rect.width) * 256) / 240 - 8 / 240)),
    Math.max(0, Math.min(1, 1 - (((event.clientY - rect.top) / rect.height) * 176 - 8) / 160))
  ];
}
function move(event: PointerEvent) {
  if (drag === null) return;
  const [x, y] = coordinate(event),
    i = drag;
  points.value[i] = [
    i === 0
      ? 0
      : i === points.value.length - 1
        ? 1
        : Math.max(points.value[i - 1][0] + 0.005, Math.min(points.value[i + 1][0] - 0.005, x)),
    y
  ];
}
function begin(event: PointerEvent) {
  const [x, y] = coordinate(event);
  const i = points.value.findIndex(
    (point) => Math.hypot((point[0] - x) * 240, (point[1] - y) * 160) < 14
  );
  if (i < 0) return;
  drag = i;
  selected.value = i;
  (event.currentTarget as SVGSVGElement).setPointerCapture(event.pointerId);
}
function finish() {
  if (drag !== null) {
    drag = null;
    emit('change', JSON.stringify(points.value));
  }
}
function add(event: MouseEvent) {
  if (points.value.length >= 32) return;
  const [x, y] = coordinate(event);
  if (points.value.some((point) => Math.abs(point[0] - x) < 0.01)) return;
  points.value.push([x, y]);
  points.value.sort((a, b) => a[0] - b[0]);
  selected.value = points.value.findIndex((point) => point[0] === x);
  emit('change', JSON.stringify(points.value));
}
function remove() {
  if (selected.value === 0 || selected.value === points.value.length - 1) return;
  points.value.splice(selected.value, 1);
  selected.value = 0;
  emit('change', JSON.stringify(points.value));
}
function edit(axis: number, event: Event) {
  const next = Number((event.target as HTMLInputElement).value) / 100;
  if (!Number.isFinite(next)) return;
  const i = selected.value;
  points.value[i][axis] =
    axis === 0
      ? Math.max(
          points.value[i - 1]?.[0] + 0.005 || 0,
          Math.min(points.value[i + 1]?.[0] - 0.005 || 1, next)
        )
      : Math.max(0, Math.min(1, next));
  emit('change', JSON.stringify(points.value));
}
</script>
<template>
  <div class="color-curve">
    <svg
      viewBox="0 0 256 176"
      role="img"
      :aria-label="tr('色彩曲线')"
      @pointerdown="begin"
      @pointermove="move"
      @pointerup="finish"
      @pointercancel="
        drag = null;
        points = parseCurve(value);
      "
      @dblclick="add"
    >
      <path
        d="M8 8H248V168H8Z M68 8V168 M128 8V168 M188 8V168 M8 48H248 M8 88H248 M8 128H248"
        class="curve-grid"
      />
      <path d="M8 168L248 8" class="curve-diagonal" />
      <path
        :d="path"
        :stroke="
          ({ red: '#ff6b70', green: '#55d6a5', blue: '#69a7ff' } as Record<string, string>)[
            channel
          ] || '#dedfe1'
        "
        fill="none"
        stroke-width="2"
      />
      <circle
        v-for="([x, y], i) in points"
        :key="i"
        :cx="x * 240 + 8"
        :cy="(1 - y) * 160 + 8"
        :r="selected === i ? 5 : 3.5"
        :class="{ selected: selected === i }"
      />
    </svg>
    <div class="curve-point-fields">
      <label
        >X
        <input
          type="number"
          min="0"
          max="100"
          :disabled="selected === 0 || selected === points.length - 1"
          :value="Math.round(points[selected]?.[0] * 100)"
          @change="edit(0, $event)"
      /></label>
      <label
        >Y
        <input
          type="number"
          min="0"
          max="100"
          :value="Math.round(points[selected]?.[1] * 100)"
          @change="edit(1, $event)"
      /></label>
      <button :disabled="selected === 0 || selected === points.length - 1" @click="remove">
        {{ tr('删除点') }}
      </button>
      <button @click="emit('change', '[[0,0],[1,1]]')">{{ tr('重置') }}</button>
    </div>
    <small class="subtle">{{ tr('双击添加控制点 · 拖动调整') }}</small>
  </div>
</template>
