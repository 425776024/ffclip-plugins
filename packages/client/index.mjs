/** @typedef {import('./types.js').Snapshot} Snapshot */
/** @typedef {import('./types.js').FileEntry} FileEntry */
/** @typedef {import('./types.js').FileListing} FileListing */
/** @typedef {import('./types.js').ConnectionInfo} ConnectionInfo */
/** @typedef {import('./types.js').RenderStatus} RenderStatus */
/** @param {number} timeoutMs */
function visionWaitDuration(timeoutMs) {
  if (!Number.isFinite(timeoutMs) || timeoutMs < 0 || timeoutMs > 60000)
    throw new RangeError('Vision timeoutMs must be between 0 and 60000');
  return timeoutMs;
}
/** @param {number} ms @param {AbortSignal} signal */
function visionDelay(ms, signal) {
  return new Promise((resolve, reject) => {
    const abort = () => {
      clearTimeout(timer);
      signal.removeEventListener('abort', abort);
      reject(signal.reason);
    };
    const timer = setTimeout(() => {
      signal.removeEventListener('abort', abort);
      resolve();
    }, ms);
    signal.addEventListener('abort', abort, { once: true });
    if (signal.aborted) abort();
  });
}
/** @param {VideoCutClient} client @param {string} id @param {string} jobId
 * @param {import('./types.js').VisionDescribeOptions} options
 * @param {import('./types.js').VisionJob} [initial]
 * @returns {Promise<import('./types.js').VisionJob>} */
async function pollVision(client, id, jobId, options, initial) {
  const timeoutMs = visionWaitDuration(options.timeoutMs ?? 30000),
    signal = options.signal;
  const controller = new AbortController();
  let current = initial,
    timedOut = false;
  const abort = () => controller.abort(signal.reason);
  signal?.addEventListener('abort', abort, { once: true });
  if (signal?.aborted) abort();
  const timer =
    timeoutMs > 0
      ? setTimeout(() => {
          timedOut = true;
          controller.abort(new DOMException('Vision wait timed out', 'TimeoutError'));
        }, timeoutMs)
      : undefined;
  try {
    for (;;) {
      signal?.throwIfAborted();
      if (current && !['queued', 'running'].includes(current.state)) return current;
      if (timeoutMs === 0 && current) return current;
      current = await client.request(
        `/sessions/${encodeURIComponent(id)}/vision-job?job=${encodeURIComponent(jobId)}`,
        { signal: controller.signal }
      );
      if (current.id !== jobId) throw new Error('Vision returned a different job');
      if (!['queued', 'running'].includes(current.state) || timeoutMs === 0) return current;
      await visionDelay(500, controller.signal);
    }
  } catch (error) {
    if (signal?.aborted) {
      await client.cancelVision(id, jobId).catch(() => {});
      throw signal.reason;
    }
    if (timedOut && current) return { ...current, waitTimedOut: true };
    throw error;
  } finally {
    clearTimeout(timer);
    signal?.removeEventListener('abort', abort);
  }
}
/** @param {VideoCutClient} client @param {'image'|'video'} kind @param {string} path @param {string} prompt
 * @param {import('./types.js').VisionVideoDescribeOptions} options
 * @returns {Promise<import('./types.js').VisionJob>} */
async function requestVisionDescription(client, kind, path, prompt, options) {
  const { timeoutMs = 30000, signal, ...parameters } = options;
  visionWaitDuration(timeoutMs);
  signal?.throwIfAborted();
  // Read the submission response before honoring cancellation so the precise job can be stopped.
  const job = await client.request(`/vision/describe-${kind}`, {
    method: 'POST',
    body: JSON.stringify({ ...parameters, path, prompt })
  });
  if (!job.sessionId || !job.id) throw new Error('Vision submission omitted job identity');
  return pollVision(client, job.sessionId, job.id, { timeoutMs, signal }, job);
}
export class VideoCutError extends Error {
  /** @param {number} status
   * @param {import('./types.js').ApiErrorPayload} details */
  constructor(status, details) {
    super(details.error || `VideoCut HTTP ${status}`);
    this.name = 'VideoCutError';
    this.status = status;
    this.details = details;
    this.code = details.code;
    this.previewUrl = details.previewUrl;
    this.version = details.version;
    this.snapshot = details;
  }
}
export class VideoCutClient {
  /** @param {string} [url] */
  constructor(url = 'http://127.0.0.1:4318') {
    this.url = url.replace(/\/$/, '');
    this.token = '';
  }
  /** @returns {Promise<import('./types.js').ConnectionInfo>} */
  async connect() {
    const r = await fetch(`${this.url}/api/bootstrap`);
    if (!r.ok) throw new Error('VideoCut 服务不可用');
    this.info = await r.json();
    this.token = this.info.token;
    return this.info;
  }
  /** @template T
   * @param {string} path
   * @param {RequestInit} [options]
   * @returns {Promise<T>} */
  async request(path, options = {}) {
    if (!this.token) await this.connect();
    const response = await fetch(`${this.url}/api${path}`, {
      ...options,
      headers: {
        'Content-Type': 'application/json',
        Authorization: `Bearer ${this.token}`,
        ...options.headers
      }
    });
    const data = await response.json();
    if (!response.ok) {
      throw new VideoCutError(response.status, data);
    }
    return data;
  }
  /** @param {import('../core/types.js').Project} [project]
   * @returns {Promise<import('./types.js').Snapshot>} */
  createSession(project) {
    return this.request('/sessions', { method: 'POST', body: JSON.stringify({ project }) });
  }
  /** Create a fresh editable starter with HTML animation, styled titles, narration and captions.
   * @param {{ locale?: 'zh' | 'en'; name?: string }} [options]
   * @returns {Promise<import('./types.js').Snapshot>} */
  initializeDemo(options = {}) {
    return this.request('/examples/starter', { method: 'POST', body: JSON.stringify(options) });
  }
  /** @param {string} id
   * @returns {Promise<import('./types.js').Snapshot>} */
  getSession(id) {
    return this.request(`/sessions/${id}`);
  }
  /** Find live cuts by their readable name, preview URL or saved local directory.
   * @returns {Promise<import('./types.js').SessionSummary[]>} */
  listSessions() {
    return this.request('/sessions');
  }
  /** @param {string} previewPath
   * @returns {Promise<import('./types.js').Snapshot>} */
  resolveSession(previewPath) {
    return this.request(`/sessions/resolve?previewPath=${encodeURIComponent(previewPath)}`);
  }
  /** @param {string} id
   * @param {import('../core/types.js').Project} project
   * @param {number} version
   * @returns {Promise<import('./types.js').Snapshot>} */
  updateSession(id, project, version) {
    return this.request(`/sessions/${id}`, {
      method: 'PUT',
      body: JSON.stringify({ project, version })
    });
  }
  /** @param {string} id
   * @param {import('../core/commands.js').EditorCommand[]} operations
   * @param {number} version
   * @returns {Promise<import('./types.js').Snapshot & {operations: import('../core/types.js').CommandResult[]; changes?: import('../core/history.mjs').CommandChanges}>} */
  editSession(id, operations, version) {
    return this.request(`/sessions/${id}/commands`, {
      method: 'POST',
      body: JSON.stringify({ operations, version })
    });
  }
  /** @param {string} path
   * @returns {Promise<import('./types.js').Snapshot>} */
  openProject(path) {
    return this.request('/projects/open', { method: 'POST', body: JSON.stringify({ path }) });
  }
  /** @param {string} id
   * @returns {Promise<import('./types.js').RenderStatus>} */
  renderStatus(id) {
    return this.request(`/sessions/${id}/render-job`);
  }
  /** @param {string} id
   * @returns {Promise<{closed: boolean}>} */
  closeSession(id) {
    return this.request(`/sessions/${id}`, { method: 'DELETE' });
  }
  /** @param {string} [path]
   * @returns {Promise<import('./types.js').FileListing>} */
  listFiles(path) {
    return this.request(`/files?path=${encodeURIComponent(path || '')}`);
  }
  /** @param {string} path
   * @returns {Promise<import('../core/types.js').Asset>} */
  importMedia(path) {
    return this.request('/assets', { method: 'POST', body: JSON.stringify({ path }) });
  }
  /** Read copied OS files on an explicit paste gesture and probe local media.
   * @returns {Promise<{assets: import('../core/types.js').Asset[], skipped: {name: string, error: string}[]}>} */
  async importClipboardMedia() {
    try {
      return await this.request('/clipboard/assets', { method: 'POST' });
    } catch (error) {
      if (error instanceof VideoCutError && error.status === 404)
        throw new Error('当前本地服务尚未支持文件粘贴，请重启 VideoCut 服务后重试');
      throw error;
    }
  }
  /** @returns {Promise<import('./types.js').TtsCatalog>} */
  listTtsVoices() {
    return this.request('/tts/catalog');
  }
  /** Fixed Base cache status; does not trigger a download.
   * @returns {Promise<import('./types.js').AsrModelStatus>} */
  asrModelStatus() {
    return this.request('/asr/model');
  }
  /** Automatically ensure Whisper Base, then recognize local media in the open preview browser.
   * @param {string} id
   * @param {import('./types.js').AsrOptions} options
   * @returns {Promise<import('./types.js').AsrJob>} */
  transcribeSpeech(id, options) {
    return this.request(`/sessions/${id}/asr`, { method: 'POST', body: JSON.stringify(options) });
  }
  /** @param {string} id
   * @returns {Promise<import('./types.js').AsrJob>} */
  asrStatus(id) {
    return this.request(`/sessions/${id}/asr-job`);
  }
  /** @param {string} id
   * @returns {Promise<import('./types.js').AsrJob>} */
  cancelAsr(id) {
    return this.request(`/sessions/${id}/asr-job`, { method: 'DELETE' });
  }
  /** Read consent and verified model status without downloading.
   * @returns {Promise<import('./types.js').VisionModelStatus>} */
  visionModelStatus() {
    return this.request('/vision/model');
  }
  /** Ask the initialization dialog for consent; never approves or downloads a model.
   * @returns {Promise<import('./types.js').VisionModelStatus & {setupUrl: string}>} */
  requestVisionSetup() {
    return this.request('/vision/setup', {
      method: 'POST',
      body: JSON.stringify({ action: 'request' })
    });
  }
  /** Queue read-only image/video understanding in the open preview browser.
   * @param {string} id
   * @param {import('./types.js').VisionOptions} options
   * @returns {Promise<import('./types.js').VisionJob>} */
  analyzeMedia(id, options) {
    return this.request(`/sessions/${id}/vision`, {
      method: 'POST',
      body: JSON.stringify(options)
    });
  }
  /** Describe an authorized local image according to prompt. Uses the latest open preview unless id is supplied.
   * @param {string} path @param {string} prompt
   * @param {import('./types.js').VisionDescribeOptions} [options]
   * @returns {Promise<import('./types.js').VisionJob>} */
  describeImage(path, prompt, options = {}) {
    return requestVisionDescription(this, 'image', path, prompt, options);
  }
  /** Automatically sample video intervals; completed results contain timestamped text and segments.
   * @param {string} path @param {string} prompt
   * @param {import('./types.js').VisionVideoDescribeOptions} [options]
   * @returns {Promise<import('./types.js').VisionJob>} */
  describeVideo(path, prompt, options = {}) {
    return requestVisionDescription(this, 'video', path, prompt, options);
  }
  /** Wait for this exact job, retaining progress with waitTimedOut on deadline. Abort cancels only this job.
   * @param {string} id @param {string} jobId
   * @param {import('./types.js').VisionDescribeOptions} [options]
   * @returns {Promise<import('./types.js').VisionJob>} */
  waitVision(id, jobId, options = {}) {
    return pollVision(this, id, jobId, options);
  }
  /** @param {string} id
   * @param {string} [jobId]
   * @returns {Promise<import('./types.js').VisionJob>} */
  visionStatus(id, jobId) {
    return this.request(
      `/sessions/${encodeURIComponent(id)}/vision-job${jobId ? `?job=${encodeURIComponent(jobId)}` : ''}`
    );
  }
  /** @param {string} id
   * @param {string} [jobId]
   * @returns {Promise<import('./types.js').VisionJob>} */
  cancelVision(id, jobId) {
    return this.request(
      `/sessions/${encodeURIComponent(id)}/vision-job${jobId ? `?job=${encodeURIComponent(jobId)}` : ''}`,
      { method: 'DELETE' }
    );
  }
  /** Explicitly download the pinned local model and selected voices; never runs inference.
   * @param {{dtype?:import('./types.js').TtsDtype, voices?:string[]}} [options]
   * @returns {Promise<import('./types.js').TtsInstallStatus>} */
  installTtsModel(options = {}) {
    return this.request('/tts/model/install', { method: 'POST', body: JSON.stringify(options) });
  }
  /** @returns {Promise<import('./types.js').TtsInstallStatus>} */
  ttsModelStatus() {
    return this.request('/tts/model/install');
  }
  /** @returns {Promise<import('./types.js').TtsInstallStatus>} */
  cancelTtsModelInstall() {
    return this.request('/tts/model/install', { method: 'DELETE' });
  }
  /** Queue browser inference; the preview page must stay open until completion.
   * @param {string} id
   * @param {import('./types.js').TtsSynthesizeOptions} options
   * @returns {Promise<import('./types.js').TtsJob>} */
  synthesizeSpeech(id, options) {
    return this.request(`/sessions/${id}/tts`, { method: 'POST', body: JSON.stringify(options) });
  }
  /** @param {string} id
   * @returns {Promise<import('./types.js').TtsJob>} */
  ttsStatus(id) {
    return this.request(`/sessions/${id}/tts-job`);
  }
  /** @param {string} id
   * @returns {Promise<import('./types.js').TtsJob>} */
  cancelTts(id) {
    return this.request(`/sessions/${id}/tts-job`, { method: 'DELETE' });
  }
  /** @param {string} path
   * @param {Partial<import('../core/types.js').HtmlContent>} [options]
   * @returns {Promise<{name:string;html:import('../core/types.js').HtmlContent}>} */
  importHtml(path, options = {}) {
    return this.request('/html/import', {
      method: 'POST',
      body: JSON.stringify({ ...options, path })
    });
  }
  /** @param {string} id
   * @param {number} version
   * @param {string} [directory]
   * @returns {Promise<{path:string;version:number}>} */
  exportProject(id, version, directory) {
    return this.request(`/sessions/${id}/export`, {
      method: 'POST',
      body: JSON.stringify({ version, directory })
    });
  }
  /** Save a complete, portable browser project with media and frozen motion resources.
   * @param {string} id
   * @param {number} version
   * @param {string} [directory]
   * @returns {Promise<{path:string;version:number;format:string;files:number}>} */
  saveProject(id, version, directory) {
    return this.request(`/sessions/${id}/save`, {
      method: 'POST',
      body: JSON.stringify({ version, directory })
    });
  }
  /** @param {string} id
   * @param {number} version
   * @param {string} [directory]
   * @param {'mp4'|'webm'} [format]
   * @returns {Promise<{path:string;version:number;format:string}>} */
  renderVideo(id, version, directory, format = 'mp4') {
    return this.request(`/sessions/${id}/render`, {
      method: 'POST',
      body: JSON.stringify({ version, directory, format })
    });
  }
  /** Reveal a completed export in the local file manager.
   * @param {string} id
   * @param {string} path
   * @returns {Promise<{opened:boolean}>} */
  revealExport(id, path) {
    return this.request(`/sessions/${id}/export-output`, {
      method: 'POST',
      body: JSON.stringify({ path })
    });
  }
  /** @param {string} id
   * @param {string} path
   * @returns {string} */
  exportOutputUrl(id, path) {
    return `${this.url}/api/sessions/${id}/export-output?path=${encodeURIComponent(path)}&token=${this.token}`;
  }
  /** @param {string} id
   * @param {'play'|'pause'|'seek'} action
   * @param {number} [timeSeconds]
   * @returns {Promise<any>} */
  controlPreview(id, action, timeSeconds) {
    return this.request(`/sessions/${id}/preview`, {
      method: 'POST',
      body: JSON.stringify({ action, timeSeconds })
    });
  }
  /** @param {string} id
   * @returns {Promise<any>} */
  previewStatus(id) {
    return this.request(`/sessions/${id}/preview`);
  }
  /** @param {string} id
   * @param {Record<string,unknown>} report
   * @returns {Promise<any>} */
  reportPreview(id, report) {
    return this.request(`/sessions/${id}/preview-status`, {
      method: 'POST',
      body: JSON.stringify(report)
    });
  }
  /** @param {string} id
   * @param {string} asset
   * @returns {string} */
  mediaUrl(id, asset) {
    return `${this.url}/api/sessions/${id}/media?asset=${encodeURIComponent(asset)}&token=${this.token}`;
  }
  /** @param {string} id
   * @returns {string} */
  eventsUrl(id) {
    return `${this.url}/api/sessions/${id}/events?token=${this.token}`;
  }
}
export * from '../core/project.mjs';
