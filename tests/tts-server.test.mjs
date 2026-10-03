import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdtemp, readFile, readdir, realpath, rm, symlink, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, relative, isAbsolute } from 'node:path';
import { Readable } from 'node:stream';
import {
  KokoroModelStore,
  KOKORO_MODEL_ID,
  KOKORO_REVISION
} from '../packages/server/tts-models.mjs';
import {
  TtsJob,
  validateTtsRequest,
  validateTtsWav,
  createTtsRoutes,
  TTS_MAX_WAV_BYTES
} from '../packages/server/tts.mjs';
import { startServer } from '../packages/server/index.mjs';
import { createProject, CommandHistory } from '../packages/core/project.mjs';
import { probeContainer } from '../packages/server/media.mjs';
import { VideoCutClient } from '../packages/client/index.mjs';

async function temporary(t) {
  const directory = await realpath(await mkdtemp(join(tmpdir(), 'videocut-tts-test-')));
  t.after(() => rm(directory, { recursive: true, force: true }));
  return directory;
}
function wav({ frames = 2400, sampleRate = 24000, codec = 1, value = 0.2 } = {}) {
  const bits = codec === 3 ? 32 : 16,
    align = bits / 8,
    bytes = Buffer.alloc(44 + frames * align);
  bytes.write('RIFF', 0);
  bytes.writeUInt32LE(bytes.length - 8, 4);
  bytes.write('WAVEfmt ', 8);
  bytes.writeUInt32LE(16, 16);
  bytes.writeUInt16LE(codec, 20);
  bytes.writeUInt16LE(1, 22);
  bytes.writeUInt32LE(sampleRate, 24);
  bytes.writeUInt32LE(sampleRate * align, 28);
  bytes.writeUInt16LE(align, 32);
  bytes.writeUInt16LE(bits, 34);
  bytes.write('data', 36);
  bytes.writeUInt32LE(frames * align, 40);
  for (let i = 0; i < frames; i++)
    codec === 3
      ? bytes.writeFloatLE(value, 44 + i * align)
      : bytes.writeInt16LE(Math.round(value * 32767), 44 + i * align);
  return bytes;
}
function incoming(bytes, headers = {}) {
  return Object.assign(Readable.from([bytes]), {
    headers: { 'content-type': 'audio/wav', ...headers }
  });
}
function fakeResources() {
  const content = Object.fromEntries(
    [
      'config.json',
      'tokenizer.json',
      'tokenizer_config.json',
      'onnx/model.onnx',
      'voices/zf_001.bin',
      'voices/zm_010.bin'
    ].map((path) => [path, Buffer.from(`fixture ${path}`)])
  );
  const files = Object.fromEntries(
    Object.entries(content).map(([path, data]) => [
      path,
      { size: data.length, sha256: createHash('sha256').update(data).digest('hex') }
    ])
  );
  return { content, files };
}

test('model installation is explicit, revision-pinned, verified, resumable and voice-selective', async (t) => {
  const directory = await temporary(t),
    { content, files } = fakeResources(),
    requests = [];
  const store = new KokoroModelStore({
    directory,
    files,
    fetchImpl: async (url) => {
      requests.push(url);
      const path = url.split(`${KOKORO_REVISION}/`)[1];
      return new Response(content[path]);
    }
  });
  assert.deepEqual((await store.catalog()).installedDtypes, []);
  assert.equal(requests.length, 0);
  for (const dtype of ['fp16', 'q8']) assert.throws(() => store.startInstall({ dtype }), /无效/);
  assert.throws(() => store.startInstall({ dtype: 'fp32', voices: ['../../etc/passwd'] }), /无效/);
  store.startInstall({ dtype: 'fp32', voices: ['zf_001'] });
  await store.pending;
  assert.equal(store.status().state, 'ready');
  assert.equal(requests.length, 5);
  assert.ok(
    requests.every((url) =>
      url.startsWith(
        `https://huggingface.co/onnx-community/Kokoro-82M-v1.1-zh-ONNX/resolve/${KOKORO_REVISION}/`
      )
    )
  );
  assert.deepEqual((await store.catalog()).installedVoices, ['zf_001']);
  const restarted = new KokoroModelStore({
    directory,
    files,
    fetchImpl: () => {
      throw Error('network must remain unused');
    }
  });
  await restarted.require('fp32', 'zf_001');
  assert.deepEqual((await restarted.catalog()).installedDtypes, ['fp32']);
  restarted.startInstall({ dtype: 'fp32', voices: ['zf_001'] });
  await restarted.pending;
  assert.equal(restarted.status().state, 'ready');
  await assert.rejects(
    restarted.require('fp32', 'zm_010'),
    (error) => error.code === 'TTS_MODEL_REQUIRED'
  );
  await assert.rejects(restarted.verifiedFile('../config.json'), /固定清单/);
  await writeFile(await restarted.resourcePath('config.json'), Buffer.from('corrupt bytes'));
  await assert.rejects(
    restarted.require('fp32', 'zf_001'),
    (error) => error.code === 'TTS_MODEL_REQUIRED'
  );
});

test('model cancellation and bad hashes never publish partial resources', async (t) => {
  const directory = await temporary(t),
    { files } = fakeResources();
  let started;
  const waiting = new Promise((resolve) => {
    started = resolve;
  });
  const store = new KokoroModelStore({
    directory,
    files,
    fetchImpl: async (_url, { signal }) => {
      started();
      return new Response(
        new ReadableStream({
          start(controller) {
            controller.enqueue(new Uint8Array([1]));
            signal.addEventListener(
              'abort',
              () => controller.error(new DOMException('cancelled', 'AbortError')),
              { once: true }
            );
          }
        })
      );
    }
  });
  store.startInstall();
  await waiting;
  await store.cancel();
  assert.equal(store.status().state, 'cancelled');
  assert.equal(await store.verifiedFile('config.json'), null);
  const root = join(directory, KOKORO_MODEL_ID, KOKORO_REVISION);
  assert.ok(!(await readdir(root)).some((name) => name.includes('.partial-')));
  const bad = new KokoroModelStore({
    directory,
    files,
    fetchImpl: async () => new Response(Buffer.alloc(files['config.json'].size))
  });
  bad.startInstall();
  await bad.pending;
  assert.equal(bad.status().state, 'error');
  assert.match(bad.status().error, /校验/);
  assert.equal(await bad.verifiedFile('config.json'), null);
});

test('model serving refuses symlink directories and symlink files', async (t) => {
  const directory = await temporary(t),
    outside = await temporary(t),
    { files } = fakeResources();
  const store = new KokoroModelStore({ directory, files });
  await store.root();
  await symlink(outside, join(directory, KOKORO_MODEL_ID, KOKORO_REVISION, 'voices'));
  await assert.rejects(store.verifiedFile('voices/zf_001.bin'), /符号链接/);
  await writeFile(join(outside, 'config.json'), 'fixture config.json');
  await symlink(
    join(outside, 'config.json'),
    join(directory, KOKORO_MODEL_ID, KOKORO_REVISION, 'config.json')
  );
  assert.equal(await store.verifiedFile('config.json'), null);
});

test('WAV validation rejects truncated, oversized, empty, nonfinite and incoherent audio', () => {
  assert.equal(validateTtsWav(wav()).frames, 2400);
  assert.equal(validateTtsWav(wav({ codec: 3 })).codec, 3);
  assert.throws(() => validateTtsWav(wav().subarray(0, 50)), /有效/);
  assert.throws(() => validateTtsWav(wav({ frames: 0 })), /必须/);
  assert.throws(() => validateTtsWav(wav({ codec: 3, value: NaN })), /无效数值/);
  const bad = wav();
  bad.writeUInt32LE(1, 28);
  assert.throws(() => validateTtsWav(bad), /必须/);
  assert.throws(() => validateTtsWav(wav({ frames: 8000 * 301, sampleRate: 8000 })), /5 分钟/);
  assert.throws(() => validateTtsRequest({ text: 'a'.repeat(8001), version: 0 }), /8000/);
  assert.throws(() => validateTtsRequest({ text: '你好', version: 0, speed: 0.1 }), /无效/);
});

function jobEnvironment(root, extra = {}) {
  return {
    root,
    allowed: async (path) => {
      const canonical = await realpath(path),
        rel = relative(root, canonical);
      if (rel.startsWith('..') || isAbsolute(rel)) throw Error('outside authorized root');
      return canonical;
    },
    probe: probeContainer,
    commit: async () => ({ state: 'completed', itemId: 'item-test' }),
    ...extra
  };
}
test('browser jobs have one owner, reject stale credentials and preserve saved audio', async (t) => {
  const root = await temporary(t),
    events = [],
    job = new TtsJob(
      validateTtsRequest({ text: '你好', version: 0 }),
      jobEnvironment(root, { emit: (type) => events.push(type) })
    );
  t.after(() => job.cancel());
  const { worker } = job.claim(job.spec.id);
  assert.throws(() => job.claim(job.spec.id), /其他窗口/);
  assert.throws(
    () => job.check('bad', job.spec.id),
    (error) => error.statusCode === 403
  );
  assert.throws(() => job.check(worker, 'stale-job'), /已更新/);
  job.progress(worker, job.spec.id, { progress: 0.4, phase: 'synthesizing', backend: 'wasm' });
  const result = await job.result(
    incoming(wav(), { 'x-tts-backend': 'wasm' }),
    worker,
    job.spec.id
  );
  assert.equal(result.state, 'completed');
  assert.equal(result.backend, 'auto');
  assert.equal(result.actualBackend, 'wasm');
  assert.equal(result.itemId, 'item-test');
  assert.equal(result.asset.kind, 'audio');
  assert.equal(result.asset.hasAudio, true);
  assert.deepEqual(await readFile(result.path), wav());
  assert.ok(events.includes('tts-progress'));
  assert.equal(job.cancel().state, 'completed');
  await assert.rejects(job.result(incoming(wav()), worker, job.spec.id), /已结束/);
});

test('cancel, timeout, invalid results and unsafe generation paths never insert audio', async (t) => {
  const root = await temporary(t);
  let committed = 0;
  const options = jobEnvironment(root, {
    commit: async () => {
      committed++;
      return { state: 'completed' };
    }
  });
  const cancelled = new TtsJob(validateTtsRequest({ text: '取消', version: 0 }), options),
    owner = cancelled.claim(cancelled.spec.id).worker;
  cancelled.cancel();
  await assert.rejects(cancelled.result(incoming(wav()), owner, cancelled.spec.id), /已结束/);
  const invalid = new TtsJob(validateTtsRequest({ text: '无效', version: 0 }), options),
    invalidOwner = invalid.claim(invalid.spec.id).worker;
  await assert.rejects(
    invalid.result(incoming(Buffer.from('invalid wav')), invalidOwner, invalid.spec.id),
    /有效/
  );
  assert.equal(invalid.status().state, 'error');
  const large = new TtsJob(validateTtsRequest({ text: '超限', version: 0 }), options),
    largeOwner = large.claim(large.spec.id).worker;
  await assert.rejects(
    large.result(
      incoming(Buffer.alloc(0), { 'content-length': String(TTS_MAX_WAV_BYTES + 1) }),
      largeOwner,
      large.spec.id
    ),
    /大小/
  );
  large.cancel();
  const timeout = new TtsJob(validateTtsRequest({ text: '超时', version: 0 }), {
    ...options,
    heartbeatMs: 10
  });
  await new Promise((resolve) => setTimeout(resolve, 35));
  assert.equal(timeout.status().state, 'error');
  await symlink(await temporary(t), join(root, '.videocut-generated'));
  const unsafe = new TtsJob(validateTtsRequest({ text: '路径', version: 0 }), options),
    unsafeOwner = unsafe.claim(unsafe.spec.id).worker;
  await assert.rejects(unsafe.result(incoming(wav()), unsafeOwner, unsafe.spec.id), /普通目录/);
  assert.equal(committed, 0);
});

test('session routing uses authoritative undo/redo and retains conflicting generation for import', async (t) => {
  const root = await temporary(t),
    events = [],
    session = {
      id: 'session-test',
      project: createProject(),
      version: 0,
      history: new CommandHistory(),
      clients: new Set([{ write: (value) => events.push(value) }])
    };
  const environment = jobEnvironment(root);
  let response;
  const routes = createTtsRoutes({
    models: { require: async () => {} },
    roots: [root],
    ...environment,
    projectPaths: async (project) => {
      for (const asset of project.assets) await environment.allowed(asset.path);
    },
    notify: () => {},
    snapshot: () => ({ version: session.version, previewUrl: 'http://localhost/?session=test' }),
    body: async (req) => req.data,
    json: (_res, status, data) => {
      response = { status, data };
      return response;
    }
  });
  const call = (action, method, data, headers = {}) =>
    routes({ method, data, headers }, {}, new URL('http://localhost/'), session, action);
  await call('tts', 'POST', { text: '插入配音', version: 0, startSeconds: 2 });
  assert.equal(response.status, 202);
  assert.ok(events.some((event) => event.startsWith('event: tts-request')));
  let job = session.ttsJob,
    owner = job.claim(job.spec.id).worker;
  await job.result(incoming(wav()), owner, job.spec.id);
  assert.equal(session.version, 1);
  assert.equal(session.project.timeline.tracks[0].items[0].placement.begin, 240000);
  const saved = job.status().asset.path;
  let edited = session.history.prepare(session.project, [{ action: 'undo' }]);
  session.history.accept(edited);
  session.project = edited.project;
  session.version++;
  assert.equal(session.project.timeline.tracks.length, 0);
  assert.deepEqual(await readFile(saved), wav());
  edited = session.history.prepare(session.project, [{ action: 'redo' }]);
  session.history.accept(edited);
  session.project = edited.project;
  session.version++;
  assert.equal(session.project.timeline.tracks[0].items.length, 1);
  await call('tts', 'POST', { text: '版本冲突', version: session.version });
  job = session.ttsJob;
  owner = job.claim(job.spec.id).worker;
  session.version++;
  const conflict = await job.result(incoming(wav()), owner, job.spec.id);
  assert.equal(conflict.state, 'conflict');
  assert.equal(session.project.timeline.tracks[0].items.length, 1);
  assert.deepEqual(await readFile(conflict.asset.path), wav());
});

test('unvalidated fp16/q8 requests are rejected before queueing; validated fp32 remains supported', async (t) => {
  const root = await temporary(t),
    required = [];
  const session = {
    id: 'dtype-test',
    project: createProject(),
    version: 0,
    history: new CommandHistory(),
    clients: new Set([{ write() {} }])
  };
  const routes = createTtsRoutes({
    models: {
      require: async (dtype) => {
        required.push(dtype);
      }
    },
    roots: [root],
    ...jobEnvironment(root),
    projectPaths: async () => {},
    notify() {},
    snapshot: () => ({ version: 0, previewUrl: 'http://localhost/' }),
    body: async (req) => req.data,
    json: (_res, status, data) => ({ status, data })
  });
  const queue = (backend, dtype) =>
    routes(
      { method: 'POST', data: { text: '后端兼容', version: 0, dtype, backend } },
      {},
      new URL('http://localhost/'),
      session,
      'tts'
    );
  for (const dtype of ['fp16', 'q8'])
    for (const backend of ['wasm', 'auto', 'webgpu'])
      await assert.rejects(queue(backend, dtype), /仅支持 FP32/);
  assert.deepEqual(required, []);
  assert.equal(session.ttsJob, undefined);
  const wasm = await queue('wasm', 'fp32');
  assert.equal(wasm.status, 202);
  assert.equal(wasm.data.dtype, 'fp32');
  assert.equal(wasm.data.backend, 'wasm');
  assert.deepEqual(required, ['fp32']);
  session.ttsJob.cancel();
});

test('HTTP TTS controls require session authentication and same-origin access; opening a browser never auto-downloads', async (t) => {
  const root = await temporary(t),
    modelDir = join(root, 'models');
  await writeFile(join(root, 'ort-runtime.mjs'), 'export const runtime = "fixture";');
  const server = await startServer({
    roots: [root],
    port: 0,
    ttsModelDir: modelDir,
    staticDir: root
  });
  t.after(() => server.close());
  const client = new VideoCutClient(server.url);
  await client.connect();
  const runtime = await fetch(`${server.url}/ort-runtime.mjs`);
  assert.equal(runtime.status, 200);
  assert.equal(runtime.headers.get('content-type'), 'text/javascript');
  assert.equal(runtime.headers.get('x-content-type-options'), 'nosniff');
  assert.match(await runtime.text(), /export const runtime/);
  assert.equal((await fetch(`${server.url}/api/tts/catalog`)).status, 401);
  assert.equal(
    (await fetch(`${server.url}/api/tts/model/install`, { method: 'POST', body: '{}' })).status,
    401
  );
  const headers = { Authorization: `Bearer ${client.token}`, 'Content-Type': 'application/json' };
  assert.equal(
    (
      await fetch(`${server.url}/api/tts/catalog`, {
        headers: { ...headers, Origin: 'https://evil.example' }
      })
    ).status,
    403
  );
  const session = await client.createSession();
  let response = await fetch(`${server.url}/api/sessions/${session.id}/tts`, {
    method: 'POST',
    headers,
    body: JSON.stringify({ text: '你好', version: 0 })
  });
  assert.equal((await response.json()).code, 'BROWSER_REQUIRED');
  const controller = new AbortController();
  t.after(() => controller.abort());
  const stream = await fetch(client.eventsUrl(session.id), { signal: controller.signal });
  const reader = stream.body.getReader();
  await reader.read();
  response = await fetch(`${server.url}/api/sessions/${session.id}/tts`, {
    method: 'POST',
    headers,
    body: JSON.stringify({ text: '你好', version: 0 })
  });
  const missing = await response.json();
  assert.equal(missing.code, 'TTS_MODEL_REQUIRED');
  assert.equal(missing.previewUrl, session.previewUrl);
  response = await fetch(`${server.url}/api/tts/catalog`, { headers });
  const catalog = await response.json();
  assert.equal(catalog.defaultVoice, 'zf_001');
  assert.deepEqual(catalog.installedDtypes, []);
  assert.equal(catalog.install.state, 'idle');
  assert.equal(
    (await fetch(`${server.url}/tts-models/kokoro-v1.1-zh/voices/not-a-voice.bin`)).status,
    404
  );
  for (const filename of ['model_fp16.onnx', 'model_quantized.onnx'])
    assert.equal(
      (await fetch(`${server.url}/tts-models/kokoro-v1.1-zh/onnx/${filename}`)).status,
      404
    );
  assert.equal(
    (
      await fetch(`${server.url}/tts-models/kokoro-v1.1-zh/config.json`, {
        headers: { Origin: 'https://evil.example' }
      })
    ).status,
    403
  );
  response = await fetch(`${server.url}/api/tts/model/install`, {
    method: 'POST',
    headers,
    body: JSON.stringify({ dtype: 'fp32', voices: ['../../etc/passwd'] })
  });
  assert.equal(response.status, 400);
  controller.abort();
});
