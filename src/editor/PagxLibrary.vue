<script setup lang="ts">
import { computed, ref } from 'vue';
import { useI18n } from './i18n';
import type { PagxContent } from '../../packages/core/types';
import { PAGX_PRESETS, pagxPreset } from './pagx-presets';
import LibraryToolbar from './LibraryToolbar.vue';
import LibraryCategoryStrip from './LibraryCategoryStrip.vue';
import Icon from './Icon.vue';
const props = defineProps<{ busy: boolean }>();
const emit = defineEmits<{ insert: [pagx: PagxContent, name: string] }>();
const { tr, locale } = useI18n();
const search = ref(''),
  category = ref('all');
const categories = computed(() => [
  { id: 'all', label: tr('全部动画') },
  { id: 'presentation', label: tr('演示动画') },
  { id: 'overlay', label: tr('叠加动画') }
]);
const shown = computed(() =>
  PAGX_PRESETS.filter(
    (p) =>
      (category.value === 'all' || category.value === p.category) &&
      (locale.value === 'en' ? p.nameEn : p.name)
        .toLocaleLowerCase()
        .includes(search.value.trim().toLocaleLowerCase())
  )
);
function insert(id: string) {
  if (!props.busy) {
    const preset = pagxPreset(id, locale.value === 'en' ? 'en' : 'zh');
    emit('insert', preset.pagx, preset.name);
  }
}
</script>
<template>
  <aside class="library html-library resource-library panel pagx-library">
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
              :disabled="busy"
              :aria-label="tr('添加动画：{name}', { name: tr(preset.name) })"
              :data-pagx-template="preset.id"
              @click="insert(preset.id)"
            >
              <img
                class="html-preset-poster"
                :src="locale === 'en' ? preset.posterEn : preset.poster"
                :alt="tr('{name}预览', { name: tr(preset.name) })"
                draggable="false"
              />
              <span class="html-preset-add" aria-hidden="true"
                ><Icon name="plus" :size="16"
              /></span></button
            ><span class="html-preset-name">{{
              locale === 'en' ? preset.nameEn : preset.name
            }}</span>
          </article>
        </div>
        <div v-if="!shown.length" class="library-no-results">{{ tr('没有匹配的资源') }}</div>
      </div>
      <div class="library-footnote">PAGX · {{ tr('点击添加 · 在右侧修改内容') }}</div>
    </div>
  </aside>
</template>
