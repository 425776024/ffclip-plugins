import test from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';

const bundled = await build({
  entryPoints: ['src/editor/html-properties.ts'],
  bundle: true,
  platform: 'node',
  format: 'esm',
  write: false,
  logLevel: 'silent'
});
const { parseHtmlProperties, applyHtmlProperties } = await import(
  'data:text/javascript;base64,' + Buffer.from(bundled.outputFiles[0].text).toString('base64')
);
const content = (html, variables) => ({
  html,
  width: 640,
  height: 360,
  duration: 600000,
  transparent: true,
  ...(variables ? { variables } : {})
});

test('browser color validation does not expose unresolved CSS expressions', () => {
  const original = Object.getOwnPropertyDescriptor(globalThis, 'CSS');
  // Browsers defer grammar validation when a value contains CSS variables.
  Object.defineProperty(globalThis, 'CSS', {
    configurable: true,
    value: { supports: (_property, value) => value.includes('var(') || value === '#123456' }
  });
  try {
    const fields = parseHtmlProperties(
      content(
        '<style>.card { background: linear-gradient(90deg, #fff, var(--accent)); color: #123456; fill: rgb(var(--rgb)); }</style>'
      )
    );
    assert.deepEqual(
      fields.map((field) => field.value),
      ['#123456']
    );
  } finally {
    if (original) Object.defineProperty(globalThis, 'CSS', original);
    else delete globalThis.CSS;
  }
});

test('visible mixed text edits are escaped and leave scripts, markup and whitespace intact', () => {
  const script =
    '<script>const title = "Original"; window.tick = t => { /* keep < and > */ };</script>';
  const initial = content(
    `<!doctype html><head><title>Not visible</title></head><body><h1>Original <em>A &amp; B</em>!</h1><div hidden>Hidden</div><template>Template</template>${script}</body>`
  );
  const fields = parseHtmlProperties(initial);
  assert.deepEqual(
    fields.map((field) => field.value),
    ['Original ', 'A & B', '!']
  );
  assert.equal(applyHtmlProperties(initial, fields).html, initial.html);
  fields.find((field) => field.value === 'A & B').value = '<b>用户文字 & 内容</b>';
  const updated = applyHtmlProperties(initial, fields);
  assert.equal(
    updated.html,
    initial.html.replace('A &amp; B', '&lt;b&gt;用户文字 &amp; 内容&lt;/b&gt;')
  );
  assert.ok(updated.html.includes(script));
});

test('CSS source patches retain important, units, selectors and unrelated rules', () => {
  const initial = content(
    `<style>/* untouched */ @media (min-width: 400px) { h1 { font-size: 64px !important; color: #101820e8; font-family: "PingFang SC", sans-serif; background-image: url("data:x;a{b}:c"); } } @keyframes pulse { from { opacity: 0; } to { opacity: 1; } }</style><h1>Title</h1>`
  );
  const fields = parseHtmlProperties(initial);
  const size = fields.find((field) => field.label === '字号');
  assert.equal(size.unit, 'px');
  size.value = 80;
  fields.find((field) => field.kind === 'color').value = '#ffffff88';
  fields.find((field) => field.kind === 'font').value = 'Arial, sans-serif';
  const updated = applyHtmlProperties(initial, fields);
  assert.equal(
    updated.html,
    initial.html
      .replace('64px', '80px')
      .replace('#101820e8', '#ffffff88')
      .replace('"PingFang SC", sans-serif', 'Arial, sans-serif')
  );
  assert.equal(
    fields.filter((field) => field.label === '不透明度').length,
    0,
    'Keyframe values are not editable static styles'
  );
});

test('inline values stay inside quoted attributes and body style blocks are not duplicated', () => {
  const initial = content(
    `<body><style>p { color: red }</style><p style='font-size: 1.5em; font-family: Arial'>Hello</p></body>`
  );
  const fields = parseHtmlProperties(initial);
  assert.equal(fields.filter((field) => field.kind === 'color').length, 1);
  fields.find((field) => field.kind === 'font').value = '"PingFang SC", sans-serif';
  fields.find((field) => field.label === '字号').value = 2;
  const updated = applyHtmlProperties(initial, fields);
  assert.equal(
    updated.html,
    initial.html
      .replace('1.5em', '2em')
      .replace('font-family: Arial', 'font-family: &quot;PingFang SC&quot;, sans-serif')
  );
  assert.equal(
    parseHtmlProperties(updated).find((field) => field.kind === 'font').value,
    '"PingFang SC", sans-serif'
  );
});

test('variables retain primitive types without exposing a JSON editor or changing scripts', () => {
  const initial = content(
    '<script>h1.textContent = window.variables.title</script><h1>Default</h1>',
    { title: '标题', color: '#34d1bf', fontSize: 64, enabled: true }
  );
  const fields = parseHtmlProperties(initial);
  const variables = fields.filter((field) => field.group === 'variable');
  assert.deepEqual(
    variables.map((field) => field.kind),
    ['text', 'color', 'number', 'boolean']
  );
  variables[0].value = '新标题';
  variables[2].value = 72;
  variables[3].value = false;
  const updated = applyHtmlProperties(initial, fields);
  assert.deepEqual(updated.variables, {
    title: '新标题',
    color: '#34d1bf',
    fontSize: 72,
    enabled: false
  });
  assert.equal(updated.html, initial.html);
  assert.equal(initial.variables.title, '标题');
});

test('invalid numbers and CSS injection fail without mutating content', () => {
  const initial = content(
    '<style>h1 { font-size: 24px; font-family: Arial }</style><h1>Title</h1>'
  );
  const fields = parseHtmlProperties(initial);
  const size = fields.find((field) => field.label === '字号');
  size.value = '';
  assert.throws(() => applyHtmlProperties(initial, fields), /有效数字/);
  size.value = size.original;
  fields.find((field) => field.kind === 'font').value = 'Arial; color:red';
  assert.throws(() => applyHtmlProperties(initial, fields), /有效字体/);
  assert.equal(
    initial.html,
    '<style>h1 { font-size: 24px; font-family: Arial }</style><h1>Title</h1>'
  );
});
