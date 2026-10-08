import test from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { once } from 'node:events';
import { resolve, join } from 'node:path';
import { mkdtemp, readdir, rm, readFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { VideoCutClient } from '../packages/client/index.mjs';

test(
  'packaged CLI speaks MCP, updates live sessions and renders a fresh preview URL',
  { timeout: 15000 },
  async (t) => {
    const directory = await mkdtemp(join(tmpdir(), 'videocut-mcp-'));
    t.after(() => rm(directory, { recursive: true, force: true }));
    const child = spawn(
      process.execPath,
      [
        resolve('bin/videocut.mjs'),
        '--mcp',
        '--port',
        '0',
        '--root',
        process.cwd(),
        '--root',
        directory
      ],
      { stdio: ['pipe', 'pipe', 'pipe'] }
    );
    t.after(() => child.kill());
    let diagnostics = '';
    child.stderr.on('data', (value) => {
      diagnostics += value;
    });
    const waiting = new Map();
    const lines = createInterface({ input: child.stdout });
    lines.on('line', (line) => {
      const response = JSON.parse(line);
      assert.equal(response.jsonrpc, '2.0');
      const resolve = waiting.get(response.id);
      waiting.delete(response.id);
      resolve?.(response);
    });
    let id = 0;
    function call(method, params = {}) {
      return new Promise((resolve) => {
        const request = ++id;
        waiting.set(request, resolve);
        child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id: request, method, params }) + '\n');
      });
    }
    const init = await call('initialize', {
      protocolVersion: '2024-11-05',
      capabilities: {},
      clientInfo: { name: 'test', version: '1' }
    });
    assert.equal(init.result.protocolVersion, '2024-11-05');
    assert.equal(
      init.result.serverInfo.version,
      JSON.parse(await readFile('package.json', 'utf8')).version
    );
    child.stdin.write(
      JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }) + '\n'
    );
    const listed = await call('tools/list');
    assert.ok(listed.result.tools.some((tool) => tool.name === 'add_text'));
    assert.ok(listed.result.tools.some((tool) => tool.name === 'save_project'));
    assert.ok(listed.result.tools.some((tool) => tool.name === 'initialize_demo'));
    const operationSchemas = listed.result.tools.find((tool) => tool.name === 'edit_timeline')
      .inputSchema.properties.operations.items.oneOf;
    const fontSchema = operationSchemas.find(
      (schema) =>
        schema.properties.action.const === 'set_property' &&
        schema.properties.property?.const === 'text.fontSize'
    );
    assert.equal(fontSchema.properties.value.maximum, 1000);
    const opacitySchema = operationSchemas.find(
      (schema) =>
        schema.properties.action.const === 'set_keyframe' &&
        schema.properties.property?.const === 'visual.opacity'
    );
    assert.equal(opacitySchema.properties.value.minimum, 0);
    assert.equal(opacitySchema.properties.value.maximum, 1);
    const blurSchema = operationSchemas.find(
      (schema) =>
        schema.properties.action.const === 'add_effect' &&
        schema.properties.templateId.const === 'blur'
    );
    assert.equal(blurSchema.properties.parameters.properties.radius.type, 'integer');
    assert.equal(blurSchema.properties.parameters.properties.radius.maximum, 64);
    const tool = async (name, args = {}) => {
      const result = (await call('tools/call', { name, arguments: args })).result;
      assert.ok(!result.isError, JSON.stringify(result));
      return JSON.parse(result.content[0].text);
    };
    const session = await tool('create_session', { name: 'Codex 花字作品' });
    assert.equal(
      decodeURIComponent(new URL(session.previewUrl).pathname),
      '/projects/Codex-花字作品'
    );
    assert.equal(session.projectPath, null);
    assert.equal((await tool('list_sessions'))[0].previewUrl, session.previewUrl);
    const updated = await tool('add_text', {
      id: session.id,
      content: 'Agent 实时文字',
      length: 240000
    });
    assert.equal(updated.version, 1);
    assert.equal(updated.project.timeline.tracks[0].items[0].clip.text.content, 'Agent 实时文字');
    assert.equal((await tool('get_session', { id: session.id })).version, 1);
    const edited = await tool('edit_timeline', {
      id: session.id,
      version: 1,
      operations: [
        {
          action: 'set_text',
          itemId: updated.project.timeline.tracks[0].items[0].id,
          content: '剪辑工具同步',
          fontSize: 42
        }
      ]
    });
    assert.equal(edited.version, 2);
    assert.equal(edited.project.timeline.tracks[0].items[0].clip.text.content, '剪辑工具同步');
    const ui = new VideoCutClient(new URL(session.previewUrl).origin);
    assert.equal((await ui.resolveSession(new URL(session.previewUrl).pathname)).id, session.id);
    const uiEdit = await ui.editSession(
      session.id,
      [
        {
          action: 'set_property',
          itemId: updated.project.timeline.tracks[0].items[0].id,
          property: 'text.color',
          value: '#ff0000'
        }
      ],
      edited.version
    );
    const undone = await tool('edit_timeline', {
      id: session.id,
      version: uiEdit.version,
      operations: [{ action: 'undo' }]
    });
    assert.equal(undone.project.timeline.tracks[0].items[0].clip.text.color, '#ffffff');
    assert.equal(undone.history.canRedo, true);
    const redone = await ui.editSession(session.id, [{ action: 'redo' }], undone.version);
    assert.equal(redone.project.timeline.tracks[0].items[0].clip.text.color, '#ff0000');
    const properties = await tool('get_properties', {
      id: session.id,
      itemId: updated.project.timeline.tracks[0].items[0].id
    });
    assert.ok(properties.descriptors['text.content'].impact.includes('layout'));
    const preview = await tool('preview_control', { id: session.id, action: 'play' });
    const starter = await tool('initialize_demo', { locale: 'en' });
    assert.equal(starter.example, 'starter');
    assert.equal(starter.project.timeline.tracks.length, 5);
    assert.equal((await fetch(starter.previewUrl)).status, 200);
    assert.equal((await tool('get_session', { id: session.id })).version, redone.version);
    assert.equal(preview.deliveredTo, 0);
    assert.deepEqual((await tool('get_preview_status', { id: session.id })).clients, []);
    const withoutBrowser = await call('tools/call', {
      name: 'render_video',
      arguments: { id: session.id, version: redone.version, directory }
    });
    assert.equal(withoutBrowser.result.isError, true);
    assert.equal(withoutBrowser.result.structuredContent.code, 'BROWSER_REQUIRED');
    assert.equal(withoutBrowser.result.structuredContent.previewUrl, session.previewUrl);

    // A real event-stream executor claims a job but deliberately does not finish.
    // Status and cancellation must return on the same MCP stdio while render waits.
    const abort = new AbortController();
    t.after(() => abort.abort());
    const events = await fetch(ui.eventsUrl(session.id), { signal: abort.signal });
    const reader = events.body.getReader();
    let renderSettled = false;
    const render = call('tools/call', {
      name: 'render_video',
      arguments: { id: session.id, version: redone.version, directory }
    }).then((result) => {
      renderSettled = true;
      return result;
    });
    let eventBuffer = '';
    while (!eventBuffer.includes('event: render-request')) {
      const { done, value } = await reader.read();
      assert.equal(done, false);
      eventBuffer += new TextDecoder().decode(value);
    }
    const renderEvent = eventBuffer.match(/event: render-request\ndata: ([^\n]+)/);
    assert.ok(renderEvent);
    const job = JSON.parse(renderEvent[1]);
    const claim = await ui.request(
      `/sessions/${session.id}/render-job?job=${job.id}&action=claim`,
      { method: 'POST' }
    );
    await ui.request(`/sessions/${session.id}/render-job?job=${job.id}&action=progress`, {
      method: 'POST',
      headers: { 'x-render-worker': claim.worker },
      body: JSON.stringify({ phase: 'rendering', completed: 3, total: 60 })
    });
    const deadline = (promise) =>
      Promise.race([
        promise,
        new Promise((_, reject) => {
          const timer = setTimeout(
            () => reject(new Error('MCP status/cancel blocked behind render')),
            2000
          );
          timer.unref();
        })
      ]);
    const status = await deadline(tool('get_render_status', { id: session.id }));
    assert.equal(status.phase, 'rendering');
    assert.equal(status.completed, 3);
    assert.equal(renderSettled, false);
    assert.deepEqual((await deadline(call('ping'))).result, {});
    assert.equal((await deadline(tool('cancel_render', { id: session.id }))).cancelled, true);
    const cancelled = await deadline(render);
    assert.equal(cancelled.result.isError, true);
    assert.match(cancelled.result.structuredContent.error, /取消/);
    assert.deepEqual(await readdir(directory), []);
    await reader.cancel();
    abort.abort();
    const blank = await tool('create_session');
    assert.equal(blank.project.timeline.tracks.length, 0);
    const motion = await tool('list_motion_templates');
    assert.ok(motion.parts.base.includes('flower-style-03'));
    assert.deepEqual(motion.nativeComposition.overlayBases, ['flower-style-03', 'flower-style-38']);
    assert.ok(motion.nativeComposition.originalOnlyBases.includes('anim-lua-cube'));
    assert.equal(motion.html.clock, 120000);
    const unsupportedCombination = await call('tools/call', {
      name: 'add_text',
      arguments: {
        id: blank.id,
        content: '不能追加背景的立方体',
        template: {
          id: 'unsafe-cube',
          version: 1,
          recipe: { base: 'anim-lua-cube', backdrop: 'bubble-tile' }
        }
      }
    });
    assert.equal(unsupportedCombination.result.isError, true);
    assert.match(
      unsupportedCombination.result.structuredContent.error,
      /只支持 flower-style-03、flower-style-38/
    );
    assert.equal((await tool('get_session', { id: blank.id })).project.timeline.tracks.length, 0);
    const flower = await tool('add_text', {
      id: blank.id,
      content: '自定义花字',
      length: 240000,
      template: {
        id: 'my-flower',
        version: 1,
        recipe: {
          base: 'flower-style-03',
          backdrop: 'bubble-tile',
          animation: 'anim-lua-letter-transform'
        }
      }
    });
    assert.equal(
      flower.project.timeline.tracks[0].items[0].clip.text.template.recipe.backdrop,
      'bubble-tile'
    );
    const html = await tool('add_html_clip', {
      id: blank.id,
      name: 'Tick graphic',
      renderer: 'html',
      html: {
        html: '<div id="title">HTML</div><script>window.tick=t=>document.getElementById("title").style.opacity=t;</script>',
        width: 320,
        height: 180,
        duration: 240000,
        transparent: true
      }
    });
    assert.equal(html.project.timeline.tracks[0].items[0].clip.type, 'html-clip');
    const htmlId = html.operations[0].itemId;
    const splitHtml = await tool('edit_timeline', {
      id: blank.id,
      version: html.version,
      operations: [{ action: 'split_clip', itemId: htmlId, atSeconds: 1 }]
    });
    assert.equal(splitHtml.project.timeline.tracks[0].items[1].clip.source.begin, 120000);
    const reverseHtml = await tool('edit_timeline', {
      id: blank.id,
      version: splitHtml.version,
      operations: [{ action: 'undo' }]
    });
    assert.equal(reverseHtml.project.timeline.tracks[0].items.length, 1);
    const result = await call('tools/call', {
      name: 'update_session',
      arguments: { id: session.id, project: updated.project, version: 0 }
    });
    assert.equal(result.result.isError, true);
    assert.equal(result.result.structuredContent.version, redone.version);
    assert.equal(
      result.result.structuredContent.project.timeline.tracks[0].items[0].clip.text.color,
      '#ff0000'
    );
    assert.match(diagnostics, /VideoCut: http:\/\/127\.0\.0\.1:/);
    const exited = once(child, 'exit');
    child.stdin.end();
    assert.equal((await exited)[0], 0);
  }
);
