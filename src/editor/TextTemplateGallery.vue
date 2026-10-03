<script setup lang="ts">
import { onBeforeUnmount, ref, watch } from 'vue';
import type { TEXT_TEMPLATES } from '../../packages/core/project.mjs';
import { templatePoster } from './text-renderer';
import { useI18n } from './i18n';
import Icon from './Icon.vue';

const props = withDefaults(
  defineProps<{
    templates: (typeof TEXT_TEMPLATES)[number][];
    busy?: boolean;
    action?: 'add' | 'apply';
    selected?: string;
  }>(),
  { busy: false, action: 'add' }
);
const emit = defineEmits<{ select: [id: string] }>();
const { tr, locale } = useI18n();
const posters = ref<Record<string, string>>({});
const failures = ref<Record<string, string>>({});
let alive = true;
async function load(preset: (typeof TEXT_TEMPLATES)[number]) {
  const language = locale.value;
  delete failures.value[preset.id];
  try {
    const poster = await templatePoster(preset.id, preset.text ? tr(preset.text) : undefined);
    if (alive && locale.value === language) posters.value[preset.id] = poster;
  } catch (error) {
    if (alive && locale.value === language)
      failures.value[preset.id] = error instanceof Error ? error.message : '预览加载失败';
  }
}
watch(
  [locale, () => props.templates],
  ([language], previous) => {
    if (language !== previous?.[0]) {
      posters.value = {};
      failures.value = {};
    }
    for (const preset of props.templates) if (!posters.value[preset.id]) void load(preset);
  },
  { immediate: true }
);
onBeforeUnmount(() => {
  alive = false;
});
</script>

<template>
  <div class="text-presets template-presets text-template-gallery">
    <div v-for="preset in templates" :key="preset.id" class="template-card">
      <button
        class="text-preset"
        :class="{ active: action === 'apply' && selected === preset.id }"
        :aria-pressed="action === 'apply' ? selected === preset.id : undefined"
        :aria-label="
          tr(action === 'add' ? '添加模板：{name}' : '切换文字模板：{name}', {
            name: tr(preset.name)
          })
        "
        :title="`${tr(preset.name)} · ${tr(preset.tag)}`"
        :disabled="busy || !posters[preset.id]"
        @click="emit('select', preset.id)"
      >
        <span class="preset-preview template-poster">
          <img
            v-if="posters[preset.id]"
            :src="posters[preset.id]"
            :alt="tr('{name}预览', { name: tr(preset.name) })"
          />
          <span v-else>{{ tr(failures[preset.id] ? '预览不可用' : '正在生成预览…') }}</span>
          <span v-if="preset.category === 'animation'" class="template-motion-badge">{{
            tr('动画')
          }}</span>
        </span>
        <span class="preset-label">
          <span class="template-name">{{ tr(preset.name) }}</span>
          <Icon v-if="action === 'add'" name="plus" :size="13" />
        </span>
      </button>
      <button
        v-if="failures[preset.id]"
        class="template-retry"
        :title="tr(failures[preset.id])"
        @click="load(preset)"
      >
        {{ tr('重试预览') }}
      </button>
    </div>
  </div>
</template>
