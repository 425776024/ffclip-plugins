import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { loadTextTemplate } from '../dist/index.mjs';
import { fixture } from './helpers.mjs';
test('copied SDK snapshot matches recorded source bytes', async () => {
  const source = JSON.parse(await readFile(new URL('../vendor/SOURCE.json', import.meta.url)));
  for (const [path, sha] of Object.entries(source.files)) {
    assert.equal(createHash('sha256').update(await readFile(new URL('../vendor/videocut/' + path, import.meta.url))).digest('hex'), sha, path);
  }
});
test('reads original native package and validates traversal and digest errors', async () => {
  const bundle = await fixture('com.videocut.text.qt-type.flower-style-09');
  assert.equal(bundle.manifest.format, 'com.videocut.text-template');
  const manifest = structuredClone(bundle.manifest);
  manifest.files = [{ path: '../outside', media_type: 'application/json' }];
  await assert.rejects(loadTextTemplate('https://fixture.invalid/manifest.json', { fetch: async () => Response.json(manifest) }), /Invalid template resource path/);
  manifest.files = [{ path: 'composition.json', digest: 'sha256:bad' }];
  await assert.rejects(loadTextTemplate('https://fixture.invalid/manifest.json', { fetch: async (url) => String(url).endsWith('manifest.json') ? Response.json(manifest) : Response.json({}) }), /digest mismatch/);
});
