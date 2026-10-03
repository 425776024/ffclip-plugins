export const SAMPLE_RATE = 24000;
export const MAX_AUDIO_SAMPLES = SAMPLE_RATE * 300;
export const MODEL_BASE_PATH = '/tts-models/kokoro-v1.1-zh/';
export const MODEL_FILES = Object.freeze({ fp32: 'onnx/model.onnx' });

export function validateRequest(request, origin) {
  if (!request || typeof request.text !== 'string' || !request.text.trim() || request.text.length > 8000)
    throw new Error('Speech text must contain 1–8000 characters');
  if (typeof request.voice !== 'string' || !/^(?:z[fm]_\d{3}|af_maple|af_sol|bf_vale)$/.test(request.voice))
    throw new Error('Unsupported Kokoro v1.1 Chinese/English voice');
  if (!Number.isFinite(request.speed) || request.speed < 0.5 || request.speed > 2)
    throw new Error('Speech speed must be between 0.5 and 2');
  if (!['auto', 'webgpu', 'wasm'].includes(request.backend) || !Object.hasOwn(MODEL_FILES, request.dtype))
    throw new Error('Unsupported speech backend or model precision');
  const url = new URL(request.modelBaseUrl, origin);
  if (url.origin !== origin || url.pathname !== MODEL_BASE_PATH || url.search || url.hash || url.username || url.password)
    throw new Error('Speech models must use the installed same-origin Kokoro model directory');
  return { ...request, text: request.text.trim(), modelBaseUrl: url.href };
}

/** Split at sentence boundaries, then limit G2P work without dropping a single character. */
export function splitText(text, limit = 200) {
  const chunks = [];
  for (const sentence of text.match(/[^。！？!?\n]+[。！？!?\n]*/gu) || []) {
    let remaining = Array.from(sentence.trim());
    while (remaining.length > limit) {
      let end = limit;
      for (let i = limit - 1; i >= Math.floor(limit / 2); i--) {
        if (/[，、；;,\s]/u.test(remaining[i])) { end = i + 1; break; }
      }
      chunks.push(remaining.slice(0, end).join(''));
      remaining = remaining.slice(end);
    }
    if (remaining.length) chunks.push(remaining.join(''));
  }
  return chunks;
}

export function tokenChunks(phonemes, tokenizer, limit = 510) {
  const vocab = tokenizer?.model?.vocab;
  if (!vocab || vocab.$ !== 0 || !Object.hasOwn(vocab, 'ㄅ')) throw new Error('The installed tokenizer is not Kokoro v1.1-zh');
  if (phonemes.includes('❓')) throw new Error('This text contains a character that the Chinese speech frontend cannot pronounce');
  const chars = Array.from(phonemes).filter((char) => Object.hasOwn(vocab, char));
  const chunks = [];
  while (chars.length) {
    let end = Math.min(chars.length, limit);
    if (chars.length > limit) {
      for (let i = limit - 1; i >= Math.floor(limit / 2); i--) {
        if (/[ ;:,.!?/]/u.test(chars[i])) { end = i + 1; break; }
      }
    }
    chunks.push(chars.splice(0, end).map((char) => vocab[char]));
  }
  return chunks;
}

export function styleVector(voiceBytes, tokenCount) {
  if (voiceBytes.byteLength % 1024 !== 0 || voiceBytes.byteLength < 510 * 1024)
    throw new Error('The installed voice data is incomplete or incompatible');
  if (!Number.isInteger(tokenCount) || tokenCount < 1 || tokenCount > 510) throw new Error('Invalid speech token count');
  // Voice packs have one 256-float style row for each phoneme length (Python KPipeline contract).
  const samples = new Float32Array(voiceBytes);
  const style = samples.slice((tokenCount - 1) * 256, tokenCount * 256);
  if (style.some((value) => !Number.isFinite(value))) throw new Error('Invalid values in voice data');
  return style;
}

export function encodeWav(chunks, limit = MAX_AUDIO_SAMPLES) {
  const length = chunks.reduce((total, chunk) => total + chunk.length, 0);
  if (!length || length > limit) throw new Error('Generated speech must be nonempty and at most five minutes');
  const wav = new ArrayBuffer(44 + length * 2), view = new DataView(wav);
  const write = (offset, value) => { for (let i = 0; i < value.length; i++) view.setUint8(offset + i, value.charCodeAt(i)); };
  write(0, 'RIFF'); view.setUint32(4, wav.byteLength - 8, true); write(8, 'WAVE'); write(12, 'fmt ');
  view.setUint32(16, 16, true); view.setUint16(20, 1, true); view.setUint16(22, 1, true);
  view.setUint32(24, SAMPLE_RATE, true); view.setUint32(28, SAMPLE_RATE * 2, true);
  view.setUint16(32, 2, true); view.setUint16(34, 16, true); write(36, 'data'); view.setUint32(40, length * 2, true);
  let offset = 44;
  for (const chunk of chunks) for (const value of chunk) {
    if (!Number.isFinite(value)) throw new Error('The speech model returned non-finite audio');
    const sample = Math.max(-1, Math.min(1, value));
    view.setInt16(offset, Math.round(sample * (sample < 0 ? 32768 : 32767)), true); offset += 2;
  }
  return { wav, sampleRate: SAMPLE_RATE, durationSeconds: length / SAMPLE_RATE };
}

/** A fallback reruns all chunks, so an output never mixes partial GPU and CPU runs. */
export async function chooseBackend(requested, gpuAvailable, run, progress = () => {}) {
  if (requested === 'wasm') return { ...(await run('wasm')), backend: 'wasm' };
  if (!gpuAvailable && requested === 'webgpu') throw new Error('WebGPU is unavailable in this browser');
  if (gpuAvailable) {
    try { return { ...(await run('webgpu')), backend: 'webgpu' }; }
    catch (error) {
      if (requested === 'webgpu') throw error;
      progress({ phase: 'fallback', progress: 0.1, message: 'WebGPU failed; retrying with browser CPU inference' });
    }
  }
  return { ...(await run('wasm')), backend: 'wasm' };
}
