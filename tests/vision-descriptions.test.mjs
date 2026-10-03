import test from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { createProject, addAsset, ticks } from '../packages/core/project.mjs';
import { VideoCutClient } from '../packages/client/index.mjs';
import { modelError } from '../packages/server/model-store.mjs';
import {
  createVisionRoutes,
  createVisionDescriptionRoutes,
  validateVisionRequest,
  validateVisionResult
} from '../packages/server/vision.mjs';
import { planVideoSegments, storyboardPrompt } from '../packages/vision/runtime.mjs';

function segmentResult(spec, text = 'A visible scene') {
  return {
    backend: 'wasm',
    segments: spec.segmentPlan.map((segment) => ({
      startSeconds: segment.startSeconds,
      endSeconds: segment.endSeconds,
      text,
      samples: segment.sampleTimes.map((requestedSeconds) => ({
        requestedSeconds,
        sourceSeconds: requestedSeconds - 0.01,
        durationSeconds: 0.04
      }))
    }))
  };
}

test('video intervals cover the whole requested range with bounded chronological samples', () => {
  const { segments, sampling } = planVideoSegments(10, 30, { maxSegments: 2 });
  assert.deepEqual(
    segments.map((s) => [s.startSeconds, s.endSeconds]),
    [
      [10, 20],
      [20, 30]
    ]
  );
  assert.equal(sampling.coarsened, true);
  assert.equal(sampling.totalSamples, 6);
  assert.equal(sampling.intervalSeconds, 10);
  for (const s of segments)
    assert.ok(s.sampleTimes.every((t) => t > s.startSeconds && t < s.endSeconds));
  const long = planVideoSegments(0, 3600, { maxSegments: 32, framesPerSegment: 4 });
  assert.equal(long.sampling.totalSamples, 128);
  assert.equal(long.segments.at(-1).endSeconds, 3600);
  assert.equal(planVideoSegments(0, 0.04).segments.length, 1);
  for (const options of [{ segmentSeconds: 0 }, { maxSegments: 33 }, { framesPerSegment: 5 }])
    assert.throws(() => planVideoSegments(0, 10, options), RangeError);
  assert.throws(() => planVideoSegments(1, 1), RangeError);
  const prompt = storyboardPrompt('用中文返回 JSON。', segments[0]);
  assert.ok(prompt.endsWith('用中文返回 JSON。'));
  assert.ok(prompt.includes('10.000 to 20.000'));
  assert.ok(prompt.includes('3 labeled panels; unlabeled areas are padding'));
});

test('segment output preserves caller format, validates boundaries/PTS and maps trimmed clip time', () => {
  const project = createProject();
  const item = addAsset(project, {
    id: 'video',
    path: '/authorized/video.mp4',
    name: 'Video',
    kind: 'video',
    duration: ticks(40),
    hasAudio: false,
    size: 100,
    width: 320,
    height: 180
  });
  item.clip.source = { begin: ticks(4), end: ticks(24) };
  item.placement = { begin: ticks(10), end: ticks(20) };
  item.clip.retime = { constantRatePpm: 2000000 };
  const spec = validateVisionRequest(
    { version: 0, itemId: item.id, resultFormat: 'segments', maxSegments: 2, prompt: '返回 JSON' },
    project
  );
  const input = segmentResult(spec, '{"scene":"street"}');
  const output = validateVisionResult(input, spec);
  assert.deepEqual(
    output.segments.map((s) => [s.timelineStartSeconds, s.timelineEndSeconds]),
    [
      [10, 15],
      [15, 20]
    ]
  );
  assert.equal(output.segments[0].text, '{"scene":"street"}');
  assert.equal(
    output.text,
    '[4.000–14.000s] {"scene":"street"}\n[14.000–24.000s] {"scene":"street"}'
  );
  for (const corrupt of [
    (data) => {
      data.segments[0].endSeconds += 1;
    },
    (data) => {
      data.segments[0].samples.pop();
    },
    (data) => {
      data.segments[0].samples[0].requestedSeconds += 1;
    },
    (data) => {
      data.segments[0].samples[0].sourceSeconds += 1;
    },
    (data) => {
      data.segments[0].text = '';
    }
  ]) {
    const data = structuredClone(input);
    corrupt(data);
    assert.throws(() => validateVisionResult(data, spec));
  }
  assert.throws(() =>
    validateVisionRequest({ version: 0, itemId: item.id, expectedKind: 'image' }, project)
  );
});

async function fixture(t, enabled = true) {
  const sessions = new Map(),
    pending = [],
    arrived = [];
  const json = (res, status, value) => {
    res.writeHead(status, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(value));
  };
  const body = async (req) => {
    const chunks = [];
    for await (const chunk of req) chunks.push(chunk);
    return JSON.parse(Buffer.concat(chunks).toString());
  };
  let serial = 0;
  const createSession = (project = createProject(), open = false) => {
    const session = {
      id: `session${++serial}`,
      project,
      version: 0,
      clients: new Set(),
      touched: serial
    };
    if (open)
      session.clients.add({
        write(value) {
          if (!value.startsWith('event: vision-request')) return;
          const job = JSON.parse(value.split('data: ')[1]);
          if (pending.length) pending.shift()(job);
          else arrived.push(job);
        }
      });
    sessions.set(session.id, session);
    return session;
  };
  const snapshot = (session) => ({ previewUrl: `http://localhost/projects/${session.id}` });
  const models = {
    async require() {
      if (!enabled) throw modelError('Consent required', 409, 'VISION_CONSENT_REQUIRED');
    }
  };
  const routes = createVisionRoutes({
    models,
    snapshot,
    body,
    json,
    allowed: async (path) => {
      if (!path.startsWith('/authorized/')) throw modelError('Unauthorized path', 403);
      return path;
    },
    probe: async (path) => ({
      id: 'detached',
      path,
      kind: path.endsWith('.png') ? 'image' : 'video',
      duration: ticks(20)
    })
  });
  const describe = createVisionDescriptionRoutes({
    models,
    sessions,
    snapshot,
    createSession,
    body,
    json,
    routes,
    setupUrl: () => 'http://localhost'
  });
  const server = createServer(async (req, res) => {
    try {
      const url = new URL(req.url, 'http://localhost');
      if (url.pathname.startsWith('/api/vision/describe-'))
        return await describe(req, res, url.pathname.endsWith('image') ? 'image' : 'video');
      const match = /^\/api\/sessions\/([^/]+)\/vision-job$/.exec(url.pathname);
      if (!match) return json(res, 404, { error: 'Not found' });
      return await routes(req, res, sessions.get(match[1]), 'vision-job');
    } catch (error) {
      json(res, error.statusCode || 400, { error: error.message, code: error.code });
    }
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  t.after(() => new Promise((resolve) => server.close(resolve)));
  t.after(() => {
    for (const s of sessions.values()) s.visionJob?.cancel();
  });
  const client = new VideoCutClient(`http://127.0.0.1:${server.address().port}`);
  client.token = 'fixture';
  return {
    client,
    sessions,
    createSession,
    nextJob: () =>
      arrived.length
        ? Promise.resolve(arrived.shift())
        : new Promise((resolve) => pending.push(resolve)),
    finish(job, text = '按要求返回的描述') {
      const active = sessions.get(job.sessionId).visionJobs.get(job.id);
      const { worker } = active.claim(job.id);
      const result =
        job.kind === 'image'
          ? {
              backend: 'wasm',
              frames: [{ requestedSeconds: 0, sourceSeconds: 0, durationSeconds: 1, text }]
            }
          : segmentResult(active.spec, text);
      return active.textResult(worker, job.id, result);
    }
  };
}

test('path + prompt functions return image text/video intervals without changing the project', async (t) => {
  const f = await fixture(t),
    old = f.createSession(createProject('Old'), true),
    latest = f.createSession(createProject('Latest'), true);
  const before = structuredClone(latest.project);
  const next = f.nextJob(),
    imageWork = f.client.describeImage('/authorized/image.png', '用中文描述，并返回 JSON。');
  const imageJob = await next;
  assert.equal(imageJob.sessionId, latest.id);
  assert.equal(imageJob.prompt, '用中文描述，并返回 JSON。');
  f.finish(imageJob, '{"内容":"红色物体"}');
  const image = await imageWork;
  assert.equal(image.state, 'completed');
  assert.equal(image.text, '{"内容":"红色物体"}');
  const video = await f.client.describeVideo('/authorized/video.mp4', '描述人物动作。', {
    id: old.id,
    beginSeconds: 10,
    endSeconds: 20,
    segmentSeconds: 5,
    timeoutMs: 0
  });
  assert.equal(video.sessionId, old.id);
  assert.equal(video.state, 'queued');
  assert.equal(video.sampleTimes.length, 6);
  f.finish(video, '人物走过街道。');
  const result = await f.client.waitVision(old.id, video.id);
  assert.deepEqual(
    result.segments.map((s) => [s.startSeconds, s.endSeconds]),
    [
      [10, 15],
      [15, 20]
    ]
  );
  assert.equal(result.text, '[10.000–15.000s] 人物走过街道。\n[15.000–20.000s] 人物走过街道。');
  assert.deepEqual(latest.project, before);
  assert.equal(latest.version, 0);
  await assert.rejects(f.client.describeImage('/authorized/video.mp4', 'Describe.'), /图像路径/);
  await assert.rejects(f.client.describeImage('/other/image.png', 'Describe.'), /Unauthorized/);
});

test('wait deadline retains a pending job; exact-job polling/cancellation cannot affect its successor', async (t) => {
  const f = await fixture(t),
    session = f.createSession(createProject(), true);
  const first = await f.client.describeVideo('/authorized/video.mp4', 'Describe.', {
    timeoutMs: 30
  });
  assert.equal(first.waitTimedOut, true);
  assert.equal(first.state, 'queued');
  f.finish(first);
  const second = await f.client.describeImage('/authorized/image.png', 'Describe.', {
    timeoutMs: 0
  });
  assert.equal((await f.client.visionStatus(session.id, first.id)).state, 'completed');
  assert.equal((await f.client.cancelVision(session.id, first.id)).state, 'completed');
  assert.equal((await f.client.visionStatus(session.id, second.id)).state, 'queued');
  await assert.rejects(
    f.client.visionStatus(session.id, 'missing'),
    (error) => error.code === 'VISION_JOB_NOT_FOUND'
  );
  const abort = new AbortController();
  const waiting = f.client.waitVision(session.id, second.id, { signal: abort.signal });
  abort.abort();
  await assert.rejects(waiting, { name: 'AbortError' });
  assert.equal((await f.client.visionStatus(session.id, second.id)).state, 'cancelled');
  await assert.rejects(
    f.client.describeImage('/authorized/image.png', 'Describe.', { timeoutMs: 60001 }),
    RangeError
  );
});

test('direct functions require explicit consent and reuse a recovery preview when none is open', async (t) => {
  const disabled = await fixture(t, false);
  await assert.rejects(
    disabled.client.describeImage('/authorized/image.png', 'Describe.'),
    (error) => error.code === 'VISION_CONSENT_REQUIRED' && error.previewUrl === 'http://localhost'
  );
  assert.equal(disabled.sessions.size, 0);
  const ready = await fixture(t);
  for (let i = 0; i < 2; i++)
    await assert.rejects(
      ready.client.describeVideo('/authorized/video.mp4', 'Describe.'),
      (error) => error.code === 'BROWSER_REQUIRED' && error.previewUrl.includes('session1')
    );
  assert.equal(ready.sessions.size, 1);
  assert.equal((await ready.client.visionStatus('session1')).state, 'idle');
  await assert.rejects(ready.client.describeImage('/authorized/image.png', ''), /prompt/);
});
