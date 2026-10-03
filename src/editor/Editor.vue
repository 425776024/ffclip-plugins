<script setup lang="ts">
import { useI18n } from './i18n';
const { tr } = useI18n();
import { computed, onBeforeUnmount, onMounted, ref, shallowRef, nextTick } from 'vue';
import {
  VideoCutClient,
  type Snapshot,
  type FileListing,
  type Asset,
  type Project,
  type Item,
  EditTransaction,
  type EditorCommand,
  createProject,
  duration,
  seconds,
  ticks,
  TEXT_TEMPLATES,
  EFFECT_TEMPLATES,
  TRANSITION_TEMPLATES,
  type EffectInstance,
  type TransitionInstance,
  findItem,
  selectionClosure,
  documentPatches,
  applyDocumentPatches
} from '../../packages/client/index.mjs';
import Preview from './Preview.vue';
import Timeline from './Timeline.vue';
import PanelDivider from './PanelDivider.vue';
import { usePanelLayout } from './panel-layout';
import Icon from './Icon.vue';
import MediaLibrary from './MediaLibrary.vue';
import Inspector from './Inspector.vue';
import TextLibrary from './TextLibrary.vue';
import HtmlLibrary from './HtmlLibrary.vue';
import type { HtmlContent } from './html-presets';
import EffectLibrary from './EffectLibrary.vue';
import { propertyCommand, keyframeCommand } from './property-command';
import { renderTemplateExport } from './template-export';
import { createSerialQueue } from './serial-queue.mjs';
import TtsPanel from './TtsPanel.vue';
import { TtsJobCoordinator } from './tts-jobs';
import { AsrJobCoordinator } from './asr-jobs';
import { VisionJobCoordinator } from './vision-jobs';
import VisionSetupDialog from './VisionSetupDialog.vue';
import SoftwareUpdateDialog from './SoftwareUpdateDialog.vue';
import ExportDialog from './ExportDialog.vue';
import type { TtsJob } from '../../packages/client/types';
const copyrightNotice = `© ${new Date().getFullYear()} FFClip`;
const renderingExport = ref(false);
let exportAbort: AbortController | undefined;
let exportRequestPending = false;
const enqueueExport = createSerialQueue();
const exportResult = shallowRef<{ sessionId: string; path: string } | null>(null);
const exportProgress = shallowRef<{ message: string; percent?: number } | null>(null);
const cancellingExport = ref(false);
const activeExportJob = shallowRef<{ sessionId: string; id: string }>();
const finishedExportJobs = new Set<string>();
function finishExportJob(id?: string) {
  if (!id) return;
  finishedExportJobs.add(id);
  if (finishedExportJobs.size > 32)
    finishedExportJobs.delete(finishedExportJobs.values().next().value!);
}
let lastCompletedExport = '';
function completeExport(sessionId: string, path: string) {
  if (snapshot.value?.id !== sessionId || !path) return;
  const key = `${sessionId}:${path}`;
  // Browser renders complete through both SSE and the initiating HTTP response.
  if (lastCompletedExport === key) return;
  lastCompletedExport = key;
  finishExportJob(activeExportJob.value?.id);
  exportProgress.value = null;
  cancellingExport.value = false;
  activeExportJob.value = undefined;
  notice.value = '';
  exportResult.value = { sessionId, path };
}
function cancelExport() {
  if (!activeExportJob.value || cancellingExport.value) return;
  cancellingExport.value = true;
  exportProgress.value = { message: '正在取消导出…' };
  exportAbort?.abort();
  const job = activeExportJob.value;
  void client
    .request(
      `/sessions/${job.sessionId}/render-job?job=${encodeURIComponent(job.id)}&action=cancel`,
      { method: 'POST' }
    )
    .catch((e) => {
      if (e.status !== 404) showError(e);
    });
}

const previewComponent = ref<InstanceType<typeof Preview>>();
const editorBody = ref<HTMLElement>();
const workspaceElement = ref<HTMLElement>();
const panelLayout = usePanelLayout(editorBody, workspaceElement);
const {
  style: panelStyle,
  resizing: resizingPanels,
  stacked: stackedPanels,
  values: dividerValues
} = panelLayout;
const previewClientId = crypto.randomUUID();
let previewSequence = 0,
  reportTimer: ReturnType<typeof setInterval> | undefined,
  reporting = false,
  previewReporting = false;
async function reportPreview() {
  if (!previewReporting || reporting || !snapshot.value || !connected.value) return;
  reporting = true;
  try {
    await nextTick();
    await client.reportPreview(snapshot.value.id, {
      clientId: previewClientId,
      version: snapshot.value.version,
      commandSequence: previewSequence,
      playing: playing.value,
      timeSeconds: seconds(time.value),
      error: error.value,
      media: previewComponent.value?.mediaStatus() || [],
      renderer: previewComponent.value?.renderStatus(),
      capabilities: {
        webgpu: 'gpu' in navigator,
        webcodecs: 'VideoDecoder' in globalThis,
        offscreenCanvas: 'OffscreenCanvas' in globalThis
      }
    });
  } catch {
  } finally {
    reporting = false;
  }
}
const client = new VideoCutClient(window.location.origin);
const snapshot = shallowRef<Snapshot | null>(null);
const ttsJob = shallowRef<TtsJob | null>(null);
let ttsCoordinator: TtsJobCoordinator | undefined;
let asrCoordinator: AsrJobCoordinator | undefined;
let visionCoordinator: VisionJobCoordinator | undefined;
const visionSetupReady = ref(false);
const project = computed(() => snapshot.value?.project || empty);
const gesture = new EditTransaction();
const draftProject = shallowRef<Project | null>(null);
const previewProject = computed(() => draftProject.value || project.value);
function draft(operations: EditorCommand[]) {
  if (!snapshot.value || busy.value) return;
  try {
    if (!gesture.active) gesture.begin(snapshot.value);
    draftProject.value = gesture.update(operations);
  } catch {
    /* Invalid intermediate drafts keep the last valid candidate until release. */
  }
}
function cancelDraft() {
  gesture.cancel();
  draftProject.value = null;
}
async function commitDraft(settle: () => void) {
  try {
    if (!gesture.active) return;
    const receipt = gesture.commit();
    draftProject.value = null;
    if (receipt.changed) await command(receipt.operations, receipt.version);
  } finally {
    await nextTick();
    settle();
  }
}
const empty = createProject(tr('未命名作品'));
const roots = ref<string[]>([]),
  listing = ref<FileListing | null>(null);
const selected = ref(''),
  panel = ref('media');
const shortcuts = ref(false);
const busy = ref(false),
  editing = ref(false),
  connected = ref(false),
  playing = ref(false),
  time = ref(0),
  zoom = ref(65),
  snap = ref(true);
const error = ref(''),
  notice = ref(''),
  nativeExport = ref(false),
  exportMenu = ref(false),
  exportDirectory = ref('');
const canUndo = computed(() => snapshot.value?.history.canUndo ?? false);
const canRedo = computed(() => snapshot.value?.history.canRedo ?? false);
const showLibrary = ref(false),
  showInspector = ref(false);
const selectedIds = ref<string[]>([]);
const rippleScope = ref<'selected' | 'all' | 'syncLocked'>('selected');
const outputFormat = ref<'mp4' | 'webm'>('mp4');
const viewportZoom = ref(1),
  viewportPan = ref({ x: 0, y: 0 }),
  previewQuality = ref<'auto' | 'full' | 'half'>('auto');
function resetViewport() {
  viewportZoom.value = 1;
  viewportPan.value = { x: 0, y: 0 };
}
function panViewport(event: PointerEvent) {
  if (event.button !== 1 && !event.altKey) return;
  event.preventDefault();
  event.stopPropagation();
  const start = {
    x: event.clientX,
    y: event.clientY,
    ...{ panX: viewportPan.value.x, panY: viewportPan.value.y }
  };
  const element = event.currentTarget as HTMLElement;
  element.setPointerCapture(event.pointerId);
  const move = (e: PointerEvent) => {
    viewportPan.value = {
      x: start.panX + e.clientX - start.x,
      y: start.panY + e.clientY - start.y
    };
  };
  const end = () => {
    element.removeEventListener('pointermove', move);
    element.removeEventListener('pointerup', end);
    element.removeEventListener('pointercancel', end);
  };
  element.addEventListener('pointermove', move);
  element.addEventListener('pointerup', end);
  element.addEventListener('pointercancel', end);
}
const openPath = ref(''),
  showOpen = ref(false);
const selectedItem = computed(() => {
  try {
    return findItem(project.value, selected.value).item;
  } catch {
    return null;
  }
});
const selectedLocked = computed(() => {
  try {
    return findItem(project.value, selected.value).track.locked;
  } catch {
    return false;
  }
});
const effectTargets = computed(() =>
  project.value.timeline.tracks
    .filter((track) => !track.locked)
    .flatMap((track) => track.items)
    .filter((item) => selectedIds.value.includes(item.id) && item.clip.type !== 'audio')
);
const transitionTarget = computed(() => {
  if (!selectedItem.value || selectedLocked.value || selectedItem.value.clip.type === 'audio')
    return null;
  const { track, item } = findItem(project.value, selectedItem.value.id);
  const next = track.items.find((candidate) => candidate.placement.begin === item.placement.end);
  return next ? { fromItemId: item.id, toItemId: next.id } : null;
});
function applyVisualPackage(kind: 'effect' | 'transition', id: string) {
  if (kind === 'effect') {
    if (!EFFECT_TEMPLATES.some((entry) => entry.id === id)) return;
    command(
      effectTargets.value.map((item) => ({
        action: 'add_effect',
        itemId: item.id,
        templateId: id as EffectInstance['templateId']
      }))
    );
  } else if (transitionTarget.value && TRANSITION_TEMPLATES.some((entry) => entry.id === id)) {
    command([
      {
        action: 'add_transition',
        ...transitionTarget.value,
        templateId: id as TransitionInstance['templateId'],
        durationSeconds: 0.5
      }
    ]);
  }
}
const total = computed(() => duration(project.value));
let source: EventSource | null = null,
  animation = 0,
  lastFrame = 0,
  pending: Snapshot | null = null,
  saving = false;
let starterPlaybackFromBeginning = false;
const mediaUrl = (id: string) => {
  const asset = project.value.assets.find((a) => a.id === id);
  return snapshot.value
    ? client.mediaUrl(snapshot.value.id, id) +
        (asset?.sourceIdentity ? '&source=' + encodeURIComponent(asset.sourceIdentity) : '')
    : '';
};
const formatTime = (value: number) => {
  const { numerator, denominator } = project.value.frameRate;
  const fps = Math.round(numerator / denominator);
  // Frame-index based, non-drop timecode. Avoid decimal second remainders
  // displaying frame 5 at the exact 1.2s / 30fps boundary (frame 36).
  const index = Math.floor(((Math.max(0, value) + 0.5) * numerator) / (120000 * denominator));
  const s = Math.floor(index / fps);
  return `${Math.floor(s / 60)
    .toString()
    .padStart(2, '0')}:${Math.floor(s % 60)
    .toString()
    .padStart(2, '0')}:${(index % fps).toString().padStart(2, '0')}`;
};
function showError(e: unknown) {
  error.value = e instanceof Error ? e.message : String(e);
  playing.value = false;
}
function adopt(value: Snapshot) {
  const firstStarter = snapshot.value?.id !== value.id && value.example === 'starter';
  if (snapshot.value?.id !== value.id) starterPlaybackFromBeginning = firstStarter;
  if (gesture.invalidate(value)) draftProject.value = null;
  // HTTP/SSE snapshots are JSON copies. Preserve unchanged document branches so
  // one edited field does not invalidate every media tile and property panel.
  if (snapshot.value?.id === value.id && project.value.id === value.project.id)
    value = {
      ...value,
      project: applyDocumentPatches(project.value, documentPatches(project.value, value.project))
    };
  snapshot.value = value;
  if (firstStarter) {
    time.value = Math.min(ticks(1.5), duration(value.project));
    starterPlaybackFromBeginning = true;
  }
  const preview = new URL(value.previewUrl);
  if (location.pathname !== preview.pathname || location.search !== preview.search)
    history.replaceState(null, '', preview.pathname + preview.search);
  document.title = `${value.project.name} · ffclip`;
  const ids = new Set(value.project.timeline.tracks.flatMap((t) => t.items.map((i) => i.id)));
  if (selectedIds.value.some((id) => !ids.has(id)))
    selectedIds.value = selectedIds.value.filter((id) => ids.has(id));
  if (!ids.has(selected.value)) selected.value = selectedIds.value[0] || '';
  if (time.value > duration(value.project)) time.value = duration(value.project);
}
function showExportFailure(value: unknown, cancelled = false, jobId?: string) {
  if (jobId && finishedExportJobs.has(jobId)) return;
  finishExportJob(jobId || activeExportJob.value?.id);
  if (jobId && activeExportJob.value && activeExportJob.value.id !== jobId) return;
  exportProgress.value = null;
  activeExportJob.value = undefined;
  cancellingExport.value = false;
  if (
    cancelled ||
    (value instanceof Error && (value.name === 'AbortError' || value.message === '用户取消导出'))
  ) {
    error.value = '';
    notice.value = '已取消导出';
  } else showError(value);
}
function subscribe(id: string) {
  ttsCoordinator?.dispose();
  asrCoordinator?.dispose();
  visionCoordinator?.dispose();
  ttsJob.value = null;
  source?.close();
  source = new EventSource(client.eventsUrl(id));
  ttsCoordinator = new TtsJobCoordinator(client, id, source, (job) => {
    const previous = ttsJob.value;
    ttsJob.value = job;
    if (job?.state === 'completed' && (previous?.id !== job.id || previous?.state !== 'completed'))
      notice.value = job.insert === false ? '语音已生成' : '语音已生成并加入音轨';
  });
  asrCoordinator = new AsrJobCoordinator(client, id, source, () => {});
  visionCoordinator = new VisionJobCoordinator(client, id, source, () => {});
  source.addEventListener('vision-setup-request', () =>
    window.dispatchEvent(new Event('videocut-vision-setup'))
  );
  previewSequence = 0;
  source.addEventListener('render-request', (event) => {
    const jobId = JSON.parse((event as MessageEvent).data).id;
    if (finishedExportJobs.has(jobId)) return;
    activeExportJob.value = { sessionId: id, id: jobId };
    exportResult.value = null;
    cancellingExport.value = false;
    exportProgress.value = { message: '正在准备导出…' };
    // The server may announce the next job before the preceding finish response reaches Safari.
    // Queue it until the old worker/abort state is cleared instead of silently dropping the event.
    void enqueueExport(async () => {
      if (snapshot.value?.id !== id || source?.readyState === EventSource.CLOSED) return;
      let claimed = false;
      const abort = new AbortController();
      exportAbort = abort;
      try {
        await renderTemplateExport(client, id, jobId, abort.signal, () => {
          claimed = true;
          error.value = '';
          playing.value = false;
          busy.value = true;
          renderingExport.value = true;
          notice.value = '';
          if (!cancellingExport.value)
            exportProgress.value = { message: '正在按作品分辨率渲染视频…', percent: 0 };
        });
      } catch (e) {
        showExportFailure(e, abort.signal.aborted, jobId);
      } finally {
        exportAbort = undefined;
        renderingExport.value = false;
        if (claimed) busy.value = exportRequestPending;
      }
    });
  });
  source.addEventListener('render-progress', (event) => {
    const value = JSON.parse((event as MessageEvent).data);
    const jobId = value.jobId || value.id;
    if (finishedExportJobs.has(jobId)) return;
    if (value.phase === 'error') {
      notice.value = '';
      // A cancellation from MCP must also stop this page's encoder before a
      // subsequent progress/chunk request reports the already-ended job.
      if (value.error === '用户取消导出') exportAbort?.abort();
      showExportFailure(new Error(value.error), false, jobId);
      return;
    }
    if (value.phase === 'complete') {
      completeExport(id, value.path);
      return;
    }
    if (cancellingExport.value) return;
    activeExportJob.value = { sessionId: id, id: jobId };
    exportResult.value = null;
    const percent =
      value.total > 0
        ? Math.min(100, Math.max(0, (value.completed / value.total) * 100))
        : undefined;
    exportProgress.value =
      value.phase === 'encoding'
        ? { message: '正在编码视频与声音…' }
        : value.phase === 'finalizing'
          ? { message: '正在完成导出文件…' }
          : {
              message: `正在导出 ${value.completed} / ${value.total} 帧（${Math.round(percent || 0)}%）`,
              percent
            };
  });
  source.addEventListener('preview-control', async (event) => {
    const command = JSON.parse((event as MessageEvent).data);
    if (command.sequence <= previewSequence) return;
    previewSequence = command.sequence;
    error.value = '';
    if (command.timeSeconds !== undefined) seek(ticks(command.timeSeconds));
    if (command.action === 'play') {
      if (starterPlaybackFromBeginning || time.value >= total.value) time.value = 0;
      starterPlaybackFromBeginning = false;
      playing.value = total.value > 0;
      lastFrame = performance.now();
    } else playing.value = false;
    await reportPreview();
  });
  source.onopen = () => {
    connected.value = true;
  };
  source.onerror = () => {
    connected.value = false;
  };
  source.onmessage = (event) => {
    const value: Snapshot = JSON.parse(event.data);
    if (saving) {
      pending = value;
      return;
    }
    if (value.version > (snapshot.value?.version ?? -1)) {
      adopt(value);
      notice.value = '已同步插件修改';
    } else if (
      value.version === snapshot.value?.version &&
      value.projectPath !== snapshot.value.projectPath
    ) {
      adopt(value);
    }
  };
}
async function connect() {
  try {
    const info = await client.connect();
    roots.value = info.roots;
    previewReporting = Boolean(info.previewControl);
    nativeExport.value = info.nativeExport;
    exportDirectory.value = info.roots[0];
    const id = new URLSearchParams(location.search).get('session');
    const value = location.pathname.startsWith('/projects/')
      ? await client.resolveSession(location.pathname)
      : id
        ? await client.getSession(id)
        : await client.createSession(createProject(tr('未命名作品')));
    adopt(value);
    visionSetupReady.value = true;
    subscribe(value.id);
    connected.value = true;
    await browse(info.roots[0]);
  } catch (e) {
    showError(e);
  }
}
async function browse(directory: string) {
  try {
    listing.value = await client.listFiles(directory);
  } catch (e) {
    showError(e);
  }
}
let commandTail: Promise<unknown> = Promise.resolve();
function command(operations: EditorCommand[], expectedVersion?: number): Promise<boolean> {
  cancelDraft();
  if (!snapshot.value || busy.value || !operations.length) return Promise.resolve(false);
  const sessionId = snapshot.value.id;
  // Serialize ordinary edits without toggling the global import/export busy UI.
  const result = commandTail.then(() => sendCommand(sessionId, operations, expectedVersion));
  commandTail = result.catch(() => false);
  return result;
}
async function sendCommand(
  sessionId: string,
  operations: EditorCommand[],
  expectedVersion?: number
) {
  if (!snapshot.value || snapshot.value.id !== sessionId || busy.value) return false;
  if (expectedVersion !== undefined && snapshot.value.version !== expectedVersion) return false;
  saving = true;
  editing.value = true;
  error.value = '';
  try {
    const result = await client.editSession(snapshot.value.id, operations, snapshot.value.version);
    adopt(result);
    const created = result.operations.find(
      (o: any) =>
        o.itemId && ['add_asset', 'add_text', 'add_html_clip', 'duplicate_clips'].includes(o.action)
    );
    if (created?.itemId) select(created.itemId);
    const duplicated = result.operations.find((o) => o.action === 'duplicate_clips');
    if (duplicated?.itemIds) {
      selectedIds.value = duplicated.itemIds;
      selected.value = duplicated.itemIds[0] || '';
    }
    return true;
  } catch (e: any) {
    if (e.status === 409 && e.snapshot?.project) {
      adopt(e.snapshot);
    }
    showError(e);
    return false;
  } finally {
    saving = false;
    editing.value = false;
    if (
      pending &&
      (pending.version > (snapshot.value?.version ?? -1) ||
        (pending.version === snapshot.value?.version &&
          pending.projectPath !== snapshot.value.projectPath))
    ) {
      adopt(pending);
    }
    pending = null;
  }
}
const onCommand = (op: EditorCommand) => command([op]);
function selectedCommand(
  action: 'set_transform',
  changes: Partial<Item['clip']['visual']>
): Promise<boolean>;
function selectedCommand(
  action: 'set_audio',
  changes: Partial<Item['clip']['audio']>
): Promise<boolean>;
function selectedCommand(
  action: 'set_transform' | 'set_audio',
  changes: Partial<Item['clip']['visual']> | Partial<Item['clip']['audio']>
) {
  const ids = selectedIds.value.length ? selectedIds.value : [selected.value];
  return command(
    ids.filter(Boolean).map((itemId): EditorCommand => ({ action, itemId, ...changes }))
  );
}
function moveSelection(id: string, begin: number, trackId?: string) {
  const ids = selectedIds.value;
  if (
    ids.includes(id) &&
    (ids.length > 1 ||
      [...(project.value.timeline.groups || []), ...(project.value.timeline.links || [])].some(
        (g) => g.itemIds.includes(id)
      ))
  ) {
    const item = findItem(project.value, id).item;
    return command([
      { action: 'move_clips', itemIds: ids, deltaSeconds: seconds(begin - item.placement.begin) }
    ]);
  }
  return command([{ action: 'move_clip', itemId: id, startSeconds: seconds(begin), trackId }]);
}
function trimSelection(id: string, begin: number, end: number) {
  return command([
    { action: 'trim_range', itemId: id, beginSeconds: seconds(begin), endSeconds: seconds(end) }
  ]);
}
function toggleTrack(id: string, key: 'visible' | 'muted' | 'locked' | 'syncLocked') {
  const track = project.value.timeline.tracks.find((t) => t.id === id)!;
  return command([{ action: 'set_track', trackId: id, [key]: !track[key] }]);
}
async function importFile(file: string) {
  await commandTail;
  if (busy.value || !snapshot.value) return;
  busy.value = true;
  error.value = '';
  notice.value = '正在读取素材信息…';
  try {
    const asset = await client.importMedia(file);
    busy.value = false;
    await command([{ action: 'add_asset', asset }]);
    notice.value = '已引用本地文件，未复制素材';
  } catch (e) {
    showError(e);
  } finally {
    busy.value = false;
  }
}
let clipboardImportPending = false;
async function pasteFiles() {
  if (clipboardImportPending || busy.value || !snapshot.value || !connected.value) return;
  clipboardImportPending = true;
  playing.value = false;
  const start = Math.round(time.value);
  let ownsBusy = false;
  try {
    await commandTail;
    if (busy.value || !snapshot.value) return;
    const { id, version } = snapshot.value;
    busy.value = true;
    ownsBusy = true;
    error.value = '';
    notice.value = '正在读取复制的文件…';
    const { assets, skipped } = await client.importClipboardMedia();
    const skippedNotice = skipped.map((file) => `${file.name}：${file.error}`).join('；');
    if (!assets.length) {
      notice.value = '';
      if (skipped.length) throw new Error(skippedNotice);
      notice.value = '请先在文件管理器中复制视频、音频或图片文件';
      return;
    }
    if (snapshot.value?.id !== id || snapshot.value.version !== version)
      throw new Error('导入期间作品已改变，请重新粘贴文件');
    let cursor = start;
    const operations: EditorCommand[] = assets.map((asset) => {
      const operation: EditorCommand = {
        action: 'add_asset',
        asset,
        startSeconds: seconds(cursor)
      };
      cursor += asset.kind === 'image' ? ticks(5) : asset.duration;
      return operation;
    });
    busy.value = false;
    ownsBusy = false;
    if (await command(operations, version)) {
      panel.value = 'media';
      notice.value = `已粘贴 ${assets.length} 个素材到时间轴${skipped.length ? `；跳过 ${skipped.length} 个文件：${skippedNotice}` : ''}`;
    } else notice.value = '';
  } catch (e) {
    notice.value = '';
    showError(e);
  } finally {
    if (ownsBusy) busy.value = false;
    clipboardImportPending = false;
  }
}
function select(id: string) {
  selectMany(id);
}
function selectMany(id: string, additive = false) {
  if (!id) {
    selectedIds.value = [];
    selected.value = '';
    return;
  }
  const closure = selectionClosure(project.value, [id]);
  if (additive)
    selectedIds.value = selectedIds.value.includes(id)
      ? selectedIds.value.filter((x) => !closure.includes(x))
      : [...new Set([...selectedIds.value, ...closure])];
  else if (!selectedIds.value.includes(id)) selectedIds.value = closure;
  selected.value = selectedIds.value.includes(id) ? id : selectedIds.value.at(-1) || '';
}
function append(asset: Asset, trackId?: string, start?: number) {
  command([
    {
      action: 'add_asset',
      asset,
      trackId,
      startSeconds: start === undefined ? undefined : seconds(start)
    }
  ]);
}
function insertText(content = tr('默认文字'), fontSize = 64, subtitle = false) {
  command([
    { action: 'add_text', content, fontSize, startSeconds: seconds(Math.round(time.value)) }
  ]);
}
function insertHtml(html: HtmlContent, name: string) {
  playing.value = false;
  command([{ action: 'add_html_clip', html, name, start: Math.round(time.value) }]);
}
function insertTemplate(id: string) {
  const template = TEXT_TEMPLATES.find((t) => t.id === id);
  if (!template) return;
  playing.value = false;
  const start = Math.round(time.value);
  command([
    {
      action: 'add_text',
      content: tr(template.text),
      startSeconds: seconds(start),
      template: { id, version: 1 }
    }
  ]).then((saved) => {
    if (saved) time.value = start + ticks(template.timeUs / 1e6);
  });
}
function step(direction: number) {
  seek(
    time.value +
      (direction * 120000 * project.value.frameRate.denominator) / project.value.frameRate.numerator
  );
}
function splitSelected() {
  const ids = (selectedIds.value.length ? selectedIds.value : [selected.value]).filter((id) => {
    const item = findItem(project.value, id).item;
    return time.value > item.placement.begin && time.value < item.placement.end;
  });
  if (ids.length)
    command(
      ids.map((itemId) => ({ action: 'split_clip', itemId, atSeconds: seconds(time.value) }))
    );
}
function deleteSelected() {
  command([{ action: 'delete_clips', itemIds: selectedIds.value }]);
}
function detach(field: 'groups' | 'links') {
  const relations =
    project.value.timeline[field]?.filter((g) =>
      g.itemIds.some((id) => selectedIds.value.includes(id))
    ) || [];
  if (relations.length)
    command(
      relations.map((g) => ({
        action: field === 'groups' ? 'ungroup_clips' : 'unlink_clips',
        groupId: g.id
      }))
    );
}
function updateText(changes: Partial<NonNullable<Item['clip']['text']>>) {
  const ids = selectedIds.value.filter((id) => {
    const text = findItem(project.value, id).item.clip.text;
    return text && (!text.template || Object.keys(changes).every((key) => key === 'content'));
  });
  command(ids.map((itemId) => ({ action: 'set_text', itemId, ...changes })));
}
async function transformItem(
  id: string,
  changes: Partial<Item['clip']['visual']> & { layoutWidth?: number },
  settle: () => void
) {
  try {
    const item = findItem(project.value, id).item;
    const local = Math.max(
      0,
      Math.min(
        item.placement.end - item.placement.begin,
        Math.round(time.value - item.placement.begin)
      )
    );
    await command(
      Object.entries(changes)
        .filter(([, value]) => value !== undefined)
        .map(([key, value]) => {
          if (key === 'layoutWidth')
            return { action: 'set_text', itemId: id, layoutWidth: Number(value) } as EditorCommand;
          const property = `visual.${key}`;
          return item.clip.automation?.[property]?.keyframes.length
            ? keyframeCommand(item, property, value, seconds(local))
            : propertyCommand(item, property, value);
        })
    );
  } catch (e) {
    showError(e);
  } finally {
    await nextTick();
    settle();
  }
}
function dropAsset(id: string, track: string, start: number) {
  const a = project.value.assets.find((a) => a.id === id);
  if (a) append(a, track, start);
}
function seek(value: number) {
  starterPlaybackFromBeginning = false;
  playing.value = false;
  time.value = Math.max(0, Math.min(value, Math.max(total.value, ticks(10))));
}
function togglePlay() {
  error.value = '';
  if (!total.value) return;
  if (!playing.value && (starterPlaybackFromBeginning || time.value >= total.value)) time.value = 0;
  starterPlaybackFromBeginning = false;
  playing.value = !playing.value;
  lastFrame = performance.now();
}
function tick(now: number) {
  if (playing.value) {
    const clock = previewComponent.value?.clockTime();
    time.value = Math.min(total.value, clock ?? time.value + ticks((now - lastFrame) / 1000));
    if (time.value >= total.value) playing.value = false;
  }
  lastFrame = now;
  animation = requestAnimationFrame(tick);
}
async function undo() {
  await commandTail;
  if (canUndo.value) await command([{ action: 'undo' }]);
}
async function redo() {
  await commandTail;
  if (canRedo.value) await command([{ action: 'redo' }]);
}
function numeric(event: Event) {
  return Number((event.target as HTMLInputElement).value);
}
function text(event: Event) {
  return (event.target as HTMLInputElement).value;
}
async function openSession() {
  await commandTail;
  if (!openPath.value) return;
  try {
    const s = await client.openProject(openPath.value);
    adopt(s);
    subscribe(s.id);
    selectedIds.value = [];
    selected.value = '';
    time.value = 0;
    playing.value = false;
    showOpen.value = false;
    notice.value = s.projectPath?.endsWith('.vcutweb')
      ? '已打开完整 Web 作品'
      : '已打开 Project Format 1 作品';
  } catch (e) {
    showError(e);
  }
}
async function exportTo(kind: 'native' | 'video' | 'web') {
  await commandTail;
  if (!snapshot.value || busy.value || (kind === 'video' && !total.value)) return;
  if (
    kind === 'native' &&
    project.value?.timeline.tracks.some((track) => track.items.some((item) => item.clip.html))
  ) {
    error.value = 'HTML 动画可以导出 MP4/WebM；当前 .vcut 格式尚未支持 HTML 片段。';
    exportMenu.value = false;
    return;
  }
  exportRequestPending = true;
  busy.value = true;
  playing.value = false;
  exportMenu.value = false;
  error.value = '';
  notice.value = '';
  exportResult.value = null;
  cancellingExport.value = false;
  exportProgress.value = {
    message:
      kind === 'web'
        ? '正在保存完整作品和素材…'
        : kind === 'native'
          ? '正在生成并验证 .vcut 作品…'
          : '正在导出视频…'
  };
  const sessionId = snapshot.value.id;
  try {
    const result =
      kind === 'web'
        ? await client.saveProject(snapshot.value.id, snapshot.value.version, exportDirectory.value)
        : kind === 'native'
          ? await client.exportProject(
              snapshot.value.id,
              snapshot.value.version,
              exportDirectory.value
            )
          : await client.renderVideo(
              snapshot.value.id,
              snapshot.value.version,
              exportDirectory.value,
              outputFormat.value
            );
    completeExport(sessionId, result.path);
  } catch (e) {
    notice.value = '';
    showExportFailure(e);
  } finally {
    exportRequestPending = false;
    busy.value = renderingExport.value;
  }
}
async function copyLink() {
  try {
    if (snapshot.value) {
      await navigator.clipboard.writeText(snapshot.value.previewUrl);
      notice.value = '已复制作品预览链接';
    }
  } catch (e) {
    showError(e);
  }
}
function textEditingTarget(target: EventTarget | null) {
  const element = target instanceof HTMLElement ? target : document.activeElement;
  return (
    element instanceof HTMLElement &&
    (element.isContentEditable || !!element.closest('input, textarea, select, [role="textbox"]'))
  );
}
function paste(event: ClipboardEvent) {
  if (
    textEditingTarget(event.target) ||
    showOpen.value ||
    shortcuts.value ||
    document.querySelector('dialog[open]')
  )
    return;
  event.preventDefault();
  void pasteFiles();
}
function keyboard(event: KeyboardEvent) {
  if (
    event.defaultPrevented ||
    event.isComposing ||
    textEditingTarget(event.target) ||
    showOpen.value ||
    shortcuts.value ||
    document.querySelector('dialog[open]')
  )
    return;
  if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === 'v' && !event.altKey) {
    event.preventDefault();
    if (!event.repeat) void pasteFiles();
    return;
  }
  if (event.code === 'Space') {
    // Keep native keyboard activation of buttons while allowing editing shortcuts.
    if (event.target instanceof Element && event.target.closest('button')) return;
    event.preventDefault();
    togglePlay();
  }
  if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === 'z') {
    event.preventDefault();
    event.shiftKey ? redo() : undo();
  }
  if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === 'b' && selected.value) {
    event.preventDefault();
    splitSelected();
  }
  if (['Delete', 'Backspace'].includes(event.key) && selected.value) {
    event.preventDefault();
    deleteSelected();
  }
  if (['ArrowLeft', 'ArrowRight'].includes(event.key)) {
    event.preventDefault();
    seek(
      time.value +
        ((event.key === 'ArrowRight' ? 1 : -1) * 120000 * project.value.frameRate.denominator) /
          project.value.frameRate.numerator
    );
  }
}
onMounted(() => {
  reportTimer = setInterval(reportPreview, 750);
  connect();
  animation = requestAnimationFrame(tick);
  window.addEventListener('keydown', keyboard);
  window.addEventListener('paste', paste);
});
onBeforeUnmount(() => {
  ttsCoordinator?.dispose();
  asrCoordinator?.dispose();
  visionCoordinator?.dispose();
  exportAbort?.abort();
  clearInterval(reportTimer);
  source?.close();
  cancelAnimationFrame(animation);
  window.removeEventListener('keydown', keyboard);
  window.removeEventListener('paste', paste);
});
</script>

<template>
  <main class="editor-shell">
    <header class="app-bar">
      <a
        class="brand"
        href="https://ffclip.com"
        target="_blank"
        rel="noopener noreferrer"
        :title="copyrightNotice"
        :aria-label="tr(`${copyrightNotice} · ffclip 官网（在新标签页打开）`)"
      >
        <span class="brand-icon"><Icon name="monitor" :size="22" /></span><strong>ffclip</strong
        ><span class="brand-copyright" aria-hidden="true">©</span>
      </a>
      <input
        class="project-name"
        :aria-label="tr('作品名称')"
        :title="snapshot?.projectPath || project.name"
        :value="project.name"
        :disabled="busy"
        @change="command([{ action: 'configure_project', name: text($event) }])"
      />
      <span class="editor-mode">{{ tr('视频剪辑') }}</span>
      <div class="app-actions">
        <span class="connection" :class="{ online: connected }"
          ><i />{{ tr(connected ? '本地连接' : '连接中') }}</span
        >
        <SoftwareUpdateDialog
          v-if="connected"
          :client="client"
          :blocked="busy || playing || editing || resizingPanels || showOpen || shortcuts"
          @install="playing = false"
        />
        <button
          class="icon-button open-session"
          :title="tr('打开 .vcutweb / .vcut 作品目录')"
          @click="showOpen = true"
          :disabled="busy"
        >
          <Icon name="folder" :size="18" /></button
        ><button
          class="icon-button layout-toggle"
          :title="tr('显示 / 隐藏素材')"
          @click="
            showLibrary = !showLibrary;
            showInspector = false;
          "
        >
          <Icon name="layout" :size="18" /></button
        ><button
          class="icon-button inspector-toggle"
          :title="tr('显示 / 隐藏属性')"
          @click="
            showInspector = !showInspector;
            showLibrary = false;
          "
        >
          <Icon name="sliders" :size="18" /></button
        ><button
          class="icon-button"
          :title="tr('复制实时预览链接')"
          @click="copyLink"
          :disabled="!snapshot"
        >
          <Icon name="link" :size="18" />
        </button>
        <div class="export-wrapper">
          <button
            class="primary export-button"
            :title="tr('导出视频')"
            @click="exportTo('video')"
            :disabled="busy || !snapshot || !total"
          >
            <Icon name="export" :size="16" />{{ tr(busy ? '处理中' : '导出') }}
          </button>
          <button
            class="primary export-settings-button"
            :title="tr('导出设置')"
            :aria-label="tr('导出设置')"
            :aria-expanded="exportMenu"
            aria-controls="export-settings"
            @click="exportMenu = !exportMenu"
            :disabled="busy || !snapshot"
          >
            <Icon name="down" :size="14" />
          </button>
          <div v-if="exportMenu" id="export-settings" class="export-menu">
            <label
              >{{ tr('保存到本地目录')
              }}<input v-model="exportDirectory" :aria-label="tr('导出目录')" /></label
            ><label
              >{{ tr('视频格式')
              }}<select v-model="outputFormat" :aria-label="tr('视频导出格式')">
                <option value="mp4">MP4 · H.264 / AAC</option>
                <option value="webm">WebM · VP9 / Opus</option>
              </select></label
            ><button @click="exportTo('video')" :disabled="!total">
              {{ tr('导出视频') }} <small>{{ outputFormat.toUpperCase() }}</small></button
            ><button @click="exportTo('web')">
              {{ tr('保存完整作品') }} <small>.vcutweb</small></button
            ><button @click="exportTo('native')" :disabled="!nativeExport">
              {{ tr('导出原生作品') }} <small>.vcut · Format 1</small></button
            ><small v-if="!nativeExport" class="subtle">{{
              tr('需配置原生格式桥接器，详见 README。')
            }}</small>
          </div>
        </div>
      </div>
    </header>
    <div
      ref="editorBody"
      class="editor-body"
      :style="panelStyle"
      :class="{
        'show-library': showLibrary,
        'show-inspector': showInspector,
        'resizing-panels': resizingPanels
      }"
    >
      <nav class="activity-rail panel" :aria-label="tr('编辑工具')">
        <div class="rail-main">
          <button
            v-for="entry in [
              { id: 'media', label: '素材', icon: 'media' },
              { id: 'text', label: '文字', icon: 'text' },
              { id: 'html', label: '动画', icon: 'layers' },
              { id: 'audio', label: '音频', icon: 'music' },
              { id: 'image', label: '图片', icon: 'image' }
            ]"
            :key="entry.id"
            :class="{ active: panel === entry.id }"
            @click="
              panel = entry.id;
              showLibrary = true;
              showInspector = false;
            "
          >
            <Icon :name="entry.icon" :size="23" /><span>{{ tr(entry.label) }}</span>
          </button>
        </div>
        <div class="rail-bottom">
          <a
            class="rail-website"
            href="https://ffclip.com"
            target="_blank"
            rel="noopener noreferrer"
            :title="tr('ffclip 官网（在新标签页打开）')"
          >
            <Icon name="link" :size="21" /><span>ffclip.com</span>
          </a>
          <button :title="tr('键盘快捷键')" @click="shortcuts = !shortcuts">
            <Icon name="info" :size="21" /><span>{{ tr('帮助') }}</span>
          </button>
        </div>
      </nav>
      <section ref="workspaceElement" class="workspace">
        <TextLibrary
          v-if="panel === 'text'"
          :busy="busy"
          @basic="insertText"
          @template="insertTemplate"
        />
        <HtmlLibrary v-else-if="panel === 'html'" :busy="busy" @insert="insertHtml" />
        <EffectLibrary
          v-else-if="panel === 'effects' || panel === 'transitions'"
          :kind="panel === 'effects' ? 'effect' : 'transition'"
          :busy="busy"
          :can-apply="panel === 'effects' ? effectTargets.length > 0 : Boolean(transitionTarget)"
          @apply="applyVisualPackage"
        />
        <TtsPanel
          v-else-if="panel === 'audio'"
          :client="client"
          :snapshot="snapshot"
          :job="ttsJob"
          :start-seconds="seconds(time)"
          :busy="busy"
          :media-url="mediaUrl"
          @queued="
            (job) => {
              ttsJob = job;
              ttsCoordinator?.refresh();
            }
          "
          @append="append"
        >
          <MediaLibrary
            :project="project"
            :listing="listing"
            :roots="roots"
            :busy="busy"
            :media-url="mediaUrl"
            filter="audio"
            @browse="browse"
            @import="importFile"
            @append="append"
          />
        </TtsPanel>
        <MediaLibrary
          v-else
          :project="project"
          :listing="listing"
          :roots="roots"
          :busy="busy"
          :media-url="mediaUrl"
          :filter="panel"
          @browse="browse"
          @import="importFile"
          @append="append"
        />
        <PanelDivider
          class="library-divider"
          :orientation="stackedPanels ? 'horizontal' : 'vertical'"
          :label="tr('拖动调节素材和预览面板大小；双击恢复默认')"
          :value="dividerValues.library"
          @start="panelLayout.start('library')"
          @resize="panelLayout.resize"
          @finish="panelLayout.finish"
          @step="(delta) => panelLayout.step('library', delta)"
          @reset="panelLayout.reset('library')"
        />
        <section class="preview-panel panel">
          <div class="panel-heading">
            <span class="panel-tab active">{{ tr('预览') }}</span
            ><button
              class="preview-resolution"
              :title="tr('画布设置')"
              :aria-label="tr('画布设置')"
              @click="
                selected = '';
                showInspector = true;
                showLibrary = false;
              "
            >
              {{ project.canvas.width }} × {{ project.canvas.height }}
            </button>
          </div>
          <div class="preview-canvas" @pointerdown.capture="panViewport">
            <Preview
              ref="previewComponent"
              :style="{
                transform: `translate(${viewportPan.x}px,${viewportPan.y}px) scale(${viewportZoom})`
              }"
              :quality="previewQuality"
              :project="previewProject"
              :time="time"
              :playing="playing"
              :media-url="mediaUrl"
              :selected="selected"
              :disabled="busy || editing || selectedLocked"
              :snap="snap"
              @select="select"
              @transform="transformItem"
              @error="showError"
            />
          </div>
          <div class="preview-scrub">
            <input
              type="range"
              :aria-label="tr('预览进度')"
              min="0"
              :max="Math.max(total, 1)"
              :step="(120000 * project.frameRate.denominator) / project.frameRate.numerator"
              :value="time"
              @input="seek(numeric($event))"
            />
          </div>
          <div class="transport">
            <div class="timecode">
              <span>{{ formatTime(time) }}</span
              ><b>/</b>{{ formatTime(total) }}
            </div>
            <div class="transport-buttons">
              <button :title="tr('回到开始')" @click="seek(0)">
                <Icon name="previous" :size="16" /></button
              ><button :title="tr('上一帧')" @click="step(-1)">
                <Icon name="left" :size="17" /></button
              ><button
                :title="tr(playing ? '暂停' : '播放')"
                :aria-label="tr(playing ? '暂停' : '播放')"
                @click="togglePlay"
                :disabled="!total"
              >
                <Icon :name="playing ? 'pause' : 'play'" :size="19" /></button
              ><button :title="tr('下一帧')" @click="step(1)">
                <Icon name="right" :size="17" /></button
              ><button :title="tr('跳到结尾')" @click="seek(total)">
                <Icon name="next" :size="16" />
              </button>
            </div>
            <div class="preview-fit">
              <select
                :aria-label="tr('预览缩放')"
                :value="viewportZoom"
                @change="
                  viewportZoom = numeric($event);
                  viewportPan = { x: 0, y: 0 };
                "
              >
                <option :value="0.5">50%</option>
                <option :value="1">{{ tr('适应') }}</option>
                <option :value="2">200%</option>
                <option :value="4">400%</option></select
              ><select :aria-label="tr('预览清晰度')" v-model="previewQuality">
                <option value="auto">{{ tr('自动') }}</option>
                <option value="full">{{ tr('完整') }}</option>
                <option value="half">1/2</option></select
              ><button :title="tr('适应画布；Alt或中键拖动画布')" @click="resetViewport">
                {{ tr('复位') }}
              </button>
            </div>
          </div>
        </section>
        <PanelDivider
          class="inspector-divider"
          :orientation="stackedPanels ? 'horizontal' : 'vertical'"
          :label="tr('拖动调节预览和属性面板大小；双击恢复默认')"
          :value="dividerValues.inspector"
          @start="panelLayout.start('inspector')"
          @resize="panelLayout.resize"
          @finish="panelLayout.finish"
          @step="(delta) => panelLayout.step('inspector', delta)"
          @reset="panelLayout.reset('inspector')"
        />
        <Inspector
          :project="project"
          :item="selectedItem"
          :disabled="busy || selectedLocked"
          :commit-html="onCommand"
          @text="updateText"
          :selection="selectedIds"
          :time="time"
          @command="onCommand"
          @commands="command"
          @error="showError"
          @visual="(changes) => selectedCommand('set_transform', changes)"
          @audio="(changes) => selectedCommand('set_audio', changes)"
          @move="(begin) => moveSelection(selected, begin)"
          @duration="
            (length) =>
              selectedItem &&
              trimSelection(
                selected,
                selectedItem.placement.begin,
                selectedItem.placement.begin + length
              )
          "
          @source="
            (begin) =>
              selectedItem &&
              command([
                {
                  action: 'trim_clip',
                  itemId: selected,
                  sourceInSeconds: seconds(begin),
                  durationSeconds: seconds(
                    selectedItem.placement.end - selectedItem.placement.begin
                  )
                }
              ])
          "
          @canvas="(width, height) => command([{ action: 'configure_project', width, height }])"
          @fps="(fps) => command([{ action: 'configure_project', fps }])"
        />
      </section>
      <PanelDivider
        class="timeline-divider"
        orientation="horizontal"
        :label="tr('拖动调节预览区和时间轴高度；双击恢复默认')"
        :value="dividerValues.timeline"
        @start="panelLayout.start('timeline')"
        @resize="panelLayout.resize"
        @finish="panelLayout.finish"
        @step="(delta) => panelLayout.step('timeline', delta)"
        @reset="panelLayout.reset('timeline')"
      />
      <section class="timeline-panel panel">
        <div class="timeline-toolbar">
          <div class="timeline-tools">
            <button :title="tr('撤销 ⌘Z')" :disabled="!canUndo || busy" @click="undo">
              <Icon name="undo" /></button
            ><button :title="tr('重做 ⇧⌘Z')" :disabled="!canRedo || busy" @click="redo">
              <Icon name="redo" /></button
            ><span class="tool-separator" /><span
              class="tool-indicator active"
              :title="tr('选择工具')"
              ><Icon name="cursor" /></span
            ><button
              :title="tr('分割 ⌘B')"
              :disabled="!selectedItem || busy || selectedLocked"
              @click="splitSelected"
            >
              <Icon name="cut" /></button
            ><button
              :title="tr('删除片段')"
              :disabled="!selectedItem || busy || selectedLocked"
              @click="deleteSelected"
            >
              <Icon name="trash" /></button
            ><span class="tool-separator" /><button
              :title="tr('添加文字')"
              :disabled="busy"
              @click="
                panel = 'text';
                insertText();
              "
            >
              <Icon name="text" />
            </button>
          </div>
          <div class="timeline-tools advanced-tools">
            <button
              :disabled="!selectedIds.length || busy"
              :title="tr('复制所选片段')"
              :aria-label="tr('复制所选片段')"
              @click="command([{ action: 'duplicate_clips', itemIds: selectedIds }])"
            >
              <Icon name="duplicate" />
            </button>
            <button
              :disabled="selectedIds.length < 2 || busy"
              :title="tr('分组')"
              :aria-label="tr('分组')"
              @click="command([{ action: 'group_clips', itemIds: selectedIds }])"
            >
              <Icon name="group" />
            </button>
            <button
              :disabled="!selectedIds.length || busy"
              :title="tr('解组')"
              :aria-label="tr('解组')"
              @click="detach('groups')"
            >
              <Icon name="ungroup" />
            </button>
            <button
              :disabled="selectedIds.length < 2 || busy"
              :title="tr('关联片段')"
              :aria-label="tr('关联片段')"
              @click="command([{ action: 'link_clips', itemIds: selectedIds }])"
            >
              <Icon name="link" />
            </button>
            <button
              :disabled="!selectedIds.length || busy"
              :title="tr('解除关联')"
              :aria-label="tr('解除关联')"
              @click="detach('links')"
            >
              <Icon name="unlink" />
            </button>
            <button
              :disabled="!selectedIds.length || busy"
              :title="tr('波纹删除')"
              :aria-label="tr('波纹删除')"
              @click="
                command([{ action: 'ripple_delete', itemIds: selectedIds, scope: rippleScope }])
              "
            >
              <Icon name="rippleDelete" />
            </button>
            <select
              v-model="rippleScope"
              :aria-label="tr('波纹编辑范围')"
              :title="tr('波纹编辑范围')"
            >
              <option value="selected">{{ tr('所选轨道') }}</option>
              <option value="syncLocked">{{ tr('所选及同步轨道') }}</option>
              <option value="all">{{ tr('所有轨道') }}</option>
            </select>
          </div>
          <div class="timeline-tools timeline-right">
            <button
              :class="{ active: snap }"
              :title="tr('预览与时间轴吸附（拖动时按住 Ctrl 临时关闭）')"
              :aria-label="tr('吸附')"
              :aria-pressed="snap"
              @click="snap = !snap"
            >
              <Icon name="magnet" /></button
            ><span class="tool-separator" /><button
              :title="tr('缩小时间轴')"
              @click="zoom = Math.max(15, zoom - 15)"
            >
              <Icon name="zoomOut" :size="18" /></button
            ><input
              type="range"
              :aria-label="tr('时间轴缩放')"
              v-model.number="zoom"
              min="15"
              max="220"
            /><button :title="tr('放大时间轴')" @click="zoom = Math.min(220, zoom + 15)">
              <Icon name="zoomIn" :size="18" />
            </button>
          </div>
        </div>
        <Timeline
          :project="project"
          :time="time"
          :selected="selected"
          :zoom="zoom"
          :snap="snap"
          :busy="busy || editing"
          :media-url="mediaUrl"
          @seek="seek"
          @select="selectMany"
          :selection="selectedIds"
          @draft="draft"
          @cancel="cancelDraft"
          @commit="commitDraft"
          @track="toggleTrack"
          @add="dropAsset"
        />
      </section>
    </div>
    <div v-if="error" class="message error" role="alert">
      <Icon name="info" :size="16" /><span>{{ tr(error) }}</span
      ><button @click="error = ''" :aria-label="tr('关闭错误')">
        <Icon name="close" :size="16" />
      </button>
    </div>
    <div v-else-if="notice" class="message notice" role="status">
      <span>{{ tr(notice) }}</span
      ><button @click="notice = ''" :aria-label="tr('关闭提示')">
        <Icon name="close" :size="14" />
      </button>
    </div>
    <VisionSetupDialog
      v-if="visionSetupReady"
      :client="client"
      :auto-prompt="snapshot?.example !== 'starter'"
    />
    <ExportDialog
      :client="client"
      :result="exportResult"
      :progress="exportProgress"
      :cancellable="!!activeExportJob && !cancellingExport"
      @cancel="cancelExport"
      @close="exportResult = null"
    />
    <div v-if="showOpen" class="dialog-backdrop" @click.self="showOpen = false">
      <form class="help-dialog panel" @submit.prevent="openSession">
        <div class="panel-heading">
          <strong>{{ tr('打开 .vcutweb / .vcut 作品目录') }}</strong
          ><button type="button" @click="showOpen = false">{{ tr('关闭') }}</button>
        </div>
        <input
          v-model="openPath"
          :aria-label="tr('作品目录路径')"
          :placeholder="tr('/素材目录/作品.vcutweb')"
          style="width: 100%"
        /><button class="primary" type="submit">{{ tr('打开作品') }}</button>
      </form>
    </div>
    <div v-if="shortcuts" class="dialog-backdrop" @click.self="shortcuts = false">
      <section class="help-dialog panel">
        <div class="panel-heading">
          <strong>{{ tr('键盘快捷键') }}</strong
          ><button @click="shortcuts = false" :aria-label="tr('关闭帮助')">
            <Icon name="close" />
          </button>
        </div>
        <p>
          <span>{{ tr('播放 / 暂停') }}</span
          ><kbd>Space</kbd>
        </p>
        <p>
          <span>{{ tr('逐帧移动') }}</span
          ><kbd>← →</kbd>
        </p>
        <p>
          <span>{{ tr('分割片段') }}</span
          ><kbd>⌘ / Ctrl B</kbd>
        </p>
        <p>
          <span>{{ tr('粘贴文件到时间轴') }}</span
          ><kbd>⌘ / Ctrl V</kbd>
        </p>
        <p>
          <span>{{ tr('撤销 / 重做') }}</span
          ><kbd>⌘ Z / ⇧⌘ Z</kbd>
        </p>
        <p>
          <span>{{ tr('删除片段') }}</span
          ><kbd>Delete</kbd>
        </p>
      </section>
    </div>
  </main>
</template>
