<script setup lang="ts">
import { computed, ref } from 'vue';
import { TEXT_TEMPLATES, type Item, type EditorCommand } from '../../packages/core/project.mjs';
import {
  recipes,
  resolveRecipe,
  TEMPLATE_COMPOSITION_RULES
} from '../../packages/text-wasm/src/recipes.mjs';
import TextTemplateGallery from './TextTemplateGallery.vue';
import { useI18n } from './i18n';
import Icon from './Icon.vue';

const { tr } = useI18n();
const props = defineProps<{ items: Item[] }>();
const emit = defineEmits<{ commands: [commands: EditorCommand[]]; error: [error: unknown] }>();
type Template = NonNullable<NonNullable<Item['clip']['text']>['template']>;
type Style = NonNullable<Template['style']>;
const swatches = [
  '#ffffff',
  '#111111',
  '#ff5c70',
  '#ffb84d',
  '#ffdf5c',
  '#55d6a5',
  '#69a7ff',
  '#b58cff'
];
const section = ref('style');
const sections = { style: '样式', templates: '花字', bubble: '文字气泡', animation: '动画' };
const templates = computed(() =>
  TEXT_TEMPLATES.filter(
    (preset) => preset.category === (section.value === 'templates' ? 'flower' : section.value)
  )
);
const texts = computed(() =>
  props.items.flatMap((item) => (item.clip.text ? [item.clip.text] : []))
);
const layoutWidthValue = computed(() => common(texts.value.map((text) => text.layoutWidth)));
function common<T>(values: T[]) {
  return values.every((value) => value === values[0]) ? values[0] : undefined;
}
function styleValue<K extends keyof Style>(key: K) {
  return common(
    texts.value.map((text) => (text.template ? text.template.style?.[key] : text[key]))
  );
}
function mixedStyle(key: keyof Style) {
  const values = texts.value.map((text) =>
    text.template ? text.template.style?.[key] : text[key]
  );
  return values.some((value) => value !== values[0]);
}
const presetId = computed(() =>
  common(
    composition.value.map((recipe) =>
      recipe ? recipes.find((entry) => entry.base === recipe.base)?.id : 'basic'
    )
  )
);
const composition = computed(() =>
  texts.value.map((text) => (text.template ? resolveRecipe(text.template) : null))
);
const flowerSelection = computed(() =>
  composition.value.every(
    (recipe) => recipe && TEMPLATE_COMPOSITION_RULES.overlayBases.includes(recipe.base)
  )
);
const animation = computed(() =>
  common(
    composition.value.map((recipe) =>
      !recipe
        ? 'none'
        : TEMPLATE_COMPOSITION_RULES.overlayBases.includes(recipe.base)
          ? recipe.animation || 'none'
          : recipes.find((entry) => entry.base === recipe.base)?.id
    )
  )
);
const backdrop = computed(() =>
  common(composition.value.map((recipe) => recipe?.backdrop || 'none'))
);

function editable(template: Template): Template {
  const next = { ...template };
  delete next.packageDigest;
  delete next.style;
  return next;
}
function styleChange(key: keyof Style, value?: string | number) {
  emit(
    'commands',
    props.items.map((item): EditorCommand => {
      const text = item.clip.text!;
      if (!text.template)
        return {
          action: 'set_text',
          itemId: item.id,
          [key]: value ?? (key === 'color' ? '#ffffff' : 64)
        };
      return {
        action: 'set_text',
        itemId: item.id,
        templateStyle:
          key === 'color'
            ? { color: value === undefined ? null : String(value) }
            : { fontSize: value === undefined ? null : Number(value) }
      };
    })
  );
}
function styleInput(key: keyof Style, event: Event) {
  const value = (event.target as HTMLInputElement).value.trim();
  styleChange(key, value === '' ? undefined : key === 'fontSize' ? Number(value) : value);
}
function selectPreset(id: string) {
  emit(
    'commands',
    props.items.map((item): EditorCommand => {
      if (id === 'basic')
        return {
          action: 'set_text',
          itemId: item.id,
          template: null
        };
      return {
        action: 'set_text',
        itemId: item.id,
        template: { id, version: 1 }
      };
    })
  );
}
function changePart(part: 'animation' | 'backdrop', value: string) {
  try {
    emit(
      'commands',
      props.items.map((item): EditorCommand => {
        const template = editable(item.clip.text!.template!),
          recipe = resolveRecipe(template);
        const next = {
          base: recipe.base,
          ...(recipe.backdrop ? { backdrop: recipe.backdrop } : {}),
          ...(recipe.animation ? { animation: recipe.animation } : {})
        };
        if (value === 'none') delete next[part];
        else next[part] = value;
        template.id = `custom-${next.base}-${next.backdrop || 'none'}-${next.animation || 'none'}`;
        template.recipe = next;
        const builtin = recipes.find(
          (entry) =>
            entry.base === next.base &&
            entry.backdrop === next.backdrop &&
            entry.animation === next.animation
        );
        if (builtin) {
          template.id = builtin.id;
          delete template.recipe;
        }
        resolveRecipe(template);
        return { action: 'set_text', itemId: item.id, template };
      })
    );
  } catch (error) {
    emit('error', error);
  }
}
function selectAnimation(value: string) {
  if (flowerSelection.value && (value === 'none' || value === 'anim-lua-letter-transform'))
    changePart('animation', value);
  else if (value === 'none') selectPreset('basic');
  else selectPreset(value);
}
</script>

<template>
  <div class="text-properties">
    <nav class="text-property-tabs" :aria-label="tr('文字编辑分类')">
      <button
        v-for="(label, key) in sections"
        :key="key"
        :class="{ active: section === key }"
        :aria-pressed="section === key"
        @click="section = key"
      >
        {{ tr(label) }}
      </button>
    </nav>
    <template v-if="section === 'style'">
      <div class="property-row text-style-row">
        <label for="text-layout-width">{{ tr('文字框宽度') }}</label>
        <div class="unit-field">
          <input
            id="text-layout-width"
            type="number"
            :aria-label="tr('文字框宽度')"
            min="1"
            max="65536"
            step="1"
            :value="layoutWidthValue === undefined ? '' : Number(layoutWidthValue.toFixed(2))"
            :placeholder="tr('自动')"
            @change="
              emit(
                'commands',
                items.map((item) => ({
                  action: 'set_text',
                  itemId: item.id,
                  layoutWidth:
                    ($event.target as HTMLInputElement).value === ''
                      ? null
                      : Number(($event.target as HTMLInputElement).value)
                }))
              )
            "
          />
          <span>px</span>
        </div>
        <button
          class="small-reset"
          :aria-label="tr('还原文字框宽度')"
          :title="tr('还原文字框宽度')"
          @click="
            emit(
              'commands',
              items.map((item) => ({ action: 'set_text', itemId: item.id, layoutWidth: null }))
            )
          "
        >
          <Icon name="reset" :size="13" />
        </button>
      </div>
      <div class="property-row text-style-row">
        <label for="text-style-size">{{ tr('字号') }}</label>
        <div class="unit-field">
          <input
            id="text-style-size"
            type="number"
            :aria-label="tr('字号')"
            min="4"
            max="1000"
            step="1"
            :value="styleValue('fontSize') ?? ''"
            :placeholder="tr(mixedStyle('fontSize') ? '混合' : '模板默认')"
            @change="styleInput('fontSize', $event)"
          />
          <span>px</span>
        </div>
        <button
          class="small-reset"
          :title="tr('还原字号')"
          :aria-label="tr('还原字号')"
          @click="styleChange('fontSize')"
        >
          <Icon name="reset" :size="13" />
        </button>
      </div>
      <div class="property-row text-style-row">
        <label for="text-style-color">{{ tr('文字颜色') }}</label>
        <div class="text-color-fields">
          <input
            id="text-style-color"
            type="color"
            :aria-label="tr('文字颜色')"
            :value="styleValue('color') || '#ffffff'"
            :class="{ inherited: styleValue('color') === undefined }"
            @change="styleChange('color', ($event.target as HTMLInputElement).value)"
          />
          <input
            type="text"
            :aria-label="tr('文字颜色 HEX')"
            maxlength="7"
            :value="styleValue('color') ?? ''"
            :placeholder="tr(mixedStyle('color') ? '混合' : '模板原色')"
            @change="styleInput('color', $event)"
          />
        </div>
        <button
          class="small-reset"
          :title="tr('还原文字颜色')"
          :aria-label="tr('还原文字颜色')"
          @click="styleChange('color')"
        >
          <Icon name="reset" :size="13" />
        </button>
      </div>
      <div class="text-color-swatches" :aria-label="tr('常用文字颜色')">
        <button
          v-for="color in swatches"
          :key="color"
          :style="{ background: color }"
          :aria-label="tr('设置文字颜色 {color}', { color })"
          :title="color"
          :aria-pressed="styleValue('color') === color"
          @click="styleChange('color', color)"
        />
      </div>
      <p v-if="texts.some((text) => text.template)" class="text-edit-hint">
        {{ tr('留空使用模板原色与字号；改色调整字面填充，保留描边与底板。') }}
      </p>
    </template>
    <template v-if="section === 'templates' || section === 'bubble'">
      <div class="group-heading text-section-heading">
        <span>{{ tr(section === 'templates' ? '花字模板' : '文字气泡') }}</span
        ><small>{{ tr('应用到选中文字') }}</small>
      </div>
      <div class="text-template-actions">
        <button
          :aria-pressed="presetId === 'basic'"
          :aria-label="tr('切换为基础文字')"
          @click="selectPreset('basic')"
        >
          <Icon name="reset" :size="13" />{{ tr('基础文字') }}
        </button>
      </div>
      <TextTemplateGallery
        :templates="templates"
        action="apply"
        :selected="presetId"
        @select="selectPreset"
      />
    </template>
    <template v-if="section === 'animation'">
      <div class="group-heading text-section-heading">
        <span>{{ tr('文字动画') }}</span
        ><small>{{ tr('应用到选中文字') }}</small>
      </div>
      <div class="text-template-actions">
        <button :aria-pressed="animation === 'none'" @click="selectAnimation('none')">
          <Icon name="reset" :size="13" />{{
            tr(
              flowerSelection || texts.every((text) => !text.template)
                ? '无动画'
                : '无动画（基础文字）'
            )
          }}
        </button>
      </div>
      <label v-if="flowerSelection" class="property-row text-style-row"
        ><span>{{ tr('逐字变换') }}</span
        ><select
          :aria-label="tr('文字动画')"
          :value="animation ?? ''"
          @change="selectAnimation(($event.target as HTMLSelectElement).value)"
        >
          <option v-if="animation === undefined" value="" disabled>{{ tr('混合') }}</option>
          <option value="none">{{ tr('无动画') }}</option>
          <option value="anim-lua-letter-transform">
            {{ tr('逐字变换') }}
          </option>
        </select></label
      >
      <label v-if="flowerSelection" class="property-row text-style-row"
        ><span>{{ tr('底板') }}</span
        ><select
          :aria-label="tr('文字底板')"
          :value="backdrop ?? ''"
          @change="changePart('backdrop', ($event.target as HTMLSelectElement).value)"
        >
          <option v-if="backdrop === undefined" value="" disabled>{{ tr('混合') }}</option>
          <option value="none">{{ tr('无底板') }}</option>
          <option value="bubble-tile">{{ tr('拼贴底板') }}</option>
          <option value="bubble-nine-slice">{{ tr('弹性底板') }}</option>
        </select></label
      >
      <p class="text-edit-hint">
        {{ tr('图案、翻转和打印动画使用配套模板；播放或拖动播放头查看效果。') }}
      </p>
      <TextTemplateGallery
        :templates="templates"
        action="apply"
        :selected="animation"
        @select="selectPreset"
      />
    </template>
    <p v-if="texts.some((text) => text.template?.style)" class="text-edit-hint">
      {{ tr('当前文字样式调整可保存为 Web 作品或导出视频。') }}
    </p>
  </div>
</template>
