import test from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { mkdtemp, rm, writeFile, symlink } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
test(
  'MCP exposes conversion and editable PAGX imports with root authorization and undo',
  { timeout: 30000 },
  async (t) => {
    const directory = await mkdtemp(join(tmpdir(), 'videocut-pagx-mcp-'));
    t.after(() => rm(directory, { recursive: true, force: true }));
    const child = spawn(
      process.execPath,
      [
        resolve(process.env.VIDEOCUT_TEST_CLI || 'bin/videocut.mjs'),
        '--mcp',
        '--port',
        '0',
        '--root',
        directory
      ],
      { stdio: ['pipe', 'pipe', 'pipe'] }
    );
    t.after(() => child.kill());
    const waiting = new Map();
    let serial = 0;
    createInterface({ input: child.stdout }).on('line', (line) => {
      const value = JSON.parse(line);
      waiting.get(value.id)?.(value);
      waiting.delete(value.id);
    });
    const rpc = (method, params = {}) =>
      new Promise((resolve) => {
        const id = ++serial;
        waiting.set(id, resolve);
        child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
      });
    const raw = async (name, args = {}) =>
      (await rpc('tools/call', { name, arguments: args })).result;
    const tool = async (name, args = {}) => {
      const r = await raw(name, args);
      assert.ok(!r.isError, r.content[0].text);
      return JSON.parse(r.content[0].text);
    };
    await rpc('initialize', {
      protocolVersion: '2024-11-05',
      capabilities: {},
      clientInfo: { name: 'pagx-test', version: '1' }
    });
    const list = (await rpc('tools/list')).result.tools;
    for (const name of ['add_pagx_clip', 'convert_html_to_pagx'])
      assert.ok(list.some((t) => t.name === name));
    const capabilities = await tool('list_motion_templates');
    assert.equal(capabilities.pagx.enterpriseRequired, false);
    assert.equal(capabilities.templates.length, 10);
    assert.equal(capabilities.defaultAnimationRenderer, 'pagx');
    const converted = await tool('convert_html_to_pagx', {
      html: {
        html: '<style>body{margin:0}div{width:40px;height:40px;background:red;animation:m 1s linear both}@keyframes m{to{transform:translateX(60px)}}</style><div></div>',
        width: 320,
        height: 180,
        duration: 120000,
        transparent: true
      }
    });
    assert.equal(converted.capturedAnimations, 1);
    const created = await tool('create_session');
    const added = await tool('add_pagx_clip', { id: created.id, pagx: converted.pagx });
    assert.equal(added.project.timeline.tracks[0].items[0].clip.type, 'pagx-clip');
    const undone = await tool('edit_timeline', {
      id: created.id,
      version: added.version,
      operations: [{ action: 'undo' }]
    });
    assert.equal(undone.project.timeline.tracks.length, 0);
    const path = join(directory, 'local.pagx');
    await writeFile(path, converted.pagx.xml);
    const imported = await tool('add_pagx_clip', { id: created.id, path });
    assert.equal(imported.project.timeline.tracks[0].items[0].clip.pagx.xml, converted.pagx.xml);
    const nativeSession = await tool('create_session');
    const native = await tool('add_pagx_clip', {
      id: nativeSession.id,
      templateId: 'lower-third',
      locale: 'en'
    });
    assert.equal(native.project.timeline.tracks[0].items[0].clip.type, 'pagx-clip');
    assert.match(native.project.timeline.tracks[0].items[0].clip.pagx.xml, /Alex Chen/);
    const defaultSession = await tool('create_session');
    const defaultResult = await tool('add_html_clip', {
      id: defaultSession.id,
      html: {
        html: '<div style="width:40px;height:30px;background:red"></div>',
        width: 320,
        height: 180,
        duration: 120000,
        transparent: true
      }
    });
    assert.equal(defaultResult.conversion.renderer, 'pagx');
    assert.equal(defaultResult.project.timeline.tracks[0].items[0].clip.type, 'pagx-clip');
    const fallback = await tool('add_html_clip', {
      id: defaultSession.id,
      html: {
        html: '<div style="width:40px;height:30px;background:red;transform:rotateY(30deg)"></div>',
        width: 320,
        height: 180,
        duration: 120000,
        transparent: true
      }
    });
    assert.equal(fallback.conversion.fallback, true);
    assert.equal(fallback.project.timeline.tracks[0].items[0].clip.type, 'html-clip');
    await symlink('/etc/passwd', join(directory, 'outside.png'));
    await writeFile(
      path,
      '<pagx width="100" height="100"><Resources><Image id="a" source="outside.png"/></Resources></pagx>'
    );
    const rejected = await raw('add_pagx_clip', { id: created.id, path });
    assert.equal(rejected.isError, true);
    assert.match(rejected.content[0].text, /授权/);
  }
);
