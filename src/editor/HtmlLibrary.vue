<script setup lang="ts">
import { useI18n } from './i18n';
const { tr, locale } = useI18n();
import Icon from './Icon.vue';
import { localizeHtmlPreset } from './localize-html-preset';
import { HTML_PRESETS, type HtmlContent, type HtmlPreset } from './html-presets';

const props = defineProps<{ busy: boolean }>();
const emit = defineEmits<{
  insert: [html: HtmlContent, name: string];
}>();
function insertPreset(preset: HtmlPreset) {
  if (!props.busy) emit('insert', localizeHtmlPreset(preset.html, locale.value), tr(preset.name));
}
</script>

<template>
  <aside class="library html-library panel">
    <div class="library-main">
      <div class="library-toolbar">
        <span class="panel-title">{{ tr('动画模板') }}</span>
      </div>
      <div class="html-preset-list">
        <article v-for="preset in HTML_PRESETS" :key="preset.id" class="html-preset-card">
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
        </article>
      </div>
    </div>
  </aside>
</template>
