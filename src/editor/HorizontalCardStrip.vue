<script setup lang="ts">
import { nextTick, onBeforeUnmount, onMounted, onUpdated, ref } from 'vue';
import Icon from './Icon.vue';
import { useI18n } from './i18n';
const { tr } = useI18n();
defineProps<{ label?: string; navigationOnOverflow?: boolean }>();
const track = ref<HTMLElement>(),
  canPrevious = ref(false),
  canNext = ref(false);
let observer: ResizeObserver | undefined;
function measure() {
  const el = track.value;
  if (!el) return;
  canPrevious.value = el.scrollLeft > 1;
  canNext.value = el.scrollLeft + el.clientWidth < el.scrollWidth - 1;
}
function scroll(direction: number) {
  const el = track.value;
  el?.scrollBy({ left: direction * Math.max(68, el.clientWidth - 68), behavior: 'smooth' });
}
function wheel(event: WheelEvent) {
  const el = track.value;
  if (!el || el.scrollWidth <= el.clientWidth) return;
  const delta = Math.abs(event.deltaX) > Math.abs(event.deltaY) ? event.deltaX : event.deltaY;
  if ((delta > 0 && canNext.value) || (delta < 0 && canPrevious.value)) {
    event.preventDefault();
    el.scrollLeft += delta;
  }
}
function reveal(event: FocusEvent) {
  const target = event.target as HTMLElement;
  target.scrollIntoView({ block: 'nearest', inline: 'nearest' });
}
onMounted(async () => {
  await nextTick();
  observer = new ResizeObserver(measure);
  if (track.value) {
    observer.observe(track.value);
    for (const child of track.value.children) observer.observe(child);
  }
  measure();
});
onUpdated(measure);
onBeforeUnmount(() => observer?.disconnect());
</script>
<template>
  <div class="horizontal-card-strip" :aria-label="label">
    <button
      v-show="!navigationOnOverflow || canPrevious || canNext"
      class="strip-arrow"
      :disabled="!canPrevious"
      :aria-label="tr('向左滚动')"
      @click="scroll(-1)"
    >
      <Icon name="left" :size="13" />
    </button>
    <div
      ref="track"
      class="horizontal-card-track"
      @scroll="measure"
      @wheel="wheel"
      @focusin="reveal"
    >
      <slot />
    </div>
    <button
      v-show="!navigationOnOverflow || canPrevious || canNext"
      class="strip-arrow"
      :disabled="!canNext"
      :aria-label="tr('向右滚动')"
      @click="scroll(1)"
    >
      <Icon name="right" :size="13" />
    </button>
  </div>
</template>
