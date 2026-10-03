import { TtsJob } from './tts.mjs';
import { modelError } from './model-store.mjs';
import { FASTVLM } from './vision-manifest.mjs';
import { sampleTimes, planVideoSegments, VISION_MAX_FRAMES } from '../vision/runtime.mjs';
import { seconds, createProject } from '../core/project.mjs';

const active = (job) => job && ['queued', 'running'].includes(job.status().state);
export function assertVisionSource(spec, info) {
  if (
    spec.sourceIdentity &&
    spec.sourceIdentity !== `${info.dev}:${info.ino}:${info.size}:${info.mtimeMs}`
  )
    throw modelError('视觉素材在分析期间发生变化，请重新提交', 409, 'VISION_SOURCE_CHANGED');
}
export function validateVisionRequest(data, project, pathAsset) {
  if (!data || !Number.isSafeInteger(data.version) || data.version < 0)
    throw modelError('请提供当前作品版本');
  if (
    ['path', 'assetId', 'itemId'].filter((key) => typeof data[key] === 'string' && data[key])
      .length !== 1
  )
    throw modelError('请提供 path、assetId 或 itemId 中的一项');
  const item = data.itemId
    ? project.timeline.tracks.flatMap((t) => t.items).find((i) => i.id === data.itemId)
    : null;
  if (data.itemId && !item) throw modelError('视觉片段不存在', 404);
  const asset =
    pathAsset || project.assets.find((a) => a.id === (item?.clip.assetId || data.assetId));
  if (!asset || !['image', 'video'].includes(asset.kind))
    throw modelError('视觉理解仅支持图像或视频');
  if (data.expectedKind !== undefined && data.expectedKind !== asset.kind)
    throw modelError(`此接口需要${data.expectedKind === 'image' ? '图像' : '视频'}路径`);
  const resultFormat = data.resultFormat ?? 'frames';
  if (
    !['frames', 'description', 'segments'].includes(resultFormat) ||
    (resultFormat === 'description' && asset.kind !== 'image') ||
    (resultFormat === 'segments' && asset.kind !== 'video')
  )
    throw modelError('视觉结果格式与素材类型不匹配');
  const prompt =
    data.prompt ??
    'Describe the visible subjects, actions, scene, and readable text. Be concise. Do not guess details that are not visible.';
  const backend = data.backend ?? 'auto',
    maxNewTokens = data.maxNewTokens ?? 192,
    maxFrames = data.maxFrames ?? 8;
  if (
    typeof prompt !== 'string' ||
    !prompt.trim() ||
    prompt.length > 4000 ||
    prompt.includes('<image>') ||
    !['auto', 'webgpu', 'wasm'].includes(backend) ||
    !Number.isInteger(maxNewTokens) ||
    maxNewTokens < 1 ||
    maxNewTokens > 512 ||
    !Number.isInteger(maxFrames) ||
    maxFrames < 1 ||
    maxFrames > VISION_MAX_FRAMES
  )
    throw modelError('视觉提示、后端、帧数或输出长度无效');
  const clipBegin = item ? seconds(item.clip.source.begin) : 0;
  const clipEnd = item ? seconds(item.clip.source.end) : seconds(asset.duration);
  const sourceBegin = data.beginSeconds ?? clipBegin,
    sourceEnd = data.endSeconds ?? clipEnd;
  const rate = item ? (item.clip.retime?.constantRatePpm ?? 1000000) / 1000000 : 1;
  if (rate <= 0) throw modelError('倒放片段请通过 assetId 分析原素材');
  if (
    !Number.isFinite(sourceBegin) ||
    !Number.isFinite(sourceEnd) ||
    sourceBegin < clipBegin ||
    sourceEnd > clipEnd ||
    sourceEnd <= sourceBegin ||
    sourceEnd - sourceBegin > 3600
  )
    throw modelError('视觉采样范围必须位于素材或片段内，且不超过 60 分钟');
  let planned;
  if (resultFormat === 'segments') {
    try {
      planned = planVideoSegments(sourceBegin, sourceEnd, data);
    } catch (error) {
      throw modelError(error.message);
    }
  }
  return {
    version: data.version,
    assetId: asset.id,
    itemId: item?.id,
    path: asset.path,
    sourceIdentity: asset.sourceIdentity,
    kind: asset.kind,
    prompt: prompt.trim(),
    backend,
    maxNewTokens,
    sourceBegin,
    sourceEnd,
    sampleTimes: asset.kind === 'image' ? [0] : sampleTimes(sourceBegin, sourceEnd, maxFrames),
    timelineBegin: item ? seconds(item.placement.begin) : undefined,
    clipBegin,
    rate,
    resultFormat,
    ...(planned
      ? {
          segmentPlan: planned.segments,
          sampling: planned.sampling,
          sampleTimes: planned.segments.flatMap((segment) => segment.sampleTimes)
        }
      : {})
  };
}

function validatedSample(f, requestedSeconds, spec) {
  if (
    !f ||
    f.requestedSeconds !== requestedSeconds ||
    !Number.isFinite(f.sourceSeconds) ||
    !Number.isFinite(f.durationSeconds) ||
    f.durationSeconds <= 0 ||
    (spec.kind === 'image'
      ? f.sourceSeconds !== 0
      : f.sourceSeconds < 0 ||
        f.sourceSeconds > requestedSeconds + 0.002 ||
        f.sourceSeconds + f.durationSeconds < requestedSeconds - 0.002)
  )
    throw modelError('视觉实际帧时间戳无效');
  return { requestedSeconds, sourceSeconds: f.sourceSeconds, durationSeconds: f.durationSeconds };
}

export function validateVisionResult(data, spec) {
  if (spec.resultFormat === 'segments') {
    if (
      !data ||
      !['webgpu', 'wasm'].includes(data.backend) ||
      !Array.isArray(data.segments) ||
      data.segments.length !== spec.segmentPlan.length
    )
      throw modelError('视觉区间结果格式无效');
    const segments = data.segments.map((segment, i) => {
      const plan = spec.segmentPlan[i];
      if (
        !segment ||
        segment.startSeconds !== plan.startSeconds ||
        segment.endSeconds !== plan.endSeconds ||
        typeof segment.text !== 'string' ||
        !segment.text.trim() ||
        segment.text.length > 12000 ||
        !Array.isArray(segment.samples) ||
        segment.samples.length !== plan.sampleTimes.length
      )
        throw modelError('视觉区间边界、描述或采样数量无效');
      return {
        startSeconds: plan.startSeconds,
        endSeconds: plan.endSeconds,
        text: segment.text.trim(),
        samples: segment.samples.map((sample, j) =>
          validatedSample(sample, plan.sampleTimes[j], spec)
        ),
        ...(spec.timelineBegin !== undefined
          ? {
              timelineStartSeconds:
                spec.timelineBegin + (plan.startSeconds - spec.clipBegin) / spec.rate,
              timelineEndSeconds:
                spec.timelineBegin + (plan.endSeconds - spec.clipBegin) / spec.rate
            }
          : {})
      };
    });
    return {
      segments,
      text: segments
        .map((s) => `[${s.startSeconds.toFixed(3)}–${s.endSeconds.toFixed(3)}s] ${s.text}`)
        .join('\n'),
      actualBackend: data.backend,
      sampled: true
    };
  }
  if (
    !data ||
    !['webgpu', 'wasm'].includes(data.backend) ||
    !Array.isArray(data.frames) ||
    data.frames.length !== spec.sampleTimes.length
  )
    throw modelError('视觉结果后端或帧数无效');
  const frames = data.frames.map((f, i) => {
    if (
      !f ||
      f.requestedSeconds !== spec.sampleTimes[i] ||
      !Number.isFinite(f.sourceSeconds) ||
      !Number.isFinite(f.durationSeconds) ||
      f.durationSeconds <= 0 ||
      typeof f.text !== 'string' ||
      !f.text.trim() ||
      f.text.length > 12000 ||
      (spec.kind === 'image'
        ? f.sourceSeconds !== 0
        : f.sourceSeconds < 0 ||
          f.sourceSeconds > f.requestedSeconds + 0.002 ||
          f.sourceSeconds + f.durationSeconds < f.requestedSeconds - 0.002)
    )
      throw modelError('视觉描述或实际帧时间戳无效');
    return {
      requestedSeconds: f.requestedSeconds,
      sourceSeconds: f.sourceSeconds,
      durationSeconds: f.durationSeconds,
      ...(spec.timelineBegin !== undefined
        ? {
            timelineSeconds:
              spec.kind === 'image'
                ? spec.timelineBegin
                : spec.timelineBegin + (f.sourceSeconds - spec.clipBegin) / spec.rate
          }
        : {}),
      text: f.text.trim()
    };
  });
  return {
    frames,
    ...(spec.resultFormat === 'description' ? { text: frames[0].text } : {}),
    actualBackend: data.backend,
    sampled: spec.kind === 'video'
  };
}

export class VisionJob extends TtsJob {
  constructor(spec, { emit = () => {}, heartbeatMs } = {}) {
    super(
      { ...spec, model: FASTVLM.id, modelBaseUrl: `/vision-models/${FASTVLM.id}/` },
      {
        emit: (type, value) => emit(type.replace(/^tts-/, 'vision-'), value),
        heartbeatMs
      }
    );
  }
  textResult(worker, id, data) {
    this.check(worker, id);
    const result = validateVisionResult(data, this.spec);
    this.value = { ...this.value, ...result, state: 'completed', phase: 'completed', progress: 1 };
    clearInterval(this.timer);
    this.emit('tts-progress', this.status());
    return this.status();
  }
}

/** Path + prompt convenience routes share the same consent, source and Worker checks. */
export function createVisionDescriptionRoutes({
  models,
  sessions,
  snapshot,
  createSession,
  body,
  json,
  routes,
  setupUrl
}) {
  let inspectionSessionId;
  return async (req, res, kind) => {
    const data = await body(req);
    if (
      !data ||
      typeof data.path !== 'string' ||
      !data.path.trim() ||
      typeof data.prompt !== 'string' ||
      !data.prompt.trim() ||
      data.prompt.length > 4000 ||
      (data.id !== undefined && typeof data.id !== 'string') ||
      data.assetId !== undefined ||
      data.itemId !== undefined
    )
      throw modelError('请提供绝对路径 path 和非空 prompt 文本，可选会话 id');
    let session = data.id
      ? sessions.get(data.id)
      : [...sessions.values()]
          .filter((s) => !s.closed && s.clients.size)
          .sort((a, b) => b.touched - a.touched)[0];
    if (data.id && (!session || session.closed)) throw modelError('会话不存在', 404);
    try {
      await models.require();
    } catch (error) {
      return json(res, error.statusCode || 400, {
        error: error.message,
        code: error.code,
        previewUrl: session ? snapshot(session).previewUrl : setupUrl()
      });
    }
    if (!session) {
      session = sessions.get(inspectionSessionId);
      if (!session || session.closed) {
        session = createSession(createProject('视觉理解'));
        inspectionSessionId = session.id;
      }
    }
    session.touched = Date.now();
    return routes(req, res, session, 'vision', {
      ...data,
      version: session.version,
      expectedKind: kind,
      resultFormat: kind === 'image' ? 'description' : 'segments'
    });
  };
}

export function createVisionRoutes({ models, snapshot, body, json, probe, allowed }) {
  return async (req, res, session, action, suppliedData) => {
    if (action === 'vision' && req.method === 'POST') {
      const data = suppliedData ?? (await body(req));
      if (data.version !== session.version)
        return json(res, 409, { error: '作品版本冲突', ...snapshot(session) });
      try {
        await models.require();
      } catch (error) {
        return json(res, error.statusCode || 400, {
          error: error.message,
          code: error.code,
          previewUrl: snapshot(session).previewUrl
        });
      }
      const asset = data.path ? await probe(await allowed(data.path)) : undefined;
      const spec = validateVisionRequest(data, session.project, asset);
      await models.require();
      if (spec.version !== session.version)
        return json(res, 409, { error: '作品版本冲突', ...snapshot(session) });
      if (!session.clients.size)
        return json(res, 409, {
          error: '请打开作品预览页执行本地视觉理解',
          code: 'BROWSER_REQUIRED',
          previewUrl: snapshot(session).previewUrl
        });
      if (active(session.visionJob)) throw modelError('此会话已有视觉任务，请等待或取消', 409);
      const emit = (type, value) => {
        for (const client of session.clients)
          client.write(`event: ${type}\ndata: ${JSON.stringify(value)}\n\n`);
      };
      session.visionJob = new VisionJob(
        { ...spec, sessionId: session.id, previewUrl: snapshot(session).previewUrl },
        { emit }
      );
      const history = (session.visionJobs ??= new Map());
      history.set(session.visionJob.spec.id, session.visionJob);
      while (history.size > 16) history.delete(history.keys().next().value);
      emit('vision-request', session.visionJob.status());
      return json(res, 202, session.visionJob.status());
    }
    const requestedJob = new URL(req.url || '/', 'http://localhost').searchParams.get('job');
    const job =
      requestedJob && action === 'vision-job'
        ? session.visionJobs?.get(requestedJob)
        : session.visionJob;
    if (requestedJob && action === 'vision-job' && !job)
      throw modelError('视觉任务已释放或不存在', 404, 'VISION_JOB_NOT_FOUND');
    if (action === 'vision-job' && req.method === 'GET')
      return json(res, 200, job?.status() || { state: 'idle' });
    if (action === 'vision-job' && req.method === 'DELETE')
      return json(res, 200, job?.cancel() || { state: 'idle' });
    if (action?.startsWith('vision-job/') && req.method === 'POST') {
      if (!job) throw modelError('视觉任务不存在', 404);
      const data = await body(req),
        operation = action.slice('vision-job/'.length),
        worker = req.headers['x-vision-worker'];
      if (operation === 'claim') return json(res, 200, job.claim(data.id));
      job.check(worker, data.id);
      if (operation === 'result') return json(res, 200, job.textResult(worker, data.id, data));
      if (operation === 'progress') return json(res, 200, job.progress(worker, data.id, data));
      if (operation === 'heartbeat') return json(res, 200, job.status());
      if (operation === 'fail') return json(res, 200, job.fail(data.error || '浏览器视觉理解失败'));
    }
    return json(res, 405, { error: '视觉接口操作无效' });
  };
}
