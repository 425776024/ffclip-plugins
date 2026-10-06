<script setup lang="ts">
import { useI18n } from './i18n';
const { tr } = useI18n();
import { computed, ref } from 'vue';
import { TEXT_TEMPLATES } from '../../packages/core/project.mjs';
import TextTemplateGallery from './TextTemplateGallery.vue';
import Icon from './Icon.vue';
import LibraryToolbar from './LibraryToolbar.vue';
import LibraryCategoryStrip from './LibraryCategoryStrip.vue';
defineProps<{ busy: boolean }>();
const emit = defineEmits<{
  basic: [content?: string, size?: number, subtitle?: boolean];
  template: [id: string];
}>();
const category = ref('flower'),
  search = ref('');
const categories = { flower: '花字', bubble: '文字气泡', animation: '动画文字', basic: '基础文字' };
const categoryEntries = computed(() =>
  Object.entries(categories).map(([id, label]) => ({ id, label: tr(label) }))
);
const templates = computed(() =>
  TEXT_TEMPLATES.filter(
    (template) =>
      template.category === category.value &&
      tr(template.name).toLocaleLowerCase().includes(search.value.trim().toLocaleLowerCase())
  )
);
const basicPresets = computed(() =>
  [
    {
      name: '默认文字',
      preview: '默认文字',
      content: undefined,
      size: undefined,
      subtitle: false,
      className: ''
    },
    {
      name: '标题',
      preview: '标题',
      content: tr('输入标题'),
      size: 96,
      subtitle: false,
      className: 'title-preset'
    },
    {
      name: '字幕',
      preview: '在这里写下你的故事',
      content: tr('输入字幕'),
      size: 48,
      subtitle: true,
      className: 'subtitle-preset'
    }
  ].filter((p) => tr(p.name).toLocaleLowerCase().includes(search.value.trim().toLocaleLowerCase()))
);
</script>
<template>
  <aside class="library text-library resource-library panel">
    <div class="library-main">
      <LibraryToolbar
        v-model:query="search"
        :search-label="tr('搜索文字')"
        :action-label="tr('添加文字')"
        :disabled="busy"
        :count="category === 'basic' ? basicPresets.length : templates.length"
        @action="emit('basic')"
      />
      <LibraryCategoryStrip
        :label="tr('文字分类')"
        :entries="categoryEntries"
        :selected="category"
        @choose="category = $event"
      />
      <div class="library-content">
        <TextTemplateGallery
          v-if="category !== 'basic'"
          :templates="templates"
          :busy="busy"
          @select="emit('template', $event)"
        />
        <div v-else class="text-presets">
          <button
            v-for="preset in basicPresets"
            :key="preset.name"
            class="text-preset"
            :disabled="busy"
            @click="emit('basic', preset.content, preset.size, preset.subtitle)"
          >
            <span class="preset-preview" :class="preset.className">{{ tr(preset.preview) }}</span>
            <span class="preset-label">{{ tr(preset.name) }}<Icon name="plus" :size="13" /></span>
          </button>
        </div>
        <div
          v-if="!(category === 'basic' ? basicPresets.length : templates.length)"
          class="library-no-results"
        >
          {{ tr('没有匹配的资源') }}
        </div>
      </div>
      <div class="library-footnote">{{ tr('点击添加 · 在右侧改字 · 播放查看动画') }}</div>
    </div>
  </aside>
</template>
