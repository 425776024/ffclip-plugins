import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm, readFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { WhisperModelStore, WHISPER_BASE } from '../packages/server/asr-models.mjs';
import { AsrJob, validateAsrRequest, validateAsrResult } from '../packages/server/asr.mjs';
import {
  createProject,
  addAsset,
  ticks,
  editTimeline,
  CommandHistory
} from '../packages/core/project.mjs';
import {
  audioWindows,
  mixMono16k,
  windowWords,
  subtitleSegments,
  isSilent
} from '../packages/asr/runtime.mjs';

async function temporary(t) {
  const path = await mkdtemp(join(tmpdir(), 'videocut-asr-'));
  t.after(() => rm(path, { recursive: true, force: true }));
  return path;
}
function fixtureResources() {
  const content = Object.fromEntries(
    Object.keys(WHISPER_BASE.files).map((path) => [path, Buffer.from(`asr ${path}`)])
  );
  const files = Object.fromEntries(
    Object.entries(content).map(([path, data]) => [
      path,
      { size: data.length, sha256: createHash('sha256').update(data).digest('hex') }
    ])
  );
  return { content, files };
}
function projectWithSpeech() {
  const project = createProject();
  const asset = {
    id: 'speech',
    name: 'Speech',
    path: '/speech.wav',
    kind: 'audio',
    hasAudio: true,
    duration: ticks(40),
    size: 100,
    width: 0,
    height: 0
  };
  const item = addAsset(project, asset);
  return { project, asset, item };
}

test('Base is fixed; automatic downloads are verified, persisted, reusable and repair corruption', async (t) => {
  const directory = await temporary(t),
    { content, files } = fixtureResources(),
    requests = [];
  const fetchImpl = async (url) => {
    requests.push(url);
    return new Response(content[url.split(`${WHISPER_BASE.revision}/`)[1]]);
  };
  const store = new WhisperModelStore({ directory, files, fetchImpl });
  assert.equal((await store.catalog()).installed, false);
  assert.equal(requests.length, 0);
  assert.equal(await store.ensure(), '/asr-models/whisper-base/');
  assert.equal(requests.length, 7);
  assert.ok(
    requests.every((url) =>
      url.includes(`${WHISPER_BASE.repository}/resolve/${WHISPER_BASE.revision}/`)
    )
  );
  assert.equal((await store.catalog()).model, 'whisper-base');
  const restarted = new WhisperModelStore({
    directory,
    files,
    fetchImpl: () => assert.fail('offline cache must not download')
  });
  await restarted.ensure();
  assert.equal((await restarted.catalog()).installed, true);
  const path = await store.resourcePath('onnx/encoder_model.onnx');
  await writeFile(path, Buffer.alloc(files['onnx/encoder_model.onnx'].size));
  await store.ensure();
  assert.equal(requests.length, 8);
  assert.deepEqual(await readFile(path), content['onnx/encoder_model.onnx']);
});

test('download failure is not ready; retry reuses completed files and cancellation stops an unused download', async (t) => {
  const directory = await temporary(t),
    { content, files } = fixtureResources();
  let corrupt = true;
  const store = new WhisperModelStore({
    directory,
    files,
    fetchImpl: async (url) => {
      const name = url.split(`${WHISPER_BASE.revision}/`)[1];
      return new Response(
        corrupt && name === 'tokenizer.json' ? Buffer.alloc(files[name].size) : content[name]
      );
    }
  });
  await assert.rejects(store.ensure(), /校验/);
  assert.equal(store.status().state, 'error');
  corrupt = false;
  await store.ensure();
  assert.equal(store.status().state, 'ready');
  const other = new WhisperModelStore({
    directory: join(directory, 'cancel'),
    files,
    fetchImpl: async (_url, { signal }) =>
      new Promise((_, reject) =>
        signal.addEventListener('abort', () => reject(signal.reason), { once: true })
      )
  });
  const abort = new AbortController(),
    work = other.ensure({ signal: abort.signal });
  abort.abort();
  await assert.rejects(work, /abort/i);
  assert.equal(other.status().state, 'cancelled');
});

test('concurrent sessions share the download; cancelling one consumer preserves the other', async (t) => {
  const directory = await temporary(t),
    { content, files } = fixtureResources();
  let release;
  const gate = new Promise((resolve) => {
    release = resolve;
  });
  let requests = 0;
  const store = new WhisperModelStore({
    directory,
    files,
    fetchImpl: async (url) => {
      requests++;
      await gate;
      return new Response(content[url.split(`${WHISPER_BASE.revision}/`)[1]]);
    }
  });
  const abort = new AbortController();
  const one = store.ensure({ signal: abort.signal }),
    two = store.ensure();
  abort.abort();
  await assert.rejects(one, /abort/i);
  release();
  await two;
  assert.equal(requests, 7);
  assert.equal(store.status().state, 'ready');
});

test('trimmed and retimed clips map source timestamps to the timeline; invalid inputs fail', () => {
  const { project, item } = projectWithSpeech();
  item.clip.source = { begin: ticks(4), end: ticks(24) };
  item.placement = { begin: ticks(10), end: ticks(20) };
  item.clip.retime = { version: 1, mode: 'constant', constantRatePpm: 2000000 };
  const spec = validateAsrRequest({ itemId: item.id, version: 3, language: 'zh' }, project);
  assert.equal(spec.sourceBegin, 4);
  assert.equal(spec.duration, 20);
  assert.equal(spec.startSeconds, 10);
  const result = validateAsrResult(
    { backend: 'webgpu', text: '你好', segments: [{ text: '你好', start: 2, end: 6 }] },
    spec
  );
  assert.deepEqual(result.segments, [{ text: '你好', start: 11, end: 13 }]);
  for (const data of [
    { assetId: 'speech', itemId: item.id, version: 3 },
    { assetId: 'speech', version: -1 },
    { assetId: 'missing', version: 0 }
  ])
    assert.throws(() => validateAsrRequest(data, project));
  assert.throws(
    () => validateAsrResult({ backend: 'wasm', text: 'x', segments: [] }, spec),
    /时间戳/
  );
  assert.throws(
    () =>
      validateAsrResult(
        { backend: 'wasm', text: 'x', segments: [{ text: 'x', start: 0, end: 30 }] },
        spec
      ),
    /无效/
  );
});

test('subtitle insertion creates one editable track and one undo step; bad timing is atomic', () => {
  const project = createProject(),
    history = new CommandHistory();
  const segments = [
    { text: '你好', start: 1, end: 2 },
    { text: 'VideoCut', start: 3, end: 4 }
  ];
  const edited = history.prepare(project, [{ action: 'add_subtitles', segments }]);
  history.accept(edited);
  assert.equal(edited.project.timeline.tracks.length, 1);
  assert.equal(edited.project.timeline.tracks[0].items.length, 2);
  assert.equal(edited.project.timeline.tracks[0].items[1].placement.begin, ticks(3));
  assert.equal(history.state.undoCount, 1);
  const undo = history.prepare(edited.project, [{ action: 'undo' }]);
  history.accept(undo);
  assert.deepEqual(undo.project, project);
  assert.throws(() =>
    editTimeline(project, [
      { action: 'add_subtitles', segments: [{ text: 'x', start: 2, end: 1 }] }
    ])
  );
  assert.equal(project.timeline.tracks.length, 0);
});

test('browser ownership, cancelled results, silence and version conflicts preserve the document', async (t) => {
  let commits = 0;
  const spec = { duration: 10, startSeconds: 0, rate: 1 };
  const job = new AsrJob(spec, {
    models: {},
    commit: () => {
      commits++;
      return { state: 'conflict' };
    }
  });
  t.after(() => job.cancel());
  const { worker } = job.claim(job.spec.id);
  assert.throws(() => job.claim(job.spec.id), /其他窗口/);
  await assert.rejects(job.textResult('bad', job.spec.id, {}), /凭证/);
  const value = await job.textResult(worker, job.spec.id, {
    backend: 'wasm',
    text: 'speech',
    segments: [{ text: 'speech', start: 0, end: 2 }]
  });
  assert.equal(commits, 1);
  assert.equal(value.state, 'conflict');
  assert.equal(value.segments.length, 1);
  const cancelled = new AsrJob(spec, {
    models: {},
    commit: () => assert.fail('cancelled job must not insert')
  });
  const claim = cancelled.claim(cancelled.spec.id);
  cancelled.cancel();
  await assert.rejects(
    cancelled.textResult(claim.worker, cancelled.spec.id, {
      backend: 'wasm',
      text: '',
      segments: []
    }),
    /结束/
  );
  assert.deepEqual(
    validateAsrResult({ backend: 'wasm', text: '', segments: [] }, spec).segments,
    []
  );
});

test('bounded overlapping audio windows retain real word timing and suppress duplicate boundary words', () => {
  const windows = audioWindows(5, 70);
  assert.ok(windows.every((w) => w.end - w.begin <= 30));
  assert.equal(windows[0].keepEnd, windows[1].keepBegin);
  const output = {
    text: 'one two',
    chunks: [
      { text: 'one', timestamp: [26, 27] },
      { text: ' two', timestamp: [28, 29] }
    ]
  };
  const words = windowWords(output, windows[0], 5);
  assert.deepEqual(words, [{ text: 'one', start: 26, end: 27 }]);
  assert.equal(subtitleSegments(words).length, 1);
  const pcm = new Float32Array(16000),
    mono = new Float32Array(48000).fill(0.5);
  mixMono16k(
    pcm,
    { data: [mono, mono], timestamp: 0, sampleRate: 48000, numberOfFrames: mono.length },
    0
  );
  assert.ok(pcm.every((value) => value === 0.5));
  assert.equal(isSilent(pcm), false);
  assert.equal(isSilent(new Float32Array(16000)), true);
});
