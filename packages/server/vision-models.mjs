import { homedir } from 'node:os';
import { join } from 'node:path';
import { lstat, readFile, open, rename, rm } from 'node:fs/promises';
import { randomBytes } from 'node:crypto';
import { PinnedModelStore, modelError } from './model-store.mjs';
import { FASTVLM } from './vision-manifest.mjs';

export function defaultVisionModelDir() {
  const cache =
    process.platform === 'darwin'
      ? join(homedir(), 'Library', 'Caches')
      : process.platform === 'win32'
        ? process.env.LOCALAPPDATA || join(homedir(), 'AppData', 'Local')
        : process.env.XDG_CACHE_HOME || join(homedir(), '.cache');
  return join(cache, 'videocut', 'vision');
}

/** Only a saved UI consent can start installation; status and inference never download. */
export class VisionModelStore extends PinnedModelStore {
  constructor({ directory = defaultVisionModelDir(), fetchImpl, files = FASTVLM.files } = {}) {
    super({
      directory,
      fetchImpl,
      files,
      modelId: FASTVLM.id,
      repository: FASTVLM.repository,
      revision: FASTVLM.revision
    });
    this.promptRequested = false;
    this.decisions = Promise.resolve();
  }
  async consentPath() {
    // Reuse the cache directory link fencing for the persisted preference too.
    return join(await this.root(), 'setup.json');
  }
  async consent() {
    const path = await this.consentPath();
    const info = await lstat(path).catch((e) => {
      if (e.code !== 'ENOENT') throw e;
    });
    if (!info) return 'unasked';
    if (!info.isFile() || info.isSymbolicLink() || info.size > 1024)
      throw modelError('视觉初始化配置无效');
    const value = JSON.parse(await readFile(path, 'utf8'));
    if (!['enabled', 'declined'].includes(value.consent)) throw modelError('视觉初始化配置无效');
    return value.consent;
  }
  async catalog() {
    let installed = true;
    for (const path of Object.keys(this.files))
      if (!(await this.verifiedFile(path))) installed = false;
    return {
      model: FASTVLM.id,
      repository: FASTVLM.repository,
      revision: FASTVLM.revision,
      license: FASTVLM.license,
      modelBaseUrl: `/vision-models/${FASTVLM.id}/`,
      consent: await this.consent(),
      promptRequested: this.promptRequested,
      installed,
      size: Object.values(this.files).reduce((sum, f) => sum + f.size, 0),
      install: this.status()
    };
  }
  decide(enabled) {
    if (typeof enabled !== 'boolean') throw modelError('enabled 必须为布尔值');
    const decision = this.decisions.then(async () => {
      if (!enabled) await this.cancel();
      const path = await this.consentPath(),
        partial = `${path}.partial-${randomBytes(12).toString('hex')}`;
      let handle;
      try {
        handle = await open(partial, 'wx', 0o600);
        await handle.writeFile(JSON.stringify({ consent: enabled ? 'enabled' : 'declined' }));
        await handle.sync();
        await handle.close();
        handle = null;
        await rename(partial, path);
      } finally {
        await handle?.close();
        await rm(partial, { force: true });
      }
      this.promptRequested = false;
      if (enabled) await this.startInstall();
      return this.catalog();
    });
    this.decisions = decision.catch(() => {});
    return decision;
  }
  async startInstall() {
    if ((await this.consent()) !== 'enabled')
      throw modelError('请在初始化对话框中同意启用视觉理解', 409, 'VISION_CONSENT_REQUIRED');
    if (this.controller) return this.status();
    const files = Object.entries(this.files).map(([path, f]) => ({
      path,
      size: f.size,
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
    return this.status();
  }
  async require() {
    if ((await this.consent()) !== 'enabled')
      throw modelError('请在初始化对话框中同意启用视觉理解', 409, 'VISION_CONSENT_REQUIRED');
    for (const path of Object.keys(this.files))
      if (!(await this.verifiedFile(path)))
        throw modelError(
          '视觉模型尚未下载完成，请在初始化对话框查看进度或重试',
          409,
          'VISION_MODEL_REQUIRED'
        );
    if ((await this.consent()) !== 'enabled')
      throw modelError('视觉理解已停用', 409, 'VISION_CONSENT_REQUIRED');
  }
}
