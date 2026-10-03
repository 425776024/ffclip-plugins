import test from 'node:test';
import assert from 'node:assert/strict';
import { HtmlFrameCache } from '../packages/server/html-frame-cache.mjs';

test('HTML exact-frame cache is bounded by original compressed bytes and frame count', () => {
  const cache = new HtmlFrameCache({ maximumBytes: 10, maximumFrames: 2 });
  const a = Buffer.alloc(4, 1),
    b = Buffer.alloc(4, 2),
    c = Buffer.alloc(6, 3);
  cache.set('sourceA:time1:full', a);
  cache.set('sourceA:time2:full', b);
  assert.equal(cache.get('sourceA:time1:full'), a);
  cache.set('sourceB:time1:full', c);
  assert.equal(cache.get('sourceA:time2:full'), undefined);
  assert.equal(cache.bytes, 10);
  assert.equal(cache.frames.size, 2);
  cache.set('sourceA:time1:full', Buffer.alloc(2));
  assert.equal(cache.bytes, 8);
  cache.set('oversize', Buffer.alloc(11));
  assert.equal(cache.get('oversize'), undefined);
  assert.equal(cache.bytes, 8);
  cache.clear();
  assert.equal(cache.bytes, 0);
  assert.throws(() => new HtmlFrameCache({ maximumFrames: 0 }), /budgets/);
});
