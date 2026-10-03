<script setup lang="ts">
import { computed } from 'vue';
import { useI18n } from './i18n';
import { isHtmlColor } from './html-properties';

const props = defineProps<{ id: string; value: string; label: string }>();
const emit = defineEmits<{ change: [value: string] }>();
const { tr } = useI18n();
const canvas = document.createElement('canvas');
canvas.width = canvas.height = 1;
const context = canvas.getContext('2d', { willReadFrequently: true })!;
const color = computed(() => {
  if (!isHtmlColor(props.value)) return { hex: '#000000', alpha: 1 };
  context.fillStyle = props.value;
  // Canvas normalizes named, hex, RGB and HSL colors without losing alpha.
  const normalized = context.fillStyle;
  if (/^#[\da-f]{6}$/i.test(normalized)) return { hex: normalized, alpha: 1 };
  const rgba = /^rgba?\(([^)]+)\)$/.exec(normalized)?.[1].split(',').map(Number);
  if (rgba?.length === 4)
    return {
      hex:
        '#' +
        rgba
          .slice(0, 3)
          .map((channel) => channel.toString(16).padStart(2, '0'))
          .join(''),
      alpha: rgba[3]
    };
  // Newer CSS color spaces are converted to the native picker's sRGB gamut.
  context.clearRect(0, 0, 1, 1);
  context.fillRect(0, 0, 1, 1);
  const [r, g, b, a] = context.getImageData(0, 0, 1, 1).data;
  return {
    hex: '#' + [r, g, b].map((channel) => channel.toString(16).padStart(2, '0')).join(''),
    alpha: a / 255
  };
});
function pick(hex: string, alpha: number) {
  if (alpha === 1) emit('change', hex);
  else {
    const rgb = [1, 3, 5].map((start) => parseInt(hex.slice(start, start + 2), 16));
    emit('change', `rgba(${rgb.join(', ')}, ${alpha})`);
  }
}
</script>

<template>
  <div class="html-color-input">
    <div class="html-property-input">
      <input
        class="html-color-picker"
        type="color"
        :value="color.hex"
        :aria-label="tr('选择颜色：{label}', { label })"
        :title="tr('选择颜色：{label}', { label })"
        @input="pick(($event.target as HTMLInputElement).value, color.alpha)"
      />
      <input
        :id="id"
        type="text"
        :value="value"
        :aria-invalid="!isHtmlColor(value)"
        @input="emit('change', ($event.target as HTMLInputElement).value)"
      />
    </div>
    <label class="html-color-alpha">
      <span>{{ tr('不透明度') }}</span>
      <input
        type="range"
        min="0"
        max="100"
        step="1"
        :value="Math.round(color.alpha * 100)"
        :aria-label="tr('颜色不透明度：{label}', { label })"
        @input="pick(color.hex, Number(($event.target as HTMLInputElement).value) / 100)"
      />
      <output>{{ Math.round(color.alpha * 100) }}%</output>
    </label>
  </div>
</template>
