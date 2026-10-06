<script setup lang="ts">
import { computed } from 'vue';
import type { Item } from '../../packages/core/project.mjs';
import Icon from './Icon.vue';
import { useI18n } from './i18n';

const { tr } = useI18n();
const props = defineProps<{ items: Item[]; property: string; label: string; time: number }>();
const emit = defineEmits<{ toggle: []; seek: [time: number] }>();
const frames = computed(() =>
  props.items.flatMap((item) =>
    (item.clip.automation?.[props.property]?.keyframes ?? []).map(
      (frame) => item.placement.begin + frame.time
    )
  )
);
const previous = computed(() => {
  const times = frames.value.filter((time) => time < Math.round(props.time));
  return times.length ? Math.max(...times) : undefined;
});
const next = computed(() => {
  const times = frames.value.filter((time) => time > Math.round(props.time));
  return times.length ? Math.min(...times) : undefined;
});
const canToggle = computed(
  () =>
    props.items.length > 0 &&
    props.items.every(
      (item) => props.time >= item.placement.begin && props.time <= item.placement.end
    )
);
const atKeyframe = computed(
  () =>
    canToggle.value &&
    props.items.every((item) =>
      item.clip.automation?.[props.property]?.keyframes.some(
        (frame) => item.placement.begin + frame.time === Math.round(props.time)
      )
    )
);
const toggleLabel = computed(() => tr(atKeyframe.value ? '删除关键帧' : '添加关键帧'));
const hint = (action: string) => `${tr(props.label)} · ${action}`;
</script>

<template>
  <div
    class="keyframe-controls"
    :class="{ bound: frames.length > 0, 'at-keyframe': atKeyframe }"
    role="group"
    :aria-label="tr('{label}关键帧', { label: tr(label) })"
  >
    <button
      type="button"
      class="keyframe-previous"
      :disabled="previous === undefined"
      :title="hint(tr('上一个关键帧'))"
      :aria-label="hint(tr('上一个关键帧'))"
      @click="previous !== undefined && emit('seek', previous)"
    >
      <Icon name="left" :size="12" />
    </button>
    <button
      type="button"
      class="keyframe-toggle"
      :disabled="!canToggle"
      :aria-pressed="atKeyframe"
      :title="hint(toggleLabel)"
      :aria-label="hint(toggleLabel)"
      @click="emit('toggle')"
    >
      <Icon name="diamond" :size="12" />
    </button>
    <button
      type="button"
      class="keyframe-next"
      :disabled="next === undefined"
      :title="hint(tr('下一个关键帧'))"
      :aria-label="hint(tr('下一个关键帧'))"
      @click="next !== undefined && emit('seek', next)"
    >
      <Icon name="right" :size="12" />
    </button>
  </div>
</template>
