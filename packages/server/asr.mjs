import { TtsJob } from './tts.mjs';
import { modelError } from './model-store.mjs';
import { WHISPER_BASE } from './asr-models.mjs';
import { seconds } from '../core/project.mjs';

const active = (job) => job && ['queued', 'running'].includes(job.status().state);
export const ASR_LANGUAGES = ['auto', 'zh', 'en', 'ja', 'ko', 'fr', 'de', 'es', 'ru'];
export function validateAsrRequest(data, project) {
  if (
    !data ||
    Object.keys(data).some(
      (key) =>
        !['version', 'itemId', 'assetId', 'language', 'backend', 'insert', 'startSeconds'].includes(
          key
        )
    )
  )
    throw modelError('识别参数无效；ASR 固定使用 Base');
  if (!data || !Number.isSafeInteger(data.version) || data.version < 0)
    throw modelError('请提供当前作品版本');
  if ((typeof data.itemId === 'string') === (typeof data.assetId === 'string'))
    throw modelError('请提供 itemId 或 assetId 中的一项');
  if (
    !ASR_LANGUAGES.includes(data.language ?? 'auto') ||
    !['auto', 'webgpu', 'wasm'].includes(data.backend ?? 'auto')
  )
    throw modelError('识别语言或后端无效');
  if (data.insert !== undefined && typeof data.insert !== 'boolean')
    throw modelError('insert 必须是布尔值');
  if (
    data.startSeconds !== undefined &&
    (!Number.isFinite(data.startSeconds) || data.startSeconds < 0 || data.startSeconds > 86400)
  )
    throw modelError('字幕插入时间无效');
  const item = data.itemId
    ? project.timeline.tracks.flatMap((t) => t.items).find((i) => i.id === data.itemId)
    : null;
  if (data.itemId && !item) throw modelError('识别片段不存在', 404);
  const asset = project.assets.find((a) => a.id === (item?.clip.assetId || data.assetId));
  if (!asset || !['video', 'audio'].includes(asset.kind) || !asset.hasAudio)
    throw modelError('请选择包含音轨的视频或音频素材');
  const rate = item ? (item.clip.retime?.constantRatePpm ?? 1000000) / 1000000 : 1;
  if (rate <= 0) throw modelError('倒放片段不支持语音识别，请使用原素材');
  const sourceBegin = item ? seconds(item.clip.source.begin) : 0;
  const sourceEnd = item ? seconds(item.clip.source.end) : seconds(asset.duration);
  const startSeconds = data.startSeconds ?? (item ? seconds(item.placement.begin) : 0);
  const duration = sourceEnd - sourceBegin;
  if (!(duration > 0) || duration > 3600 || startSeconds + duration / rate > 86400)
    throw modelError('单次识别音频应在 0 至 60 分钟内，字幕须位于作品一天内');
  return {
    version: data.version,
    assetId: asset.id,
    itemId: item?.id,
    language: data.language ?? 'auto',
    backend: data.backend ?? 'auto',
    insert: data.insert ?? true,
    startSeconds,
    sourceBegin,
    sourceEnd,
    rate,
    duration
  };
}

export function validateAsrResult(data, spec) {
  if (
    !data ||
    !['webgpu', 'wasm'].includes(data.backend) ||
    typeof data.text !== 'string' ||
    data.text.length > 200000 ||
    !Array.isArray(data.segments) ||
    data.segments.length > 10000
  )
    throw modelError('识别结果格式或长度无效');
  let previousEnd = 0;
  const segments = data.segments.map((segment) => {
    if (
      typeof segment?.text !== 'string' ||
      !segment.text.trim() ||
      segment.text.length > 1000 ||
      !Number.isFinite(segment.start) ||
      !Number.isFinite(segment.end) ||
      segment.start < previousEnd ||
      segment.end <= segment.start ||
      segment.end > spec.duration + 0.001
    )
      throw modelError('字幕时间、文字或顺序无效');
    previousEnd = segment.end;
    return {
      text: segment.text.trim(),
      start: spec.startSeconds + segment.start / spec.rate,
      end: spec.startSeconds + Math.min(segment.end, spec.duration) / spec.rate
    };
  });
  if (data.text.trim() && !segments.length) throw modelError('识别文本缺少有效时间戳');
  return { text: data.text.trim(), segments, actualBackend: data.backend };
}

/** Reuse the existing browser lease/cancellation protocol; ASR returns timed text instead of WAV. */
export class AsrJob extends TtsJob {
  constructor(spec, { emit = () => {}, models, commit, heartbeatMs }) {
    super(
      { ...spec, model: WHISPER_BASE.id, modelBaseUrl: `/asr-models/${WHISPER_BASE.id}/` },
      {
        emit: (type, value) => emit(type.replace(/^tts-/, 'asr-'), value),
        heartbeatMs
      }
    );
    this.models = models;
    this.commitText = commit;
  }
  async prepare(worker, id) {
    this.check(worker, id);
    if (!this.preparing)
      this.preparing = this.models
        .ensure({
          signal: this.controller.signal,
          onProgress: (status) => {
            if (active(this))
              this.progress(worker, id, {
                phase: 'download-model',
                progress: 0.15 * status.progress
              });
          }
        })
        .then((modelBaseUrl) => {
          this.active();
          return { modelBaseUrl };
        });
    return this.preparing;
  }
  async textResult(worker, id, data) {
    this.check(worker, id);
    if (this.writing) throw modelError('识别结果正在保存', 409);
    this.writing = true;
    try {
      const result = validateAsrResult(data, this.spec);
      this.active();
      const committed = await this.commitText(result.segments, this);
      this.active();
      this.value = { ...this.value, ...result, ...committed, phase: committed.state, progress: 1 };
      clearInterval(this.timer);
      this.emit('tts-progress', this.status());
      return this.status();
    } catch (error) {
      this.fail(error.message);
      throw error;
    } finally {
      this.writing = false;
    }
  }
}

export function createAsrRoutes({ models, snapshot, notify, body, json }) {
  return async (req, res, url, session, action) => {
    if (action === 'asr' && req.method === 'POST') {
      const spec = validateAsrRequest(await body(req), session.project);
      if (spec.version !== session.version)
        return json(res, 409, { error: '作品版本冲突', ...snapshot(session) });
      if (!session.clients.size)
        return json(res, 409, {
          error: '请打开作品预览页执行本地语音识别',
          code: 'BROWSER_REQUIRED',
          previewUrl: snapshot(session).previewUrl
        });
      if (active(session.asrJob)) throw modelError('此会话已有识别任务，请等待或取消', 409);
      const emit = (type, value) => {
        for (const client of session.clients)
          client.write(`event: ${type}\ndata: ${JSON.stringify(value)}\n\n`);
      };
      session.asrJob = new AsrJob(
        { ...spec, previewUrl: snapshot(session).previewUrl },
        {
          models,
          emit,
          commit: async (segments, job) => {
            if (!spec.insert || !segments.length) return { state: 'completed' };
            if (spec.version !== session.version)
              return {
                state: 'conflict',
                error: '作品版本已变化，识别结果已保留，请基于新版本插入字幕'
              };
            const edited = session.history.prepare(session.project, [
              { action: 'add_subtitles', segments }
            ]);
            job.active();
            session.history.accept(edited);
            session.project = edited.project;
            session.version++;
            notify(session);
            return {
              state: 'completed',
              itemIds: edited.operations[0].itemIds,
              resultVersion: session.version
            };
          }
        }
      );
      emit('asr-request', session.asrJob.status());
      return json(res, 202, session.asrJob.status());
    }
    const job = session.asrJob;
    if (action === 'asr-job' && req.method === 'GET')
      return json(res, 200, job?.status() || { state: 'idle' });
    if (action === 'asr-job' && req.method === 'DELETE')
      return json(res, 200, job?.cancel() || { state: 'idle' });
    if (action?.startsWith('asr-job/') && req.method === 'POST') {
      if (!job) throw modelError('识别任务不存在', 404);
      const data = await body(req),
        operation = action.slice('asr-job/'.length),
        worker = req.headers['x-asr-worker'];
      if (operation === 'claim') return json(res, 200, job.claim(data.id));
      job.check(worker, data.id);
      if (operation === 'prepare') return json(res, 200, await job.prepare(worker, data.id));
      if (operation === 'result')
        return json(res, 200, await job.textResult(worker, data.id, data));
      if (operation === 'progress') return json(res, 200, job.progress(worker, data.id, data));
      if (operation === 'heartbeat') return json(res, 200, job.status());
      if (operation === 'fail') return json(res, 200, job.fail(data.error || '浏览器语音识别失败'));
    }
    return json(res, 405, { error: '识别接口操作无效' });
  };
}
