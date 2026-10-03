<script setup lang="ts">
import { useI18n } from './i18n';
const { tr } = useI18n();
import { computed, ref } from 'vue';
import { TEXT_TEMPLATES } from '../../packages/core/project.mjs';
import TextTemplateGallery from './TextTemplateGallery.vue';
import Icon from './Icon.vue';
defineProps<{ busy: boolean }>();
const emit = defineEmits<{
  basic: [content?: string, size?: number, subtitle?: boolean];
  template: [id: string];
}>();
const category = ref('flower');
const categories = { flower: '花字', bubble: '文字气泡', animation: '动画文字', basic: '基础文字' };
const templates = computed(() =>
  TEXT_TEMPLATES.filter((template) => template.category === category.value)
);
</script>
<template>
  <aside class="library text-library panel">
    <nav class="library-categories" :aria-label="tr('文字分类')">
      <div class="category-heading"><Icon name="up" :size="12" />{{ tr('文字') }}</div>
      <button
        v-for="(label, key) in categories"
        :key="key"
        :class="{ active: category === key }"
        :aria-pressed="category === key"
        @click="category = key"
      >
        {{ tr(label) }}
      </button>
    </nav>
    <div class="library-main">
      <div class="library-toolbar">
        <span class="panel-title">{{ tr(categories[category as keyof typeof categories]) }}</span
        ><small v-if="category !== 'basic'">{{
          tr('{count} 款', { count: templates.length })
        }}</small>
      </div>
      <TextTemplateGallery
        v-if="category !== 'basic'"
        :templates="templates"
        :busy="busy"
        @select="emit('template', $event)"
      />
      <div v-else class="text-presets">
        <button class="text-preset" :disabled="busy" @click="emit('basic')">
          <span class="preset-preview">{{ tr('默认文字') }}</span
          ><span class="preset-label">{{ tr('默认文字') }}<Icon name="plus" :size="15" /></span>
        </button>
        <button class="text-preset" :disabled="busy" @click="emit('basic', tr('输入标题'), 96)">
          <span class="preset-preview title-preset">{{ tr('标题') }}</span
          ><span class="preset-label">{{ tr('标题') }}<Icon name="plus" :size="15" /></span>
        </button>
        <button
          class="text-preset"
          :disabled="busy"
          @click="emit('basic', tr('输入字幕'), 48, true)"
        >
          <span class="preset-preview subtitle-preset">{{ tr('在这里写下你的故事') }}</span
          ><span class="preset-label">{{ tr('字幕') }}<Icon name="plus" :size="15" /></span>
        </button>
      </div>
      <div class="library-footnote">{{ tr('点击添加 · 在右侧改字 · 播放查看动画') }}</div>
    </div>
  </aside>
</template>
