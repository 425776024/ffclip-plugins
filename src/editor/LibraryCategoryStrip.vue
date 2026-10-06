<script setup lang="ts">
import HorizontalCardStrip from './HorizontalCardStrip.vue';
defineProps<{
  label: string;
  entries: { id: string; label: string }[];
  selected: string;
}>();
const emit = defineEmits<{ choose: [id: string] }>();
</script>
<template>
  <nav class="library-category-strip" :aria-label="label">
    <HorizontalCardStrip class="library-category-scroll" :label="label" navigation-on-overflow>
      <button
        v-for="entry in entries"
        :key="entry.id"
        :class="{ active: selected === entry.id }"
        :aria-pressed="selected === entry.id"
        :title="entry.label"
        @click="emit('choose', entry.id)"
      >
        {{ entry.label }}
      </button>
    </HorizontalCardStrip>
  </nav>
</template>
