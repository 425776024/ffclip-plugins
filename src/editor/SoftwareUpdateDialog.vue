<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, shallowRef } from 'vue';
import type { VideoCutClient } from '../../packages/client/index.mjs';
import type { SoftwareUpdateStatus } from '../../packages/client/types';
import { useI18n } from './i18n';
import Icon from './Icon.vue';

const props = defineProps<{ client: VideoCutClient; blocked: boolean }>();
const emit = defineEmits<{ install: [] }>();
const { tr } = useI18n();
const dialog = ref<HTMLDialogElement>();
const promptInput = ref<HTMLTextAreaElement>();
const status = shallowRef<SoftwareUpdateStatus>();
const checking = ref(false),
  submitting = ref(false),
  agentMode = ref(false),
  copied = ref(false);
const error = ref('');
const installing = computed(() => submitting.value || status.value?.state === 'installing');
const installed = computed(() => status.value?.state === 'installed');
const available = computed(() => status.value?.available && !installed.value);
const prompt = computed(() =>
  tr(
    '请帮我将本机 ffclip 更新到官方最新版（@ffclip-com/videocut@latest）。当前预览地址：{url}。{installation}先保存当前作品为完整 .vcutweb，记录启动参数和已授权素材目录，等待导出、配音和分析任务完成。根据实际的全局、本地或 npx 安装方式更新官方 npm 包；不要覆盖源码工程。保留已有 MCP 服务及参数，将 ffclip 启动配置中的固定版本改为 @latest。只重启 ffclip 服务或对应 MCP 连接，重新打开刚保存的作品，并验证实际运行版本和预览内容。不要关闭其他服务，不要丢弃未保存的编辑。',
    {
      url: window.location.href,
      installation: installed.value
        ? tr('自动更新已安装 {version}，无需重复安装。', {
            version: status.value?.installedVersion || ''
          }) +
          tr('已保存作品：{paths}。', {
            paths: status.value?.savedProjects.map((p) => p.path).join('; ') || '—'
          })
        : ''
    }
  )
);
const controller = new AbortController();
const interval = 6 * 60 * 60 * 1000;
let alive = true,
  lastRequest = 0,
  nextRequest = 0,
  dismissedUntil = 0;
let timer: ReturnType<typeof setTimeout> | undefined;
let previousFocus: HTMLElement | null = null;

function snoozeKey() {
  return `ffclip:update:${status.value?.currentVersion}:${status.value?.latestVersion}`;
}
function snoozed() {
  try {
    return Date.now() < Math.max(dismissedUntil, Number(localStorage.getItem(snoozeKey())) || 0);
  } catch {
    return Date.now() < dismissedUntil;
  }
}
async function show(manual = false) {
  if (!alive || dialog.value?.open || document.hidden || props.blocked) return;
  if (document.querySelector('.dialog-backdrop, dialog[open]')) return;
  if (!manual && (!available.value || snoozed())) return;
  previousFocus = document.activeElement as HTMLElement;
  await nextTick();
  if (alive) dialog.value?.showModal();
}
function dismiss(event?: Event) {
  event?.preventDefault();
  if (installing.value) return;
  dismissedUntil = Date.now() + 24 * 60 * 60 * 1000;
  try {
    localStorage.setItem(snoozeKey(), String(dismissedUntil));
  } catch {}
  dialog.value?.close();
  agentMode.value = false;
  previousFocus?.focus();
}
async function refresh(manual = false) {
  if (!alive || checking.value) return;
  checking.value = true;
  lastRequest = Date.now();
  try {
    const value = await props.client.request<SoftwareUpdateStatus>(
      manual ? '/updates?refresh=1' : '/updates',
      { signal: controller.signal }
    );
    if (!alive) return;
    if (value.latestVersion !== status.value?.latestVersion) dismissedUntil = 0;
    status.value = value;
    error.value = manual && value.checkError ? tr('暂时无法检查最新版，请稍后重试。') : '';
    nextRequest = Date.now() + (value.checkError ? 30 * 60 * 1000 : interval);
    if (manual) await show(true);
    else await show();
  } catch (e) {
    if (!alive) return;
    nextRequest = Date.now() + 30 * 60 * 1000;
    if (manual) {
      error.value = tr(e instanceof Error ? e.message : String(e));
      await show(true);
    }
  } finally {
    checking.value = false;
  }
}
async function install() {
  if (installing.value || !status.value?.canInstall || props.blocked) return;
  submitting.value = true;
  error.value = '';
  emit('install');
  try {
    const value = await props.client.request<SoftwareUpdateStatus>('/updates/install', {
      method: 'POST',
      body: '{}',
      signal: controller.signal
    });
    if (alive) status.value = value;
  } catch (e) {
    if (alive) error.value = tr(e instanceof Error ? e.message : String(e));
  } finally {
    submitting.value = false;
    schedule();
  }
}
async function copyPrompt() {
  agentMode.value = true;
  copied.value = false;
  await nextTick();
  try {
    await navigator.clipboard.writeText(prompt.value);
    copied.value = true;
  } catch {
    promptInput.value?.focus();
    promptInput.value?.select();
  }
}
function schedule() {
  clearTimeout(timer);
  if (!alive) return;
  timer = setTimeout(
    async () => {
      if (!document.hidden) {
        if ((installing.value && Date.now() - lastRequest >= 1000) || Date.now() >= nextRequest)
          await refresh();
        await show();
      }
      schedule();
    },
    installing.value || (available.value && !snoozed()) ? 1000 : 60000
  );
}
function visibilityChanged() {
  if (!document.hidden) {
    if (installing.value || Date.now() >= nextRequest) void refresh();
    else void show();
    schedule();
  }
}
onMounted(() => {
  void refresh().finally(schedule);
  document.addEventListener('visibilitychange', visibilityChanged);
});
onBeforeUnmount(() => {
  alive = false;
  controller.abort();
  clearTimeout(timer);
  dialog.value?.close();
  document.removeEventListener('visibilitychange', visibilityChanged);
});
</script>

<template>
  <button
    class="icon-button software-update-trigger"
    :class="{ 'update-available': available }"
    :title="tr('检查更新')"
    :aria-label="tr('检查更新')"
    :disabled="blocked || checking"
    @click="refresh(true)"
  >
    <Icon name="update" :size="18" />
  </button>
  <Teleport to="body">
    <dialog
      ref="dialog"
      class="help-dialog panel update-dialog"
      aria-labelledby="software-update-title"
      @cancel="dismiss"
      @keydown.stop
      @paste.stop
    >
      <div class="panel-heading">
        <strong id="software-update-title">{{
          tr(installed ? '更新已安装' : available ? '发现新版本' : '检查更新')
        }}</strong>
        <button :disabled="installing" :aria-label="tr('关闭提示')" @click="dismiss">
          <Icon name="close" />
        </button>
      </div>
      <p v-if="status" class="update-versions">
        {{
          tr('当前运行：{current} · 最新版：{latest}', {
            current: status.currentVersion,
            latest: status.latestVersion || '—'
          })
        }}
      </p>
      <p v-if="installed">
        {{
          tr(
            '已安装 {version}，当前作品已保存。请重启 ffclip 服务或 MCP 连接；刷新页面不会重启服务。',
            { version: status?.installedVersion || '' }
          )
        }}
      </p>
      <p v-else-if="installing" role="status">
        {{
          tr(
            status?.phase === 'saving'
              ? '正在保存完整作品，随后安装更新…'
              : '正在安装官方最新版，请保持本地服务运行…'
          )
        }}
      </p>
      <p v-else-if="available">
        {{
          tr(
            '自动更新会先保存完整作品，也可以把提示词发给 Codex、WorkBuddy、Qoder 等 AI 助手更新。安装完成后需要重启服务。'
          )
        }}
      </p>
      <p v-else-if="status && !status.checkError">{{ tr('当前已是最新版。') }}</p>
      <p v-if="available && !status?.canInstall">
        {{ tr('当前从源码运行，请通过 AI 助手安装官方包；自动更新不会覆盖源码工程。') }}
      </p>
      <p v-if="error || status?.error" class="update-error" role="alert">
        {{ tr(error || status?.error) }}
      </p>
      <div v-if="status?.savedProjects.length" class="update-saved">
        <strong>{{ tr('已保存作品') }}</strong>
        <div v-for="saved in status.savedProjects" :key="saved.path">
          <code>{{ saved.path }}</code>
        </div>
      </div>
      <template v-if="agentMode">
        <p class="update-agent-hint" role="status">
          {{
            tr(
              copied
                ? '更新提示词已复制，粘贴到 AI 助手对话中发送即可。'
                : '将以下提示词复制到 AI 助手对话中发送即可。'
            )
          }}
        </p>
        <textarea ref="promptInput" :value="prompt" readonly :aria-label="tr('更新提示词')" />
      </template>
      <div class="update-actions">
        <button :disabled="installing" @click="dismiss">
          {{ tr(installed ? '稍后重启' : available ? '稍后提醒' : '关闭') }}
        </button>
        <button v-if="available || installed" :disabled="installing" @click="copyPrompt">
          {{ tr(installed ? '交给 AI 助手重启' : '交给 AI 助手更新') }}
        </button>
        <button
          v-if="available && status?.canInstall"
          class="primary"
          :disabled="installing || blocked"
          @click="install"
        >
          {{ tr(installing ? '安装中…' : status?.state === 'error' ? '重试更新' : '自动更新') }}
        </button>
      </div>
    </dialog>
  </Teleport>
</template>

<style scoped>
.update-available {
  color: #34d1bf;
}
.update-dialog {
  width: min(560px, calc(100vw - 40px));
  max-height: calc(100vh - 40px);
  padding: 0 0 18px;
  color: #e8ecef;
}
.update-dialog::backdrop {
  background: #0008;
}
.update-dialog p {
  display: block;
  line-height: 1.6;
}
.update-versions {
  font-size: 12px;
}
.update-dialog .update-error {
  color: #ff8585;
  overflow-wrap: anywhere;
  white-space: pre-wrap;
}
.update-dialog textarea {
  display: block;
  width: calc(100% - 36px);
  min-height: 150px;
  margin: 0 18px;
  resize: vertical;
  font-size: 12px;
  line-height: 1.6;
}
.update-saved {
  margin: 18px;
  font-size: 12px;
  overflow-wrap: anywhere;
}
.update-actions {
  display: flex;
  flex-wrap: wrap;
  justify-content: flex-end;
  gap: 8px;
  margin: 24px 18px 0;
}
</style>
