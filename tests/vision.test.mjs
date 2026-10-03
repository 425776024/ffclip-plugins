import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdtemp, rm, readdir, writeFile, realpath, symlink } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { VisionModelStore } from '../packages/server/vision-models.mjs';
import { FASTVLM } from '../packages/server/vision-manifest.mjs';
import {
  VisionJob,
  validateVisionRequest,
  validateVisionResult,
  createVisionRoutes,
  assertVisionSource
} from '../packages/server/vision.mjs';
import { sampleTimes, describeFrame } from '../packages/vision/runtime.mjs';
import { createProject, addAsset, ticks } from '../packages/core/project.mjs';
import { startServer } from '../packages/server/index.mjs';
import { VideoCutClient } from '../packages/client/index.mjs';

async function temporary(t) {
  const directory = await realpath(await mkdtemp(join(tmpdir(), 'videocut-vision-')));
  t.after(() => rm(directory, { recursive: true, force: true }));
  return directory;
}
function resources() {
  const content = Object.fromEntries(
    Object.keys(FASTVLM.files).map((p) => [p, Buffer.from(`vision ${p}`)])
  );
  const files = Object.fromEntries(
    Object.entries(content).map(([p, b]) => [
      p,
      { size: b.length, sha256: createHash('sha256').update(b).digest('hex') }
    ])
  );
  return { content, files };
}
function scene(kind = 'video') {
  const project = createProject();
  const asset = {
    id: 'scene',
    name: 'Scene',
    path: '/scene.mp4',
    kind,
    duration: ticks(40),
    hasAudio: false,
    size: 100,
    width: 320,
    height: 180
  };
  const item = addAsset(project, asset);
  return { project, asset, item };
}

test('vision reads and inference cannot download before consent; preference survives restart', async (t) => {
  const directory = await temporary(t),
    { content, files } = resources(),
    requests = [];
  const fetchImpl = async (url) => {
    requests.push(url);
    return new Response(content[url.split(`${FASTVLM.revision}/`)[1]]);
  };
  const store = new VisionModelStore({ directory, files, fetchImpl });
  assert.equal((await store.catalog()).consent, 'unasked');
  await assert.rejects(store.require(), { code: 'VISION_CONSENT_REQUIRED' });
  await assert.rejects(store.startInstall(), { code: 'VISION_CONSENT_REQUIRED' });
  await store.decide(false);
  const restarted = new VisionModelStore({ directory, files, fetchImpl });
  assert.equal((await restarted.catalog()).consent, 'declined');
  assert.equal(requests.length, 0);
  await restarted.decide(true);
  await restarted.pending;
  await restarted.require();
  assert.equal(requests.length, Object.keys(files).length);
  assert.ok(
    requests.every((url) => url.includes(`${FASTVLM.repository}/resolve/${FASTVLM.revision}/`))
  );
  assert.equal(restarted.status().progress, 1);
  assert.equal(restarted.status().state, 'ready');
  const offline = new VisionModelStore({
    directory,
    files,
    fetchImpl: () => assert.fail('offline inference must not fetch')
  });
  await offline.require();
  assert.equal((await offline.catalog()).installed, true);
  await offline.decide(true);
  await offline.pending;
  await offline.decide(false);
  await assert.rejects(offline.require(), { code: 'VISION_CONSENT_REQUIRED' });
  assert.equal((await offline.catalog()).installed, true, 'disabling preserves verified weights');
});

test('download progress reflects streamed bytes; corruption fails verification and retries reuse files', async (t) => {
  const directory = await temporary(t),
    { content, files } = resources();
  let corrupt = true,
    calls = 0;
  const store = new VisionModelStore({
    directory,
    files,
    fetchImpl: async (url) => {
      calls++;
      const path = url.split(`${FASTVLM.revision}/`)[1];
      return new Response(
        corrupt && path === 'tokenizer.json' ? Buffer.alloc(files[path].size) : content[path]
      );
    }
  });
  await store.decide(true);
  await store.pending;
  assert.equal(store.status().state, 'error');
  await assert.rejects(store.require(), { code: 'VISION_MODEL_REQUIRED' });
  assert.ok(store.status().progress > 0 && store.status().progress < 1);
  assert.ok(
    !(await readdir(await store.root(), { recursive: true })).some((p) => p.includes('.partial-'))
  );
  const initialCalls = calls;
  corrupt = false;
  await store.decide(true);
  await store.pending;
  assert.equal(store.status().state, 'ready');
  assert.equal(calls - initialCalls, Object.keys(files).length - 5);
  const path = await store.resourcePath('onnx/embed_tokens_fp16.onnx');
  await writeFile(path, Buffer.alloc(files['onnx/embed_tokens_fp16.onnx'].size));
  await assert.rejects(store.require(), { code: 'VISION_MODEL_REQUIRED' });
  const beforeRepair = calls;
  await store.decide(true);
  await store.pending;
  assert.equal(calls, beforeRepair + 1);
});

test('cancellation aborts an in-flight stream, removes partials and preserves consent for retry', async (t) => {
  const directory = await temporary(t),
    { files } = resources();
  let started;
  const fetching = new Promise((resolve) => {
    started = resolve;
  });
  const store = new VisionModelStore({
    directory,
    files,
    fetchImpl: async (_url, { signal }) => {
      started();
      return new Response(
        new ReadableStream({
          start(controller) {
            controller.enqueue(new Uint8Array([1]));
            signal.addEventListener('abort', () => controller.error(signal.reason), { once: true });
          }
        })
      );
    }
  });
  await store.decide(true);
  await fetching;
  await store.cancel();
  assert.equal(store.status().state, 'cancelled');
  assert.equal(await store.consent(), 'enabled');
  assert.ok(
    !(await readdir(await store.root(), { recursive: true })).some((p) => p.includes('.partial-'))
  );
});

test('preference and resource symlinks cannot bypass consent or resource verification', async (t) => {
  const directory = await temporary(t),
    { files } = resources();
  const store = new VisionModelStore({ directory, files });
  const external = join(directory, 'enabled.json');
  await writeFile(external, JSON.stringify({ consent: 'enabled' }));
  await symlink(external, await store.consentPath());
  await assert.rejects(store.consent(), /配置无效/);
  await rm(await store.consentPath());
  await symlink(external, await store.resourcePath('config.json', true));
  assert.equal(await store.verifiedFile('config.json'), null);
});

test('sampling stays inside trimmed source ranges and maps actual PTS through clip speed', () => {
  const { project, item } = scene();
  item.clip.source = { begin: ticks(4), end: ticks(24) };
  item.placement = { begin: ticks(10), end: ticks(20) };
  item.clip.retime = { constantRatePpm: 2000000 };
  const spec = validateVisionRequest({ version: 0, itemId: item.id, maxFrames: 2 }, project);
  assert.deepEqual(spec.sampleTimes, [9, 19]);
  const result = validateVisionResult(
    {
      backend: 'wasm',
      frames: [
        { requestedSeconds: 9, sourceSeconds: 8.96, durationSeconds: 0.08, text: 'A cyclist' },
        { requestedSeconds: 19, sourceSeconds: 18.96, durationSeconds: 0.08, text: 'A street' }
      ]
    },
    spec
  );
  assert.equal(result.frames[0].timelineSeconds, 12.48);
  assert.equal(result.sampled, true);
  for (const options of [
    { beginSeconds: 3 },
    { endSeconds: 25 },
    { maxFrames: 33 },
    { maxNewTokens: 513 },
    { prompt: '<image> bad' },
    { assetId: 'scene' }
  ])
    assert.throws(() =>
      validateVisionRequest({ version: 0, itemId: item.id, ...options }, project)
    );
  assert.throws(() =>
    validateVisionResult(
      {
        backend: 'wasm',
        frames: [
          { requestedSeconds: 9, sourceSeconds: 8, durationSeconds: 0.1, text: 'wrong frame' }
        ]
      },
      { ...spec, sampleTimes: [9] }
    )
  );
  assert.deepEqual(sampleTimes(0, 0.001, 1), [0.0005]);
});

test('images run once; path analysis uses a detached asset and does not alter the document', () => {
  const { project, asset } = scene('image');
  const before = structuredClone(project);
  const spec = validateVisionRequest(
    { version: 0, path: asset.path, maxFrames: 32 },
    createProject(),
    asset
  );
  assert.deepEqual(spec.sampleTimes, [0]);
  assert.equal(
    validateVisionResult(
      {
        backend: 'webgpu',
        frames: [{ requestedSeconds: 0, sourceSeconds: 0, durationSeconds: 1, text: 'A picture' }]
      },
      spec
    ).sampled,
    false
  );
  assert.deepEqual(project, before);
});

test('generation uses the image chat template and strips input tokens from output', async () => {
  const calls = [];
  const processor = Object.assign(
    async (image, prompt, options) => {
      assert.equal(image, 'pixels');
      assert.equal(prompt, 'templated');
      assert.equal(options.add_special_tokens, false);
      return { input_ids: { dims: [1, 12] }, pixel_values: 'image tensor' };
    },
    {
      apply_chat_template: (messages, options) => {
        assert.equal(messages[0].content, '<image>\nWhat is visible?');
        assert.equal(options.add_generation_prompt, true);
        return 'templated';
      },
      batch_decode: (tokens, options) => {
        assert.equal(tokens, 'new tokens');
        assert.equal(options.skip_special_tokens, true);
        return ['  A red car.  '];
      }
    }
  );
  const model = {
    generate: async (inputs) => {
      assert.equal(inputs.pixel_values, 'image tensor');
      assert.equal(inputs.max_new_tokens, 64);
      assert.equal(inputs.do_sample, false);
      return {
        slice: (...args) => {
          calls.push(args);
          return 'new tokens';
        }
      };
    }
  };
  assert.equal(
    await describeFrame({
      processor,
      model,
      image: 'pixels',
      prompt: 'What is visible?',
      maxNewTokens: 64
    }),
    'A red car.'
  );
  assert.deepEqual(calls, [[null, [12, null]]]);
});

test('job ownership, cancellation and terminal result fences reject stale workers', () => {
  const { project, asset } = scene('image');
  const spec = validateVisionRequest({ version: 0, assetId: asset.id }, project);
  const events = [],
    job = new VisionJob(spec, { emit: (type) => events.push(type) });
  const claim = job.claim(job.spec.id);
  assert.throws(() => job.claim(job.spec.id), /其他窗口/);
  assert.throws(() => job.textResult('wrong', job.spec.id, {}), /凭证/);
  const result = {
    backend: 'wasm',
    frames: [{ requestedSeconds: 0, sourceSeconds: 0, durationSeconds: 1, text: 'A dog' }]
  };
  assert.equal(job.textResult(claim.worker, job.spec.id, result).state, 'completed');
  assert.throws(() => job.textResult(claim.worker, job.spec.id, result), /已结束/);
  assert.ok(events.every((event) => event.startsWith('vision-')));
  assert.equal(job.cancel().state, 'completed');
  const cancelled = new VisionJob(spec);
  const worker = cancelled.claim(cancelled.spec.id).worker;
  cancelled.cancel();
  assert.throws(() => cancelled.textResult(worker, cancelled.spec.id, result), /已结束/);
});

test('HTTP setup requests cannot approve consent; decline survives a service restart without downloads', async (t) => {
  const directory = await temporary(t),
    visionModelDir = join(directory, 'models');
  let server = await startServer({ roots: [directory], port: 0, visionModelDir });
  t.after(() => server.close());
  const client = new VideoCutClient(server.url);
  await client.connect();
  const session = await client.createSession();
  assert.equal((await client.visionModelStatus()).consent, 'unasked');
  assert.equal((await client.requestVisionSetup()).install.state, 'idle');
  assert.equal((await client.visionModelStatus()).consent, 'unasked');
  await assert.rejects(
    client.analyzeMedia(session.id, { version: 0, path: join(directory, 'x.png') }),
    (error) => error.code === 'VISION_CONSENT_REQUIRED'
  );
  await client.request('/vision/setup', {
    method: 'POST',
    body: JSON.stringify({ enabled: false })
  });
  assert.equal((await client.visionModelStatus()).promptRequested, false);
  await server.close();
  server = await startServer({ roots: [directory], port: 0, visionModelDir });
  const restarted = new VideoCutClient(server.url);
  await restarted.connect();
  assert.equal((await restarted.visionModelStatus()).consent, 'declined');
  assert.equal((await restarted.visionModelStatus()).install.state, 'idle');
});

test('browser job routes complete read-only analysis with a frozen source while edits continue', async () => {
  const { project, asset } = scene('image');
  const session = { project, version: 0, clients: new Set([{ write() {} }]) };
  const routes = createVisionRoutes({
    models: { require: async () => {} },
    snapshot: (s) => ({ version: s.version, previewUrl: 'http://127.0.0.1/projects/scene' }),
    body: async (req) => req.data,
    json: (_res, status, value) => ({ status, value })
  });
  const queued = await routes(
    { method: 'POST', data: { version: 0, assetId: asset.id } },
    {},
    session,
    'vision'
  );
  assert.equal(queued.status, 202);
  const id = queued.value.id;
  const claimed = await routes(
    { method: 'POST', headers: {}, data: { id } },
    {},
    session,
    'vision-job/claim'
  );
  const headers = { 'x-vision-worker': claimed.value.worker };
  session.version = 1;
  session.project = createProject('Edited');
  const completed = await routes(
    {
      method: 'POST',
      headers,
      data: {
        id,
        backend: 'wasm',
        frames: [
          { requestedSeconds: 0, sourceSeconds: 0, durationSeconds: 1, text: 'A still image' }
        ]
      }
    },
    {},
    session,
    'vision-job/result'
  );
  assert.equal(completed.value.state, 'completed');
  assert.equal(completed.value.version, 0);
  assert.equal(session.version, 1);
  assert.equal(session.project.name, 'Edited');
  assert.equal(session.visionJob.spec.path, asset.path);
});

test('changed source identity is rejected rather than describing replacement media', () => {
  const info = { dev: 1, ino: 2, size: 100, mtimeMs: 200 };
  const spec = { sourceIdentity: '1:2:100:200' };
  assert.doesNotThrow(() => assertVisionSource(spec, info));
  assert.throws(() => assertVisionSource(spec, { ...info, mtimeMs: 201 }), {
    code: 'VISION_SOURCE_CHANGED'
  });
});

test('a trimmed still image anchors its observation to the clip rather than subtracting source trim', () => {
  const { project, item } = scene('image');
  item.clip.source = { begin: ticks(4), end: ticks(24) };
  item.placement = { begin: ticks(10), end: ticks(30) };
  const spec = validateVisionRequest({ version: 0, itemId: item.id }, project);
  const result = validateVisionResult(
    {
      backend: 'wasm',
      frames: [{ requestedSeconds: 0, sourceSeconds: 0, durationSeconds: 1, text: 'A still image' }]
    },
    spec
  );
  assert.equal(result.frames[0].timelineSeconds, 10);
});
