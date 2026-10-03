<script setup lang="ts">
import { useI18n } from './i18n';
const { tr } = useI18n();
import { computed, onBeforeUnmount, ref, watch } from 'vue';
import {
  seconds,
  ticks,
  findItem,
  sampleProperty,
  getPropertyDescriptor,
  isPropertyApplicable,
  PROPERTY_DESCRIPTORS,
  EFFECT_TEMPLATES,
  TRANSITION_TEMPLATES,
  TEXT_TEMPLATES,
  validateHtmlContent,
  HTML_MAX_SIDE,
  type Project,
  type Item,
  type PropertyDescriptor,
  type EditorCommand
} from '../../packages/core/project.mjs';
import Icon from './Icon.vue';
import type { Interpolation, TransitionArguments } from '../../packages/core/commands.js';
import { propertyCommand, keyframeCommand, keyframeProperty } from './property-command';
import type { HtmlContent } from './html-presets';
import HtmlPropertyFields from './HtmlPropertyFields.vue';
import TextProperties from './TextProperties.vue';
import { parseHtmlProperties, applyHtmlProperties, type HtmlProperty } from './html-properties';
const props = defineProps<{
  project: Project;
  item: Item | null;
  selection: string[];
  time: number;
  disabled: boolean;
  commitHtml: (command: EditorCommand) => Promise<boolean>;
}>();
const emit = defineEmits<{
  error: [error: unknown];
  command: [command: EditorCommand];
  commands: [commands: EditorCommand[]];
  text: [changes: Partial<NonNullable<Item['clip']['text']>>];
  visual: [changes: Partial<Item['clip']['visual']>];
  audio: [changes: Partial<Item['clip']['audio']>];
  move: [begin: number];
  duration: [value: number];
  source: [begin: number];
  canvas: [width: number, height: number];
  fps: [value: number];
}>();
const optionLabels: Record<string, string> = {
  contain: '适应',
  cover: '填充',
  stretch: '拉伸',
  nativeCrop: '原始尺寸',
  normal: '正常',
  multiply: '正片叠底',
  screen: '滤色',
  overlay: '叠加',
  darken: '变暗',
  lighten: '变亮',
  warm: '暖色',
  cool: '冷色',
  cinema: '电影'
};
const transitionColor = ref<'#000000' | '#ffffff'>('#000000'),
  transitionDirection = ref<'left' | 'right' | 'up' | 'down'>('left');
const tab = ref('canvas'),
  interpolation = ref<Interpolation>('linear'),
  transition = ref<TransitionArguments['templateId']>('dissolve'),
  transitionDuration = ref(1);
const selection = computed(() =>
  props.project.timeline.tracks
    .flatMap((t) => t.items)
    .filter((i) => props.selection.includes(i.id))
);
const tabs = computed(() =>
  !props.item
    ? { canvas: '画布' }
    : props.item.clip.type === 'audio'
      ? { audio: '音频', time: '时间', canvas: '画布' }
      : props.item.clip.type === 'text'
        ? { text: '文字', visual: '画面', effects: '特效', time: '时间', canvas: '画布' }
        : props.item.clip.type === 'html-clip'
          ? { html: '内容', visual: '画面', effects: '特效', time: '时间', canvas: '画布' }
          : isPropertyApplicable(props.project, props.item, 'audio.gainLinear')
            ? { visual: '画面', audio: '音频', time: '时间', effects: '特效', canvas: '画布' }
            : { visual: '画面', time: '时间', effects: '特效', canvas: '画布' }
);
const htmlProperties = ref<HtmlProperty[]>([]),
  htmlName = ref(''),
  htmlWidth = ref(1920),
  htmlHeight = ref(1080),
  htmlDuration = ref(6),
  htmlTransparent = ref(true);
const htmlApplying = ref(0),
  htmlApplyError = ref('');
let htmlBase: { itemId: string; sourceEnd: number; content: HtmlContent } | undefined;
let htmlSavedDraft = '',
  htmlGeneration = 0,
  htmlComposing = false;
let htmlApplyTimer: ReturnType<typeof setTimeout> | undefined;
type HtmlSubmission = {
  itemId: string;
  html: string;
  name: string;
  generation: number;
  signature: string;
};
const htmlPending = new Set<HtmlSubmission>();
let htmlDisposed = false;
function htmlDraftSignature() {
  return JSON.stringify([
    htmlName.value,
    htmlWidth.value,
    htmlHeight.value,
    htmlDuration.value,
    htmlTransparent.value,
    htmlProperties.value.map((field) => field.value)
  ]);
}
function cancelHtmlApply() {
  clearTimeout(htmlApplyTimer);
  htmlApplyTimer = undefined;
}
function resetHtmlDraft() {
  cancelHtmlApply();
  htmlGeneration++;
  htmlApplyError.value = '';
  htmlComposing = false;
  const content = props.item?.clip.html;
  if (!content) {
    htmlBase = undefined;
    return;
  }
  // Keep source offsets anchored to this document while our own saves arrive.
  htmlBase = { itemId: props.item!.id, sourceEnd: props.item!.clip.source.end, content };
  htmlProperties.value = parseHtmlProperties(content);
  htmlName.value = props.item!.name;
  htmlWidth.value = content.width;
  htmlHeight.value = content.height;
  htmlDuration.value = seconds(content.duration);
  htmlTransparent.value = content.transparent;
  htmlSavedDraft = htmlDraftSignature();
}
watch(
  () => [props.item?.id, props.item?.name, JSON.stringify(props.item?.clip.html)],
  () => {
    if (
      [...htmlPending].some(
        (pending) =>
          pending.generation === htmlGeneration &&
          pending.itemId === props.item?.id &&
          pending.name === props.item?.name &&
          pending.html === JSON.stringify(props.item?.clip.html)
      )
    )
      return;
    // Selection changes must not discard the last debounced edit.
    if (
      htmlBase &&
      htmlBase.itemId !== props.item?.id &&
      props.project.timeline.tracks.some((track) =>
        track.items.some((item) => item.id === htmlBase!.itemId)
      )
    )
      void applyHtml(true);
    resetHtmlDraft();
  },
  { immediate: true }
);
function scheduleHtmlApply() {
  cancelHtmlApply();
  htmlApplyError.value = '';
  if (htmlComposing || props.disabled || htmlDisposed) return;
  htmlApplyTimer = setTimeout(() => void applyHtml(), 300);
}
async function applyHtml(previousItem = false) {
  cancelHtmlApply();
  if (
    !htmlBase ||
    (!previousItem && htmlApplying.value > 0) ||
    htmlComposing ||
    (!previousItem && props.disabled) ||
    htmlDraftSignature() === htmlSavedDraft
  )
    return;
  const base = htmlBase,
    generation = htmlGeneration,
    signature = htmlDraftSignature();
  const name = htmlName.value.trim() || 'HTML 动画';
  if (
    [...htmlPending].some(
      (pending) => pending.generation === generation && pending.signature === signature
    )
  )
    return;
  let submission: HtmlSubmission | undefined;
  try {
    const html: HtmlContent = {
      ...applyHtmlProperties(base.content, htmlProperties.value),
      width: htmlWidth.value,
      height: htmlHeight.value,
      duration: ticks(htmlDuration.value),
      transparent: htmlTransparent.value
    };
    validateHtmlContent(html);
    const sourceEnd = props.item?.id === base.itemId ? props.item.clip.source.end : base.sourceEnd;
    if (html.duration < sourceEnd)
      throw new Error('动画时长不能短于当前片段的源出点。请先裁剪时间轴片段。');
    htmlApplying.value++;
    submission = { itemId: base.itemId, html: JSON.stringify(html), name, generation, signature };
    htmlPending.add(submission);
    const applied = await props.commitHtml({
      action: 'set_html_clip',
      itemId: base.itemId,
      html,
      name
    });
    if (generation !== htmlGeneration) return;
    if (applied) {
      htmlSavedDraft = signature;
      htmlApplyError.value = '';
    } else htmlApplyError.value = tr('修改未应用，请重试。');
  } catch (error) {
    if (generation === htmlGeneration)
      htmlApplyError.value = tr(error instanceof Error ? error.message : String(error));
  } finally {
    if (submission) {
      htmlApplying.value--;
      htmlPending.delete(submission);
    }
    if (
      !htmlDisposed &&
      !htmlApplying.value &&
      !htmlApplyError.value &&
      htmlBase &&
      htmlDraftSignature() !== htmlSavedDraft
    )
      scheduleHtmlApply();
  }
}
onBeforeUnmount(() => {
  if (!props.disabled) void applyHtml(true);
  htmlDisposed = true;
  cancelHtmlApply();
});
watch(
  () => props.item?.id,
  () => {
    if (!props.item) tab.value = 'canvas';
    else if (tab.value === 'canvas' || !tabs.value[tab.value as keyof typeof tabs.value])
      tab.value = Object.keys(tabs.value)[0];
  }
);
const descriptors = computed(() =>
  Object.entries(PROPERTY_DESCRIPTORS).filter(
    ([path]) =>
      path.startsWith(tab.value + '.') &&
      applicable(path).length &&
      !['text.fontSize', 'text.color'].includes(path)
  )
);
const number = (e: Event) => Number((e.target as HTMLInputElement).value);
const string = (e: Event) => (e.target as HTMLInputElement).value;
const local = (item: Item) =>
  Math.max(
    0,
    Math.min(
      item.placement.end - item.placement.begin,
      Math.round(props.time - item.placement.begin)
    )
  );
const read = (object: unknown, path: string): any =>
  path.split('.').reduce((v: any, k) => v?.[k], object);
function value(path: string) {
  const values = applicable(path).map((item) => sampleProperty(item, path, local(item)));
  return values.every((v) => v === values[0]) ? values[0] : undefined;
}
function applicable(path: string) {
  return selection.value.filter((item) => isPropertyApplicable(props.project, item, path));
}
const textSelection = computed(() => selection.value.filter((i) => i.clip.type === 'text'));
function textValue(key: 'content' | 'fontSize' | 'color') {
  const values = textSelection.value
    .filter((i) => key === 'content' || !i.clip.text?.template)
    .map((i) => i.clip.text?.[key]);
  return values.every((v) => v === values[0]) ? values[0] : undefined;
}
function display(path: string, d: PropertyDescriptor) {
  const v = value(path);
  return v === undefined
    ? ''
    : d.unit === 'ticks'
      ? seconds(Number(v))
      : typeof v === 'boolean'
        ? String(v)
        : v;
}
function edit(path: string, d: PropertyDescriptor, event: Event) {
  const raw =
    d.type === 'boolean'
      ? (event.target as HTMLInputElement).checked
      : ['enum', 'text', 'color'].includes(d.type)
        ? string(event)
        : number(event);
  const next = d.unit === 'ticks' ? ticks(Number(raw)) : raw;
  if (path === 'time.start' && selection.value.length > 1 && props.item) {
    try {
      propertyCommand(props.item, path, next);
      emit('command', {
        action: 'move_clips',
        itemIds: selection.value.map((item) => item.id),
        deltaSeconds: seconds(Number(next) - props.item.placement.begin)
      });
    } catch (error) {
      emit('error', error);
    }
    return;
  }
  try {
    emit(
      'commands',
      applicable(path).map((item) =>
        item.clip.automation?.[path]?.keyframes.length
          ? keyframeCommand(item, path, next, seconds(local(item)), interpolation.value)
          : propertyCommand(item, path, next)
      )
    );
  } catch (error) {
    emit('error', error);
  }
}
function reset(path: string, d: PropertyDescriptor) {
  if (path === 'time.start' && selection.value.length > 1 && props.item) {
    emit('command', {
      action: 'move_clips',
      itemIds: selection.value.map((item) => item.id),
      deltaSeconds: seconds(Number(d.default) - props.item.placement.begin)
    });
    return;
  }
  emit(
    'commands',
    applicable(path).flatMap((item): EditorCommand[] => [
      propertyCommand(item, path, d.default),
      ...(item.clip.automation?.[path]?.keyframes ?? []).map((k): EditorCommand => ({
        action: 'remove_keyframe',
        itemId: item.id,
        property: keyframeProperty(item, path),
        keyframeId: k.id
      }))
    ])
  );
}
function keyframe(path: string) {
  emit(
    'commands',
    applicable(path).map((item): EditorCommand => {
      const frame = item.clip.automation?.[path]?.keyframes.find((k) => k.time === local(item));
      return frame
        ? {
            action: 'remove_keyframe',
            itemId: item.id,
            property: keyframeProperty(item, path),
            keyframeId: frame.id
          }
        : keyframeCommand(
            item,
            path,
            sampleProperty(item, path, local(item)),
            seconds(local(item)),
            interpolation.value
          );
    })
  );
}
function animated(path: string) {
  return selection.value.some((item) => item.clip.automation?.[path]?.keyframes.length);
}
const keys = computed(() =>
  props.item
    ? Object.entries(props.item.clip.automation ?? {}).flatMap(([property, b]) =>
        b.keyframes.map((k) => ({ ...k, property }))
      )
    : []
);
type ItemCommand = Extract<EditorCommand, { itemId: string }>;
type WithoutItem<T> = T extends unknown ? Omit<T, 'itemId'> : never;
function one(command: WithoutItem<ItemCommand>) {
  if (props.item) emit('command', { ...command, itemId: props.item.id });
}
function removeKeyframe(property: string, keyframeId: string) {
  if (props.item)
    one({
      action: 'remove_keyframe',
      property: keyframeProperty(props.item, property),
      keyframeId
    });
}
function addEffect(templateId: string) {
  if (templateId === 'blur' || templateId === 'glow' || templateId === 'lut')
    one({ action: 'add_effect', templateId });
}
function addTransition() {
  if (!props.item || !nextItem.value) return;
  const common = {
    action: 'add_transition' as const,
    fromItemId: props.item.id,
    toItemId: nextItem.value.id,
    durationSeconds: transitionDuration.value
  };
  if (transition.value === 'fade')
    emit('command', {
      ...common,
      templateId: 'fade',
      parameters: { color: transitionColor.value }
    });
  else if (transition.value === 'wipe' || transition.value === 'slide')
    emit('command', {
      ...common,
      templateId: transition.value,
      parameters: { direction: transitionDirection.value }
    });
  else emit('command', { ...common, templateId: 'dissolve' });
}
const nextItem = computed(() => {
  if (!props.item) return null;
  const track = findItem(props.project, props.item.id).track;
  return track.items.find((i) => i.placement.begin === props.item!.placement.end) ?? null;
});
const transitions = computed(
  () =>
    props.project.timeline.transitions?.filter(
      (t) => t.fromItemId === props.item?.id || t.toItemId === props.item?.id
    ) ?? []
);
const asset = computed(() => props.project.assets.find((a) => a.id === props.item?.clip.assetId));
function effectKeyframe(effectId: string, parameter: string) {
  if (!props.item) return;
  const property = `effects.${effectId}.${parameter}`,
    frame = props.item.clip.automation?.[property]?.keyframes.find(
      (k) => k.time === local(props.item!)
    );
  if (frame) removeKeyframe(property, frame.id);
  else
    emit(
      'command',
      keyframeCommand(
        props.item,
        property,
        sampleProperty(props.item, property, local(props.item)),
        seconds(local(props.item)),
        interpolation.value
      )
    );
}
function effectParameter(effectId: string, parameter: string, value: unknown) {
  if (!props.item) return;
  const property = `effects.${effectId}.${parameter}`;
  try {
    if (props.item.clip.automation?.[property]?.keyframes.length)
      emit(
        'command',
        keyframeCommand(
          props.item,
          property,
          value,
          seconds(local(props.item)),
          interpolation.value
        )
      );
    else emit('command', propertyCommand(props.item, property, value));
  } catch (error) {
    emit('error', error);
  }
}
function reorderEffect(effectId: string, delta: number) {
  if (!props.item) return;
  const ids = (props.item.clip.effects ?? []).map((effect) => effect.id);
  const index = ids.indexOf(effectId),
    target = index + delta;
  if (index < 0 || target < 0 || target >= ids.length) return;
  [ids[index], ids[target]] = [ids[target], ids[index]];
  one({ action: 'reorder_effects', effectIds: ids });
}
function speed(event: Event) {
  emit(
    'commands',
    selection.value
      .filter((i) => ['video', 'audio', 'html-clip'].includes(i.clip.type))
      .map((i) => ({ action: 'set_speed', itemId: i.id, rate: number(event), ripple: true }))
  );
}
</script>
<template>
  <aside class="inspector panel">
    <nav class="inspector-tabs" :aria-label="tr('属性分类')">
      <button
        v-for="(label, key) in tabs"
        :key="key"
        :class="{ active: tab === key }"
        @click="tab = key"
      >
        {{ tr(label) }}
      </button>
    </nav>
    <fieldset class="inspector-body" :disabled="disabled">
      <div v-if="selection.length > 1" class="text-edit-hint">
        {{ tr('已选择 {count} 个片段 · 不同值显示“混合”', { count: selection.length }) }}
      </div>
      <template v-if="item?.clip.html && tab === 'html'">
        <div
          class="html-content-fields"
          @input="scheduleHtmlApply"
          @change="scheduleHtmlApply"
          @compositionstart="
            htmlComposing = true;
            cancelHtmlApply();
          "
          @compositionend="
            htmlComposing = false;
            scheduleHtmlApply();
          "
        >
          <div class="group-heading">
            <span>{{ tr('动画内容') }}</span
            ><small>{{ tr(item.clip.html.transparent ? '透明叠加' : '实色背景') }}</small>
          </div>
          <div v-if="selection.length > 1" class="text-edit-hint">
            {{ tr('内容与动画尺寸只应用于当前片段。') }}
          </div>
          <label class="html-field"
            >{{ tr('片段名称')
            }}<input v-model="htmlName" :aria-label="tr('HTML 片段名称')" maxlength="256"
          /></label>
          <div class="html-dimensions">
            <label class="html-field"
              >{{ tr('宽度')
              }}<input
                v-model.number="htmlWidth"
                :aria-label="tr('HTML 动画宽度')"
                type="number"
                min="1"
                :max="HTML_MAX_SIDE"
                step="1" /></label
            ><label class="html-field"
              >{{ tr('高度')
              }}<input
                v-model.number="htmlHeight"
                :aria-label="tr('HTML 动画高度')"
                type="number"
                min="1"
                :max="HTML_MAX_SIDE"
                step="1" /></label
            ><label class="html-field"
              >{{ tr('源时长 / 秒')
              }}<input
                v-model.number="htmlDuration"
                :aria-label="tr('HTML 动画源时长')"
                type="number"
                :min="seconds(item.clip.source.end)"
                max="86400"
                step="0.1"
            /></label>
          </div>
          <label class="html-transparent"
            ><input v-model="htmlTransparent" type="checkbox" />{{ tr('透明背景') }}</label
          >
          <HtmlPropertyFields :fields="htmlProperties" />
          <div v-if="!htmlProperties.length" class="html-library-note">
            {{ tr('此动画暂无可直接修改的内容属性，可调整尺寸、背景和画面属性。') }}
          </div>
          <div v-if="htmlApplyError" class="html-library-note html-apply-error" role="alert">
            {{ htmlApplyError }}
            <button @click="applyHtml()">{{ tr('重试') }}</button>
            <button @click="resetHtmlDraft">{{ tr('还原') }}</button>
          </div>
          <div v-else class="text-edit-hint" role="status">
            {{ tr(htmlApplying ? '正在应用修改…' : '修改会自动应用，可撤销。') }}
          </div>
          <div class="text-edit-hint">
            {{ tr('源时长是动画可裁剪范围；片段在时间轴上的时长请在“时间”中调整。') }}
          </div>
        </div>
      </template>
      <template v-if="item && ['visual', 'audio', 'text', 'time'].includes(tab)">
        <div
          v-if="
            ['visual', 'audio'].includes(tab) &&
            selection.length >
              applicable(tab + (tab === 'visual' ? '.opacity' : '.gainLinear')).length
          "
          class="text-edit-hint"
        >
          {{ tr(tab === 'visual' ? '仅修改具备画面的选中片段。' : '仅修改具备声音的选中片段。') }}
        </div>
        <div class="group-heading">
          <span>{{ tr('{label}属性', { label: tr(tabs[tab as keyof typeof tabs]) }) }}</span
          ><small>{{ tr('◇ 关键帧') }}</small>
        </div>
        <div v-for="[path, d] in descriptors" :key="path" class="property-row descriptor-row">
          <label :for="path"
            >{{ tr(d.label)
            }}<small v-if="path === 'time.start' && selection.length > 1">{{
              tr('按当前片段起点整体移动')
            }}</small></label
          >
          <textarea
            v-if="d.type === 'text'"
            :id="path"
            :aria-label="tr(d.label)"
            :value="display(path, d)"
            :placeholder="tr(value(path) === undefined ? '混合' : '')"
            @change="edit(path, d, $event)"
          />
          <div v-else-if="d.type === 'color'">
            <span
              v-if="value(path) === undefined"
              class="mixed-color"
              :aria-label="tr('颜色混合')"
              >{{ tr('混合') }}</span
            >
            <input
              type="color"
              :id="path"
              :aria-label="
                tr(value(path) === undefined ? `${d.label}（混合，选择以统一）` : d.label)
              "
              :style="value(path) === undefined ? { opacity: 0.35 } : undefined"
              :value="value(path) === undefined ? String(d.default) : display(path, d)"
              @change="edit(path, d, $event)"
            />
          </div>
          <input
            v-else-if="d.type === 'boolean'"
            :id="path"
            type="checkbox"
            :checked="value(path) === true"
            :indeterminate="value(path) === undefined"
            @change="edit(path, d, $event)"
          />
          <select
            v-else-if="d.type === 'enum'"
            :id="path"
            :aria-label="tr(d.label)"
            :value="display(path, d)"
            @change="edit(path, d, $event)"
          >
            <option v-if="value(path) === undefined" value="" disabled>{{ tr('混合') }}</option>
            <option v-for="option in d.values" :key="option" :value="option">
              {{ tr(optionLabels[option] || option) }}
            </option>
          </select>
          <div v-else class="unit-field">
            <input
              :id="path"
              type="number"
              :aria-label="tr(d.label)"
              :min="d.unit === 'ticks' ? seconds(d.min ?? 0) : d.min"
              :max="d.unit === 'ticks' ? seconds(d.max ?? 10368000000) : d.max"
              :step="d.step ?? (d.unit === 'px' ? 1 : d.unit === 'ticks' ? 0.001 : 0.01)"
              :value="display(path, d)"
              :placeholder="tr(value(path) === undefined ? '混合' : '')"
              @change="edit(path, d, $event)"
            /><span>{{ d.unit === 'ticks' ? 's' : d.unit }}</span>
          </div>
          <button
            v-if="d.keyframe"
            class="keyframe-button"
            :class="{ active: animated(path) }"
            :title="tr(`${d.label}：在播放头添加/移除关键帧`)"
            @click="keyframe(path)"
          >
            ◇
          </button>
          <button class="small-reset" :title="tr(`重置${d.label}和关键帧`)" @click="reset(path, d)">
            <Icon name="reset" :size="13" />
          </button>
        </div>
        <label v-if="tab === 'visual' || tab === 'audio'" class="property-row"
          ><span>{{ tr('关键帧插值') }}</span
          ><select v-model="interpolation">
            <option value="linear">{{ tr('线性') }}</option>
            <option value="hold">{{ tr('保持') }}</option>
            <option value="easeIn">{{ tr('缓入') }}</option>
            <option value="easeOut">{{ tr('缓出') }}</option>
            <option value="easeInOut">{{ tr('缓入缓出') }}</option>
          </select></label
        >
        <div v-if="keys.length" class="keyframe-list">
          <div v-for="k in keys" :key="k.id" class="property-row">
            <span
              >{{ tr(getPropertyDescriptor(item, k.property)?.label) }} ·
              {{ seconds(k.time).toFixed(3) }}s</span
            ><small>{{ k.value }}</small
            ><button :title="tr('删除关键帧')" @click="removeKeyframe(k.property, k.id)">×</button>
          </div>
        </div>
      </template>
      <TextProperties
        v-if="item?.clip.text && tab === 'text'"
        :items="textSelection"
        @commands="emit('commands', $event)"
        @error="emit('error', $event)"
      />
      <div v-if="item && tab === 'time'" class="text-edit-hint">
        {{
          tr(
            item.clip.html
              ? '裁剪与变速使用动画源时间；左右拖动播放头可以精确预览。'
              : '关联音画会同步裁剪和变速；恒速修改联动后续片段，音调随速度变化。'
          )
        }}
      </div>
      <template v-if="item && tab === 'effects'">
        <div class="group-heading">
          <span>{{ tr('片段特效') }}</span
          ><small>{{ tr('当前片段') }}</small>
        </div>
        <div class="effect-presets">
          <button v-for="effect in EFFECT_TEMPLATES" :key="effect.id" @click="addEffect(effect.id)">
            ＋ {{ tr(effect.name) }}
          </button>
        </div>
        <div v-for="effect in item.clip.effects" :key="effect.id" class="effect-card">
          <div class="group-heading">
            <label
              ><input
                type="checkbox"
                :checked="effect.enabled"
                @change="
                  one({ action: 'update_effect', effectId: effect.id, enabled: !effect.enabled })
                "
              />{{ tr(EFFECT_TEMPLATES.find((e) => e.id === effect.templateId)?.name) }}</label
            ><button
              :title="tr('上移特效')"
              :disabled="item.clip.effects?.[0]?.id === effect.id"
              @click="reorderEffect(effect.id, -1)"
            >
              ↑
            </button>
            <button
              :title="tr('下移特效')"
              :disabled="item.clip.effects?.at(-1)?.id === effect.id"
              @click="reorderEffect(effect.id, 1)"
            >
              ↓
            </button>
            <button
              :title="tr('移除特效')"
              @click="one({ action: 'remove_effect', effectId: effect.id })"
            >
              ×
            </button>
          </div>
          <label
            v-for="(d, key) in EFFECT_TEMPLATES.find((e) => e.id === effect.templateId)?.parameters"
            :key="key"
            class="property-row"
            ><span>{{ tr(d.label || key) }}</span
            ><select
              v-if="d.type === 'enum'"
              :value="effect.parameters[key]"
              @change="effectParameter(effect.id, String(key), string($event))"
            >
              <option v-for="v in d.values" :key="v" :value="v">
                {{ tr(optionLabels[v] || v) }}
              </option></select
            ><input
              v-else
              type="number"
              :min="d.min"
              :max="d.max"
              :step="effect.templateId === 'blur' ? 1 : 0.1"
              :value="sampleProperty(item, `effects.${effect.id}.${key}`, local(item))"
              @change="effectParameter(effect.id, String(key), number($event))"
            /><button
              v-if="d.keyframe"
              :class="{ active: item.clip.automation?.[`effects.${effect.id}.${key}`] }"
              :title="tr(`${d.label}关键帧`)"
              @click.prevent="effectKeyframe(effect.id, String(key))"
            >
              ◇
            </button></label
          >
        </div>
        <div class="group-heading">
          <span>{{ tr('相邻片段转场') }}</span>
        </div>
        <label class="property-row"
          ><span>{{ tr('类型') }}</span
          ><select v-model="transition">
            <option v-for="t in TRANSITION_TEMPLATES" :key="t.id" :value="t.id">
              {{ tr(t.name) }}
            </option>
          </select></label
        >
        <label class="property-row"
          ><span>{{ tr('时长（秒）') }}</span
          ><input v-model.number="transitionDuration" type="number" min="0.05" step="0.1"
        /></label>
        <label v-if="transition === 'fade'" class="property-row"
          ><span>{{ tr('颜色') }}</span
          ><select v-model="transitionColor">
            <option value="#000000">{{ tr('黑色') }}</option>
            <option value="#ffffff">{{ tr('白色') }}</option>
          </select></label
        >
        <label v-if="transition === 'wipe' || transition === 'slide'" class="property-row"
          ><span>{{ tr('方向') }}</span
          ><select v-model="transitionDirection">
            <option value="left">{{ tr('向左') }}</option>
            <option value="right">{{ tr('向右') }}</option>
            <option value="up">{{ tr('向上') }}</option>
            <option value="down">{{ tr('向下') }}</option>
          </select></label
        >
        <button class="subtle-button" :disabled="!nextItem" @click="addTransition">
          {{ tr('添加到下一个片段') }}
        </button>
        <div class="text-edit-hint">
          {{ tr('需要同轨相邻片段，并在剪辑点两侧保留足够素材余量。') }}
        </div>
        <div v-for="t in transitions" :key="t.id" class="property-row">
          <span
            >{{ tr(TRANSITION_TEMPLATES.find((p) => p.id === t.templateId)?.name) }} ·
            {{ seconds(t.duration) }}s</span
          >
          <input
            type="number"
            min="0.05"
            step="0.1"
            :aria-label="tr('转场时长')"
            :value="seconds(t.duration)"
            @change="
              emit('command', {
                action: 'update_transition',
                transitionId: t.id,
                durationSeconds: number($event)
              })
            "
          />
          <select
            v-if="t.templateId === 'fade'"
            :aria-label="tr('转场颜色')"
            :value="t.parameters.color"
            @change="
              emit('command', {
                action: 'update_transition',
                transitionId: t.id,
                parameters: { color: string($event) as '#000000' | '#ffffff' }
              })
            "
          >
            <option value="#000000">{{ tr('黑色') }}</option>
            <option value="#ffffff">{{ tr('白色') }}</option>
          </select>
          <select
            v-if="t.templateId === 'wipe' || t.templateId === 'slide'"
            :aria-label="tr('转场方向')"
            :value="t.parameters.direction"
            @change="
              emit('command', {
                action: 'update_transition',
                transitionId: t.id,
                parameters: { direction: string($event) as 'left' | 'right' | 'up' | 'down' }
              })
            "
          >
            <option value="left">{{ tr('向左') }}</option>
            <option value="right">{{ tr('向右') }}</option>
            <option value="up">{{ tr('向上') }}</option>
            <option value="down">{{ tr('向下') }}</option>
          </select>
          <button @click="emit('command', { action: 'remove_transition', transitionId: t.id })">
            {{ tr('移除') }}
          </button>
        </div>
      </template>
      <template v-if="tab === 'canvas'">
        <div class="group-heading">
          <span>{{ tr('作品画布') }}</span
          ><Icon name="monitor" :size="16" />
        </div>
        <div class="ratio-presets">
          <button
            v-for="ratio in [
              { label: '16:9', w: 1920, h: 1080 },
              { label: '9:16', w: 1080, h: 1920 },
              { label: '1:1', w: 1080, h: 1080 }
            ]"
            :key="ratio.label"
            :class="{
              active: project.canvas.width === ratio.w && project.canvas.height === ratio.h
            }"
            @click="emit('canvas', ratio.w, ratio.h)"
          >
            <span :style="{ aspectRatio: `${ratio.w}/${ratio.h}` }" />{{ ratio.label }}
          </button>
        </div>
        <label class="property-row"
          ><span>{{ tr('宽度') }}</span
          ><input
            :aria-label="tr('画布宽度')"
            type="number"
            step="2"
            :value="project.canvas.width"
            @change="emit('canvas', number($event), project.canvas.height)"
        /></label>
        <label class="property-row"
          ><span>{{ tr('高度') }}</span
          ><input
            :aria-label="tr('画布高度')"
            type="number"
            step="2"
            :value="project.canvas.height"
            @change="emit('canvas', project.canvas.width, number($event))"
        /></label>
        <label class="property-row"
          ><span>{{ tr('帧率') }}</span
          ><select
            :aria-label="tr('帧率')"
            :value="`${project.frameRate.numerator}/${project.frameRate.denominator}`"
            @change="
              emit('command', {
                action: 'configure_project',
                frameRate: {
                  numerator: Number(string($event).split('/')[0]),
                  denominator: Number(string($event).split('/')[1])
                }
              })
            "
          >
            <option
              v-for="rate in [
                [24, 1],
                [25, 1],
                [30000, 1001],
                [30, 1],
                [50, 1],
                [60000, 1001],
                [60, 1]
              ]"
              :key="rate.join('/')"
              :value="rate.join('/')"
            >
              {{ (rate[0] / rate[1]).toFixed(rate[1] === 1 ? 0 : 2) }} fps
            </option>
          </select></label
        >
        <button
          v-if="asset?.width && asset?.height"
          class="subtle-button"
          @click="emit('canvas', asset.width, asset.height)"
        >
          {{ tr('匹配所选素材尺寸') }}
        </button>
        <button
          v-else-if="item?.clip.html"
          class="subtle-button"
          @click="emit('canvas', item.clip.html.width, item.clip.html.height)"
        >
          {{ tr('匹配所选动画尺寸') }}
        </button>
        <div class="text-edit-hint">
          {{ tr('画布决定导出尺寸。预览缩放与显示清晰度独立调整。') }}
        </div>
      </template>
    </fieldset>
  </aside>
</template>
