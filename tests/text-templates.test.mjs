import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { join } from 'node:path';
import {
  createProject,
  addText,
  clone,
  splitItem,
  ticks,
  validateProject,
  TEXT_TEMPLATES,
  assertTextExportSupported
} from '../packages/core/project.mjs';
import { textAssetFiles } from '../scripts/text-assets.mjs';
import { composeRecipe, recipes } from '../packages/text-wasm/src/recipes.mjs';
import { loadTextTemplate } from '../packages/text-wasm/dist/index.mjs';
import { startServer } from '../packages/server/index.mjs';
import { VideoCutClient } from '../packages/client/index.mjs';

test('template authoring survives JSON, splitting, editing and rejects unknown versions', () => {
  for (const template of TEXT_TEMPLATES) {
    const project = createProject();
    const item = addText(project, {
      content: template.text,
      start: ticks(2),
      template: { id: template.id, version: 1 }
    });
    const right = splitItem(project, item.id, ticks(4));
    const restored = validateProject(clone(project));
    assert.deepEqual(restored.timeline.tracks[0].items[1].clip.text.template, {
      id: template.id,
      version: 1
    });
    assert.equal(right.clip.source.begin, ticks(2));
    right.clip.text.content = '新的文字';
    validateProject(project);
    right.clip.text.template.version = 2;
    assert.throws(() => validateProject(project), /模板或版本/);
    right.clip.text.template = { id: '../unknown', version: 1 };
    assert.throws(() => validateProject(project), /模板或版本/);
  }
  const project = createProject();
  assert.throws(
    () => addText(project, { content: '字'.repeat(101), template: { id: 'cube', version: 1 } }),
    /100/
  );
});

test('production resources include the complete native template closure and namespaced backdrop', async () => {
  const read = async (url) => {
    const path = new URL(url).pathname.slice(1);
    assert.ok(textAssetFiles.has(path), `Missing packaged resource: ${path}`);
    const bytes = await readFile(join('dist/web', path));
    assert.deepEqual(bytes, await readFile(textAssetFiles.get(path)));
    return new Response(bytes);
  };
  for (const recipe of recipes) {
    const { bundle } = await composeRecipe(recipe, (id) =>
      loadTextTemplate(
        `http://localhost/text-templates/templates/com.videocut.text.qt-type.${id}/manifest.json`,
        { fetch: read }
      )
    );
    assert.ok(bundle.composition);
    if (recipe.backdrop) {
      assert.ok([...bundle.assets.keys()].some((k) => k.startsWith('backdrop/')));
      assert.ok(bundle.composition.resources.some((r) => r.resource_id.startsWith('backdrop/')));
    }
  }
});

test('complex export requires prepared pixels and an attached browser, not flattened text', async (t) => {
  const project = createProject();
  addText(project, { content: '测试', template: { id: 'cube', version: 1 } });
  assert.doesNotThrow(() => assertTextExportSupported(project));
  const server = await startServer({ port: 0, roots: [process.cwd()] });
  t.after(() => server.close());
  const client = new VideoCutClient(server.url);
  const session = await client.createSession(project);
  await assert.rejects(
    () => client.renderVideo(session.id, session.version, process.cwd()),
    /请打开作品网页/
  );
  project.canvas = { width: 7680, height: 4320 };
  assert.throws(() => assertTextExportSupported(project), /4096/);
});
