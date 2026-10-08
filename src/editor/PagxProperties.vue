<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import type { Item, EditorCommand } from '../../packages/core/project.mjs';
import { seconds, ticks } from '../../packages/core/project.mjs';
import { pagxFields, updatePagxContent } from '../../packages/core/pagx.mjs';
import { useI18n } from './i18n';
const props = defineProps<{
  item: Item;
  commit: (command: EditorCommand) => Promise<boolean>;
  disabled: boolean;
}>();
const { tr } = useI18n();
const fields = ref<ReturnType<typeof pagxFields>>([]),
  name = ref(''),
  width = ref(1920),
  height = ref(1080),
  duration = ref(6),
  transparent = ref(true);
const error = ref(''),
  applying = ref(0),
  fontFamilies = ref<string[]>([]);
let base = props.item.clip.pagx!,
  saved = '',
  generation = 0,
  disposed = false;
const composing = ref(false);
function compositionStart() {
  composing.value = true;
  clearTimeout(timer);
}
function compositionEnd() {
  composing.value = false;
  schedule();
}
let timer: ReturnType<typeof setTimeout> | undefined;
const pending = new Set<string>();
const signature = () =>
  JSON.stringify([
    name.value,
    width.value,
    height.value,
    duration.value,
    transparent.value,
    fields.value.map((f) => f.value)
  ]);
const groups = computed(() =>
  [...new Set(fields.value.map((f) => f.group))]
    .map((group) => ({
      name: group,
      fields: fields.value.filter((f) => f.group === group)
    }))
    .sort((a, b) => {
      const primary = ['主标题', '第二行标题', '标题三', '标题四', '核心数值', '产品名称'];
      const rank = (g: { name: string; fields: typeof fields.value }) => {
        const index = primary.indexOf(g.name);
        return index >= 0 ? index : g.fields.some((f) => f.type === 'text') ? 10 : 20;
      };
      return rank(a) - rank(b);
    })
);
function reset() {
  clearTimeout(timer);
  generation++;
  base = props.item.clip.pagx!;
  fields.value = pagxFields(base.xml);
  name.value = props.item.name;
  width.value = base.width;
  height.value = base.height;
  duration.value = seconds(base.duration);
  transparent.value = base.transparent;
  error.value = '';
  saved = signature();
}
watch(
  () => JSON.stringify([props.item.name, props.item.clip.pagx]),
  (value) => {
    if (!pending.has(value)) reset();
  },
  { immediate: true }
);
onMounted(async () => {
  try {
    const response = await fetch('/api/fonts');
    if (response.ok) {
      const catalog = await response.json();
      fontFamilies.value = [...new Set<string>(catalog.fonts.map((f: any) => f.family))].sort();
    }
  } catch {
    /* Existing authored font names remain editable offline. */
  }
});
function schedule() {
  clearTimeout(timer);
  if (composing.value || disposed || props.disabled) return;
  error.value = '';
  timer = setTimeout(() => void apply(), 300);
}
async function apply(leaving = false) {
  clearTimeout(timer);
  if (composing.value || props.disabled || (!leaving && applying.value) || signature() === saved)
    return;
  const captured = signature(),
    currentGeneration = generation,
    itemId = props.item.id;
  const clipName = name.value.trim() || 'PAGX 动画';
  let key = '';
  let registered = false;
  try {
    if (clipName.length > 256) throw Error('名称不能超过 256 个字符');
    const originals = new Map(pagxFields(base.xml).map((f) => [f.key, f.value]));
    const values = Object.fromEntries(
      fields.value
        .filter((f) => !f.animated && f.value !== originals.get(f.key))
        .map((f) => [f.key, f.value])
    );
    const pagx = updatePagxContent(base, values, {
      width: width.value,
      height: height.value,
      duration: ticks(duration.value),
      transparent: transparent.value
    });
    if (pagx.duration < props.item.clip.source.end) throw Error('动画源时长不能短于片段使用的范围');
    key = JSON.stringify([clipName, pagx]);
    if (pending.has(key)) return;
    pending.add(key);
    registered = true;
    applying.value++;
    const ok = await props.commit({ action: 'set_pagx_clip', itemId, pagx, name: clipName });
    if (currentGeneration !== generation) return;
    if (!ok) throw Error('修改未应用，请重试。');
    base = pagx;
    saved = captured;
    error.value = '';
  } catch (e) {
    if (currentGeneration === generation) error.value = e instanceof Error ? e.message : String(e);
  } finally {
    if (registered) {
      pending.delete(key);
      applying.value--;
    }
    if (!disposed && !applying.value && !error.value && signature() !== saved) schedule();
  }
}
function rgb(value: string) {
  const hex = value.slice(1);
  return (
    '#' +
    (hex.length < 5
      ? hex
          .slice(0, 3)
          .split('')
          .map((c) => c + c)
          .join('')
      : hex.slice(0, 6))
  );
}
function edit(field: ReturnType<typeof pagxFields>[number], event: Event) {
  const target = event.target as HTMLInputElement;
  field.value =
    field.type === 'boolean'
      ? String(target.checked)
      : target.type === 'color' && [5, 9].includes(field.value.length)
        ? target.value +
          (field.value.length === 5 ? field.value[4].repeat(2) : field.value.slice(7))
        : target.value;
  schedule();
}
onBeforeUnmount(() => {
  void apply(true);
  disposed = true;
  clearTimeout(timer);
});
</script>
<template>
  <fieldset
    class="pagx-content-fields html-content-fields"
    :disabled="disabled"
    @compositionstart="compositionStart"
    @compositionend="compositionEnd"
  >
    <div class="html-library-note">PAGX · {{ tr('可编辑动画') }}</div>
    <label class="html-field"
      >{{ tr('名称')
      }}<input v-model="name" :aria-label="tr('PAGX 片段名称')" maxlength="256" @input="schedule"
    /></label>
    <div class="html-dimensions">
      <label class="html-field"
        >{{ tr('宽度')
        }}<input
          v-model.number="width"
          type="number"
          min="1"
          max="4096"
          :aria-label="tr('PAGX 动画宽度')"
          @input="schedule"
      /></label>
      <label class="html-field"
        >{{ tr('高度')
        }}<input
          v-model.number="height"
          type="number"
          min="1"
          max="4096"
          :aria-label="tr('PAGX 动画高度')"
          @input="schedule"
      /></label>
      <label class="html-field"
        >{{ tr('源时长')
        }}<input
          v-model.number="duration"
          type="number"
          min="0.01"
          max="86400"
          step="0.1"
          :aria-label="tr('PAGX 动画源时长')"
          @input="schedule"
      /></label>
    </div>
    <label class="html-transparent"
      ><input v-model="transparent" type="checkbox" @change="schedule" />{{ tr('透明背景') }}</label
    >
    <datalist :id="'pagx-fonts-' + item.id">
      <option value="system" />
      <option v-for="family in fontFamilies" :key="family" :value="family" />
    </datalist>
    <details v-for="group in groups" :key="group.name" class="pagx-property-group" open>
      <summary>{{ tr(group.name) }}</summary>
      <label
        v-for="field in group.fields"
        :key="field.key"
        class="html-field"
        :data-pagx-field="field.key"
      >
        <span
          >{{ tr(field.label) }}<small v-if="field.animated"> · {{ tr('由动画控制') }}</small></span
        >
        <textarea
          v-if="field.type === 'text'"
          :value="field.value"
          :aria-label="tr(group.name) + ' · ' + tr(field.label)"
          :disabled="field.animated"
          rows="2"
          @input="edit(field, $event)"
        />
        <input
          v-else-if="field.type === 'boolean'"
          type="checkbox"
          :checked="field.value === 'true'"
          :aria-label="tr(group.name) + ' · ' + tr(field.label)"
          :disabled="field.animated"
          @change="edit(field, $event)"
        />
        <div v-else-if="field.type === 'color'" class="pagx-color-field">
          <input
            type="color"
            :value="rgb(field.value)"
            :aria-label="tr(group.name) + ' · ' + tr('选择颜色')"
            :disabled="field.animated"
            @input="edit(field, $event)"
          />
          <input
            :value="field.value"
            :aria-label="tr(group.name) + ' · ' + tr(field.label)"
            :disabled="field.animated"
            @input="edit(field, $event)"
          />
        </div>
        <input
          v-else
          :type="field.type === 'number' ? 'number' : 'text'"
          :value="field.value"
          :min="field.min"
          :max="field.max"
          :step="field.type === 'number' ? 'any' : undefined"
          :list="field.type === 'font' ? 'pagx-fonts-' + item.id : undefined"
          :aria-label="tr(group.name) + ' · ' + tr(field.label)"
          :disabled="field.animated"
          @input="edit(field, $event)"
        />
      </label>
    </details>
    <p v-if="!fields.length" class="html-library-note">{{ tr('此动画没有可编辑属性。') }}</p>
    <p v-if="error" role="alert" class="html-library-note html-apply-error">
      {{ tr(error) }} <button type="button" @click="apply()">{{ tr('重试') }}</button>
      <button type="button" @click="reset">{{ tr('还原') }}</button>
    </p>
    <small>{{ tr(applying ? '正在应用修改…' : '修改会自动应用，可撤销。') }}</small>
  </fieldset>
</template>
<style scoped>
.pagx-property-group {
  border-top: 1px solid var(--border, #30343c);
  padding: 8px 0;
}
.pagx-property-group summary {
  cursor: pointer;
  font-size: 12px;
  margin-bottom: 8px;
}
.pagx-property-group .html-field {
  margin-top: 8px;
}
.pagx-color-field {
  display: flex;
  gap: 6px;
}
.pagx-color-field input[type='color'] {
  width: 34px;
  min-width: 34px;
  padding: 2px;
}
.pagx-color-field input:not([type='color']) {
  min-width: 0;
  width: 100%;
}
.pagx-content-fields textarea {
  resize: vertical;
  width: 100%;
  min-height: 48px;
  font: inherit;
  color: inherit;
  background: var(--input-bg, #181b21);
  border: 1px solid var(--border, #30343c);
  border-radius: 4px;
  padding: 6px;
}
</style>
