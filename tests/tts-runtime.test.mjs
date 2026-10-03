import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
import { createHash } from 'node:crypto';
import { build } from 'esbuild';
import { validateRequest, splitText, tokenChunks, styleVector, chooseBackend, encodeWav } from '../packages/tts/runtime.mjs';
import { phonemize } from '../packages/tts/vendor/phonemize.mjs';
import { ttsAssetFiles, ttsAssetBytes } from '../scripts/tts-assets.mjs';

const request = { text: '你好。', voice: 'zf_001', speed: 1, backend: 'auto', dtype: 'fp32', modelBaseUrl: '/tts-models/kokoro-v1.1-zh/' };
const tokenizer = { model: { vocab: { $: 0, 'ㄅ': 30, 'ㄚ': 100, '1': 171, ' ': 16, ',': 3 } } };

test('English source receipts match every retained file and exclude the removed compiled engine', async () => {
  const receipt = JSON.parse(await readFile('packages/tts/vendor/ENGLISH-SOURCE.json', 'utf8'));
  for (const [file, hashes] of Object.entries(receipt.files))
    assert.equal(createHash('sha256').update(await readFile(`packages/tts/vendor/${file}`)).digest('hex'), hashes.sha256, file);
  const pkg = JSON.parse(await readFile('package.json', 'utf8'));
  assert.equal(pkg.devDependencies.phonemizer, undefined);
  assert.ok(![...ttsAssetFiles.keys()].some((file) => /ESpeak|Phonemizer|GPL/.test(file)));
  assert.ok(ttsAssetFiles.has('tts-runtime/licenses/HeadTTS-MIT.txt'));
  assert.ok(ttsAssetFiles.has('tts-runtime/licenses/Misaki-Apache-2.0.txt'));
});

test('speech resources cannot escape the installed same-origin model directory or carry tokens', () => {
  assert.equal(validateRequest(request, 'http://127.0.0.1:4318').modelBaseUrl, 'http://127.0.0.1:4318/tts-models/kokoro-v1.1-zh/');
  for (const modelBaseUrl of ['https://huggingface.co/model/', '/tts-models/kokoro-v1.1-zh/?token=secret', '/api/', '//example.com/tts-models/kokoro-v1.1-zh/'])
    assert.throws(() => validateRequest({ ...request, modelBaseUrl }, 'http://127.0.0.1:4318'), /same-origin/);
  assert.throws(() => validateRequest({ ...request, text: 'x'.repeat(8001) }, 'http://127.0.0.1:4318'), /8000/);
  assert.throws(() => validateRequest({ ...request, voice: '../voice' }, 'http://127.0.0.1:4318'), /voice/);
  for (const dtype of ['fp16', 'q8'])
    assert.throws(() => validateRequest({ ...request, dtype }, 'http://127.0.0.1:4318'), /precision/);
});

test('Chinese front end pronounces numbers and preserves English in mixed scripts without Python', async () => {
  const digits = await phonemize('完成率是95%。', 'z');
  const spoken = await phonemize('完成率是百分之九十五。', 'z');
  assert.equal(digits, spoken);
  assert.match(digits, /ㄅㄞ3ㄈㄣ1ㄓ十1/);
  const mixed = await phonemize('欢迎使用 VideoCut。', 'z');
  assert.match(mixed, /ㄏ万1应2/);
  assert.match(mixed, /vˈɪdiO kˈʌt/);
  for (const [written, spoken] of [
    ['温度是12.5℃。', '温度是摄氏十二点五度。'],
    ['温度是-12.5℃。', '温度是摄氏负十二点五度。'],
    ['变化是-12.5。', '变化是负十二点五。'],
    ['价格是-12元。', '价格是负十二元。'],
    ['重量是5kg。', '重量是五千克。'],
    ['版本 v1.1。', '版本 v一点一。']
  ]) assert.equal(await phonemize(written, 'z'), await phonemize(spoken, 'z'), written);
});

test('embedded English frontend runs in a Worker environment with all network access disabled', async () => {
  const bundle = await build({ entryPoints: ['packages/tts/vendor/english.mjs'], bundle: true,
    platform: 'browser', format: 'iife', globalName: '__frontend', write: false });
  const source = bundle.outputFiles[0].text;
  let requests = 0;
  const context = vm.createContext({
    atob, TextDecoder, TextEncoder, Blob, Response, DecompressionStream, Intl,
    setTimeout, clearTimeout, console,
    importScripts() { requests++; throw new Error('Unexpected importScripts'); },
    XMLHttpRequest: class { constructor() { requests++; throw new Error('Unexpected XHR'); } },
    fetch() { requests++; throw new Error('Unexpected fetch'); }
  });
  vm.runInContext(source, context);
  assert.equal(typeof context.process, 'undefined');
  assert.equal(typeof context.Buffer, 'undefined');
  assert.match((await context.__frontend.phonemize('Welcome to VideoCut.', 'en-us')).join(''), /vˈɪdiO kˈʌt/);
  assert.match((await context.__frontend.phonemize('Hello World', 'en')).join(''), /wˈɜːld/);
  assert.match((await context.__frontend.phonemize('schedule', 'en')).join(''), /ʃˈɛdjuːl/);
  assert.match((await context.__frontend.phonemize('schedule', 'en-us')).join(''), /skˈɛʤˌul/);
  assert.ok((await context.__frontend.phonemize('Xyloflex 123.45!', 'en-us')).join('').length > 10);
  assert.doesNotMatch(source, /Emscripten|node:fs|XMLHttpRequest|importScripts|espeakng\.worker/);
  assert.equal(requests, 0);
});

test('long sentences and more than 510 phonemes are retained in bounded windows', () => {
  const text = '你好，'.repeat(240) + '12.5元。';
  const sentences = splitText(text);
  assert.equal(sentences.join(''), text);
  assert.ok(sentences.every((part) => Array.from(part).length <= 200));
  const phonemes = 'ㄅㄚ1'.repeat(400);
  const chunks = tokenChunks(phonemes, tokenizer);
  assert.equal(chunks.flat().length, 1200);
  assert.ok(chunks.every((ids) => ids.length <= 510));
  assert.equal(chunks.flat().filter((id) => id === 30).length, 400);
  assert.throws(() => tokenChunks('❓', tokenizer), /cannot pronounce/);
});

test('voice style length selects the first and final valid row without an overflow', () => {
  const values = new Float32Array(510 * 256);
  values.fill(1, 0, 256); values.fill(510, 509 * 256);
  assert.equal(styleVector(values.buffer, 1)[0], 1);
  assert.equal(styleVector(values.buffer, 510)[0], 510);
  assert.throws(() => styleVector(values.buffer, 511), /token count/);
  assert.throws(() => styleVector(new ArrayBuffer(32), 1), /incomplete/);
});

test('a failed GPU generation reruns the complete job and reports the CPU backend truthfully', async () => {
  const runs = [], events = [];
  const result = await chooseBackend('auto', true, async (backend) => {
    runs.push(backend);
    if (backend === 'webgpu') throw new Error('GPU device lost after first segment');
    return { wav: 'complete CPU output' };
  }, (progress) => events.push(progress));
  assert.deepEqual(runs, ['webgpu', 'wasm']);
  assert.equal(result.backend, 'wasm');
  assert.equal(result.wav, 'complete CPU output');
  assert.equal(events[0].phase, 'fallback');
  await assert.rejects(chooseBackend('webgpu', true, async () => { throw new Error('device lost'); }), /device lost/);
  await assert.rejects(chooseBackend('webgpu', false, async () => { assert.fail('must not run'); }), /unavailable/);
});

test('24 kHz mono WAV preserves segment duration, clamps PCM and rejects invalid or oversized output', () => {
  const result = encodeWav([Float32Array.of(-2, -1, 0), Float32Array.of(0.5, 1, 2)]);
  const view = new DataView(result.wav);
  assert.equal(new TextDecoder().decode(new Uint8Array(result.wav, 0, 4)), 'RIFF');
  assert.equal(view.getUint32(24, true), 24000);
  assert.equal(view.getUint16(22, true), 1);
  assert.equal(view.getUint32(40, true), 12);
  assert.equal(result.durationSeconds, 6 / 24000);
  assert.deepEqual(Array.from({ length: 6 }, (_, i) => view.getInt16(44 + i * 2, true)), [-32768, -32768, 0, 16384, 32767, 32767]);
  assert.throws(() => encodeWav([Float32Array.of(NaN)]), /non-finite/);
  assert.throws(() => encodeWav([new Float32Array(4)], 3), /five minutes/);
});

test('cancelling a client terminates its active worker, rejects the job, and permits a new worker', async (t) => {
  const output = await build({ entryPoints: ['packages/tts/client.ts'], bundle: true, platform: 'browser', format: 'esm', write: false, define: { 'import.meta.url': '"https://localhost/assets/tts-client.js"' } });
  const { TtsWorkerClient } = await import(`data:text/javascript;base64,${Buffer.from(output.outputFiles[0].text).toString('base64')}`);
  const workers = [];
  const savedWorker = globalThis.Worker;
  globalThis.Worker = class {
    constructor() { workers.push(this); }
    postMessage(value) { this.request = value; }
    terminate() { this.terminated = true; }
  };
  t.after(() => { globalThis.Worker = savedWorker; });
  const client = new TtsWorkerClient(), controller = new AbortController();
  const first = client.synthesize(request, { signal: controller.signal });
  const rejection = assert.rejects(first, { name: 'AbortError' });
  controller.abort(); await rejection;
  assert.equal(workers[0].terminated, true);
  const second = client.synthesize(request);
  const rejectedAgain = assert.rejects(second, { name: 'AbortError' });
  assert.equal(workers.length, 2);
  client.dispose(); await rejectedAgain;
  assert.equal(workers[1].terminated, true);
});

test('runtime resource closure ships real local WASM, licenses and no source-map pointers', async () => {
  assert.equal([...ttsAssetFiles.keys()].filter((name) => name.endsWith('.wasm')).length, 2);
  for (const name of ttsAssetFiles.keys()) {
    const bytes = ttsAssetBytes(name);
    if (name.endsWith('.wasm')) assert.equal(WebAssembly.validate(bytes), true, name);
    else assert.doesNotMatch(bytes.toString('utf8'), /sourceMappingURL=|sourceURL=/);
  }
  const source = await readFile('packages/tts/vendor/phonemize.mjs', 'utf8');
  assert.doesNotMatch(source, /@huggingface|fs\/promises|from ['"]path['"]/);
});
