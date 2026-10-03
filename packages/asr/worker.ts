import type * as Transformers from '@huggingface/transformers';
import type { AsrRequest, AsrResult, AsrProgress } from './types';
import type { AsrSegment } from '../client/types';
import { MediaEngine } from '../media/browser';
import {
  audioWindows,
  mixMono16k,
  isSilent,
  windowWords,
  subtitleSegments,
  ASR_SAMPLE_RATE
} from './runtime.mjs';

const progress = (value: AsrProgress) => self.postMessage({ type: 'progress', progress: value });
const languages = {
  auto: undefined,
  zh: 'chinese',
  en: 'english',
  ja: 'japanese',
  ko: 'korean',
  fr: 'french',
  de: 'german',
  es: 'spanish',
  ru: 'russian'
};
async function run(request: AsrRequest): Promise<AsrResult> {
  const modelUrl = new URL(request.modelBaseUrl, self.location.origin),
    mediaUrl = new URL(request.mediaUrl);
  if (
    modelUrl.origin !== self.location.origin ||
    modelUrl.pathname !== '/asr-models/whisper-base/' ||
    modelUrl.search ||
    modelUrl.hash ||
    modelUrl.username ||
    modelUrl.password ||
    mediaUrl.origin !== self.location.origin ||
    !/^\/api\/sessions\/[a-f0-9]+\/media$/.test(mediaUrl.pathname)
  )
    throw new Error('ASR resources must be local');
  if (
    !Object.hasOwn(languages, request.language) ||
    !['auto', 'webgpu', 'wasm'].includes(request.backend)
  )
    throw new Error('Invalid ASR language or backend');
  const windows = audioWindows(request.sourceBegin, request.sourceEnd);
  const tf = (await import(
    /* @vite-ignore */ new URL('/asr-runtime/transformers.min.js', self.location.origin).href
  )) as typeof Transformers;
  tf.env.allowRemoteModels = false;
  tf.env.allowLocalModels = true;
  tf.env.localModelPath = new URL('/asr-models/', self.location.origin).href;
  tf.env.useBrowserCache = false; // The service owns a revisioned, SHA256-verified persistent cache.
  const wasm = tf.env.backends.onnx.wasm;
  if (!wasm) throw new Error('Whisper WASM runtime is missing');
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
    let pipeline: Transformers.AutomaticSpeechRecognitionPipeline | undefined;
    const media = new MediaEngine();
    try {
      progress({ phase: 'load-model', progress: 0.17, backend });
      const createPipeline = tf.pipeline as (
        task: 'automatic-speech-recognition',
        model: string,
        options: Record<string, unknown>
      ) => Promise<Transformers.AutomaticSpeechRecognitionPipeline>;
      pipeline = await createPipeline('automatic-speech-recognition', 'whisper-base', {
        device: backend,
        dtype: { encoder_model: 'fp32', decoder_model_merged: 'q4' },
        local_files_only: true
      });
      const words: AsrSegment[] = [];
      for (let index = 0; index < windows.length; index++) {
        const window = windows[index];
        progress({
          phase: 'decode-audio',
          progress: 0.2 + (0.75 * index) / windows.length,
          backend
        });
        const pcm = new Float32Array(Math.round((window.end - window.begin) * ASR_SAMPLE_RATE));
        for await (const block of media.audioChunks(mediaUrl.href, window.begin, window.end))
          mixMono16k(pcm, block, window.begin);
        if (isSilent(pcm)) continue;
        progress({
          phase: 'transcribe',
          progress: 0.2 + (0.75 * (index + 0.2)) / windows.length,
          backend
        });
        const output = await pipeline(pcm, {
          return_timestamps: 'word',
          language: languages[request.language],
          task: 'transcribe',
          max_new_tokens: 448
        });
        const result = Array.isArray(output) ? output[0] : output;
        words.push(...windowWords(result, window, request.sourceBegin));
      }
      const segments = subtitleSegments(words);
      progress({ phase: 'complete', progress: 0.98, backend });
      return { text: segments.map((segment) => segment.text).join('\n'), segments, backend };
    } catch (error) {
      lastError = error;
      if (backend === 'webgpu' && backends.length > 1)
        progress({ phase: 'fallback', progress: 0.16, backend: 'wasm' });
    } finally {
      media.dispose();
      await pipeline?.dispose().catch(() => {});
    }
  }
  throw lastError;
}
self.onmessage = async ({ data }: MessageEvent<AsrRequest>) => {
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
