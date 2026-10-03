<script setup lang="ts">
import { useI18n } from './i18n';
const { tr } = useI18n();
import { computed, onBeforeUnmount, onMounted, ref, shallowRef } from 'vue';
import type { Asset, Snapshot, VideoCutClient } from '../../packages/client/index.mjs';
import type { TtsBackend, TtsCatalog, TtsInstallStatus, TtsJob } from '../../packages/client/types';
import Icon from './Icon.vue';

const props = defineProps<{
  client: VideoCutClient;
  snapshot: Snapshot | null;
  job: TtsJob | null;
  startSeconds: number;
  busy: boolean;
  mediaUrl: (id: string) => string;
}>();
const emit = defineEmits<{
  queued: [job: TtsJob];
  append: [asset: Asset];
}>();
const tab = ref<'synthesis' | 'assets'>('synthesis');
const content = ref('');
const voice = ref('zf_001');
const speed = ref(1);
const backend = ref<TtsBackend>('auto');
const catalog = shallowRef<TtsCatalog>();
const installation = shallowRef<TtsInstallStatus>();
const loading = ref(false);
const submitting = ref(false);
const failure = ref('');
const webgpuAvailable = 'gpu' in navigator;
let timer: ReturnType<typeof setInterval> | undefined;
let polling = false;
let disposed = false;

const active = computed(() => ['queued', 'running'].includes(props.job?.state || ''));
const generatedAssetImported = computed(() =>
  Boolean(
    props.job?.asset &&
    props.snapshot?.project.assets.some((entry) => entry.id === props.job?.asset?.id)
  )
);
const downloading = computed(() => installation.value?.state === 'downloading');
const voiceInstalled = computed(
  () =>
    catalog.value?.installedVoices?.includes(voice.value) ||
    catalog.value?.voices.find((entry) => entry.id === voice.value)?.installed
);
const fp32Installed = computed(() => Boolean(catalog.value?.installedDtypes.includes('fp32')));
const installed = computed(() => Boolean(fp32Installed.value && voiceInstalled.value));
const modelStatus = computed(() => {
  if (!catalog.value) return loading.value ? '正在读取模型状态…' : '模型状态未读取';
  if (!voiceInstalled.value) return '所选音色尚未下载';
  if (!installed.value) return 'FP32 模型尚未下载';
  return 'FP32 模型与音色已就绪';
});
const canGenerate = computed(
  () =>
    !!props.snapshot &&
    !!content.value.trim() &&
    content.value.length <= 8000 &&
    installed.value &&
    !props.busy &&
    !active.value &&
    !submitting.value &&
    Number.isFinite(speed.value) &&
    speed.value >= 0.5 &&
    speed.value <= 2
);
const progress = (value?: number) => Math.round(Math.max(0, Math.min(1, value || 0)) * 100);
const actualBackend = computed(
  () => props.job?.actualBackend || (props.job?.backend !== 'auto' ? props.job?.backend : undefined)
);
const jobLabel = computed(() => {
  if (!props.job) return '';
  if (props.job.state === 'queued') return '等待浏览器开始合成…';
  if (['completed', 'conflict'].includes(props.job.state) && generatedAssetImported.value)
    return '语音已加入当前作品';
  if (props.job.state === 'completed') return '语音已生成';
  if (props.job.state === 'conflict') return '语音已生成，作品在合成期间已修改';
  if (props.job.state === 'cancelled') return '已取消合成';
  if (props.job.state === 'error') return '合成未完成';
  const phases: Record<string, string> = {
    loading: '正在加载语音模型…',
    model: '正在加载语音模型…',
    initializing: '正在初始化推理…',
    initialize: '正在初始化推理…',
    frontend: '正在处理文本…',
    'load-model': '正在加载语音模型…',
    phonemizing: '正在处理文本…',
    synthesizing: '正在合成语音…',
    synthesis: '正在合成语音…',
    synthesize: '正在合成语音…',
    fallback: '正在切换到 WASM 推理…',
    encode: '正在编码语音…',
    complete: '正在保存语音…',
    saving: '正在保存语音…'
  };
  return phases[props.job.phase || ''] || '正在合成语音…';
});
const positionLabel = computed(() => {
  const value = Math.max(0, props.startSeconds);
  return `${Math.floor(value / 60)
    .toString()
    .padStart(2, '0')}:${(value % 60).toFixed(2).padStart(5, '0')}`;
});

function message(error: unknown) {
  const code = (error as { code?: string })?.code;
  if (code === 'TTS_MODEL_REQUIRED') return '请先下载所选模型和音色。';
  if (code === 'BROWSER_REQUIRED') return '浏览器预览尚未连接，请稍候重试。';
  return error instanceof Error ? error.message : String(error);
}

function watchInstallation() {
  clearInterval(timer);
  if (downloading.value && !disposed) timer = setInterval(() => void pollInstallation(), 1000);
}

async function loadCatalog() {
  loading.value = true;
  try {
    const value = await props.client.request<TtsCatalog>('/tts/catalog');
    if (disposed) return;
    catalog.value = value;
    installation.value = value.install;
    if (!value.voices.some((entry) => entry.id === voice.value))
      voice.value = value.defaultVoice || value.voices[0]?.id || 'zf_001';
    watchInstallation();
  } catch (error) {
    failure.value = message(error);
  } finally {
    loading.value = false;
  }
}

async function pollInstallation() {
  if (polling || disposed) return;
  polling = true;
  try {
    const value = await props.client.request<TtsInstallStatus>('/tts/model/install');
    if (disposed) return;
    installation.value = value;
    if (value.state !== 'downloading') {
      clearInterval(timer);
      await loadCatalog();
    }
  } catch (error) {
    failure.value = message(error);
  } finally {
    polling = false;
  }
}

async function install() {
  if (loading.value || downloading.value || !catalog.value) return;
  failure.value = '';
  loading.value = true;
  try {
    installation.value = await props.client.request<TtsInstallStatus>('/tts/model/install', {
      method: 'POST',
      body: JSON.stringify({ dtype: 'fp32', voices: [voice.value] })
    });
    watchInstallation();
    if (!downloading.value) await loadCatalog();
  } catch (error) {
    failure.value = message(error);
  } finally {
    loading.value = false;
  }
}

async function cancelInstall() {
  try {
    installation.value = await props.client.request<TtsInstallStatus>('/tts/model/install', {
      method: 'DELETE'
    });
    clearInterval(timer);
    await loadCatalog();
  } catch (error) {
    failure.value = message(error);
  }
}

async function generate() {
  if (!canGenerate.value || !props.snapshot) return;
  submitting.value = true;
  failure.value = '';
  try {
    const job = await props.client.request<TtsJob>(`/sessions/${props.snapshot.id}/tts`, {
      method: 'POST',
      body: JSON.stringify({
        text: content.value.trim(),
        voice: voice.value,
        speed: speed.value,
        backend: backend.value,
        dtype: 'fp32',
        version: props.snapshot.version,
        startSeconds: props.startSeconds,
        insert: true
      })
    });
    emit('queued', job);
  } catch (error) {
    failure.value = message(error);
    if ((error as { code?: string })?.code === 'TTS_MODEL_REQUIRED') await loadCatalog();
  } finally {
    submitting.value = false;
  }
}

async function cancelJob() {
  if (!props.snapshot) return;
  try {
    const job = await props.client.request<TtsJob>(`/sessions/${props.snapshot.id}/tts-job`, {
      method: 'DELETE'
    });
    emit('queued', job);
  } catch (error) {
    failure.value = message(error);
  }
}

onMounted(() => void loadCatalog());
onBeforeUnmount(() => {
  disposed = true;
  clearInterval(timer);
});
</script>

<template>
  <aside class="library panel tts-library">
    <nav class="tts-tabs" :aria-label="tr('音频工具')">
      <button :class="{ active: tab === 'synthesis' }" @click="tab = 'synthesis'">
        <Icon name="text" :size="14" />{{ tr('语音合成') }}
      </button>
      <button :class="{ active: tab === 'assets' }" @click="tab = 'assets'">
        <Icon name="music" :size="14" />{{ tr('音频素材') }}
      </button>
    </nav>
    <div v-if="tab === 'synthesis'" class="tts-content">
      <div class="tts-heading">
        <strong>{{ tr('本地语音合成') }}</strong
        ><small>Kokoro</small>
      </div>
      <p class="tts-description">{{ tr('输入中英文配音，生成后加入音轨。') }}</p>
      <form class="tts-form" @submit.prevent="generate">
        <label class="tts-text-label" for="tts-text"
          >{{ tr('配音文本') }} <small>{{ content.length }} / 8000</small></label
        >
        <textarea
          id="tts-text"
          v-model="content"
          :placeholder="tr('输入要朗读的内容…')"
          rows="6"
          maxlength="8000"
          :disabled="active || submitting"
        />
        <label for="tts-voice">{{ tr('音色') }}</label>
        <select id="tts-voice" v-model="voice" :disabled="active || downloading || !catalog">
          <optgroup
            v-for="language in ['zh', 'en']"
            :key="language"
            :label="tr(language === 'zh' ? '中文' : '英文')"
          >
            <option
              v-for="entry in catalog?.voices.filter(
                (candidate) => candidate.language === language
              )"
              :key="entry.id"
              :value="entry.id"
            >
              {{ tr(entry.name)
              }}{{
                tr(
                  entry.installed || catalog?.installedVoices?.includes(entry.id) ? ' · 已下载' : ''
                )
              }}
            </option>
          </optgroup>
        </select>
        <div class="tts-settings">
          <label for="tts-speed"
            >{{ tr('语速') }} <span>{{ Number(speed).toFixed(2) }}×</span></label
          >
          <input
            id="tts-speed"
            v-model.number="speed"
            type="range"
            min="0.5"
            max="2"
            step="0.05"
            :disabled="active"
          />
        </div>
        <details class="tts-advanced">
          <summary>{{ tr('推理后端设置') }}</summary>
          <label for="tts-backend">{{ tr('推理后端') }}</label>
          <select id="tts-backend" v-model="backend" :disabled="active">
            <option value="auto">{{ tr('自动选择') }}</option>
            <option value="webgpu" :disabled="!webgpuAvailable">
              WebGPU{{ tr(webgpuAvailable ? '' : ' · 当前浏览器不可用') }}
            </option>
            <option value="wasm">WASM · CPU</option>
          </select>
        </details>
        <div class="tts-install">
          <template v-if="downloading">
            <span>{{
              tr('正在下载模型与音色 · {progress}%', { progress: progress(installation?.progress) })
            }}</span>
            <progress
              :value="installation?.progress || 0"
              max="1"
              :aria-label="tr('模型下载进度')"
            />
            <button type="button" @click="cancelInstall">{{ tr('取消下载') }}</button>
          </template>
          <template v-else>
            <span :class="{ ready: installed }">{{ tr(modelStatus) }}</span>
            <button
              v-if="!installed"
              type="button"
              :disabled="loading || active || !catalog"
              @click="install"
            >
              <Icon name="import" :size="14" />{{
                tr(loading ? '正在读取模型…' : '下载 FP32 模型与音色')
              }}
            </button>
            <button v-if="!catalog && !loading" type="button" @click="loadCatalog">
              {{ tr('重新读取') }}
            </button>
          </template>
          <small>{{ tr('Kokoro FP32 兼容模型') }}</small>
          <small>{{ tr('模型保存在本机；文本由当前浏览器合成。') }}</small>
        </div>
        <p v-if="failure || installation?.state === 'error'" class="tts-error" role="alert">
          {{ tr(failure || installation?.error) }}
        </p>
        <button type="submit" class="primary tts-generate" :disabled="!canGenerate">
          <Icon name="plus" :size="15" />{{
            tr(active ? '正在合成…' : submitting ? '正在提交…' : '生成并加入音轨')
          }}
        </button>
        <small class="tts-position">{{
          tr('插入到播放头 {position}', { position: positionLabel })
        }}</small>
      </form>
      <section v-if="job && job.state !== 'idle'" class="tts-job" aria-live="polite">
        <div class="tts-job-heading">
          <strong>{{ tr(jobLabel) }}</strong
          ><small v-if="active">{{ progress(job.progress) }}%</small>
        </div>
        <p v-if="job.text" class="tts-job-text">{{ job.text }}</p>
        <progress
          v-if="active"
          :value="job.progress || 0"
          max="1"
          :aria-label="tr('语音合成进度')"
        />
        <button v-if="active" @click="cancelJob">{{ tr('取消合成') }}</button>
        <p v-if="job.error" class="tts-error" role="alert">{{ tr(job.error) }}</p>
        <button
          v-if="
            ['conflict', 'completed'].includes(job.state) && job.asset && !generatedAssetImported
          "
          class="primary"
          :disabled="busy"
          @click="emit('append', job.asset)"
        >
          {{ tr('加入当前作品') }}
        </button>
        <audio
          v-if="generatedAssetImported && job.asset"
          :src="mediaUrl(job.asset.id)"
          controls
          preload="none"
          :aria-label="tr('试听生成语音')"
        />
        <small v-if="job.state === 'completed' && actualBackend"
          >{{ actualBackend === 'webgpu' ? 'WebGPU' : 'WASM' }} · {{ job.asset?.name }}</small
        >
      </section>
    </div>
    <div v-else class="tts-assets"><slot /></div>
  </aside>
</template>

<style scoped>
.tts-library {
  flex-direction: column;
  min-width: 0;
  min-height: 0;
  overflow: hidden;
}
.tts-tabs {
  display: flex;
  flex-shrink: 0;
  background: var(--header);
  border-bottom: 1px solid #08090a;
  padding: 3px 6px;
  gap: 4px;
}
.tts-tabs button {
  flex: 1;
}
.tts-content {
  padding: 14px 12px;
  overflow: auto;
  min-height: 0;
}
.tts-heading,
.tts-job-heading,
.tts-text-label,
.tts-settings label {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
}
.tts-heading small,
.tts-position,
.tts-description,
.tts-job > small,
.tts-install small,
.tts-text-label small {
  color: var(--muted);
}
.tts-description {
  margin: 7px 0 14px;
  line-height: 1.5;
}
.tts-form {
  display: flex;
  flex-direction: column;
  gap: 8px;
}
.tts-form textarea {
  width: 100%;
  resize: vertical;
  min-height: 100px;
  line-height: 1.7;
}
.tts-form select {
  width: 100%;
  min-height: 30px;
}
.tts-settings {
  margin: 5px 0;
}
.tts-settings label span {
  color: var(--accent);
  font-variant-numeric: tabular-nums;
}
.tts-settings input {
  width: 100%;
  margin: 12px 0 2px;
}
.tts-advanced {
  border-top: 1px solid var(--line);
  padding-top: 8px;
}
.tts-advanced summary {
  cursor: pointer;
  color: var(--muted);
  margin-bottom: 8px;
}
.tts-advanced label {
  display: block;
  margin: 9px 0 6px;
}
.tts-install {
  display: flex;
  flex-direction: column;
  align-items: stretch;
  gap: 8px;
  padding: 10px;
  margin-top: 4px;
  border: 1px solid var(--line);
  border-radius: 5px;
  background: #ffffff03;
}
.tts-install .ready {
  color: var(--accent);
}
.tts-install button {
  background: #ffffff08;
}
.tts-install small {
  font-size: 10px;
  line-height: 1.6;
}
.tts-generate {
  width: 100%;
  min-height: 33px;
  margin-top: 3px;
}
.tts-position {
  text-align: center;
}
.tts-error {
  color: #f1a3a3;
  margin: 0;
  line-height: 1.6;
  overflow-wrap: anywhere;
}
.tts-job {
  display: flex;
  flex-direction: column;
  gap: 9px;
  margin-top: 16px;
  padding-top: 14px;
  border-top: 1px solid var(--line);
}
.tts-job-text {
  color: var(--muted);
  line-height: 1.6;
  margin: 0;
  display: -webkit-box;
  -webkit-line-clamp: 3;
  -webkit-box-orient: vertical;
  overflow: hidden;
  overflow-wrap: anywhere;
}
.tts-job audio {
  width: 100%;
  height: 34px;
}
.tts-job small {
  overflow-wrap: anywhere;
}
progress {
  width: 100%;
  height: 5px;
  accent-color: var(--accent);
}
.tts-assets {
  min-height: 0;
  flex: 1;
  display: flex;
}
.tts-assets :deep(.library) {
  flex: 1;
  min-width: 0;
  border: 0;
}
</style>
