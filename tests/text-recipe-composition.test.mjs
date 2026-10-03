import test from 'node:test';
import assert from 'node:assert/strict';
import * as sourceRecipes from '../packages/text-wasm/src/recipes.mjs';
import * as packagedRecipes from '../packages/text-wasm/dist/recipes.mjs';
import {
  createProject,
  addText,
  validateProject,
  editTimeline
} from '../packages/core/project.mjs';

const overlays = [
  { backdrop: 'bubble-tile' },
  { animation: 'anim-lua-letter-transform' },
  { backdrop: 'bubble-nine-slice', animation: 'anim-lua-letter-transform' }
];
const template = (recipe) => ({ id: 'authored-composition', version: 1, recipe });
const combinationError = /只支持 flower-style-03、flower-style-38/;

for (const [name, implementation] of [
  ['source', sourceRecipes],
  ['packaged', packagedRecipes]
]) {
  test(`${name}: cube, printer and external graph overrides fail before loading resources`, async () => {
    for (const base of implementation.TEMPLATE_COMPOSITION_RULES.originalOnlyBases) {
      for (const overlay of overlays) {
        const recipe = { base, ...overlay };
        assert.throws(() => implementation.resolveRecipe(template(recipe)), combinationError);
        let loadCalls = 0;
        await assert.rejects(
          () =>
            implementation.composeRecipe(recipe, async () => {
              loadCalls++;
              throw new Error('An unsupported composition must not reach resource loading');
            }),
          combinationError
        );
        assert.equal(loadCalls, 0);
      }
    }
  });

  test(`${name}: all built-in presets and base-only original graphs remain selectable`, async () => {
    assert.equal(implementation.recipes.length, 30);
    for (const preset of implementation.recipes)
      assert.deepEqual(implementation.resolveRecipe({ id: preset.id, version: 1 }), preset);
    for (const base of implementation.TEMPLATE_COMPOSITION_RULES.originalOnlyBases) {
      const resolved = implementation.resolveRecipe(template({ base }));
      assert.equal(resolved.base, base);
      assert.equal(resolved.backdrop, undefined);
      assert.equal(resolved.animation, undefined);
      const graph = {
        animation: { execution_graph: { nodes: [{ id: `${base}-native-terminal` }] } }
      };
      const original = { composition: {}, animation: graph, effectProgram: {}, assets: new Map() };
      const { bundle, sources, adapted } = await implementation.composeRecipe(
        resolved,
        async () => original
      );
      assert.equal(bundle, original);
      assert.equal(bundle.animation, graph);
      assert.deepEqual(bundle.animation.animation.execution_graph.nodes, [
        { id: `${base}-native-terminal` }
      ]);
      assert.deepEqual(sources, [base]);
      assert.equal(adapted, false);
    }
  });
}

test('document and atomic edit boundaries reject invalid overrides of original template graphs', () => {
  for (const base of sourceRecipes.TEMPLATE_COMPOSITION_RULES.originalOnlyBases) {
    for (const overlay of overlays) {
      const project = createProject();
      const item = addText(project, { content: '原始图', template: template({ base }) });
      item.clip.text.template.recipe = { base, ...overlay };
      assert.throws(() => validateProject(project), combinationError);
      const pristine = createProject();
      const before = JSON.stringify(pristine);
      assert.throws(
        () =>
          editTimeline(pristine, [
            { action: 'configure_project', name: 'Must not publish' },
            {
              action: 'add_text',
              content: '不支持的组合',
              template: template({ base, ...overlay })
            }
          ]),
        combinationError
      );
      assert.equal(JSON.stringify(pristine), before);
    }
  }
});

test('supported flower overlays validate while the built-in descriptors remain unchanged', () => {
  for (const base of sourceRecipes.TEMPLATE_COMPOSITION_RULES.overlayBases) {
    for (const overlay of overlays) {
      const project = createProject();
      const item = addText(project, {
        content: '支持组合',
        template: template({ base, ...overlay })
      });
      assert.equal(validateProject(project), project);
      assert.deepEqual(item.clip.text.template.recipe, { base, ...overlay });
      assert.doesNotThrow(() => sourceRecipes.resolveRecipe(item.clip.text.template));
    }
  }
  for (const preset of sourceRecipes.recipes) {
    const project = createProject();
    const descriptor = { id: preset.id, version: 1 };
    const item = addText(project, { content: preset.text, template: descriptor });
    assert.equal(validateProject(project), project);
    assert.deepEqual(item.clip.text.template, descriptor);
  }
});
