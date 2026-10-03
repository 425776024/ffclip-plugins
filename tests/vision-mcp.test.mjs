import test from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

test(
  'external stdio host discovers read-only vision with UI consent and no auto-download',
  { timeout: 20000 },
  async (t) => {
    const directory = await mkdtemp(join(tmpdir(), 'videocut-vision-mcp-'));
    t.after(() => rm(directory, { recursive: true, force: true }));
    const child = spawn(
      process.execPath,
      [
        resolve('bin/videocut.mjs'),
        '--mcp',
        '--port',
        '0',
        '--root',
        directory,
        '--vision-model-dir',
        join(directory, 'models')
      ],
      {
        env: { ...process.env, PATH: '' },
        stdio: ['pipe', 'pipe', 'pipe']
      }
    );
    t.after(() => child.kill());
    const waiting = new Map(),
      lines = createInterface({ input: child.stdout });
    t.after(() => lines.close());
    let serial = 0;
    lines.on('line', (line) => {
      const result = JSON.parse(line);
      waiting.get(result.id)?.(result);
      waiting.delete(result.id);
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
      const result = await raw(name, args);
      assert.ok(!result.isError, JSON.stringify(result));
      return JSON.parse(result.content[0].text);
    };
    await rpc('initialize', {
      protocolVersion: '2024-11-05',
      capabilities: {},
      clientInfo: { name: 'vision-host', version: '1' }
    });
    const {
      result: { tools }
    } = await rpc('tools/list');
    for (const name of [
      'get_vision_model_status',
      'request_vision_setup',
      'describe_image',
      'describe_video',
      'analyze_media',
      'get_vision_status',
      'cancel_vision',
      'cancel_vision_model_install'
    ])
      assert.ok(
        tools.some((entry) => entry.name === name),
        name
      );
    assert.equal(
      tools.some((entry) => /^(install|enable)_vision/.test(entry.name)),
      false
    );
    const schema = tools.find((entry) => entry.name === 'analyze_media').inputSchema;
    assert.equal(schema.oneOf.length, 3);
    assert.equal(schema.properties.maxFrames.maximum, 32);
    assert.equal(schema.properties.model, undefined);
    const videoSchema = tools.find((entry) => entry.name === 'describe_video').inputSchema;
    assert.deepEqual(videoSchema.required, ['path', 'prompt']);
    assert.equal(videoSchema.properties.maxSegments.maximum, 32);
    assert.equal(videoSchema.properties.framesPerSegment.default, 3);
    assert.equal(videoSchema.properties.waitSeconds.maximum, 60);
    assert.equal((await tool('get_vision_model_status')).consent, 'unasked');
    const requested = await tool('request_vision_setup');
    assert.equal(requested.install.state, 'idle');
    assert.equal(requested.consent, 'unasked');
    assert.ok(requested.setupUrl.startsWith('http://127.0.0.1:'));
    const session = await tool('create_session');
    const missing = await raw('analyze_media', {
      id: session.id,
      version: 0,
      path: join(directory, 'scene.png')
    });
    assert.equal(missing.isError, true);
    assert.equal(missing.structuredContent.code, 'VISION_CONSENT_REQUIRED');
    assert.equal(missing.structuredContent.previewUrl, session.previewUrl);
    for (const name of ['describe_image', 'describe_video']) {
      const description = await raw(name, {
        path: join(directory, 'scene.png'),
        prompt: '用中文描述。'
      });
      assert.equal(description.isError, true);
      assert.equal(description.structuredContent.code, 'VISION_CONSENT_REQUIRED');
      assert.ok(description.structuredContent.previewUrl.startsWith('http://127.0.0.1:'));
    }
    const badWait = await raw('describe_video', {
      path: join(directory, 'scene.mp4'),
      prompt: 'Describe.',
      waitSeconds: 61
    });
    assert.equal(badWait.isError, true);
    assert.ok(badWait.content[0].text.includes('waitSeconds'));
    assert.equal((await tool('get_vision_model_status')).install.state, 'idle');
    assert.equal((await tool('get_vision_status', { id: session.id })).state, 'idle');
    assert.equal((await tool('get_session', { id: session.id })).version, 0);
    assert.equal((await tool('cancel_vision', { id: session.id })).state, 'idle');
    assert.equal((await tool('cancel_vision_model_install')).state, 'idle');
    assert.deepEqual((await rpc('ping')).result, {});
  }
);
