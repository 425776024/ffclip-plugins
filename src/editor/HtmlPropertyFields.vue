<script setup lang="ts">
import { useI18n } from './i18n';
const { tr } = useI18n();
import { computed } from 'vue';
import type { HtmlProperty } from './html-properties';
import HtmlColorInput from './HtmlColorInput.vue';

const props = defineProps<{ fields: HtmlProperty[] }>();
const groups = computed(() =>
  [
    {
      id: 'variable',
      label: '动画参数',
      fields: props.fields.filter((field) => field.group === 'variable')
    },
    {
      id: 'text',
      label: '文字内容',
      fields: props.fields.filter((field) => field.group === 'text')
    },
    {
      id: 'style',
      label: '样式属性',
      fields: props.fields.filter((field) => field.group === 'style')
    }
  ].filter((group) => group.fields.length)
);

function edit(field: HtmlProperty, event: Event) {
  const input = event.target as HTMLInputElement;
  field.value =
    field.kind === 'boolean'
      ? input.checked
      : field.kind === 'number'
        ? input.value === ''
          ? ''
          : Number(input.value)
        : input.value;
}
</script>

<template>
  <section
    v-for="group in groups"
    :key="group.id"
    class="html-property-group"
    :aria-label="tr(group.label)"
  >
    <div class="group-heading">
      <span>{{ tr(group.label) }}</span
      ><small>{{ tr('{count} 项', { count: group.fields.length }) }}</small>
    </div>
    <div v-for="field in group.fields" :key="field.id" class="html-property-field">
      <label :for="field.id"
        >{{ field.labelIsAuthored ? field.label : tr(field.label)
        }}<small>{{ field.contextIsAuthored ? field.context : tr(field.context) }}</small></label
      >
      <textarea
        v-if="field.kind === 'text'"
        :id="field.id"
        :value="String(field.value)"
        rows="2"
        @input="edit(field, $event)"
      />
      <input
        v-else-if="field.kind === 'boolean'"
        :id="field.id"
        type="checkbox"
        :checked="field.value === true"
        @change="edit(field, $event)"
      />
      <HtmlColorInput
        v-else-if="field.kind === 'color'"
        :id="field.id"
        :value="String(field.value)"
        :label="field.labelIsAuthored ? field.label : tr(field.label)"
        @change="field.value = $event"
      />
      <div v-else class="html-property-input">
        <input
          :id="field.id"
          :type="field.kind === 'number' ? 'number' : 'text'"
          :value="String(field.value)"
          :step="field.kind === 'number' ? 'any' : undefined"
          :placeholder="tr(field.kind === 'font' ? '字体名称' : undefined)"
          @input="edit(field, $event)"
        />
        <span v-if="field.unit" class="html-property-unit">{{ field.unit }}</span>
      </div>
    </div>
  </section>
</template>
