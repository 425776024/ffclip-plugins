<script setup lang="ts">
import { computed, ref } from 'vue';
import {
  EFFECT_TEMPLATES,
  sampleProperty,
  type Item,
  type EffectInstance
} from '../../packages/core/project.mjs';
import { LOOK_NAMES, parseCube } from '../../packages/core/color.mjs';
import { useI18n } from './i18n';
import ColorCurve from './ColorCurve.vue';
import Icon from './Icon.vue';
import KeyframeButtons from './KeyframeButtons.vue';
const { tr } = useI18n();
const props = defineProps<{ effect: EffectInstance; item: Item; time: number }>();
const emit = defineEmits<{
  change: [parameter: string, value: number | string];
  keyframe: [parameter: string];
  seek: [time: number];
  error: [error: unknown];
}>();
const group = ref('basic'),
  tone = ref('shadows'),
  channel = ref('rgb'),
  fileInput = ref<HTMLInputElement>();
const definition = computed(() => EFFECT_TEMPLATES.find((e) => e.id === props.effect.templateId)!);
const groups = { basic: '基础', hsl: 'HSL', curves: '曲线', grading: '分级' };
const basic = [
  'brightness',
  'contrast',
  'saturation',
  'hue',
  'temperature',
  'tint',
  'exposure',
  'gamma',
  'highlights',
  'shadows',
  'grayscale'
];
const parameters = computed(() =>
  Object.entries(definition.value.parameters).filter(([key]) => {
    if (props.effect.templateId === 'custom-lut') return key !== 'cube';
    if (props.effect.templateId !== 'color-grade') return true;
    return group.value === 'basic'
      ? basic.includes(key)
      : group.value === 'hsl'
        ? key.startsWith('hsl')
        : group.value === 'grading'
          ? ['Hue', 'Saturation', 'Brightness', 'Contrast'].some((s) => key === tone.value + s)
          : false;
  })
);
const labels: Record<string, string> = {
  ...LOOK_NAMES,
  cinema: '电影',
  up: '向上',
  down: '向下',
  left: '向左',
  right: '向右'
};
function value(key: string) {
  return sampleProperty(
    props.item,
    `effects.${props.effect.id}.${key}`,
    Math.max(
      0,
      Math.min(
        props.item.placement.end - props.item.placement.begin,
        Math.round(props.time - props.item.placement.begin)
      )
    )
  );
}
async function upload(event: Event) {
  const file = (event.target as HTMLInputElement).files?.[0];
  try {
    if (file) {
      if (file.size > 8388608) throw new Error('LUT 文件过大');
      const text = await file.text();
      parseCube(text);
      emit('change', 'cube', text);
    }
  } catch (error) {
    emit('error', error);
  } finally {
    (event.target as HTMLInputElement).value = '';
  }
}
</script>
<template>
  <nav
    v-if="effect.templateId === 'color-grade'"
    class="parameter-tabs"
    :aria-label="tr('调色分类')"
  >
    <button
      v-for="(label, key) in groups"
      :key="key"
      :class="{ active: group === key }"
      @click="group = key"
    >
      {{ tr(label) }}
    </button>
  </nav>
  <nav v-if="effect.templateId === 'color-grade' && group === 'grading'" class="parameter-tabs">
    <button
      v-for="(label, key) in { shadows: '阴影', midtones: '中间调', highlights: '高光' }"
      :key="key"
      :class="{ active: tone === key }"
      @click="tone = key"
    >
      {{ tr(label) }}
    </button>
  </nav>
  <template v-if="effect.templateId === 'color-grade' && group === 'curves'">
    <nav class="parameter-tabs">
      <button
        v-for="(label, key) in { rgb: 'RGB', red: 'R', green: 'G', blue: 'B' }"
        :key="key"
        :class="{ active: channel === key }"
        @click="channel = key"
      >
        {{ label }}
      </button>
    </nav>
    <ColorCurve
      :key="channel"
      :channel="channel"
      :value="String(effect.parameters['curve' + channel])"
      @change="emit('change', 'curve' + channel, $event)"
    />
  </template>
  <div v-if="effect.templateId === 'custom-lut'" class="lut-file-row">
    <input ref="fileInput" type="file" accept=".cube" hidden @change="upload" />
    <button class="subtle-button" @click="fileInput?.click()">
      <Icon name="plus" :size="13" />{{ tr('导入 .cube') }}
    </button>
    <small class="subtle">{{ tr('三维 LUT · 随工程保存') }}</small>
  </div>
  <div v-for="[key, d] in parameters" :key="key" class="property-row effect-parameter-row">
    <label :for="effect.id + '-' + key">{{ tr(d.label || key) }}</label>
    <select
      v-if="d.type === 'enum'"
      :id="effect.id + '-' + key"
      :value="value(key)"
      @change="emit('change', key, ($event.target as HTMLSelectElement).value)"
    >
      <option v-for="v in d.values" :key="v" :value="v">{{ tr(labels[v] || v) }}</option>
    </select>
    <template v-else>
      <input
        type="range"
        :aria-label="tr(d.label || key)"
        :min="d.min"
        :max="d.max"
        :step="d.step || 0.1"
        :value="value(key)"
        @change="emit('change', key, Number(($event.target as HTMLInputElement).value))"
      />
      <div class="unit-field">
        <input
          :id="effect.id + '-' + key"
          type="number"
          :min="d.min"
          :max="d.max"
          :step="d.step || 0.1"
          :value="Number(Number(value(key)).toFixed(2))"
          @change="emit('change', key, Number(($event.target as HTMLInputElement).value))"
        /><span>{{ d.unit }}</span>
      </div>
    </template>
    <button
      class="small-reset"
      :title="tr('重置')"
      :aria-label="tr('重置') + ' ' + tr(d.label || key)"
      @click="emit('change', key, d.default as number | string)"
    >
      <Icon name="reset" :size="12" />
    </button>
    <KeyframeButtons
      v-if="d.keyframe"
      :items="[item]"
      :property="`effects.${effect.id}.${key}`"
      :label="d.label || key"
      :time="time"
      @toggle="emit('keyframe', key)"
      @seek="emit('seek', $event)"
    />
  </div>
</template>
