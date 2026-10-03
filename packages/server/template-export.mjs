import { open, link, unlink, stat } from 'node:fs/promises';
import { randomBytes } from 'node:crypto';
import { spawn } from 'node:child_process';
import { clone, duration } from '../core/project.mjs';
import { run } from './media.mjs';

/** One frozen render plan; browser codecs or the existing local FFmpeg encode its output. */
export async function createBrowserExportJob(
  project,
  version,
  outputBase,
  format,
  progress,
  ffmpeg = 'ffmpeg'
) {
  if (!['mp4', 'webm'].includes(format)) throw new Error('无效导出格式');
  if (!duration(project)) throw new Error('时间轴为空');
  // Optional capability probe only: never install, download or require a binary
  // for the ordinary browser-encoded path. Do not cache across PATH changes.
  const localEncoder =
    typeof ffmpeg === 'string' && ffmpeg.length > 0
      ? await run(ffmpeg, ['-version'], { timeout: 1500, maxOutput: 128 * 1024 })
          .then((output) => /^ffmpeg version\s/m.test(output))
          .catch(() => false)
      : false;
  const path = `${outputBase}.${format}`,
    nonce = randomBytes(8).toString('hex');
  const temporary = `${path}.partial-${nonce}`,
    pcmPath = `${temporary}.pcm`,
    muxPath = `${temporary}.mux`;
  const file = await open(temporary, 'wx', 0o600);
  const fps = project.frameRate.numerator / project.frameRate.denominator;
  const totalFrames = Math.ceil((duration(project) / 120000) * fps);
  const totalAudioFrames = Math.ceil((duration(project) / 120000) * 48000);
  const spec = {
    id: randomBytes(20).toString('hex'),
    version,
    project: clone(project),
    width: project.canvas.width,
    height: project.canvas.height,
    fps,
    format,
    localEncoder
  };
  const abort = new AbortController();
  let owner = '',
    sequence = 0,
    extent = 0,
    writing = false,
    audioWriting = false,
    closed = false,
    finishing = false;
  let encoding = 'browser',
    configured = false,
    hasAudio = false,
    frameIndex = 0,
    audioSequence = 0,
    audioFrames = 0;
  let pcm, encoder, encoderDone, timer, resolveResult, rejectResult;
  let state = { id: spec.id, version, phase: 'waiting-browser', completed: 0, total: totalFrames };
  const completed = new Promise((resolve, reject) => {
    resolveResult = resolve;
    rejectResult = reject;
  });
  completed.catch(() => {});
  const emit = (value) => {
    state = { ...state, ...value, jobId: spec.id };
    progress(state);
  };
  const arm = (ms = 120000) => {
    clearTimeout(timer);
    timer = setTimeout(
      () => fail(new Error(owner ? '渲染页面已停止响应' : '请打开作品网页完成视频导出')),
      ms
    );
    timer.unref();
  };
  function fail(error) {
    if (closed) return;
    closed = true;
    clearTimeout(timer);
    abort.abort();
    encoder?.kill('SIGKILL');
    emit({ phase: 'error', error: error.message });
    rejectResult(error);
  }
  function check(worker) {
    if (closed) throw new Error('导出任务已结束');
    if (!owner || worker !== owner) throw new Error('不是此导出任务的渲染客户端');
  }
  async function writeAll(handle, bytes, position) {
    for (let offset = 0; offset < bytes.length;) {
      const r = await handle.write(bytes, offset, bytes.length - offset, position + offset);
      if (!r.bytesWritten) throw new Error('输出文件写入失败');
      offset += r.bytesWritten;
    }
  }
  async function binary(req, maxBytes) {
    const chunks = [];
    let size = 0;
    for await (const c of req) {
      size += c.length;
      if (size > maxBytes) throw new Error('导出数据超过限制');
      chunks.push(c);
    }
    return Buffer.concat(chunks, size);
  }
  function rawEncoder() {
    const codec =
      format === 'mp4'
        ? ['-c:v', 'libx264', '-preset', 'veryfast', '-crf', '18']
        : [
            '-c:v',
            'libvpx-vp9',
            '-deadline',
            'realtime',
            '-cpu-used',
            '4',
            '-crf',
            '25',
            '-b:v',
            '0'
          ];
    encoder = spawn(
      ffmpeg,
      [
        '-hide_banner',
        '-v',
        'error',
        '-y',
        '-f',
        'rawvideo',
        '-pixel_format',
        'rgba',
        '-video_size',
        `${spec.width}x${spec.height}`,
        '-framerate',
        `${project.frameRate.numerator}/${project.frameRate.denominator}`,
        '-i',
        'pipe:0',
        '-an',
        ...codec,
        '-pix_fmt',
        'yuv420p',
        '-f',
        format,
        temporary
      ],
      { stdio: ['pipe', 'ignore', 'pipe'], windowsHide: true }
    );
    let diagnostic = '';
    encoder.stderr.on('data', (c) => {
      diagnostic = (diagnostic + c).slice(-16384);
    });
    encoder.stdin.on('error', () => {});
    encoderDone = new Promise((resolve, reject) => {
      encoder.once('error', reject);
      encoder.once('close', (code) =>
        code === 0 ? resolve() : reject(new Error(`${ffmpeg}: ${diagnostic || `退出码 ${code}`}`))
      );
    });
    encoderDone.catch(fail);
  }
  arm(30000);
  return {
    spec,
    completed,
    fail,
    check,
    status: () => ({ ...state }),
    claim() {
      if (closed || owner) return null;
      owner = randomBytes(24).toString('hex');
      arm();
      emit({ phase: 'rendering' });
      return owner;
    },
    async configure(worker, value) {
      check(worker);
      if (configured || sequence || frameIndex || audioFrames || writing)
        throw new Error('编码方案已经确定');
      if (
        !['browser', 'video-with-pcm', 'frames-with-pcm'].includes(value.encoding) ||
        typeof value.hasAudio !== 'boolean'
      )
        throw new Error('无效编码方案');
      if (value.encoding !== 'browser' && !localEncoder)
        throw new Error(
          '本机没有可用的 FFmpeg 后备编码器，请使用浏览器编码；若浏览器不支持 AAC，请选择 WebM。'
        );
      configured = true;
      encoding = value.encoding;
      hasAudio = value.hasAudio;
      try {
        if (encoding !== 'browser') {
          await run(ffmpeg, ['-version'], { signal: abort.signal });
          if (hasAudio) pcm = await open(pcmPath, 'wx', 0o600);
          if (encoding === 'frames-with-pcm') {
            await file.close();
            rawEncoder();
          }
        }
        check(worker);
        arm();
        emit({ encoding });
        return { encoding, accepted: true };
      } catch (error) {
        fail(error);
        throw error;
      }
    },
    progress(worker, value) {
      check(worker);
      if (finishing) return { accepted: true };
      arm();
      const { completed: done, total, phase } = value;
      if (
        !['rendering', 'audio', 'encoding', 'finalizing'].includes(phase) ||
        !Number.isFinite(done) ||
        !Number.isFinite(total) ||
        done < 0 ||
        total < done
      )
        throw new Error('无效导出进度');
      emit({ phase: phase.slice(0, 40), completed: done, total });
      return { accepted: true };
    },
    async chunk(req, worker, position, index) {
      check(worker);
      if (
        !configured ||
        encoding === 'frames-with-pcm' ||
        finishing ||
        writing ||
        index !== sequence ||
        !Number.isSafeInteger(position) ||
        position < 0 ||
        position > extent
      )
        throw new Error('导出块顺序或文件偏移无效');
      writing = true;
      arm();
      try {
        const data = await binary(req, 8 * 1024 * 1024);
        if (position + data.length > 64 * 1024 ** 3) throw new Error('输出超过64GB');
        check(worker);
        await writeAll(file, data, position);
        check(worker);
        extent = Math.max(extent, position + data.length);
        sequence++;
        arm();
        return { sequence, size: extent };
      } catch (error) {
        fail(error);
        throw error;
      } finally {
        writing = false;
      }
    },
    async frame(req, worker, index) {
      check(worker);
      if (
        !configured ||
        encoding !== 'frames-with-pcm' ||
        writing ||
        finishing ||
        index !== frameIndex ||
        index >= totalFrames
      )
        throw new Error('视频帧顺序无效');
      writing = true;
      arm();
      try {
        const bytes = spec.width * spec.height * 4,
          data = await binary(req, bytes);
        if (data.length !== bytes) throw new Error('视频帧尺寸不完整');
        check(worker);
        await new Promise((resolve, reject) =>
          encoder.stdin.write(data, (error) => (error ? reject(error) : resolve()))
        );
        check(worker);
        frameIndex++;
        arm();
        return { frames: frameIndex };
      } catch (error) {
        fail(error);
        throw error;
      } finally {
        writing = false;
      }
    },
    async audio(req, worker, sampleOffset, rate, channels, index) {
      check(worker);
      if (
        !pcm ||
        audioWriting ||
        finishing ||
        rate !== 48000 ||
        channels !== 2 ||
        index !== audioSequence ||
        sampleOffset !== audioFrames
      )
        throw new Error('音频块顺序或配置无效');
      audioWriting = true;
      arm();
      try {
        const data = await binary(req, 1024 * 1024);
        if (!data.length || data.length % 8 || audioFrames + data.length / 8 > totalAudioFrames)
          throw new Error('音频采样数量无效');
        check(worker);
        await writeAll(pcm, data, audioFrames * 8);
        check(worker);
        audioFrames += data.length / 8;
        audioSequence++;
        arm();
        return { audioFrames, sequence: audioSequence };
      } catch (error) {
        fail(error);
        throw error;
      } finally {
        audioWriting = false;
      }
    },
    async finish(worker, value) {
      check(worker);
      if (!configured) throw new Error('编码方案尚未配置');
      const size = typeof value === 'number' ? value : value.size;
      if (finishing || writing || audioWriting) throw new Error('导出文件仍在写入');
      if (encoding === 'frames-with-pcm') {
        if (frameIndex !== totalFrames) throw new Error('导出帧不完整');
      } else if (!Number.isSafeInteger(size) || size !== extent || size < 32)
        throw new Error('导出文件不完整');
      if (encoding !== 'browser' && hasAudio && audioFrames !== totalAudioFrames)
        throw new Error('导出音频不完整');
      finishing = true;
      arm(600000);
      try {
        if (encoding === 'frames-with-pcm') {
          encoder.stdin.end();
          await encoderDone;
          extent = (await stat(temporary)).size;
        } else {
          await file.sync();
          await file.close();
        }
        check(worker);
        let final = temporary;
        if (encoding !== 'browser' && hasAudio) {
          await pcm.sync();
          await pcm.close();
          emit({ phase: 'encoding', completed: totalFrames, total: totalFrames });
          const args = [
            '-hide_banner',
            '-v',
            'error',
            '-n',
            '-i',
            temporary,
            '-f',
            'f32le',
            '-ar',
            '48000',
            '-ac',
            '2',
            '-i',
            pcmPath,
            '-map',
            '0:v:0',
            '-map',
            '1:a:0',
            '-c:v',
            'copy',
            '-c:a',
            format === 'mp4' ? 'aac' : 'libopus',
            '-b:a',
            '192k',
            '-t',
            String(duration(project) / 120000),
            ...(format === 'mp4' ? ['-movflags', '+faststart'] : []),
            '-f',
            format,
            muxPath
          ];
          await run(ffmpeg, args, { timeout: 600000, signal: abort.signal });
          check(worker);
          final = muxPath;
        }
        const finalSize = (await stat(final)).size;
        check(worker);
        await link(final, path);
        if (closed) {
          await unlink(path);
          throw new Error('导出已取消');
        }
        closed = true;
        clearTimeout(timer);
        const receipt = {
          path,
          version,
          size: finalSize,
          format,
          jobId: spec.id,
          encoding,
          frames: totalFrames
        };
        emit({ phase: 'complete', ...receipt });
        resolveResult(receipt);
        return receipt;
      } catch (error) {
        fail(error);
        throw error;
      }
    },
    async dispose() {
      fail(new Error('导出已取消'));
      clearTimeout(timer);
      encoder?.kill('SIGKILL');
      await encoderDone?.catch(() => {});
      await file.close().catch(() => {});
      await pcm?.close().catch(() => {});
      for (const p of [temporary, pcmPath, muxPath]) await unlink(p).catch(() => {});
    }
  };
}
