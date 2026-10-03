<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, ref, watch } from 'vue';
import type { VideoCutClient } from '../../packages/client/index.mjs';
import { useI18n } from './i18n';
import Icon from './Icon.vue';

const props = defineProps<{
  client: VideoCutClient;
  result: { sessionId: string; path: string } | null;
  progress: { message: string; percent?: number } | null;
  cancellable: boolean;
}>();
const emit = defineEmits<{ close: []; cancel: [] }>();
const { tr } = useI18n();
const dialog = ref<HTMLDialogElement>();
const video = ref<HTMLVideoElement>();
const opening = ref(false),
  copied = ref(false),
  previewFailed = ref(false);
const error = ref('');
const isVideo = computed(() => /\.(mp4|webm)$/i.test(props.result?.path || ''));
const name = computed(() => props.result?.path.split(/[/\\]/).pop() || '');
const url = computed(() =>
  props.result && isVideo.value
    ? props.client.exportOutputUrl(props.result.sessionId, props.result.path)
    : ''
);
let previousFocus: HTMLElement | null = null;
function close(event?: Event) {
  event?.preventDefault();
  if (props.progress) return;
  video.value?.pause();
  dialog.value?.close();
  emit('close');
  previousFocus?.focus();
}
watch(
  () => [props.result, props.progress] as const,
  async ([result, progress], previous) => {
    if (result !== previous?.[0]) {
      error.value = '';
      copied.value = false;
      previewFailed.value = false;
    }
    if (!result && !progress) {
      video.value?.pause();
      dialog.value?.close();
      previousFocus?.focus();
      return;
    }
    if (!dialog.value?.open) previousFocus = document.activeElement as HTMLElement;
    await nextTick();
    if ((props.result || props.progress) && !dialog.value?.open) dialog.value?.showModal();
  },
  { flush: 'post' }
);
async function reveal() {
  const result = props.result;
  if (!result || opening.value) return;
  opening.value = true;
  error.value = '';
  try {
    await props.client.revealExport(result.sessionId, result.path);
  } catch {
    if (props.result === result) error.value = '无法打开所在目录，请复制路径后在文件管理器中打开。';
  } finally {
    opening.value = false;
  }
}
async function copyPath() {
  const result = props.result;
  if (!result) return;
  try {
    await navigator.clipboard.writeText(result.path);
    if (props.result === result) {
      copied.value = true;
      error.value = '';
    }
  } catch {
    if (props.result === result) error.value = '无法复制路径，请选中下方路径手动复制。';
  }
}
onBeforeUnmount(() => {
  video.value?.pause();
  dialog.value?.close();
});
</script>

<template>
  <Teleport to="body">
    <dialog
      ref="dialog"
      class="help-dialog panel export-dialog"
      tabindex="-1"
      :class="{ 'export-complete-dialog': !!result }"
      aria-labelledby="export-complete-title"
      @cancel="close"
      @keydown.stop
      @keydown.esc.prevent.stop="close"
      @paste.stop
    >
      <div class="panel-heading">
        <strong id="export-complete-title">{{ tr(progress ? '正在导出' : '导出完成') }}</strong>
        <button v-if="!progress" :aria-label="tr('关闭提示')" @click="close">
          <Icon name="close" />
        </button>
      </div>
      <div v-if="progress" class="export-progress" role="status" aria-live="polite">
        <p>{{ tr(progress.message) }}</p>
        <div class="export-progress-meter">
          <progress :value="progress.percent" max="100" :aria-label="tr('导出进度')" />
          <span v-if="progress.percent !== undefined">{{ Math.round(progress.percent) }}%</span>
        </div>
        <div class="export-result-actions">
          <button v-if="cancellable" @click="emit('cancel')">{{ tr('取消导出') }}</button>
        </div>
      </div>
      <div v-else-if="result" class="export-result">
        <strong class="export-result-name">{{ name }}</strong>
        <video
          v-if="isVideo"
          :key="url"
          ref="video"
          :src="url"
          controls
          playsinline
          preload="metadata"
          :aria-label="tr('导出视频预览')"
          @error="previewFailed = true"
        />
        <p v-if="previewFailed" role="status">
          {{ tr('浏览器无法播放此视频，可以打开所在目录查看。') }}
        </p>
        <p v-else>
          {{
            tr(isVideo ? '视频已保存，可以直接播放查看。' : '作品已保存，可以打开所在目录查看。')
          }}
        </p>
        <code class="export-result-path">{{ result.path }}</code>
        <p v-if="error" class="export-result-error" role="alert">{{ tr(error) }}</p>
        <div class="export-result-actions">
          <button @click="copyPath">{{ tr(copied ? '路径已复制' : '复制路径') }}</button>
          <button class="primary" :disabled="opening" @click="reveal">
            <Icon name="folder" />{{ tr(opening ? '正在打开…' : '打开所在目录') }}
          </button>
          <button @click="close">{{ tr('完成') }}</button>
        </div>
      </div>
    </dialog>
  </Teleport>
</template>

<style scoped>
.export-dialog {
  width: min(680px, calc(100vw - 32px));
  max-height: calc(100dvh - 32px);
  overflow-y: auto;
  padding: 0;
  color: #e8ecef;
}
.export-dialog::backdrop {
  background: #0009;
}
.export-result {
  padding: 18px;
}
.export-progress {
  padding: 18px;
}
.export-progress p {
  display: block;
  margin: 0 0 18px;
  line-height: 1.6;
}
.export-progress-meter {
  display: flex;
  align-items: center;
  gap: 14px;
  font-variant-numeric: tabular-nums;
}
.export-progress-meter progress {
  width: 100%;
  height: 10px;
  accent-color: #34d1bf;
}
.export-progress-meter span {
  min-width: 40px;
  text-align: right;
}
.export-result-name {
  display: block;
  margin-bottom: 14px;
  overflow-wrap: anywhere;
}
.export-result video {
  display: block;
  width: 100%;
  max-height: 48dvh;
  background: #080a0c;
  border-radius: 6px;
}
.export-result p {
  display: block;
  margin: 14px 0;
  font-size: 13px;
  line-height: 1.6;
}
.export-result-path {
  display: block;
  padding: 10px 12px;
  border-radius: 6px;
  background: #171b20;
  color: #b5bdc5;
  font-size: 12px;
  line-height: 1.6;
  overflow-wrap: anywhere;
  user-select: text;
}
.export-result .export-result-error {
  color: #ff8585;
}
.export-result-actions {
  display: flex;
  flex-wrap: wrap;
  justify-content: flex-end;
  gap: 8px;
  margin-top: 18px;
}
</style>
