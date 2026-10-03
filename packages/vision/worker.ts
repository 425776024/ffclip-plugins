import type * as Transformers from '@huggingface/transformers';
import type { VisionRequest, VisionProgress, VisionResult } from './types';
import { MediaEngine } from '../media/browser';
import {
  describeFrame,
  storyboardPrompt,
  VISION_MAX_FRAMES,
  VISION_MAX_SEGMENTS,
  VISION_MAX_SAMPLES
} from './runtime.mjs';

const progress = (value: VisionProgress) => self.postMessage({ type: 'progress', progress: value });
async function run(request: VisionRequest): Promise<VisionResult> {
  const modelUrl = new URL(request.modelBaseUrl),
    mediaUrl = new URL(request.mediaUrl);
  if (
    modelUrl.origin !== self.location.origin ||
    modelUrl.pathname !== '/vision-models/fastvlm-0.5b/' ||
    modelUrl.search ||
    modelUrl.hash ||
    modelUrl.username ||
    modelUrl.password ||
    mediaUrl.origin !== self.location.origin ||
    !/^\/api\/sessions\/[a-f0-9]+\/vision-media$/.test(mediaUrl.pathname)
  )
    throw new Error('Vision resources must be local');
  if (
    !['image', 'video'].includes(request.kind) ||
    !Array.isArray(request.sampleTimes) ||
    !request.sampleTimes.length ||
    request.sampleTimes.length >
      (request.resultFormat === 'segments' ? VISION_MAX_SAMPLES : VISION_MAX_FRAMES) ||
    request.sampleTimes.some(
      (t, i, a) => !Number.isFinite(t) || t < 0 || (i > 0 && t <= a[i - 1])
    ) ||
    typeof request.prompt !== 'string' ||
    request.prompt.length > 4000 ||
    !Number.isInteger(request.maxNewTokens) ||
    request.maxNewTokens < 1 ||
    request.maxNewTokens > 512 ||
    !['auto', 'webgpu', 'wasm'].includes(request.backend)
  )
    throw new Error('Invalid vision request');
  if (request.resultFormat === 'segments') {
    const plan = request.segmentPlan;
    if (
      request.kind !== 'video' ||
      !Array.isArray(plan) ||
      !plan.length ||
      plan.length > VISION_MAX_SEGMENTS ||
      plan.some(
        (s, i) =>
          !s ||
          !Number.isFinite(s.startSeconds) ||
          !Number.isFinite(s.endSeconds) ||
          s.startSeconds < 0 ||
          s.endSeconds <= s.startSeconds ||
          (i > 0 && s.startSeconds !== plan[i - 1].endSeconds) ||
          !Array.isArray(s.sampleTimes) ||
          !s.sampleTimes.length ||
          s.sampleTimes.length > 4 ||
          s.sampleTimes.some((t) => !Number.isFinite(t) || t <= s.startSeconds || t >= s.endSeconds)
      ) ||
      plan.flatMap((s) => s.sampleTimes).length !== request.sampleTimes.length ||
      plan.flatMap((s) => s.sampleTimes).some((t, i) => t !== request.sampleTimes[i])
    )
      throw new Error('Invalid vision segment plan');
  }
  // Share the installed Transformers/ORT assets with ASR. No CDN or remote model fallback.
  const tf = (await import(
    /* @vite-ignore */ new URL('/asr-runtime/transformers.min.js', self.location.origin).href
  )) as typeof Transformers;
  tf.env.allowRemoteModels = false;
  tf.env.allowLocalModels = true;
  tf.env.localModelPath = new URL('/vision-models/', self.location.origin).href;
  tf.env.useBrowserCache = false;
  const wasm = tf.env.backends.onnx.wasm;
  if (!wasm) throw new Error('Local ONNX WASM runtime is unavailable');
  wasm.numThreads = 1;
  wasm.proxy = false;
  wasm.wasmPaths = new URL('/asr-runtime/', self.location.origin).href;
  const gpu = (navigator as Navigator & { gpu?: { requestAdapter(): Promise<object | null> } }).gpu;
  const adapter = request.backend === 'wasm' ? null : await gpu?.requestAdapter().catch(() => null);
  if (request.backend === 'webgpu' && !adapter)
    throw new Error('WebGPU is unavailable in this browser');
  const backends: ('webgpu' | 'wasm')[] =
    request.backend === 'auto' ? (adapter ? ['webgpu', 'wasm'] : ['wasm']) : [request.backend];
  let lastError;
  for (const backend of backends) {
    let model: Transformers.PreTrainedModel | undefined;
    const media = new MediaEngine();
    let reader: Awaited<ReturnType<MediaEngine['createVideoReader']>> | undefined;
    try {
      progress({ phase: 'load-model', progress: 0.02, backend });
      const processor = await tf.AutoProcessor.from_pretrained('fastvlm-0.5b', {
        local_files_only: true
      });
      model = await tf.AutoModelForImageTextToText.from_pretrained('fastvlm-0.5b', {
        device: backend,
        dtype: { embed_tokens: 'fp16', vision_encoder: 'q4', decoder_model_merged: 'q4' },
        local_files_only: true
      });
      if (request.kind === 'video')
        reader = await media.createVideoReader(mediaUrl.href, { width: 1024, height: 1024 });
      if (request.resultFormat === 'segments') {
        const segments: NonNullable<VisionResult['segments']> = [];
        let decoded = 0;
        for (const segment of request.segmentPlan!) {
          const columns = segment.sampleTimes.length === 1 ? 1 : 2;
          const panelWidth = 1024 / columns,
            panelHeight = columns === 1 ? 1024 : 512;
          const canvas = new OffscreenCanvas(
            1024,
            Math.ceil(segment.sampleTimes.length / columns) * panelHeight
          );
          const context = canvas.getContext('2d');
          if (!context) throw new Error('Cannot prepare vision storyboard');
          context.fillStyle = '#111111';
          context.fillRect(0, 0, canvas.width, canvas.height);
          const samples = [];
          for (let i = 0; i < segment.sampleTimes.length; i++) {
            const requestedSeconds = segment.sampleTimes[i];
            progress({
              phase: 'decode-frame',
              progress: 0.1 + (0.85 * decoded) / request.sampleTimes.length,
              backend
            });
            const frame = await reader!.frameAt(requestedSeconds);
            try {
              const left = (i % columns) * panelWidth,
                top = Math.floor(i / columns) * panelHeight;
              const scale = Math.min(
                (panelWidth - 8) / frame.width,
                (panelHeight - 38) / frame.height
              );
              const width = frame.width * scale,
                height = frame.height * scale;
              context.fillStyle = '#ffffff';
              context.fillRect(
                left + (panelWidth - width) / 2,
                top + 4 + (panelHeight - 38 - height) / 2,
                width,
                height
              );
              context.drawImage(
                frame.frame,
                left + (panelWidth - width) / 2,
                top + 4 + (panelHeight - 38 - height) / 2,
                width,
                height
              );
              context.fillStyle = '#ffffff';
              context.font = '18px monospace';
              context.fillText(`${frame.timestamp.toFixed(3)}s`, left + 8, top + panelHeight - 10);
              samples.push({
                requestedSeconds,
                sourceSeconds: frame.timestamp,
                durationSeconds: frame.duration
              });
            } finally {
              frame.close();
            }
            decoded++;
          }
          const pixels = context.getImageData(0, 0, canvas.width, canvas.height);
          progress({
            phase: 'describe-segment',
            progress: 0.1 + (0.85 * decoded) / request.sampleTimes.length,
            backend
          });
          const text = await describeFrame({
            processor,
            model,
            image: new tf.RawImage(
              new Uint8ClampedArray(pixels.data),
              canvas.width,
              canvas.height,
              4
            ),
            prompt: storyboardPrompt(request.prompt, segment),
            maxNewTokens: request.maxNewTokens
          });
          segments.push({
            startSeconds: segment.startSeconds,
            endSeconds: segment.endSeconds,
            text,
            samples
          });
        }
        return { frames: [], segments, backend };
      }
      const frames: VisionResult['frames'] = [];
      for (let i = 0; i < request.sampleTimes.length; i++) {
        const requestedSeconds = request.sampleTimes[i];
        progress({
          phase: 'decode-frame',
          progress: 0.1 + (0.85 * i) / request.sampleTimes.length,
          backend
        });
        const frame = reader
          ? await reader.frameAt(requestedSeconds)
          : await media.image(mediaUrl.href, { width: 1024, height: 1024 });
        try {
          const canvas = new OffscreenCanvas(frame.width, frame.height),
            context = canvas.getContext('2d');
          if (!context) throw new Error('Cannot prepare vision pixels');
          context.fillStyle = '#ffffff';
          context.fillRect(0, 0, frame.width, frame.height);
          context.drawImage(frame.frame, 0, 0);
          const pixels = context.getImageData(0, 0, frame.width, frame.height);
          const image = new tf.RawImage(
            new Uint8ClampedArray(pixels.data),
            frame.width,
            frame.height,
            4
          );
          progress({
            phase: 'describe-frame',
            progress: 0.1 + (0.85 * (i + 0.2)) / request.sampleTimes.length,
            backend
          });
          const text = await describeFrame({
            processor,
            model,
            image,
            prompt: request.prompt,
            maxNewTokens: request.maxNewTokens
          });
          frames.push({
            requestedSeconds,
            sourceSeconds: frame.timestamp,
            durationSeconds: request.kind === 'image' ? 1 : frame.duration,
            text
          });
        } finally {
          frame.close();
        }
      }
      return { frames, backend };
    } catch (error) {
      lastError = error;
      if (backend === 'webgpu' && backends.length > 1)
        progress({ phase: 'fallback', progress: 0.01, backend: 'wasm' });
    } finally {
      await reader?.close();
      media.dispose();
      await model?.dispose().catch(() => {});
    }
  }
  throw lastError;
}
self.onmessage = async ({ data }: MessageEvent<VisionRequest>) => {
  try {
    self.postMessage({ type: 'result', result: await run(data) });
  } catch (error) {
    self.postMessage({
      type: 'result',
      error: {
        name: error instanceof Error ? error.name : 'Error',
        message: error instanceof Error ? error.message : String(error)
      }
    });
  }
};
