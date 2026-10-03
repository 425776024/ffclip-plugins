import {
  Output,
  Mp4OutputFormat,
  WebMOutputFormat,
  StreamTarget,
  CanvasSource,
  AudioSample,
  canEncodeVideo,
  canEncodeAudio,
  Quality,
  type VideoCodec,
  type AudioCodec
} from 'mediabunny';
import { duration, type Project } from '../core/project.mjs';
import { sharedMediaEngine } from '../media/browser';
import { SceneRenderer } from './renderer';
import { frameTime } from './plan.mjs';
import { selectEncoding, videoLatencyMode } from './capabilities.mjs';
import {
  calibrateAacEncoder,
  GaplessAacSource,
  TimedAudioPacketSource,
  TimedOpusSource
} from './audio-encoder';

export type EncodingMode = 'browser' | 'video-with-pcm' | 'frames-with-pcm';
export interface ExportOptions {
  preferredEncoding?: EncodingMode | 'auto';
  /** Same-pixel sequential comparison / compatibility diagnostic. Enabled by default. */
  htmlPrefetch?: boolean;
  /** Disable only for a same-pixel sequential native-text comparison. */
  textWorkers?: boolean;
}
export interface ExportCallbacks {
  configure(value: { encoding: EncodingMode; hasAudio: boolean }): Promise<void>;
  write(position: number, data: Uint8Array): Promise<void>;
  audio(
    sampleOffset: number,
    sampleRate: number,
    channels: number,
    data: Uint8Array
  ): Promise<void>;
  frame(index: number, data: Uint8Array): Promise<void>;
  progress(value: { completed: number; total: number; phase: string }): void;
}
export async function encodingProfile(
  project: Project,
  format: 'mp4' | 'webm',
  localEncoder = false,
  preferredEncoding: EncodingMode | 'auto' = 'auto'
) {
  const hasAudio = project.timeline.tracks.some(
    (t) =>
      !t.muted &&
      t.items.some(
        (i) =>
          i.enabled &&
          !i.clip.audio.muted &&
          (i.clip.type === 'audio' || project.assets.find((a) => a.id === i.clip.assetId)?.hasAudio)
      )
  );
  const fps = project.frameRate.numerator / project.frameRate.denominator;
  const videoCandidates: VideoCodec[] = format === 'mp4' ? ['avc'] : ['vp9', 'vp8'];
  const audioCodec: AudioCodec = format === 'mp4' ? 'aac' : 'opus';
  const bitrate = Math.round(
    Math.max(2e6, Math.min(60e6, project.canvas.width * project.canvas.height * fps * 0.14))
  );
  let videoCodec: VideoCodec | undefined;
  for (const codec of videoCandidates)
    if (
      await canEncodeVideo(codec, {
        ...project.canvas,
        frameRate: fps,
        quality: new Quality({ bitrate })
      })
    ) {
      videoCodec = codec;
      break;
    }
  const audioSupported =
    !hasAudio ||
    (await canEncodeAudio(audioCodec, {
      sampleRate: 48000,
      numberOfChannels: 2,
      quality: new Quality({ bitrate: 192000 })
    }));
  const encoding = selectEncoding({
    format,
    localEncoder,
    videoSupported: !!videoCodec,
    audioSupported,
    preferredEncoding
  });
  return { hasAudio, videoCodec, audioCodec, bitrate, fps, encoding };
}

/** All three encoding paths consume the exact same evaluated, composited frames. */
export async function exportProject(
  project: Project,
  format: 'mp4' | 'webm',
  urls: Record<string, string>,
  callbacks: ExportCallbacks,
  externalSignal: AbortSignal,
  localEncoder = false,
  options: ExportOptions = {}
) {
  const profile = await encodingProfile(project, format, localEncoder, options.preferredEncoding);
  let aacDelay = 0;
  if (profile.encoding === 'browser' && profile.hasAudio && format === 'mp4') {
    try {
      aacDelay = (await calibrateAacEncoder()).delay;
    } catch (error) {
      if (localEncoder && (!options.preferredEncoding || options.preferredEncoding === 'auto'))
        profile.encoding = 'video-with-pcm';
      else
        throw new Error(
          `当前浏览器无法校准 AAC 音频同步，请选择 WebM${localEncoder ? '或本地编码器' : ''}。${error instanceof Error ? error.message : error}`
        );
    }
  }
  const length = duration(project) / 120000,
    total = Math.ceil(length * profile.fps);
  if (!total) throw new Error('时间轴为空');
  const controller = new AbortController(),
    signal = controller.signal;
  const cancel = () => controller.abort();
  externalSignal.addEventListener('abort', cancel, { once: true });
  if (externalSignal.aborted) cancel();
  const check = () => {
    if (signal.aborted) throw new DOMException('用户取消导出', 'AbortError');
  };
  const canvas = new OffscreenCanvas(project.canvas.width, project.canvas.height);
  const renderer = new SceneRenderer(canvas, (id) => urls[id], {
    textWorkers: options.textWorkers !== false
  });
  let size = 0,
    audioFrames = 0,
    encodedFrames = 0,
    output: Output | undefined;
  try {
    check();
    await callbacks.configure({ encoding: profile.encoding, hasAudio: profile.hasAudio });
    let video: CanvasSource | undefined, audio: TimedAudioPacketSource | undefined;
    if (profile.encoding !== 'frames-with-pcm') {
      const target = new StreamTarget(
        new WritableStream({
          async write(chunk) {
            check();
            await callbacks.write(chunk.position, chunk.data);
            size = Math.max(size, chunk.position + chunk.data.length);
          }
        }),
        { chunked: true, chunkSize: 1024 * 1024 }
      );
      output = new Output({
        format:
          format === 'mp4' ? new Mp4OutputFormat({ fastStart: false }) : new WebMOutputFormat(),
        target
      });
      video = new CanvasSource(canvas, {
        codec: profile.videoCodec!,
        quality: new Quality({ bitrate: profile.bitrate }),
        latencyMode: videoLatencyMode(profile.videoCodec!, navigator.userAgent),
        hardwareAcceleration: 'no-preference',
        keyFrameInterval: 2,
        onEncodedPacket() {
          encodedFrames++;
        }
      });
      output.addVideoTrack(video, { frameRate: profile.fps });
      if (profile.hasAudio && profile.encoding === 'browser') {
        audio =
          format === 'mp4'
            ? new GaplessAacSource(aacDelay, Math.ceil(length * 48000))
            : new TimedOpusSource(Math.ceil(length * 48000));
        output.addAudioTrack(audio.source);
      }
      await output.start();
    }
    const videos = async () => {
      try {
        for (let index = 0; index < total; index++) {
          check();
          await renderer.render(
            project,
            frameTime(index, project.frameRate),
            canvas.width,
            canvas.height,
            signal,
            { htmlPrefetch: options.htmlPrefetch !== false, prefetch: true }
          );
          check();
          if (video)
            await video.add(
              index / profile.fps,
              Math.min(1 / profile.fps, length - index / profile.fps)
            );
          else await callbacks.frame(index, await renderer.readPixels());
          if (index % 5 === 0 || index === total - 1)
            callbacks.progress({ completed: index + 1, total, phase: 'rendering' });
          // Let cancellation/messages run even for very small all-image scenes.
          if (index % 10 === 0) await new Promise((resolve) => setTimeout(resolve, 0));
        }
      } finally {
        video?.close();
      }
    };
    const audios = async () => {
      if (!profile.hasAudio) return;
      try {
        for await (const chunk of sharedMediaEngine.mixAudio(
          project,
          (id) => urls[id],
          0,
          duration(project),
          { sampleRate: 48000, channels: 2, blockFrames: 4096, signal }
        )) {
          check();
          const channels = chunk.data.length,
            data = new Float32Array(chunk.numberOfFrames * channels);
          if (audio) {
            chunk.data.forEach((plane, index) => data.set(plane, index * chunk.numberOfFrames));
            const sample = new AudioSample({
              data,
              format: 'f32-planar',
              numberOfChannels: channels,
              sampleRate: chunk.sampleRate,
              timestamp: chunk.timestamp
            });
            try {
              await audio.add(sample);
            } finally {
              sample.close();
            }
          } else {
            for (let i = 0; i < chunk.numberOfFrames; i++)
              for (let c = 0; c < channels; c++) data[i * channels + c] = chunk.data[c][i];
            await callbacks.audio(
              audioFrames,
              chunk.sampleRate,
              channels,
              new Uint8Array(data.buffer)
            );
          }
          audioFrames += chunk.numberOfFrames;
        }
      } finally {
        if (signal.aborted) audio?.cancel();
        else await audio?.close();
      }
    };
    const tasks = [videos(), audios()];
    try {
      await Promise.all(tasks);
    } catch (error) {
      controller.abort();
      await output?.cancel().catch(() => {});
      await Promise.allSettled(tasks);
      throw error;
    }
    check();
    callbacks.progress({ completed: total, total, phase: 'finalizing' });
    await output?.finalize();
    check();
    if (output && encodedFrames !== total)
      throw new Error(
        `视频编码帧数不完整（${encodedFrames}/${total}），请降低分辨率或使用本地编码器。`
      );
    return { size, frames: total, audioFrames, format, encoding: profile.encoding };
  } catch (error) {
    controller.abort();
    await output?.cancel().catch(() => {});
    throw error;
  } finally {
    externalSignal.removeEventListener('abort', cancel);
    renderer.dispose();
  }
}
