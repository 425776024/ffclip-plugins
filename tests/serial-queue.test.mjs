import test from 'node:test';
import assert from 'node:assert/strict';
import { createSerialQueue } from '../src/editor/serial-queue.mjs';

test('the next export announcement is retained while the previous finish response is pending', async () => {
  const enqueue = createSerialQueue();
  const events = [];
  let release;
  const finishing = new Promise((resolve) => {
    release = resolve;
  });
  const first = enqueue(async () => {
    events.push('first started');
    await finishing;
    events.push('first cleaned');
  });
  await Promise.resolve();
  const second = enqueue(async () => {
    events.push('second started');
  });
  await Promise.resolve();
  assert.deepEqual(events, ['first started']);
  release();
  await Promise.all([first, second]);
  assert.deepEqual(events, ['first started', 'first cleaned', 'second started']);
});
test('a failed or cancelled export does not discard the following request', async () => {
  const enqueue = createSerialQueue();
  const first = enqueue(async () => {
    throw new Error('cancelled');
  });
  const second = enqueue(async () => 'second completed');
  await assert.rejects(first, /cancelled/);
  assert.equal(await second, 'second completed');
});
