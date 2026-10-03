import test from 'node:test';
import assert from 'node:assert/strict';
import { normalizeAacDecoderConfig } from '../packages/render/aac-config.mjs';
import { videoLatencyMode } from '../packages/render/capabilities.mjs';

// Actual Safari 27 AudioEncoder output, 48 kHz stereo AAC-LC.
const appleDescription = Uint8Array.from([
  3, 128, 128, 128, 34, 0, 0, 0, 4, 128, 128, 128, 20, 64, 20, 0, 24, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5,
  128, 128, 128, 2, 17, 144, 6, 128, 128, 128, 1, 2
]);
test('AAC config unwraps the real Apple ES descriptor without changing codec metadata', () => {
  const config = {
    codec: 'mp4a.40.2',
    sampleRate: 48000,
    numberOfChannels: 2,
    description: appleDescription
  };
  const normalized = normalizeAacDecoderConfig(config);
  assert.deepEqual([...normalized.description], [0x11, 0x90]);
  assert.equal(normalized.sampleRate, 48000);
  assert.equal(normalized.numberOfChannels, 2);
  assert.equal(normalized.codec, config.codec);
  assert.equal(config.description, appleDescription);
  const backing = Uint8Array.from([0xff, ...appleDescription, 0xff]);
  assert.deepEqual(
    [...normalizeAacDecoderConfig({ ...config, description: backing.subarray(1, -1) }).description],
    [0x11, 0x90]
  );
  assert.deepEqual(
    [...normalizeAacDecoderConfig({ ...config, description: appleDescription.buffer }).description],
    [0x11, 0x90]
  );
});
test('raw AAC configs and omitted metadata are preserved', () => {
  const config = { codec: 'mp4a.40.2', description: Uint8Array.of(0x11, 0x90) };
  assert.equal(normalizeAacDecoderConfig(config), config);
  assert.equal(normalizeAacDecoderConfig(undefined), undefined);
  const withoutDescription = { codec: 'mp4a.40.2' };
  assert.equal(normalizeAacDecoderConfig(withoutDescription), withoutDescription);
});
test('malformed wrapped AAC configs fail before decoding or muxing', () => {
  for (const description of [
    appleDescription.subarray(0, 30),
    Uint8Array.of(3, 128, 128, 128, 128, 0),
    Uint8Array.of(3, 3, 0, 0, 64),
    Uint8Array.of(3, 3, 0, 0, 0)
  ])
    assert.throws(() => normalizeAacDecoderConfig({ description }), /AAC/);
});
test('Safari AVC avoids the buffered encoder hang while other codec/browser policies stay quality', () => {
  const safari =
    'Mozilla/5.0 (Macintosh) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/27.0 Safari/605.1.15';
  const chrome =
    'Mozilla/5.0 AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36';
  assert.equal(videoLatencyMode('avc', safari), 'realtime');
  assert.equal(videoLatencyMode('vp9', safari), 'quality');
  assert.equal(videoLatencyMode('avc', chrome), 'quality');
  assert.equal(videoLatencyMode('avc', 'Firefox/141.0'), 'quality');
  assert.equal(videoLatencyMode('avc'), 'quality');
});
