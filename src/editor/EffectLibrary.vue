<script setup lang="ts">
import { useI18n } from './i18n';
const { tr } = useI18n();
import { computed, onBeforeUnmount, ref, watch } from 'vue';
import {
  EFFECT_PACKAGES,
  TRANSITION_PACKAGES,
  type VisualPackage
} from '../../packages/render/catalog';
import { EffectPreviewGallery } from './effect-preview';
import Icon from './Icon.vue';
const props = defineProps<{ kind: 'effect' | 'transition'; busy: boolean; canApply: boolean }>();
const emit = defineEmits<{ apply: [kind: VisualPackage['kind'], id: string] }>();
const category = ref('全部'),
  gallery = new EffectPreviewGallery();
const posters = ref<Record<string, string>>({}),
  failures = ref<Record<string, string>>({});
const packages = computed(() => (props.kind === 'effect' ? EFFECT_PACKAGES : TRANSITION_PACKAGES));
const categories = computed(() => ['全部', ...new Set(packages.value.map((p) => p.category))]);
const shown = computed(() =>
  packages.value.filter((p) => category.value === '全部' || p.category === category.value)
);
let alive = true;
async function load(pack: VisualPackage) {
  delete failures.value[pack.id];
  try {
    const poster = await gallery.poster(pack);
    if (alive) posters.value[pack.id] = poster;
  } catch (error) {
    if (alive) failures.value[pack.id] = error instanceof Error ? error.message : '预览失败';
  }
}
watch(
  () => props.kind,
  () => {
    category.value = '全部';
    for (const p of packages.value) if (!posters.value[p.id]) void load(p);
  },
  { immediate: true }
);
onBeforeUnmount(() => {
  alive = false;
  gallery.dispose();
});
</script>
<template>
  <aside class="library text-library panel">
    <nav class="library-categories" :aria-label="tr(kind === 'effect' ? '特效分类' : '转场分类')">
      <div class="category-heading">
        <Icon name="up" :size="12" />{{ tr(kind === 'effect' ? '特效' : '转场') }}
      </div>
      <button
        v-for="c in categories"
        :key="c"
        :class="{ active: category === c }"
        @click="category = c"
      >
        {{ tr(c) }}
      </button>
    </nav>
    <div class="library-main">
      <div class="library-toolbar">
        <span class="panel-title">{{ tr(kind === 'effect' ? '特效模板' : '转场模板') }}</span
        ><small>{{ tr('{count} 款', { count: packages.length }) }}</small>
      </div>
      <div class="text-presets template-presets">
        <div v-for="pack in shown" :key="pack.id" class="template-card">
          <button
            class="text-preset"
            :aria-label="
              tr('应用{kind}：{name}', {
                kind: tr(kind === 'effect' ? '特效' : '转场'),
                name: tr(pack.name)
              })
            "
            :title="tr(pack.description)"
            :disabled="busy || !canApply || !posters[pack.id]"
            @click="emit('apply', kind, pack.id)"
          >
            <span class="preset-preview template-poster"
              ><img
                v-if="posters[pack.id]"
                :src="posters[pack.id]"
                :alt="tr('{name}预览', { name: tr(pack.name) })"
              /><span v-else>{{
                tr(failures[pack.id] ? '预览不可用' : '正在生成预览…')
              }}</span></span
            >
            <span class="preset-label"
              ><span class="template-name">{{ tr(pack.name) }}</span
              ><Icon name="plus" :size="13"
            /></span>
          </button>
          <button
            v-if="failures[pack.id]"
            class="template-retry"
            :title="tr(failures[pack.id])"
            @click="load(pack)"
          >
            {{ tr('重试预览') }}
          </button>
        </div>
      </div>
      <div class="library-footnote">
        {{
          tr(
            kind === 'effect'
              ? '选择画面片段后添加 · 在右侧调整参数'
              : '选择相邻片段中的前一段 · 在右侧调整时长与方向'
          )
        }}
      </div>
    </div>
  </aside>
</template>
