import test from 'node:test';
import assert from 'node:assert/strict';
import {
  createProject,
  addAsset,
  editTimeline,
  ticks,
  EFFECT_TEMPLATES,
  TRANSITION_TEMPLATES
} from '../packages/core/project.mjs';
import { parseCurve, parseCube, IDENTITY_CUBE, LOOK_NAMES } from '../packages/core/color.mjs';
import { colorTransform, cubeTransform, makeColorLut } from '../packages/render/color-lut.mjs';
import { buildRenderPlan } from '../packages/render/plan.mjs';
import { saveWebProject, openWebProject } from '../packages/server/web-project.mjs';
import { mkdtemp, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
const close = (a, b) => a.forEach((v, i) => assert.ok(Math.abs(v - b[i]) < 1e-7, `${a} != ${b}`));
function fixture() {
  const project = createProject();
  const asset = {
    id: 'image',
    name: 'image',
    kind: 'image',
    path: '/image.png',
    size: 10,
    width: 640,
    height: 360,
    duration: ticks(2),
    hasAudio: false
  };
  const from = addAsset(project, asset),
    to = addAsset(project, asset, { trackId: project.timeline.tracks[0].id });
  return { project, from, to };
}
test('neutral grading and identity LUT preserve color; exposure and curves change the intended channels', () => {
  const rgb = [0.13, 0.47, 0.82];
  close(colorTransform()(rgb), rgb);
  close(cubeTransform(IDENTITY_CUBE)(rgb), rgb);
  close(colorTransform({ exposure: 1 })([0.1, 0.2, 0.3]), [0.2, 0.4, 0.6]);
  close(
    colorTransform({ curvered: '[[0,0],[0.5,1],[1,1]]' })([0.25, 0.25, 0.25]),
    [0.5, 0.25, 0.25]
  );
  const gray = colorTransform({ grayscale: 100 })(rgb);
  close(gray, [gray[0], gray[0], gray[0]]);
});
test('cube order and domains are honored; malformed LUTs and curves are rejected', () => {
  const inverted = IDENTITY_CUBE.split('\n')
    .map((line, i) =>
      i
        ? line
            .split(' ')
            .map((v) => 1 - Number(v))
            .join(' ')
        : line
    )
    .join('\n');
  close(cubeTransform(inverted)([0.2, 0.4, 0.6]), [0.8, 0.6, 0.4]);
  close(
    cubeTransform(IDENTITY_CUBE + '\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2')([0.4, 0.8, 1.2]),
    [0.2, 0.4, 0.6]
  );
  for (const source of ['', 'LUT_3D_SIZE 1', 'LUT_3D_SIZE 2\nNaN 1 2', IDENTITY_CUBE + '\n1 1 1'])
    assert.throws(() => parseCube(source));
  for (const points of ['[]', '[[0,0],[0,1],[1,1]]', '[[0,-1],[1,1]]', '[[0.1,0],[1,1]]'])
    assert.throws(() => parseCurve(points));
});
test('every restored filter survives edits and JSON persistence with atomic invalid parameter rejection', () => {
  const { project, from } = fixture();
  const authored = editTimeline(
    project,
    EFFECT_TEMPLATES.map((t) => ({ action: 'add_effect', itemId: from.id, templateId: t.id }))
  ).project;
  const reopened = JSON.parse(JSON.stringify(authored));
  assert.equal(buildRenderPlan(reopened, 0).layers[0].layer.effects.length, 7);
  const grade = reopened.timeline.tracks[0].items[0].clip.effects.find(
    (f) => f.templateId === 'color-grade'
  );
  assert.throws(
    () =>
      editTimeline(reopened, [
        {
          action: 'update_effect',
          itemId: from.id,
          effectId: grade.id,
          parameters: { curvergb: 'bad' }
        }
      ]),
    /曲线/
  );
  assert.equal(grade.parameters.curvergb, '[[0,0],[1,1]]');
  for (const preset of Object.keys(LOOK_NAMES)) {
    const lut = makeColorLut({ templateId: 'looks', parameters: { preset } }, 3);
    assert.equal(lut.data.length, 108);
    assert.ok(lut.data.every(Number.isFinite));
  }
});
test('all 21 transitions use both source handles and retain editable easing and direction', () => {
  assert.equal(TRANSITION_TEMPLATES.length, 21);
  for (const t of TRANSITION_TEMPLATES) {
    const { project, from, to } = fixture();
    const p = editTimeline(project, [
      {
        action: 'add_transition',
        fromItemId: from.id,
        toItemId: to.id,
        templateId: t.id,
        durationSeconds: 0.5
      }
    ]).project;
    const plan = buildRenderPlan(p, from.placement.end);
    assert.equal(plan.layers.length, 1);
    assert.equal(plan.layers[0].kind, 'transition');
    assert.equal(plan.layers[0].transition.progress, 0.5);
    assert.equal(plan.layers[0].from.item.id, from.id);
    assert.equal(plan.layers[0].to.item.id, to.id);
    assert.deepEqual(JSON.parse(JSON.stringify(p)).timeline.transitions, p.timeline.transitions);
  }
});
test('portable project reopen preserves restored color curves, embedded LUTs and creative transitions', async (t) => {
  const root = await mkdtemp(join(tmpdir(), 'videocut-restored-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  const { project, from, to } = fixture();
  const source = join(root, 'image.png');
  await writeFile(source, 'fixture');
  project.assets[0].path = source;
  project.assets[0].size = 7;
  const p = editTimeline(project, [
    {
      action: 'add_effect',
      itemId: from.id,
      templateId: 'color-grade',
      parameters: { exposure: 1, curvered: '[[0,0],[0.5,0.7],[1,1]]' }
    },
    {
      action: 'add_effect',
      itemId: from.id,
      templateId: 'custom-lut',
      parameters: { cube: IDENTITY_CUBE, amount: 0.65 }
    },
    {
      action: 'add_transition',
      fromItemId: from.id,
      toItemId: to.id,
      templateId: 'pageCurl',
      durationSeconds: 0.5,
      parameters: { direction: 'up', easing: 'easeIn' }
    }
  ]).project;
  const destination = join(root, 'restored.vcutweb');
  await saveWebProject(p, destination, resolve('dist/web'));
  const { project: reopened } = await openWebProject(destination);
  assert.deepEqual(reopened.timeline, p.timeline);
  assert.deepEqual(
    buildRenderPlan(reopened, from.placement.end).layers[0].from.effects,
    p.timeline.tracks[0].items[0].clip.effects
  );
});
