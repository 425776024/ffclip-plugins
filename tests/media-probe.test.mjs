import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, rm, chmod, readFile, access } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { probe, probeContainer } from '../packages/server/media.mjs';
import { containerTrackIndices, jpegOrientation } from '../packages/server/media-metadata.mjs';
import { Input, FilePathSource, ALL_FORMATS } from 'mediabunny';

async function directory(t) {
  const path = await mkdtemp(join(tmpdir(), 'videocut-probe-'));
  t.after(() => rm(path, { recursive: true, force: true }));
  return path;
}
function wav(seconds = 2) {
  const rate = 8000,
    size = rate * seconds * 2,
    bytes = Buffer.alloc(44 + size);
  bytes.write('RIFF', 0);
  bytes.writeUInt32LE(36 + size, 4);
  bytes.write('WAVEfmt ', 8);
  bytes.writeUInt32LE(16, 16);
  bytes.writeUInt16LE(1, 20);
  bytes.writeUInt16LE(1, 22);
  bytes.writeUInt32LE(rate, 24);
  bytes.writeUInt32LE(rate * 2, 28);
  bytes.writeUInt16LE(2, 32);
  bytes.writeUInt16LE(16, 34);
  bytes.write('data', 36);
  bytes.writeUInt32LE(size, 40);
  return bytes;
}
function exifJpeg() {
  const tiff = Buffer.alloc(26);
  tiff.write('II');
  tiff.writeUInt16LE(42, 2);
  tiff.writeUInt32LE(8, 4);
  tiff.writeUInt16LE(1, 8);
  tiff.writeUInt16LE(0x112, 10);
  tiff.writeUInt16LE(3, 12);
  tiff.writeUInt32LE(1, 14);
  tiff.writeUInt16LE(6, 18);
  const exif = Buffer.concat([Buffer.from('Exif\0\0'), tiff]),
    app = Buffer.alloc(4);
  app[0] = 255;
  app[1] = 225;
  app.writeUInt16BE(exif.length + 2, 2);
  const sof = Buffer.from([255, 192, 0, 11, 8, 0, 100, 0, 200, 1, 1, 17, 0]);
  return Buffer.concat([Buffer.from([255, 216]), app, exif, sof, Buffer.from([255, 217])]);
}
test('default metadata probe reads WAV without any external executable', async (t) => {
  const root = await directory(t),
    path = join(root, 'silent.wav');
  await writeFile(path, wav());
  const value = await probe(path, '/definitely/missing/ffprobe');
  assert.equal(value.kind, 'audio');
  assert.equal(value.duration, 240000);
  assert.equal(value.streams[0].sample_rate, 8000);
  assert.equal(value.streams[0].channels, 1);
  assert.equal(value.streams[0].codec_name, 'pcm_s16le');
  assert.match(value.sourceIdentity, /^\d+:\d+:\d+:\d+/);
  const native = await probeContainer(path);
  assert.equal(native.duration, value.duration);
  assert.equal(native.streams[0].index, 0);
});

test('AAC edit-list and Opus preroll remain source zero when browser exports are reimported', async (t) => {
  const directory = resolve('.local/architecture-qa/render-results');
  let fixtures;
  try {
    fixtures = await Promise.all(
      ['aac', 'opus'].map(async (codec) =>
        JSON.parse(await readFile(join(directory, `${codec}-sync-validation.json`), 'utf8'))
      )
    );
    await Promise.all(fixtures.map((fixture) => access(fixture.path)));
  } catch {
    t.skip('Run browser-audio-sync.html for calibrated AAC/Opus export fixtures');
    return;
  }
  for (const fixture of fixtures) {
    const asset = await probe(fixture.path, '/definitely/unavailable/ffprobe');
    const input = new Input({ source: new FilePathSource(fixture.path), formats: ALL_FORMATS });
    try {
      const rawFirst = await input.getFirstTimestamp();
      assert.ok(rawFirst < 0, 'fixture includes actual negative codec preroll');
      const end = await input.computeDuration();
      assert.equal(Math.max(0, rawFirst), 0, 'browser MediaEngine origin normalization');
      assert.equal(end, 3);
      assert.equal(asset.firstTimestamp, 0);
      assert.equal(asset.duration, 360000);
      assert.equal(asset.streams.find((stream) => stream.codec_type === 'video').start_time, '0');
    } finally {
      input.dispose();
    }
  }
});
test('JPEG EXIF rotation changes displayed dimensions while retaining coded dimensions', async (t) => {
  const root = await directory(t),
    path = join(root, 'portrait.jpg'),
    data = exifJpeg();
  await writeFile(path, data);
  assert.equal(jpegOrientation(data), 6);
  const value = await probe(path);
  assert.equal(value.width, 100);
  assert.equal(value.height, 200);
  assert.equal(value.streams[0].width, 200);
  assert.equal(value.streams[0].exif_orientation, 6);
});
test('invalid metadata fails without spawning fallback; an explicit fallback may handle it', async (t) => {
  const root = await directory(t),
    path = join(root, 'legacy.wav');
  await writeFile(path, 'not a WAV file');
  await assert.rejects(probe(path));
  const command = join(root, 'configured-probe'),
    marker = join(root, 'ran');
  await writeFile(
    command,
    `#!/bin/sh\nprintf ran > '${marker}'\nprintf '%s' '{"streams":[{"index":0,"codec_type":"audio","codec_name":"pcm_s16le","sample_rate":"8000","channels":1}],"format":{"duration":"2"}}'\n`
  );
  await chmod(command, 0o700);
  const value = await probe(path, command);
  assert.equal(value.duration, 240000);
  assert.equal(await readFile(marker, 'utf8'), 'ran');
});
function box(type, ...contents) {
  const data = Buffer.concat(contents),
    head = Buffer.alloc(8);
  head.writeUInt32BE(data.length + 8);
  head.write(type, 4);
  return Buffer.concat([head, data]);
}
function trak(id) {
  const header = Buffer.alloc(20);
  header.writeUInt32BE(id, 12);
  return box('trak', box('tkhd', header));
}
test('physical MP4 stream indices preserve subtitle/unknown tracks before AV tracks', async (t) => {
  const root = await directory(t),
    path = join(root, 'tracks.mp4'),
    data = box('moov', trak(91), trak(15), trak(34));
  await writeFile(path, data);
  const indices = await containerTrackIndices(path, '.mp4', data.length);
  assert.deepEqual(
    [...indices],
    [
      [91, 0],
      [15, 1],
      [34, 2]
    ]
  );
});
test('physical Matroska track numbers preserve sparse IDs and non-AV track positions', async (t) => {
  const root = await directory(t),
    path = join(root, 'tracks.mkv');
  const element = (id, data) =>
    Buffer.concat([Buffer.from(id), Buffer.from([0x80 + data.length]), data]);
  const entries = [8, 2, 19].map((id) => element([0xae], element([0xd7], Buffer.from([id]))));
  const tracks = element([0x16, 0x54, 0xae, 0x6b], Buffer.concat(entries)),
    segment = element([0x18, 0x53, 0x80, 0x67], tracks);
  await writeFile(path, segment);
  const indices = await containerTrackIndices(path, '.mkv', segment.length);
  assert.deepEqual(
    [...indices],
    [
      [8, 0],
      [2, 1],
      [19, 2]
    ]
  );
});
test('real 29.97/VFR and one-hour FLAC metadata parse with the configured ffprobe absent', async (t) => {
  const root = resolve('.local/architecture-qa');
  try {
    await access(join(root, 'vfr.mp4'));
    await access(join(root, 'hour-pulses.flac'));
  } catch {
    t.skip('Run tests/prepare-media-fixtures.mjs for real fixture coverage');
    return;
  }
  const [cfr, vfr, audio] = await Promise.all(
    ['timecode.mp4', 'vfr.mp4', 'hour-pulses.flac'].map((name) =>
      probe(join(root, name), '/definitely/missing/ffprobe')
    )
  );
  assert.equal(cfr.streams[0].frame_rate_metrics.frameRateIsConstant, true);
  assert.ok(Math.abs(cfr.streams[0].frame_rate_metrics.averageFrameRate - 30000 / 1001) < 1e-8);
  assert.equal(vfr.streams[0].frame_rate_metrics.frameRateIsConstant, false);
  assert.equal(cfr.streams[0].sample_aspect_ratio, '1:1');
  assert.equal(audio.duration, 432000000);
  assert.equal(audio.streams[0].channels, 2);
});

test('real rotation and subtitle-first MP4/Matroska preserve native stream identities', async (t) => {
  const root = resolve('.local/architecture-qa/probe');
  try {
    await access(join(root, 'subtitle-first.mkv'));
  } catch {
    t.skip('Optional rotation/subtitle fixtures are absent');
    return;
  }
  const rotated = await probe(join(root, 'rotated.mov'), '/no/ffprobe');
  assert.equal(rotated.width, 360);
  assert.equal(rotated.height, 640);
  assert.equal(rotated.streams[0].side_data_list[0].rotation, 90);
  assert.equal(rotated.streams[0].display_rotation, 270);
  for (const name of ['subtitle-first.mp4', 'subtitle-first.mkv']) {
    const value = await probe(join(root, name), '/no/ffprobe');
    assert.deepEqual(
      value.streams.map((stream) => [stream.index, stream.codec_type]),
      [
        [1, 'video'],
        [2, 'audio']
      ]
    );
  }
});
