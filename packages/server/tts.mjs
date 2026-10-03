import { randomBytes } from 'node:crypto';
import { lstat, mkdir, open, rename, rm } from 'node:fs/promises';
import { join } from 'node:path';
import { KOKORO_MODEL_ID, KOKORO_DTYPES, ttsError } from './tts-models.mjs';

export const TTS_MAX_WAV_BYTES = 64 * 1024 * 1024;
export const TTS_MAX_TEXT_LENGTH = 8000;
const activeStates = new Set(['queued', 'running']);

export function validateTtsRequest(data) {
  if (
    !data ||
    typeof data !== 'object' ||
    typeof data.text !== 'string' ||
    !data.text.trim() ||
    data.text.length > TTS_MAX_TEXT_LENGTH
  )
    throw ttsError(`请提供 1 至 ${TTS_MAX_TEXT_LENGTH} 字的配音文案`);
  if (data.text.split(/[。！？!?\n]+/u).filter((part) => part.trim()).length > 500)
    throw ttsError('单次语音合成最多 500 个文案段落');
  const voice = data.voice ?? 'zf_001',
    speed = data.speed ?? 1,
    backend = data.backend ?? 'auto',
    dtype = data.dtype ?? 'fp32';
  if (dtype !== 'fp32') throw ttsError('当前语音合成仅支持 FP32 模型');
  if (
    typeof voice !== 'string' ||
    !/^[a-z]{2}_[a-z0-9]+$/.test(voice) ||
    !Number.isFinite(speed) ||
    speed < 0.5 ||
    speed > 2 ||
    !['auto', 'webgpu', 'wasm'].includes(backend) ||
    !Object.hasOwn(KOKORO_DTYPES, dtype)
  )
    throw ttsError('音色、语速、推理后端或模型精度无效');
  if (!Number.isSafeInteger(data.version) || data.version < 0) throw ttsError('请提供当前作品版本');
  if (
    data.startSeconds !== undefined &&
    (!Number.isFinite(data.startSeconds) || data.startSeconds < 0 || data.startSeconds > 86400)
  )
    throw ttsError('音频插入时间无效');
  if (data.trackId !== undefined && (typeof data.trackId !== 'string' || data.trackId.length > 200))
    throw ttsError('音轨无效');
  if (data.insert !== undefined && typeof data.insert !== 'boolean')
    throw ttsError('insert 必须是布尔值');
  return {
    text: data.text.trim(),
    voice,
    speed,
    backend,
    dtype,
    version: data.version,
    startSeconds: data.startSeconds ?? 0,
    trackId: data.trackId,
    insert: data.insert ?? true
  };
}

/** Accept only bounded, nonempty PCM16/float32 WAV, with coherent RIFF/chunk/sample sizes. */
export function validateTtsWav(bytes) {
  if (
    bytes.length < 44 ||
    bytes.length > TTS_MAX_WAV_BYTES ||
    bytes.toString('ascii', 0, 4) !== 'RIFF' ||
    bytes.toString('ascii', 8, 12) !== 'WAVE' ||
    bytes.readUInt32LE(4) + 8 !== bytes.length
  )
    throw ttsError('语音结果不是有效的 PCM WAV');
  let format, audio;
  for (let cursor = 12; cursor < bytes.length;) {
    if (cursor + 8 > bytes.length) throw ttsError('WAV 区块不完整');
    const kind = bytes.toString('ascii', cursor, cursor + 4),
      size = bytes.readUInt32LE(cursor + 4),
      start = cursor + 8,
      end = start + size;
    if (end > bytes.length || end + (size % 2) > bytes.length) throw ttsError('WAV 区块长度无效');
    if (kind === 'fmt ') {
      if (format || size < 16 || size > 40) throw ttsError('WAV 音频格式无效');
      format = {
        codec: bytes.readUInt16LE(start),
        channels: bytes.readUInt16LE(start + 2),
        sampleRate: bytes.readUInt32LE(start + 4),
        byteRate: bytes.readUInt32LE(start + 8),
        blockAlign: bytes.readUInt16LE(start + 12),
        bits: bytes.readUInt16LE(start + 14)
      };
    } else if (kind === 'data') {
      if (audio) throw ttsError('WAV 数据区块重复');
      audio = { start, size };
    }
    cursor = end + (size % 2);
  }
  if (
    !format ||
    !audio ||
    !audio.size ||
    ![1, 2].includes(format.channels) ||
    format.sampleRate < 8000 ||
    format.sampleRate > 96000 ||
    !((format.codec === 1 && format.bits === 16) || (format.codec === 3 && format.bits === 32)) ||
    format.blockAlign !== (format.channels * format.bits) / 8 ||
    format.byteRate !== format.sampleRate * format.blockAlign ||
    audio.size % format.blockAlign
  )
    throw ttsError('WAV 必须是单声道或双声道 PCM16/float32 音频');
  const durationSeconds = audio.size / format.byteRate;
  if (durationSeconds > 300) throw ttsError('单次语音结果不能超过 5 分钟');
  if (format.codec === 3)
    for (let cursor = audio.start; cursor < audio.start + audio.size; cursor += 4) {
      if (!Number.isFinite(bytes.readFloatLE(cursor))) throw ttsError('WAV 样本包含无效数值');
    }
  return { ...format, frames: audio.size / format.blockAlign, durationSeconds };
}

/** One browser owner per job. Tokens never appear in status/SSE payloads. */
export class TtsJob {
  constructor(spec, { emit = () => {}, heartbeatMs = 30000, root, allowed, probe, commit }) {
    this.spec = {
      id: randomBytes(18).toString('hex'),
      model: KOKORO_MODEL_ID,
      modelBaseUrl: `/tts-models/${KOKORO_MODEL_ID}/`,
      ...spec
    };
    this.value = { ...this.spec, state: 'queued', phase: 'queued', progress: 0 };
    this.emit = emit;
    this.root = root;
    this.allowed = allowed;
    this.probe = probe;
    this.commit = commit;
    this.lastBeat = Date.now();
    this.owner = null;
    this.writing = false;
    this.controller = new AbortController();
    this.timer = setInterval(
      () => {
        if (Date.now() - this.lastBeat > heartbeatMs)
          this.fail('浏览器语音工作线程超过 30 秒未响应');
      },
      Math.min(1000, heartbeatMs)
    );
    this.timer.unref();
  }
  status() {
    return structuredClone(this.value);
  }
  active() {
    if (!activeStates.has(this.value.state)) throw ttsError('语音任务已结束', 409);
  }
  claim(id) {
    this.active();
    if (id !== this.spec.id) throw ttsError('语音任务已更新', 409);
    if (this.owner) throw ttsError('语音任务已由其他窗口接管', 409);
    this.owner = randomBytes(32).toString('hex');
    this.lastBeat = Date.now();
    this.value.state = 'running';
    this.value.phase = 'loading';
    this.emit('tts-progress', this.status());
    return { worker: this.owner, job: this.status() };
  }
  check(worker, id) {
    if (!this.owner || worker !== this.owner) throw ttsError('语音工作线程凭证无效', 403);
    if (id !== this.spec.id) throw ttsError('语音任务已更新', 409);
    this.active();
    this.lastBeat = Date.now();
  }
  progress(worker, id, data) {
    this.check(worker, id);
    if (
      data.progress !== undefined &&
      (!Number.isFinite(data.progress) || data.progress < 0 || data.progress > 1)
    )
      throw ttsError('语音进度无效');
    if (data.backend !== undefined && !['webgpu', 'wasm'].includes(data.backend))
      throw ttsError('实际推理后端无效');
    if (data.phase !== undefined && (typeof data.phase !== 'string' || data.phase.length > 80))
      throw ttsError('语音阶段无效');
    if (data.progress !== undefined) this.value.progress = data.progress;
    if (data.phase !== undefined) this.value.phase = data.phase;
    if (data.backend !== undefined) {
      this.value.actualBackend = data.backend;
    }
    this.emit('tts-progress', this.status());
    return this.status();
  }
  stop(state, error) {
    if (!activeStates.has(this.value.state)) return this.status();
    this.value.state = state;
    this.value.phase = state;
    if (error) this.value.error = String(error).slice(0, 2000);
    clearInterval(this.timer);
    this.controller.abort();
    this.emit(state === 'cancelled' ? 'tts-cancel' : 'tts-progress', this.status());
    return this.status();
  }
  fail(error) {
    return this.stop('error', error);
  }
  cancel() {
    return this.stop('cancelled');
  }
  async result(req, worker, id, reportedBackend) {
    this.check(worker, id);
    if (this.writing) throw ttsError('语音结果正在保存', 409);
    if ((req.headers['content-type'] || '').split(';')[0] !== 'audio/wav')
      throw ttsError('语音结果必须使用 audio/wav');
    if (req.headers['content-length'] && Number(req.headers['content-length']) > TTS_MAX_WAV_BYTES)
      throw ttsError('语音结果超过大小限制', 413);
    const actualBackend = req.headers['x-tts-backend'] ?? reportedBackend;
    if (actualBackend !== undefined && !['webgpu', 'wasm'].includes(actualBackend))
      throw ttsError('实际推理后端无效');
    if (actualBackend) {
      this.value.actualBackend = actualBackend;
    }
    this.writing = true;
    let partial,
      target,
      keep = false;
    try {
      this.value.phase = 'saving';
      const chunks = [];
      let size = 0;
      for await (const bytes of req) {
        this.controller.signal.throwIfAborted();
        this.lastBeat = Date.now();
        size += bytes.length;
        if (size > TTS_MAX_WAV_BYTES) throw ttsError('语音结果超过大小限制', 413);
        chunks.push(bytes);
      }
      const wav = Buffer.concat(chunks);
      validateTtsWav(wav);
      this.active();
      let directory = this.root;
      for (const child of ['.videocut-generated', 'speech']) {
        directory = join(directory, child);
        const info = await lstat(directory).catch((error) => {
          if (error.code !== 'ENOENT') throw error;
        });
        if (info && (!info.isDirectory() || info.isSymbolicLink()))
          throw ttsError('语音生成目录必须是普通目录');
        if (!info)
          await mkdir(directory).catch((error) => {
            if (error.code !== 'EEXIST') throw error;
          });
        await this.allowed(directory);
      }
      target = join(directory, `speech-${this.spec.id}.wav`);
      partial = `${target}.partial-${randomBytes(8).toString('hex')}`;
      const handle = await open(partial, 'wx', 0o600);
      try {
        await handle.writeFile(wav);
        await handle.sync();
      } finally {
        await handle.close();
      }
      this.active();
      await rename(partial, target);
      partial = null;
      const asset = await this.probe(await this.allowed(target));
      this.active();
      asset.name = `配音 ${this.spec.text.slice(0, 30)}`;
      const result = await this.commit(asset, this);
      this.active();
      keep = true;
      this.value = {
        ...this.value,
        ...result,
        asset,
        path: asset.path,
        phase: result.state,
        progress: 1
      };
      clearInterval(this.timer);
      this.emit('tts-progress', this.status());
      return this.status();
    } catch (error) {
      if (activeStates.has(this.value.state)) this.fail(error.message);
      throw error;
    } finally {
      this.writing = false;
      if (partial) await rm(partial, { force: true });
      if (target && !keep) await rm(target, { force: true });
    }
  }
}

export function createTtsRoutes({
  models,
  roots,
  allowed,
  probe,
  projectPaths,
  notify,
  snapshot,
  body,
  json
}) {
  return async (req, res, url, session, action) => {
    if (action === 'tts' && req.method === 'POST') {
      const spec = validateTtsRequest(await body(req));
      if (spec.version !== session.version)
        return json(res, 409, { error: '作品版本冲突', ...snapshot(session) });
      if (!session.clients.size)
        return json(res, 409, {
          error: '请打开作品网页完成浏览器语音合成',
          code: 'BROWSER_REQUIRED',
          previewUrl: snapshot(session).previewUrl
        });
      if (session.ttsJob && activeStates.has(session.ttsJob.status().state))
        throw ttsError('此会话已有语音任务，请等待或取消', 409);
      if (spec.trackId) {
        const track = session.project.timeline.tracks.find((t) => t.id === spec.trackId);
        if (!track || track.type !== 'audio' || track.locked)
          throw ttsError('目标音轨不存在、类型不匹配或已锁定');
      }
      try {
        await models.require(spec.dtype, spec.voice);
      } catch (error) {
        error.previewUrl = snapshot(session).previewUrl;
        throw error;
      }
      if (spec.version !== session.version)
        return json(res, 409, { error: '作品版本冲突', ...snapshot(session) });
      if (session.closed || !session.clients.size)
        return json(res, 409, {
          error: '作品网页已关闭，请重新打开后合成',
          code: 'BROWSER_REQUIRED',
          previewUrl: snapshot(session).previewUrl
        });
      if (session.ttsJob && activeStates.has(session.ttsJob.status().state))
        throw ttsError('此会话已有语音任务', 409);
      const emit = (type, value) => {
        for (const client of session.clients)
          client.write(`event: ${type}\ndata: ${JSON.stringify(value)}\n\n`);
      };
      const job = (session.ttsJob = new TtsJob(
        { ...spec, previewUrl: snapshot(session).previewUrl },
        {
          emit,
          root: roots[0],
          allowed,
          probe,
          commit: async (asset, activeJob) => {
            if (!spec.insert) return { state: 'completed' };
            if (spec.version !== session.version)
              return { state: 'conflict', error: '作品版本已变化，语音已保存，可重新导入' };
            const edited = session.history.prepare(session.project, [
              { action: 'add_asset', asset, trackId: spec.trackId, startSeconds: spec.startSeconds }
            ]);
            await projectPaths(edited.project);
            activeJob.active();
            if (spec.version !== session.version)
              return { state: 'conflict', error: '作品版本已变化，语音已保存，可重新导入' };
            session.history.accept(edited);
            session.project = edited.project;
            session.version++;
            notify(session);
            return {
              state: 'completed',
              itemId: edited.operations[0].itemId,
              resultVersion: session.version
            };
          }
        }
      ));
      emit('tts-request', job.status());
      return json(res, 202, job.status());
    }
    if (action === 'tts-job') {
      if (req.method === 'GET')
        return json(res, 200, session.ttsJob?.status() || { state: 'idle' });
      if (req.method === 'DELETE')
        return json(res, 200, session.ttsJob?.cancel() || { state: 'idle' });
    }
    if (action?.startsWith('tts-job/') && req.method === 'POST') {
      const job = session.ttsJob;
      if (!job) throw ttsError('语音任务不存在', 404);
      const operation = action.slice('tts-job/'.length),
        worker = req.headers['x-tts-worker'];
      if (operation === 'result')
        return json(
          res,
          200,
          await job.result(
            req,
            worker,
            url.searchParams.get('job'),
            url.searchParams.get('backend') ?? undefined
          )
        );
      const data = await body(req),
        id = data.id || url.searchParams.get('job');
      if (operation === 'claim') return json(res, 200, job.claim(id));
      job.check(worker, id);
      if (operation === 'progress') return json(res, 200, job.progress(worker, id, data));
      if (operation === 'heartbeat') return json(res, 200, job.status());
      if (operation === 'fail') return json(res, 200, job.fail(data.error || '浏览器语音合成失败'));
    }
    return json(res, 405, { error: '语音接口操作无效' });
  };
}
