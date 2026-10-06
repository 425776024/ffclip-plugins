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
import LibraryToolbar from './LibraryToolbar.vue';
import LibraryCategoryStrip from './LibraryCategoryStrip.vue';
const props = defineProps<{
  kind: 'effect' | 'transition';
  busy: boolean;
  canApply: boolean;
  categoryFilter?: string;
}>();
const emit = defineEmits<{ apply: [kind: VisualPackage['kind'], id: string] }>();
const search = ref('');
const category = ref('全部'),
  gallery = new EffectPreviewGallery();
const posters = ref<Record<string, string>>({}),
  failures = ref<Record<string, string>>({});
const packages = computed(() =>
  (props.kind === 'effect' ? EFFECT_PACKAGES : TRANSITION_PACKAGES).filter(
    (p) => !props.categoryFilter || p.category === props.categoryFilter
  )
);
const resourceName = computed(() =>
  props.kind === 'transition' ? '转场' : props.categoryFilter === '调色' ? '滤镜' : '特效'
);
function group(pack: VisualPackage) {
  if (props.categoryFilter === '调色')
    return pack.id.startsWith('look-')
      ? 'looks'
      : pack.template.id.includes('lut')
        ? 'lut'
        : 'grading';
  return pack.id === 'detail' ? 'detail' : pack.category;
}
const groupNames: Record<string, string> = {
  looks: '风格预设',
  grading: '调色工具',
  lut: 'LUT',
  detail: '画面细节'
};
const categories = computed(() => [
  {
    id: '全部',
    label: tr(
      props.kind === 'transition'
        ? '全部转场'
        : props.categoryFilter === '调色'
          ? '全部滤镜'
          : '全部特效'
    )
  },
  ...Array.from(new Set(packages.value.map(group)), (id) => ({
    id,
    label: tr(groupNames[id] || id)
  }))
]);
const shown = computed(() =>
  packages.value.filter(
    (p) =>
      (category.value === '全部' || group(p) === category.value) &&
      (!search.value.trim() ||
        tr(p.name).toLocaleLowerCase().includes(search.value.trim().toLocaleLowerCase()))
  )
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
  () => [props.kind, props.categoryFilter],
  () => {
    category.value = '全部';
    search.value = '';
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
  <aside class="library text-library effect-library resource-library panel">
    <div class="library-main">
      <LibraryToolbar
        v-model:query="search"
        :search-label="
          tr(
            kind === 'transition' ? '搜索转场' : categoryFilter === '调色' ? '搜索滤镜' : '搜索特效'
          )
        "
        :count="shown.length"
      />
      <LibraryCategoryStrip
        :label="
          tr(
            kind === 'transition' ? '转场分类' : categoryFilter === '调色' ? '滤镜分类' : '特效分类'
          )
        "
        :entries="categories"
        :selected="category"
        @choose="category = $event"
      />
      <div class="library-content">
        <div class="text-presets template-presets">
          <div v-for="pack in shown" :key="pack.id" class="template-card">
            <button
              class="text-preset"
              :aria-label="
                tr('应用{kind}：{name}', {
                  kind: tr(resourceName),
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
        <div v-if="!shown.length" class="library-no-results">{{ tr('没有匹配的资源') }}</div>
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
