import { spawn } from 'node:child_process';
import { mkdtemp, rm, access } from 'node:fs/promises';
import { tmpdir, homedir } from 'node:os';
import { join } from 'node:path';

export class ChromiumTransportError extends Error {
  constructor(message, code, browser, method) {
    super(message);
    this.name = 'ChromiumTransportError';
    this.code = code;
    this.method = method;
    Object.defineProperty(this, 'browser', { value: browser });
  }
}

export async function findChromium() {
  const candidates = [
    process.env.VIDEOCUT_CHROMIUM,
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    '/Applications/Chromium.app/Contents/MacOS/Chromium',
    join(homedir(), 'Applications/Google Chrome.app/Contents/MacOS/Google Chrome'),
    '/usr/bin/chromium',
    '/usr/bin/chromium-browser',
    '/usr/bin/google-chrome',
    process.env.PROGRAMFILES &&
      join(process.env.PROGRAMFILES, 'Google/Chrome/Application/chrome.exe')
  ].filter(Boolean);
  for (const path of candidates)
    if (
      await access(path).then(
        () => true,
        () => false
      )
    )
      return path;
  throw new Error('HTML 动画需要本机 Chrome/Chromium；请安装浏览器或设置 VIDEOCUT_CHROMIUM。');
}

/** A private Chromium profile and a small, dependency-free CDP transport. */
export class Chromium {
  pending = new Map();
  listeners = new Set();
  sequence = 0;
  async start({ args = [], frameRateLimit = false } = {}) {
    const executable = await findChromium();
    this.profile = await mkdtemp(join(tmpdir(), 'videocut-html-'));
    this.process = spawn(
      executable,
      [
        '--headless=new',
        '--remote-debugging-port=0',
        '--remote-debugging-address=127.0.0.1',
        `--user-data-dir=${this.profile}`,
        '--no-first-run',
        '--no-default-browser-check',
        '--disable-background-networking',
        '--disable-component-update',
        '--disable-sync',
        '--disable-extensions',
        '--disable-dev-shm-usage',
        '--hide-scrollbars',
        // These pages use an explicit seek clock. Waiting for desktop vsync adds
        // 33-50 ms per screenshot without changing any raster pixels.
        ...(frameRateLimit ? [] : ['--disable-frame-rate-limit', '--disable-gpu-vsync']),
        ...args,
        'about:blank'
      ],
      { stdio: ['ignore', 'ignore', 'pipe'] }
    );
    try {
      const endpoint = await new Promise((resolve, reject) => {
        let log = '';
        const timeout = setTimeout(() => reject(new Error('Chromium 启动超时')), 15000);
        this.process.once('error', (error) => {
          clearTimeout(timeout);
          reject(error);
        });
        this.process.once('exit', () => {
          clearTimeout(timeout);
          reject(new Error('Chromium 提前退出'));
        });
        this.process.stderr.on('data', (bytes) => {
          log = (log + bytes.toString()).slice(-8192);
          const match = /DevTools listening on (ws:\/\/[^\s]+)/.exec(log);
          if (match) {
            clearTimeout(timeout);
            resolve(match[1]);
          }
        });
      });
      this.socket = new WebSocket(endpoint);
      await new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('Chromium 调试连接超时')), 15000);
        this.socket.addEventListener(
          'open',
          () => {
            clearTimeout(timer);
            resolve();
          },
          { once: true }
        );
        this.socket.addEventListener(
          'error',
          () => {
            clearTimeout(timer);
            reject(new Error('Chromium 调试连接失败'));
          },
          {
            once: true
          }
        );
        this.socket.addEventListener(
          'close',
          () => {
            clearTimeout(timer);
            reject(new Error('Chromium 调试连接已关闭'));
          },
          { once: true }
        );
      });
      this.socket.addEventListener('message', ({ data }) => {
        const value = JSON.parse(String(data));
        if (value.id) {
          const entry = this.pending.get(value.id);
          if (!entry) return;
          this.pending.delete(value.id);
          clearTimeout(entry.timer);
          value.error ? entry.reject(new Error(value.error.message)) : entry.resolve(value.result);
        } else for (const listener of this.listeners) listener(value);
      });
      const disconnected = () =>
        this.fail(
          new ChromiumTransportError('Chromium 渲染连接已关闭', 'CHROMIUM_DISCONNECTED', this)
        );
      this.socket.addEventListener('close', disconnected);
      this.socket.addEventListener('error', disconnected);
      this.process.once('exit', disconnected);
      return this;
    } catch (error) {
      await this.close();
      throw error;
    }
  }
  send(method, params = {}, sessionId) {
    if (this.failure) return Promise.reject(this.failure);
    return new Promise((resolve, reject) => {
      const id = ++this.sequence;
      const timer = setTimeout(() => {
        // A timed-out context may still be created by Chrome later. Retire the
        // entire transport so callers cannot accumulate orphan contexts or
        // spend another timeout on every cleanup/queued command.
        this.fail(
          new ChromiumTransportError(`HTML 渲染超时：${method}`, 'CHROMIUM_TIMEOUT', this, method)
        );
      }, 15000);
      this.pending.set(id, { resolve, reject, timer });
      try {
        this.socket.send(
          JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) })
        );
      } catch (error) {
        this.fail(
          new ChromiumTransportError(
            error.message || 'Chromium 渲染连接已关闭',
            'CHROMIUM_DISCONNECTED',
            this,
            method
          )
        );
      }
    });
  }
  fail(error) {
    this.failure ||= error;
    for (const entry of this.pending.values()) {
      clearTimeout(entry.timer);
      entry.reject(this.failure);
    }
    this.pending.clear();
  }
  close() {
    return (this.closing ||= this.shutdown());
  }
  async shutdown() {
    this.fail(new ChromiumTransportError('HTML 渲染器已关闭', 'CHROMIUM_CLOSED', this));
    this.listeners.clear();
    this.socket?.close();
    if (this.process && this.process.exitCode === null && this.process.signalCode === null) {
      const exited = new Promise((resolve) => this.process.once('exit', resolve));
      this.process.kill();
      const timer = setTimeout(() => this.process.kill('SIGKILL'), 2000);
      await exited;
      clearTimeout(timer);
    }
    if (this.profile)
      await rm(this.profile, { recursive: true, force: true, maxRetries: 3, retryDelay: 50 });
  }
}
