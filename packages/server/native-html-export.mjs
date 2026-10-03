import { spawn } from 'node:child_process';
import { readFile, stat, link, unlink, readdir } from 'node:fs/promises';
import { basename, dirname, join } from 'node:path';
import { createHash, randomBytes } from 'node:crypto';
import { duration, clone } from '../core/project.mjs';
import { documentFor, initialize, htmlTickExpression } from './html-renderer.mjs';
import { run } from './media.mjs';

/** Encoded animation frames stay in RAM, independently of the current narration. */
export class HtmlVideoCache {
  constructor(maximumBytes = 256 * 1024 * 1024) {
    if (!Number.isSafeInteger(maximumBytes) || maximumBytes < 1) throw new RangeError('Invalid HTML video cache budget');
    this.maximumBytes = maximumBytes;
    this.entries = new Map();
    this.bytes = 0;
  }
  get(key) {
    const value = this.entries.get(key);
    if (value) { this.entries.delete(key); this.entries.set(key, value); }
    return value;
  }
  set(key, bytes) {
    if (this.entries.has(key)) { this.bytes -= this.entries.get(key).length; this.entries.delete(key); }
    if (bytes.length > this.maximumBytes) return;
    while (this.bytes + bytes.length > this.maximumBytes) {
      const oldest = this.entries.keys().next().value;
      this.bytes -= this.entries.get(oldest).length; this.entries.delete(oldest);
    }
    this.entries.set(key, bytes); this.bytes += bytes.length;
  }
  clear() { this.entries.clear(); this.bytes = 0; }
}

function unmodified(item) {
  const clip = item.clip;
  return !clip.effects.length && !Object.keys(clip.automation).length &&
    clip.retime.mode === 'constant' && clip.retime.constantRatePpm === 1000000;
}

/** The native backend never bypasses the shared compositor for layers/effects. */
export function nativeHtmlPlan(project, format = 'mp4') {
  if (format !== 'mp4' || project.timeline.transitions.length || project.timeline.groups.length) return null;
  const visible = project.timeline.tracks.filter(t => t.visible).flatMap(t => t.items.filter(i => i.enabled && i.clip.type !== 'audio'));
  if (visible.length !== 1) return null;
  const item = visible[0], clip = item.clip, html = clip.html, v = clip.visual;
  const end = duration(project);
  if (!html || !unmodified(item) || item.placement.begin !== 0 || item.placement.end !== end ||
      html.width !== project.canvas.width || html.height !== project.canvas.height ||
      v.positionX !== 0 || v.positionY !== 0 || v.scaleX !== 1 || v.scaleY !== 1 ||
      v.rotationDegrees !== 0 || v.opacity !== 1 || v.flipHorizontal || v.flipVertical ||
      v.blendMode !== 'normal' || Object.values(v.crop).some(Boolean)) return null;
  const sounds = project.timeline.tracks.filter(t => !t.muted).flatMap(t => t.items.filter(i => i.enabled && !i.clip.audio.muted && i.clip.type === 'audio'));
  if (project.timeline.tracks.some(t => !t.muted && t.items.some(i => i.enabled && !i.clip.audio.muted && i.clip.type !== 'audio' && project.assets.some(a => a.id === i.clip.assetId && a.hasAudio)))) return null;
  if (sounds.length > 1 || sounds.some(i => !unmodified(i) || i.clip.audio.fadeIn || i.clip.audio.fadeOut)) return null;
  const sound = sounds[0], asset = sound && project.assets.find(a => a.id === sound.clip.assetId);
  if (sound && !asset) return null;
  return { item, sound, asset, seconds: end / 120000, total: Math.ceil(end / 120000 * project.frameRate.numerator / project.frameRate.denominator) };
}

export async function createNativeHtmlExportJob(project, version, outputBase, progress, renderer, ffmpeg, cache) {
  const plan = nativeHtmlPlan(project);
  if (!plan) throw new Error('作品需要通用合成器');
  const id = randomBytes(20).toString('hex'), path = outputBase + '.mp4';
  const videoPath = outputBase + `.partial-${id}.mp4`, muxPath = outputBase + `.mux-${id}.mp4`;
  const backend = await stat(renderer);
  const key = createHash('sha256').update(JSON.stringify([
    'native-html-v1', renderer, backend.size, backend.mtimeMs, project.canvas, project.frameRate,
    plan.item.clip.html, plan.item.clip.source, plan.item.placement.end
  ])).digest('hex');
  const spec = { id, version, project: clone(project), format: 'mp4', native: true };
  const abort = new AbortController();
  let process, settled = false, state = { id, jobId: id, version, phase: 'rendering', completed: 0, total: plan.total, encoding: 'native-html' };
  let resolveResult, rejectResult, nativeResult;
  const completed = new Promise((resolve, reject) => { resolveResult = resolve; rejectResult = reject; });
  completed.catch(() => {});
  const emit = value => { state = { ...state, ...value }; progress(state); };
  const check = () => { if (abort.signal.aborted) throw new Error('用户取消导出'); };
  const fail = error => {
    if (settled) return;
    settled = true; abort.abort(); process?.kill();
    emit({ phase: 'error', error: error.message }); rejectResult(error);
  };
  const task = Promise.resolve().then(async () => {
    let encoded = cache.get(key), cacheHit = !!encoded;
    if (!encoded) {
      const fps = project.frameRate.numerator / project.frameRate.denominator;
      const payload = {
        document: await documentFor(plan.item.clip.html), initialize,
        tick: htmlTickExpression(plan.item.clip.html, '__VIDEOCUT_TICK__'), output: videoPath,
        ...project.canvas, frames: plan.total, fpsNumerator: project.frameRate.numerator,
        fpsDenominator: project.frameRate.denominator, sourceBegin: plan.item.clip.source.begin,
        sourceEnd: plan.item.clip.source.end, duration: plan.seconds,
        bitrate: Math.round(Math.max(2e6, Math.min(60e6, project.canvas.width * project.canvas.height * fps * .14)))
      };
      check();
      await new Promise((resolve, reject) => {
        process = spawn(renderer, ['-'], { stdio: ['pipe', 'pipe', 'pipe'], signal: abort.signal });
        let pending = '', errors = '', timer;
        const arm = () => { clearTimeout(timer); timer = setTimeout(() => { process.kill(); reject(new Error('本机 HTML 渲染已停止响应')); }, 30000); };
        arm();
        process.stdout.on('data', bytes => {
          pending += bytes.toString();
          let offset;
          while ((offset = pending.indexOf('\n')) >= 0) {
            const line = pending.slice(0, offset); pending = pending.slice(offset + 1);
            try {
              const value = JSON.parse(line);
              arm();
              if (value.error) errors = value.error;
              else if (value.complete) nativeResult = value;
              else emit(value);
            } catch { errors = '本机渲染器返回了无效进度'; }
          }
        });
        process.stderr.on('data', bytes => { errors = (errors + bytes).slice(-8192); });
        process.on('error', error => { clearTimeout(timer); reject(error); });
        process.on('close', code => { clearTimeout(timer); code === 0 && nativeResult?.frames === plan.total ? resolve() : reject(new Error(errors || '本机 HTML 导出失败')); });
        process.stdin.on('error', () => {});
        process.stdin.end(JSON.stringify(payload));
      });
      check();
      if ((await stat(videoPath)).size <= cache.maximumBytes) {
        encoded = await readFile(videoPath);
        check();
        cache.set(key, encoded);
      }
    }
    check(); emit({ phase: 'encoding', completed: plan.total, cacheHit });
    const args = ['-hide_banner', '-v', 'error', '-n', '-i', encoded ? 'pipe:0' : videoPath];
    if (plan.sound) {
      args.push('-i', plan.asset.path, '-map', '0:v:0', '-map', '1:a:0', '-c:v', 'copy', '-c:a', 'aac', '-b:a', '192k', '-ar', '48000', '-ac', '2');
      const sound = plan.sound;
      const filter = [`atrim=start=${sound.clip.source.begin / 120000}:duration=${(sound.placement.end - sound.placement.begin) / 120000}`, 'asetpts=PTS-STARTPTS', 'aresample=48000', `volume=${sound.clip.audio.gainLinear}`];
      if (sound.placement.begin) filter.push(`adelay=${Math.round(sound.placement.begin / 120000 * 48000)}S:all=1`);
      filter.push('apad', `atrim=duration=${plan.seconds}`);
      args.push('-af', filter.join(','));
    } else args.push('-map', '0:v:0', '-c:v', 'copy', '-an');
    args.push('-t', String(plan.seconds), '-movflags', '+faststart', muxPath);
    await run(ffmpeg, args, { input: encoded, signal: abort.signal, timeout: 600000 });
    check(); await link(muxPath, path);
    const receipt = { path, version, size: (await stat(path)).size, format: 'mp4', jobId: id, encoding: 'native-html', frames: plan.total, cacheHit, pngFrames: 0, frameTransferBytes: 0, native: nativeResult };
    settled = true; emit({ phase: 'complete', ...receipt }); resolveResult(receipt);
  }).catch(fail);
  return {
    native: true, spec, completed, fail, status: () => state, claim: () => null,
    check: () => { throw new Error('本机导出不接收浏览器视频帧'); },
    async dispose() {
      if (!settled) fail(new Error('导出已取消'));
      await task;
      await Promise.all([videoPath, muxPath].map(p => unlink(p).catch(() => {})));
      // AVFoundation may leave its network-optimization work file on abrupt
      // process exit. Only files belonging to this random job nonce are removed.
      const prefix = basename(videoPath) + '.sb-';
      await Promise.all((await readdir(dirname(videoPath))).filter(name => name.startsWith(prefix)).map(name => unlink(join(dirname(videoPath), name)).catch(() => {})));
    }
  };
}
