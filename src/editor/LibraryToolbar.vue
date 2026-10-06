<script setup lang="ts">
import Icon from './Icon.vue';
import { useI18n } from './i18n';
const { tr } = useI18n();
defineProps<{
  query: string;
  searchLabel: string;
  actionLabel?: string;
  disabled?: boolean;
  count?: number;
}>();
const emit = defineEmits<{ 'update:query': [query: string]; action: [] }>();
</script>
<template>
  <div class="library-toolbar resource-toolbar">
    <button
      v-if="actionLabel"
      class="library-action"
      :disabled="disabled"
      :title="actionLabel"
      :aria-label="actionLabel"
      @click="emit('action')"
    >
      <Icon name="plus" :size="14" /><span>{{ actionLabel }}</span>
    </button>
    <slot name="leading" />
    <div class="search-field">
      <Icon name="search" :size="13" />
      <input
        type="search"
        :value="query"
        :aria-label="searchLabel"
        :placeholder="searchLabel"
        @input="emit('update:query', ($event.target as HTMLInputElement).value)"
      />
      <button
        v-if="query"
        class="search-clear"
        :title="tr('清空搜索')"
        :aria-label="tr('清空搜索')"
        @click="emit('update:query', '')"
      >
        <Icon name="close" :size="12" />
      </button>
    </div>
    <slot />
    <small v-if="count !== undefined" class="library-result-count" aria-live="polite">
      {{ tr('{count} 项', { count }) }}
    </small>
  </div>
</template>
