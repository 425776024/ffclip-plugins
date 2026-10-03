import test from 'node:test';
import assert from 'node:assert/strict';
import { matroskaCodecDelay, readMatroskaCodecDelay } from '../packages/media/container-timing.mjs';

const element = (id, data) =>
  Buffer.concat([Buffer.from(id), Buffer.from([128 + data.length]), data]);
function header(delay = true) {
  const fields = [element([0xd7], Buffer.from([2]))];
  if (delay) fields.push(element([0x56, 0xaa], Buffer.from([0x63, 0x2e, 0xa0]))); // 6,500,000 ns
  const entry = element([0xae], Buffer.concat(fields));
  return Buffer.concat([
    Buffer.from([0x18, 0x53, 0x80, 0x67, 0xff]),
    element([0x16, 0x54, 0xae, 0x6b], entry)
  ]);
}
test('Matroska timing distinguishes declared CodecDelay from explicit negative-PTS convention', () => {
  assert.equal(matroskaCodecDelay(header(), 2), 0.0065);
  assert.equal(matroskaCodecDelay(header(false), 2), 0);
  assert.equal(matroskaCodecDelay(header(), 1), null);
  assert.equal(matroskaCodecDelay(header().subarray(0, 12), 2), null);
  assert.equal(matroskaCodecDelay(new Uint8Array([0, 255]), 2), null);
});
test('Opus timing reads only a bounded response prefix even when server ignores Range', async () => {
  const originalFetch = globalThis.fetch;
  let cancelled = false,
    readCount = 0;
  globalThis.fetch = async (_url, options) => {
    assert.equal(options.headers.Range, 'bytes=0-262143');
    const first = new Uint8Array(262144);
    first.set(header());
    return {
      ok: true,
      body: {
        getReader: () => ({
          async read() {
            readCount++;
            return { done: false, value: first };
          },
          async cancel() {
            cancelled = true;
          }
        })
      }
    };
  };
  try {
    assert.equal(await readMatroskaCodecDelay('http://localhost/media', 2), 0.0065);
    assert.equal(readCount, 1);
    assert.equal(cancelled, true);
  } finally {
    globalThis.fetch = originalFetch;
  }
});
