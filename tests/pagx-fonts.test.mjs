import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { aliasPagxFont } from '../packages/render/pagx-fonts.mjs';
const tables = (font) => {
  const v = new DataView(font.buffer, font.byteOffset, font.byteLength),
    out = new Map();
  for (let i = 0; i < v.getUint16(4); i++) {
    const p = 12 + i * 16,
      offset = v.getUint32(p + 8),
      length = v.getUint32(p + 12);
    out.set(String.fromCharCode(...font.subarray(p, p + 4)), font.slice(offset, offset + length));
  }
  return out;
};
test('PAGX face aliases preserve glyph tables, source bytes, and the sfnt checksum', async () => {
  const input = new Uint8Array(
    await readFile(new URL('../packages/text-wasm/fixtures/fonts/Inter.ttf', import.meta.url))
  );
  const original = input.slice(),
    output = aliasPagxFont(input, 'VideoCut-bold-face');
  assert.deepEqual(input, original);
  const before = tables(input),
    after = tables(output);
  for (const [tag, data] of before)
    if (!['head', 'name'].includes(tag)) assert.deepEqual(after.get(tag), data, tag);
  const view = new DataView(output.buffer);
  let sum = 0;
  for (let i = 0; i < output.length; i += 4) sum = (sum + view.getUint32(i)) >>> 0;
  assert.equal(sum, 0xb1b0afba, 'A valid, checksummed sfnt is sent to WASM');
  const names = after.get('name'),
    nv = new DataView(names.buffer),
    start = nv.getUint16(4);
  for (let i = 0; i < nv.getUint16(2); i++) {
    const p = 6 + i * 12,
      platform = nv.getUint16(p),
      id = nv.getUint16(p + 6);
    if (![0, 3].includes(platform) || ![1, 4, 6, 16, 21].includes(id)) continue;
    const slice = names.subarray(
      start + nv.getUint16(p + 10),
      start + nv.getUint16(p + 10) + nv.getUint16(p + 8)
    );
    assert.equal(new TextDecoder('utf-16be').decode(slice), 'VideoCut-bold-face');
  }
});
test('PAGX aliases reject invalid fonts without changing the source', () => {
  assert.throws(() => aliasPagxFont(new Uint8Array(12), 'VideoCut-face'));
  assert.throws(() => aliasPagxFont(new Uint8Array(20), '../face'));
});
