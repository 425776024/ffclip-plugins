<script setup lang="ts">
import { useI18n } from './i18n';
const { tr, locale } = useI18n();
import Icon from './Icon.vue';
import { computed, ref } from 'vue';
import LibraryToolbar from './LibraryToolbar.vue';
import LibraryCategoryStrip from './LibraryCategoryStrip.vue';
import { OVERLAY_PRESETS } from './html-overlay-presets';
import { localizeHtmlPreset } from './localize-html-preset';
import { HTML_PRESETS, type HtmlContent, type HtmlPreset } from './html-presets';

const props = defineProps<{ busy: boolean }>();
const emit = defineEmits<{
  insert: [html: HtmlContent, name: string];
}>();
const search = ref(''),
  category = ref('all');
const overlayIds = new Set(OVERLAY_PRESETS.map((preset) => preset.id));
const categories = computed(() => [
  { id: 'all', label: tr('全部动画') },
  { id: 'presentation', label: tr('演示动画') },
  { id: 'overlay', label: tr('叠加动画') }
]);
const shown = computed(() =>
  HTML_PRESETS.filter(
    (preset) =>
      (category.value === 'all' ||
        (overlayIds.has(preset.id) ? 'overlay' : 'presentation') === category.value) &&
      tr(preset.name).toLocaleLowerCase().includes(search.value.trim().toLocaleLowerCase())
  )
);
function insertPreset(preset: HtmlPreset) {
  if (!props.busy) emit('insert', localizeHtmlPreset(preset.html, locale.value), tr(preset.name));
}
</script>

<template>
  <aside class="library html-library resource-library panel">
    <div class="library-main">
      <LibraryToolbar v-model:query="search" :search-label="tr('搜索动画')" :count="shown.length" />
      <LibraryCategoryStrip
        :label="tr('动画分类')"
        :entries="categories"
        :selected="category"
        @choose="category = $event"
      />
      <div class="library-content">
        <div class="html-preset-list">
          <article v-for="preset in shown" :key="preset.id" class="html-preset-card">
            <button
              class="html-preset-preview"
              :class="preset.id"
              :disabled="busy"
              :aria-label="tr('添加动画：{name}', { name: tr(preset.name) })"
              @click="insertPreset(preset)"
            >
              <img
                v-if="preset.poster"
                class="html-preset-poster"
                :src="preset.poster"
                :alt="tr('{name}预览', { name: tr(preset.name) })"
                draggable="false"
              />
              <span v-else-if="preset.id === 'lower-third'" class="html-poster-lower"
                ><small>VIDEOCUT STUDIO</small><strong>{{ tr('每一帧，都有故事') }}</strong
                ><span>{{ tr('自由剪辑，灵感成片') }}</span></span
              >
              <span v-else class="html-poster-title"
                ><small>CREATE IN MOTION</small><strong>{{ tr('让创意动起来') }}</strong
                ><i
              /></span>
              <span class="html-preset-add" aria-hidden="true">
                <Icon name="plus" :size="16" />
              </span>
            </button>
            <span class="html-preset-name" :title="tr(preset.name)">{{ tr(preset.name) }}</span>
          </article>
        </div>
        <div v-if="!shown.length" class="library-no-results">{{ tr('没有匹配的资源') }}</div>
      </div>
      <div class="library-footnote">{{ tr('点击添加 · 在右侧修改内容') }}</div>
    </div>
  </aside>
</template>
