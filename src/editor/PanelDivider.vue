<script setup lang="ts">
import { onBeforeUnmount } from 'vue';

const props = defineProps<{
  label: string;
  orientation: 'vertical' | 'horizontal';
  value: number;
}>();
const emit = defineEmits<{
  start: [];
  resize: [delta: number];
  finish: [cancelled: boolean];
  step: [delta: number];
  reset: [];
}>();
let pointer: number | undefined;
let origin = 0;
let target: HTMLElement | undefined;
const position = (event: PointerEvent) =>
  props.orientation === 'vertical' ? event.clientX : event.clientY;
function finish(cancelled = false) {
  if (pointer === undefined) return;
  const id = pointer;
  pointer = undefined;
  window.removeEventListener('blur', cancel);
  if (target?.hasPointerCapture(id)) target.releasePointerCapture(id);
  emit('finish', cancelled);
}
function cancel() {
  finish(true);
}
function start(event: PointerEvent) {
  if (event.button !== 0 || pointer !== undefined) return;
  event.preventDefault();
  target = event.currentTarget as HTMLElement;
  target.focus({ preventScroll: true });
  target.setPointerCapture(event.pointerId);
  pointer = event.pointerId;
  origin = position(event);
  window.addEventListener('blur', cancel);
  emit('start');
}
function move(event: PointerEvent) {
  if (event.pointerId === pointer) emit('resize', position(event) - origin);
}
function release(event: PointerEvent) {
  if (event.pointerId !== pointer) return;
  move(event);
  finish();
}
function keyboard(event: KeyboardEvent) {
  // Divider keystrokes must not seek or delete the selected timeline clip.
  event.stopPropagation();
  if (event.key === 'Escape') {
    event.preventDefault();
    cancel();
    return;
  }
  if (pointer !== undefined) return;
  const keys =
    props.orientation === 'vertical' ? ['ArrowLeft', 'ArrowRight'] : ['ArrowUp', 'ArrowDown'];
  const direction = keys.indexOf(event.key);
  if (direction !== -1) {
    event.preventDefault();
    emit('step', (direction === 0 ? -1 : 1) * (event.shiftKey ? 50 : 10));
  } else if (event.key === 'Home' || event.key === 'End') {
    event.preventDefault();
    emit('step', event.key === 'Home' ? -100000 : 100000);
  } else if (event.key === 'Enter') {
    event.preventDefault();
    emit('reset');
  }
}
onBeforeUnmount(cancel);
</script>

<template>
  <div
    class="panel-divider"
    :class="orientation"
    role="separator"
    tabindex="0"
    :aria-label="label"
    :title="label"
    :aria-orientation="orientation"
    :aria-valuenow="Math.round(value)"
    aria-valuemin="0"
    aria-valuemax="100"
    @pointerdown="start"
    @pointermove="move"
    @pointerup="release"
    @pointercancel="cancel"
    @lostpointercapture="cancel"
    @keydown="keyboard"
    @dblclick="emit('reset')"
  />
</template>
