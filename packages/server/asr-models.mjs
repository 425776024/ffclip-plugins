import { homedir } from 'node:os';
import { join } from 'node:path';
import { PinnedModelStore, modelError } from './model-store.mjs';
import { WHISPER_BASE } from './asr-manifest.mjs';
export { WHISPER_BASE } from './asr-manifest.mjs';

export function defaultAsrModelDir() {
  const cache =
    process.platform === 'darwin'
      ? join(homedir(), 'Library', 'Caches')
      : process.platform === 'win32'
        ? process.env.LOCALAPPDATA || join(homedir(), 'AppData', 'Local')
        : process.env.XDG_CACHE_HOME || join(homedir(), '.cache');
  return join(cache, 'videocut', 'asr');
}

/** One fixed Base model, shared by WebGPU and WASM. Every ASR job ensures it automatically. */
export class WhisperModelStore extends PinnedModelStore {
  constructor({ directory = defaultAsrModelDir(), fetchImpl, files = WHISPER_BASE.files } = {}) {
    super({
      directory,
      fetchImpl,
      files,
      modelId: WHISPER_BASE.id,
      repository: WHISPER_BASE.repository,
      revision: WHISPER_BASE.revision
    });
    this.users = new Set();
  }
  async catalog() {
    const paths = Object.keys(this.files);
    let installed = true;
    for (const path of paths) if (!(await this.verifiedFile(path))) installed = false;
    return {
      model: WHISPER_BASE.id,
      revision: WHISPER_BASE.revision,
      modelBaseUrl: `/asr-models/${WHISPER_BASE.id}/`,
      installed,
      size: paths.reduce((sum, path) => sum + this.files[path].size, 0),
      install: this.status()
    };
  }
  async ensure({ signal, onProgress = () => {} } = {}) {
    signal?.throwIfAborted();
    const user = Symbol('asr-download');
    this.users.add(user);
    let timer;
    try {
      if (this.controller?.signal.aborted) await this.pending;
      signal?.throwIfAborted();
      if (!this.pending || !this.controller) {
        const files = Object.keys(this.files).map((path) => ({
          path,
          size: this.files[path].size,
          downloaded: 0
        }));
        this.install = { state: 'downloading', progress: 0, files };
        const controller = (this.controller = new AbortController());
        this.pending = this.downloadAll(controller, files)
          .catch((error) => {
            this.install.state = controller.signal.aborted ? 'cancelled' : 'error';
            if (!controller.signal.aborted)
              this.install.error = String(error.message || error).slice(0, 2000);
          })
          .finally(() => {
            if (this.controller === controller) this.controller = null;
          });
      }
      const pending = this.pending;
      const installation = this.install;
      onProgress(this.status());
      timer = setInterval(() => onProgress(this.status()), 500);
      let abort;
      try {
        await Promise.race([
          pending,
          new Promise((_, reject) => {
            abort = () => reject(signal.reason || new DOMException('ASR cancelled', 'AbortError'));
            signal?.addEventListener('abort', abort, { once: true });
            if (signal?.aborted) abort();
          })
        ]);
      } finally {
        if (abort) signal?.removeEventListener('abort', abort);
      }
      signal?.throwIfAborted();
      if (installation.state !== 'ready')
        throw modelError(installation.error || '语音识别模型下载已取消', 503);
      onProgress(this.status());
      return `/asr-models/${WHISPER_BASE.id}/`;
    } finally {
      clearInterval(timer);
      this.users.delete(user);
      if (!this.users.size && this.controller) await this.cancel();
    }
  }
}
