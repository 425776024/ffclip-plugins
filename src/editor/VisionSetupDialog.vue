<script setup lang="ts">
import { useI18n } from './i18n';
const { tr } = useI18n();
import { computed, ref, shallowRef, onMounted, onBeforeUnmount, nextTick } from 'vue';
import type { VideoCutClient } from '../../packages/client/index.mjs';
import type { VisionModelStatus } from '../../packages/client/types';
const props = withDefaults(defineProps<{ client: VideoCutClient; autoPrompt?: boolean }>(), {
  autoPrompt: true
});
const status = shallowRef<VisionModelStatus>();
const visible = ref(false),
  busy = ref(false),
  error = ref('');
const primary = ref<HTMLButtonElement>();
let alive = true,
  timer: ReturnType<typeof setTimeout> | undefined;
let sequence = 0;
let previousFocus: HTMLElement | null = null;
const downloading = computed(() => status.value?.install.state === 'downloading');
const progress = computed(() => Math.round((status.value?.install.progress || 0) * 100));
const bytes = (value: number) => `${(value / 1024 / 1024).toFixed(1)} MB`;
const downloaded = computed(
  () => status.value?.install.files.reduce((n, f) => n + f.downloaded, 0) || 0
);
function show() {
  if (!visible.value) previousFocus = document.activeElement as HTMLElement;
  visible.value = true;
  void nextTick(() => primary.value?.focus());
}
function close() {
  sequence++;
  clearTimeout(timer);
  visible.value = false;
  previousFocus?.focus();
}
async function refresh(initial = false, force = false) {
  clearTimeout(timer);
  if (busy.value) {
    timer = setTimeout(() => void refresh(initial, force), 700);
    return;
  }
  const current = ++sequence;
  try {
    const value = await props.client.visionModelStatus();
    if (!alive || current !== sequence) return;
    status.value = value;
    if (
      force ||
      value.promptRequested ||
      (initial &&
        props.autoPrompt !== false &&
        (value.consent === 'unasked' || (value.consent === 'enabled' && !value.installed)))
    )
      show();
    if (value.consent === 'declined' && !value.promptRequested) close();
    error.value = '';
  } catch (e) {
    if (!alive || current !== sequence) return;
    error.value = e instanceof Error ? e.message : String(e);
    if (initial && props.autoPrompt !== false) show();
  }
  if (alive && visible.value) timer = setTimeout(() => void refresh(), 700);
}
async function decide(enabled: boolean) {
  if (busy.value) return;
  sequence++;
  busy.value = true;
  error.value = '';
  try {
    const value = await props.client.request<VisionModelStatus>('/vision/setup', {
      method: 'POST',
      body: JSON.stringify({ enabled })
    });
    if (!alive) return;
    status.value = value;
    if (!enabled) close();
    else void refresh();
  } catch (e) {
    if (alive) error.value = e instanceof Error ? e.message : String(e);
  } finally {
    busy.value = false;
  }
}
async function cancel() {
  if (busy.value) return;
  sequence++;
  busy.value = true;
  try {
    await props.client.request('/vision/model/install', { method: 'DELETE' });
    await refresh();
  } catch (e) {
    error.value = e instanceof Error ? e.message : String(e);
  } finally {
    busy.value = false;
  }
}
function keydown(event: KeyboardEvent) {
  if (event.key !== 'Tab') return;
  const buttons = Array.from(
    (event.currentTarget as HTMLElement).querySelectorAll<HTMLElement>(
      'button:not(:disabled), a[href]'
    )
  );
  const first = buttons[0],
    last = buttons.at(-1);
  if (event.shiftKey && document.activeElement === first) {
    event.preventDefault();
    last?.focus();
  } else if (!event.shiftKey && document.activeElement === last) {
    event.preventDefault();
    first?.focus();
  }
}
const requested = () => void refresh(false, true);
onMounted(() => {
  window.addEventListener('videocut-vision-setup', requested);
  void refresh(true);
});
onBeforeUnmount(() => {
  alive = false;
  clearTimeout(timer);
  window.removeEventListener('videocut-vision-setup', requested);
});
</script>

<template>
  <div v-if="visible" class="dialog-backdrop">
    <section
      class="help-dialog panel vision-setup"
      role="dialog"
      aria-modal="true"
      aria-labelledby="vision-setup-title"
      @keydown.stop="keydown"
    >
      <div class="panel-heading">
        <strong id="vision-setup-title">{{ tr('本地视觉理解') }}</strong>
      </div>
      <p>{{ tr('是否启用本地视觉理解，让外部 Agent 理解图像和视频？') }}</p>
      <p>
        {{
          tr('同意后下载 FastVLM 模型{size}', {
            size: status ? tr('（约 {size}）', { size: bytes(status.size) }) : ''
          })
        }}
      </p>
      <p>
        {{ tr('素材在本机处理，无需上传；编辑界面不增加视觉分析入口。') }}
      </p>
      <p class="vision-license">
        {{ tr('模型许可：')
        }}<a
          href="https://huggingface.co/onnx-community/FastVLM-0.5B-ONNX/blob/ca35eb9373f8a8761df0855fca19dea330f2407a/LICENSE"
          target="_blank"
          rel="noopener noreferrer"
          >Apple AMLR</a
        >
      </p>
      <template v-if="downloading">
        <label for="vision-download">{{
          tr('正在下载 {progress}% · {downloaded} / {size}', {
            progress,
            downloaded: bytes(downloaded),
            size: bytes(status!.size)
          })
        }}</label>
        <progress id="vision-download" :value="status!.install.progress" max="1" />
        <small>{{
          tr(status!.install.files.find((f) => f.downloaded < f.size)?.path || '正在校验模型…')
        }}</small>
      </template>
      <p v-else-if="status?.installed && status.consent === 'enabled'" role="status">
        {{ tr('模型已准备就绪，外部 Agent 可以开始使用。') }}
      </p>
      <p v-else-if="status?.install.state === 'cancelled'" role="status">
        {{ tr('下载已取消。重试会复用已校验的文件。') }}
      </p>
      <p v-if="error || status?.install.error" class="vision-error" role="alert">
        {{ tr(error || status?.install.error) }}
      </p>
      <div class="vision-actions">
        <template v-if="downloading">
          <button :disabled="busy" @click="cancel">{{ tr('取消下载') }}</button>
          <button ref="primary" @click="close">{{ tr('后台下载') }}</button>
        </template>
        <template v-else-if="status?.installed && status.consent === 'enabled'">
          <button :disabled="busy" @click="decide(false)">{{ tr('停用') }}</button>
          <button ref="primary" class="primary" @click="close">{{ tr('完成') }}</button>
        </template>
        <template v-else>
          <button :disabled="busy" @click="decide(false)">{{ tr('暂不启用') }}</button>
          <button ref="primary" class="primary" :disabled="busy || !status" @click="decide(true)">
            {{ tr(status?.consent === 'enabled' ? '重试下载' : '同意并下载') }}
          </button>
          <button v-if="!status" :disabled="busy" @click="refresh(true)">
            {{ tr('重试连接') }}
          </button>
        </template>
      </div>
    </section>
  </div>
</template>

<style scoped>
.vision-setup {
  max-width: 480px;
  width: calc(100vw - 40px);
}
.vision-setup progress {
  width: calc(100% - 36px);
  height: 12px;
  accent-color: #34d1bf;
  margin: 12px 18px;
}
.vision-setup label,
.vision-setup small {
  display: block;
  font-size: 12px;
  overflow-wrap: anywhere;
  margin: 0 18px;
}
.vision-license {
  font-size: 12px;
}
.vision-license a {
  color: #34d1bf;
}
.vision-actions {
  display: flex;
  gap: 10px;
  justify-content: flex-end;
  margin: 24px 18px 0;
}
.vision-error {
  color: #ff8585;
}
</style>
