import test from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

test(
  'stdio hosts discover fixed Base ASR with no installation/model option or Python dependency',
  { timeout: 20000 },
  async (t) => {
    const directory = await mkdtemp(join(tmpdir(), 'videocut-asr-mcp-'));
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
        '--asr-model-dir',
        join(directory, 'models')
      ],
      { env: { ...process.env, PATH: '' }, stdio: ['pipe', 'pipe', 'pipe'] }
    );
    t.after(() => child.kill());
    const waiting = new Map();
    let serial = 0;
    createInterface({ input: child.stdout }).on('line', (line) => {
      const response = JSON.parse(line);
      waiting.get(response.id)?.(response);
      waiting.delete(response.id);
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
      clientInfo: { name: 'asr-host', version: '1' }
    });
    const {
      result: { tools }
    } = await rpc('tools/list');
    for (const name of [
      'transcribe_speech',
      'get_asr_status',
      'cancel_asr',
      'get_asr_model_status'
    ])
      assert.ok(tools.some((entry) => entry.name === name));
    const schema = tools.find((entry) => entry.name === 'transcribe_speech').inputSchema;
    assert.equal(schema.properties.model, undefined);
    assert.equal(schema.additionalProperties, false);
    assert.equal(
      tools.some((entry) => entry.name === 'install_asr_model'),
      false
    );
    const status = await tool('get_asr_model_status');
    assert.equal(status.model, 'whisper-base');
    assert.equal(status.installed, false);
    assert.equal(status.install.state, 'idle');
    const bytes = Buffer.alloc(44 + 32000);
    bytes.write('RIFF');
    bytes.writeUInt32LE(bytes.length - 8, 4);
    bytes.write('WAVEfmt ', 8);
    bytes.writeUInt32LE(16, 16);
    bytes.writeUInt16LE(1, 20);
    bytes.writeUInt16LE(1, 22);
    bytes.writeUInt32LE(16000, 24);
    bytes.writeUInt32LE(32000, 28);
    bytes.writeUInt16LE(2, 32);
    bytes.writeUInt16LE(16, 34);
    bytes.write('data', 36);
    bytes.writeUInt32LE(32000, 40);
    const path = join(directory, 'silence.wav');
    await writeFile(path, bytes);
    const session = await tool('create_session');
    const added = await tool('add_media', { id: session.id, path });
    const missing = await raw('transcribe_speech', {
      id: session.id,
      version: added.version,
      assetId: added.project.assets[0].id
    });
    assert.equal(missing.isError, true);
    assert.equal(missing.structuredContent.code, 'BROWSER_REQUIRED');
    assert.equal(missing.structuredContent.previewUrl, session.previewUrl);
    assert.equal((await tool('get_asr_model_status')).install.state, 'idle');
    assert.equal((await tool('get_asr_status', { id: session.id })).state, 'idle');
    assert.deepEqual((await rpc('ping')).result, {});
  }
);
