import test from 'node:test';
import assert from 'node:assert/strict';
import { audioPacketTiming, measureAudioDelay } from '../packages/render/audio-timing.mjs';
test('AAC packet timing preserves priming and cuts tail at the authored sample boundary', () => {
  assert.deepEqual(audioPacketTiming(0, 1024, 2112, 144000, 48000), {
    timestamp: -0.044,
    duration: 1024 / 48000
  });
  assert.deepEqual(audioPacketTiming(145408, 1024, 2112, 144000, 48000), {
    timestamp: 143296 / 48000,
    duration: 704 / 48000
  });
  assert.equal(audioPacketTiming(146432, 1024, 2112, 144000, 48000), null);
});
test('AAC probe measures an unknown encoder delay and rejects ambiguous silence', () => {
  const reference = Float32Array.from({ length: 8192 }, (_, i) =>
    i >= 1024 && i < 4096 ? Math.sin(i * i * 0.173) : 0
  );
  const decoded = new Float32Array(11000);
  decoded.set(reference, 1789);
  assert.equal(measureAudioDelay(reference, decoded, 1024, 3072).delay, 1789);
  assert.throws(() => measureAudioDelay(new Float32Array(8192), decoded, 1024, 3072), /校准失败/);
});
