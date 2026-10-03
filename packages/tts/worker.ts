import type * as Ort from 'onnxruntime-web';
import type { TtsRequest, TtsProgress } from './types';
import { phonemize } from './vendor/phonemize.mjs';
import { validateRequest, splitText, tokenChunks, styleVector, chooseBackend, encodeWav, MODEL_FILES, MAX_AUDIO_SAMPLES } from './runtime.mjs';

const progress = (value: TtsProgress) => self.postMessage({ type: 'progress', progress: value });
const runtimeBase = new URL('/tts-runtime/', self.location.origin);

async function readLocal(url: string, maxBytes: number): Promise<ArrayBuffer> {
  const target = new URL(url);
  if (target.origin !== self.location.origin || target.username || target.password || target.search || target.hash)
    throw new Error('Speech resources must be local and must not include credentials');
  const response = await fetch(target.href, { credentials: 'omit', mode: 'same-origin', redirect: 'error', cache: 'no-cache' });
  if (!response.ok) throw new Error(`Missing installed speech resource (${target.pathname}); install this model precision and voice first`);
  const statedLength = Number(response.headers.get('content-length'));
  if (statedLength > maxBytes) throw new Error('Speech resource exceeds its size limit');
  const bytes = await response.arrayBuffer();
  if (bytes.byteLength > maxBytes || (statedLength > 0 && bytes.byteLength !== statedLength))
    throw new Error('Speech resource is incomplete or exceeds its size limit');
  return bytes;
}
async function jsonLocal(base: string, name: string) {
  return JSON.parse(new TextDecoder().decode(await readLocal(new URL(name, base).href, 2_000_000)));
}

async function run(request: TtsRequest) {
  progress({ phase: 'frontend', progress: 0.01, message: 'Preparing text and installed voice' });
  const [tokenizer, voiceBytes] = await Promise.all([
    jsonLocal(request.modelBaseUrl, 'tokenizer.json'),
    readLocal(new URL(`voices/${request.voice}.bin`, request.modelBaseUrl).href, 1_000_000)
  ]);
  const language = request.voice.startsWith('z') ? 'z' : request.voice.startsWith('b') ? 'b' : 'a';
  const chunks: number[][] = [];
  for (const text of splitText(request.text)) chunks.push(...tokenChunks(await phonemize(text, language), tokenizer));
  if (!chunks.length) throw new Error('This text produced no speech tokens');
  // A token produces multiple audio frames; token splitting also prevents style-index overflow.
  if (chunks.length > 100) throw new Error('Speech text is too long for one synthesis job');
  const gpu = (navigator as Navigator & { gpu?: { requestAdapter(): Promise<object | null> } }).gpu;
  const adapter = request.backend === 'wasm' ? null : await gpu?.requestAdapter().catch(() => null);
  return chooseBackend(request.backend, Boolean(adapter), async (backend) => {
    const dtype = request.dtype;
    progress({ phase: 'load-model', progress: 0.06, message: `Loading installed ${dtype} model for ${backend}` });
    const ort = await import(/* @vite-ignore */ new URL(backend === 'webgpu' ? 'ort.webgpu.min.mjs' : 'ort.wasm.min.mjs', runtimeBase).href) as typeof Ort;
    const suffix = backend === 'webgpu' ? '.asyncify' : '';
    ort.env.wasm.numThreads = 1;
    ort.env.wasm.proxy = false;
    ort.env.wasm.wasmPaths = {
      mjs: new URL(`ort-wasm-simd-threaded${suffix}.mjs`, runtimeBase).href,
      wasm: new URL(`ort-wasm-simd-threaded${suffix}.wasm`, runtimeBase).href
    };
    const modelBytes = await readLocal(new URL(MODEL_FILES[dtype], request.modelBaseUrl).href, 400_000_000);
    progress({ phase: 'initialize', progress: 0.1, message: `Initializing ${backend} inference` });
    let session: Ort.InferenceSession | undefined;
    try {
      // A second JS backend would silently mask WebGPU initialization failure.
      // ORT's WebGPU session still has CPU kernels for individual unsupported operators.
      session = await ort.InferenceSession.create(modelBytes, { executionProviders: [backend], graphOptimizationLevel: 'all' });
      if (!['input_ids', 'style', 'speed'].every((name) => session!.inputNames.includes(name)) || !session.outputNames.includes('waveform'))
        throw new Error('The installed graph is not a compatible Kokoro browser export');
      const audio: Float32Array[] = [];
      let totalSamples = 0;
      for (let index = 0; index < chunks.length; index++) {
        progress({ phase: 'synthesize', progress: 0.15 + 0.8 * index / chunks.length, message: `Synthesizing segment ${index + 1}/${chunks.length} with ${backend}` });
        const ids = chunks[index];
        const input = new ort.Tensor('int64', BigInt64Array.from([0, ...ids, 0], (value) => BigInt(value)), [1, ids.length + 2]);
        const style = new ort.Tensor('float32', styleVector(voiceBytes, ids.length), [1, 256]);
        const speed = new ort.Tensor('float32', Float32Array.of(request.speed), [1]);
        let outputs: Ort.InferenceSession.ReturnType | undefined;
        try {
          outputs = await session.run({ input_ids: input, style, speed });
          const waveform = outputs.waveform;
          if (waveform.type !== 'float32') throw new Error('The speech model must return FP32 audio');
          const samples = waveform.data as Float32Array;
          totalSamples += samples.length;
          if (!samples.length || totalSamples > MAX_AUDIO_SAMPLES) throw new Error('Generated speech exceeds the five-minute limit or is empty');
          if (samples.some((sample) => !Number.isFinite(sample))) throw new Error('The speech model returned invalid audio');
          audio.push(samples.slice());
        } finally {
          input.dispose(); style.dispose(); speed.dispose();
          for (const output of Object.values(outputs || {})) output.dispose();
        }
      }
      progress({ phase: 'encode', progress: 0.97, message: 'Encoding narration WAV' });
      return encodeWav(audio);
    } finally { await session?.release(); }
  }, progress);
}

self.onmessage = async ({ data }: MessageEvent<TtsRequest>) => {
  try {
    const request = validateRequest(data, self.location.origin);
    const result = await run(request);
    progress({ phase: 'complete', progress: 1 });
    self.postMessage({ type: 'result', result }, { transfer: [result.wav] });
  } catch (error) {
    self.postMessage({ type: 'result', error: { name: error instanceof Error ? error.name : 'Error', message: error instanceof Error ? error.message : String(error) } });
  }
};
