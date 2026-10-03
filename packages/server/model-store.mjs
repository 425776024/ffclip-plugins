import { createHash, randomBytes } from 'node:crypto';
import { createReadStream } from 'node:fs';
import { lstat, mkdir, open, realpath, rename, rm } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { MODEL_SOURCES, modelDownloadError, selectModelDownload } from './model-download.mjs';
export function modelError(message, statusCode = 400, code) {
  return Object.assign(new Error(message), { statusCode, ...(code ? { code } : {}) });
}
/** Persistent, content-verified resources with atomic downloads and a fixed upstream manifest. */
export class PinnedModelStore {
  constructor({ directory, fetchImpl = globalThis.fetch, files, modelId, repository, revision }) {
    this.directory = resolve(directory);
    this.files = files;
    this.fetchImpl = fetchImpl;
    this.modelId = modelId;
    this.repository = repository;
    this.revision = revision;
    this.preferredSource = null;
    this.verified = new Map();
    this.install = { state: 'idle', progress: 0, files: [] };
    this.controller = null;
    this.pending = null;
  }
  async root() {
    await mkdir(this.directory, { recursive: true });
    const canonical = await realpath(this.directory);
    const root = join(canonical, this.modelId, this.revision);
    // Fence every fixed child directory against links; user-selected cache root may itself be a link.
    for (const path of [join(canonical, this.modelId), root]) {
      const info = await lstat(path).catch((error) => {
        if (error.code !== 'ENOENT') throw error;
      });
      if (info && (!info.isDirectory() || info.isSymbolicLink()))
        throw modelError('模型缓存目录不是普通目录');
      if (!info)
        await mkdir(path).catch((error) => {
          if (error.code !== 'EEXIST') throw error;
        });
    }
    return root;
  }
  async resourcePath(path, create = false) {
    if (!Object.hasOwn(this.files, path)) throw modelError('模型资源不在固定清单内', 404);
    const root = await this.root();
    const parent = dirname(join(root, path));
    if (parent !== root) {
      const info = await lstat(parent).catch((error) => {
        if (error.code !== 'ENOENT') throw error;
      });
      if (info && (!info.isDirectory() || info.isSymbolicLink()))
        throw modelError('模型资源目录不能是符号链接');
      if (!info) {
        if (!create) return null;
        await mkdir(parent).catch((error) => {
          if (error.code !== 'EEXIST') throw error;
        });
      }
    }
    return join(root, path);
  }
  async verifiedFile(path) {
    const file = await this.resourcePath(path);
    if (!file) return null;
    const info = await lstat(file).catch((error) => {
      if (error.code !== 'ENOENT') throw error;
    });
    if (!info || !info.isFile() || info.isSymbolicLink() || info.size !== this.files[path].size)
      return null;
    const signature = `${info.dev}:${info.ino}:${info.size}:${info.mtimeMs}:${info.ctimeMs}`;
    if (this.verified.get(path) === signature) return file;
    const hash = createHash('sha256');
    for await (const bytes of createReadStream(file)) hash.update(bytes);
    if (hash.digest('hex') !== this.files[path].sha256) return null;
    this.verified.set(path, signature);
    return file;
  }
  status() {
    return structuredClone(this.install);
  }
  async cancel() {
    this.controller?.abort();
    await this.pending;
    return this.status();
  }
  async downloadAll(controller, files) {
    const update = () => {
      this.install.progress =
        files.reduce((sum, f) => sum + f.downloaded, 0) / files.reduce((sum, f) => sum + f.size, 0);
    };
    for (const file of files) {
      controller.signal.throwIfAborted();
      if (await this.verifiedFile(file.path)) {
        file.downloaded = file.size;
        update();
        continue;
      }
      const target = await this.resourcePath(file.path, true);
      controller.signal.throwIfAborted();
      const sources = this.preferredSource
        ? [
            this.preferredSource,
            ...MODEL_SOURCES.filter((source) => source !== this.preferredSource)
          ]
        : [...MODEL_SOURCES];
      const failures = [];
      while (sources.length) {
        let download;
        try {
          download = await selectModelDownload({
            sources,
            path: `${this.repository}/resolve/${this.revision}/${file.path}`,
            size: file.size,
            fetchImpl: this.fetchImpl,
            signal: controller.signal
          });
          await this.saveDownload(download, controller.signal, file, target, update);
          this.preferredSource = download.source;
          break;
        } catch (error) {
          controller.signal.throwIfAborted();
          if (error.code !== 'MODEL_DOWNLOAD_FAILED') throw error;
          file.downloaded = 0;
          update();
          if (!download) {
            if (!failures.length) throw error;
            throw modelDownloadError(`${failures.join('；')}；${error.message}`);
          }
          failures.push(`${download.source}: ${error.message}`);
          sources.splice(sources.indexOf(download.source), 1);
          if (!sources.length)
            throw modelDownloadError(`所有模型下载源均失败：${failures.join('；')}`);
        } finally {
          download?.close();
        }
      }
    }
    controller.signal.throwIfAborted();
    this.install.state = 'ready';
    this.install.progress = 1;
  }
  async saveDownload(download, signal, file, target, update) {
    const partial = `${target}.partial-${randomBytes(12).toString('hex')}`;
    let handle;
    try {
      signal.throwIfAborted();
      handle = await open(partial, 'wx', 0o600);
      const hash = createHash('sha256');
      let chunk = download.first;
      while (!chunk.done) {
        signal.throwIfAborted();
        const bytes = chunk.value;
        if (file.downloaded + bytes.length > file.size)
          throw modelDownloadError(`模型超过固定大小：${file.path}`);
        hash.update(bytes);
        let offset = 0;
        while (offset < bytes.length) {
          const result = await handle.write(bytes, offset, bytes.length - offset);
          offset += result.bytesWritten;
        }
        file.downloaded += bytes.length;
        update();
        chunk = await download.read();
      }
      signal.throwIfAborted();
      if (file.downloaded !== file.size || hash.digest('hex') !== this.files[file.path].sha256)
        throw modelDownloadError(`模型校验失败：${file.path}`);
      await handle.sync();
      await handle.close();
      handle = null;
      signal.throwIfAborted();
      await rename(partial, target);
      this.verified.delete(file.path);
    } finally {
      await handle?.close();
      await rm(partial, { force: true });
    }
  }
}
