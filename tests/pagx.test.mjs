import test from 'node:test';
import assert from 'node:assert/strict';
import {
  createProject,
  addPagxClip,
  createPagxContent,
  pagxFields,
  setPagxField,
  validateProject,
  ticks,
  editTimeline,
  evaluateFrame,
  CommandHistory,
  findItem
} from '../packages/core/project.mjs';
const xml =
  '<pagx width="320" height="180"><Layer id="label"><Text text="Title" fontSize="32"/><Fill color="#ff0000"/></Layer><Animations><Animation duration="240" frameRate="60"><Object target="label"><Channel name="x" type="float"><Key time="0" value="0"/><Key time="240" value="100"/></Channel></Object></Animation></Animations></pagx>';

test('PAGX source clock survives trim, speed, split and serialization without fake media or audio', () => {
  const p = createProject(),
    i = addPagxClip(p, { pagx: createPagxContent(xml), start: ticks(1) });
  const edited = editTimeline(p, [
    { action: 'trim_clip', itemId: i.id, sourceInSeconds: 1, durationSeconds: 3 },
    { action: 'set_speed', itemId: i.id, rate: 2 },
    { action: 'split_clip', itemId: i.id, atSeconds: 1.5 }
  ]).project;
  assert.equal(evaluateFrame(edited, ticks(2)).layers[0].sourceTime, ticks(3));
  assert.equal(evaluateFrame(edited, ticks(1.25)).layers[0].sourceTime, ticks(1.5));
  assert.equal(evaluateFrame(edited, ticks(2)).audio.length, 0);
  assert.equal(edited.assets.length, 0);
  assert.deepEqual(validateProject(JSON.parse(JSON.stringify(edited))), edited);
  assert.throws(
    () =>
      editTimeline(p, [
        { action: 'trim_clip', itemId: i.id, sourceInSeconds: 3, durationSeconds: 2 }
      ]),
    /时长/
  );
});

test('PAGX text/paint edits are reversible commands and source payloads are independent', () => {
  let p = createProject();
  const h = new CommandHistory();
  const apply = (ops) => {
    const r = h.prepare(p, ops);
    h.accept(r);
    p = r.project;
    return r;
  };
  const i = apply([{ action: 'add_pagx_clip', pagx: createPagxContent(xml) }]).operations[0].itemId;
  const c = findItem(p, i).item.clip.pagx;
  const f = pagxFields(c.xml).find((f) => f.type === 'text');
  apply([{ action: 'set_pagx_clip', itemId: i, pagx: setPagxField(c, f.key, '新标题 & "文本"') }]);
  assert.match(findItem(p, i).item.clip.pagx.xml, /新标题 &amp;/);
  apply([{ action: 'undo' }]);
  assert.equal(findItem(p, i).item.clip.pagx.xml, xml);
  apply([{ action: 'redo' }]);
  assert.match(findItem(p, i).item.clip.pagx.xml, /新标题/);
});

test('PAGX admission rejects external entities/resources and nondeterministic timelines', () => {
  for (const body of [
    '<!DOCTYPE pagx [<!ENTITY a SYSTEM "file:///etc/passwd">]>' + xml,
    xml.replace('<Text ', '<Image source="https://example.com/a.png"/><Text '),
    xml.replace('<Text ', '<Image source="../a.png"/><Text '),
    xml.replace('<Text ', '<Fill><ImagePattern image="../a.png"/></Fill><Text '),
    xml.replace(
      '<Text ',
      '<Resources><Font file="../a.ttf"/><Font><Glyph image="../a.png" advance="2"/></Font></Resources><Text '
    ),
    xml.replace('<Text ', '<Layer composition="external.pagx"/><Text '),
    xml.replace('<Text ', '<Layer matrix3D="1"/><Text '),
    xml.replace('name="x" type="float"', 'name="point1.x" type="float"'),
    xml.replace('<Text ', '<StateMachine/><Text '),
    xml.replace('<Text ', '<Timelines/><Text '),
    xml.replace('<Text ', '<Layer import="a.svg"/><Text '),
    xml.replace('width="320"', 'width="8000"'),
    xml.replace('<Animation duration=', '<Animation loop="pingPong" duration='),
    xml
      .replace('<Animations>', '<Resources><Composition width="100" height="100"><Animations>')
      .replace('</Animations>', '</Animations></Composition></Resources>'),
    xml.replace('duration="240"', 'duration="NaN"')
  ])
    assert.throws(() => createPagxContent(body));
  assert.throws(() => createPagxContent(xml, { duration: ticks(5) }), /超过/);
});

test('PAGX inspector protects animated layout values and keeps retimed keys on integer frames', async () => {
  const { updatePagxContent, parsePagx } = await import('../packages/core/pagx.mjs');
  const content = createPagxContent(xml.replace('id="label"', 'id="label" left="10" top="20"'));
  const fields = pagxFields(content.xml);
  const left = fields.find((f) => f.key.endsWith(':left'));
  assert.equal(left.animated, true);
  assert.throws(() => updatePagxContent(content, { [left.key]: '25' }), /动画控制/);
  const font = fields.find((f) => f.key.endsWith(':fontSize'));
  assert.throws(() => updatePagxContent(content, { [font.key]: 'NaN' }));
  const updated = updatePagxContent(
    content,
    { [font.key]: '48' },
    { duration: ticks(3.333), width: 640 }
  );
  const doc = parsePagx(updated.xml);
  assert.equal(updated.width, 640);
  assert.equal(doc.getElementsByTagName('Text')[0].getAttribute('fontSize'), '48');
  for (const key of Array.from(doc.getElementsByTagName('Key')))
    assert.ok(Number.isInteger(Number(key.getAttribute('time'))));
  assert.equal(content.width, 320);
});
