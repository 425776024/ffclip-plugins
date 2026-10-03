import test from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

test('stdio hosts discover and call browser TTS without Python or executable discovery', { timeout: 20000 }, async (t) => {
  const directory = await mkdtemp(join(tmpdir(), 'videocut-tts-mcp-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const child = spawn(process.execPath, [resolve('bin/videocut.mjs'), '--mcp', '--port', '0', '--root', directory, '--tts-model-dir', join(directory, 'models')], {
    env: { ...process.env, PATH: '' }, stdio: ['pipe', 'pipe', 'pipe']
  });
  t.after(() => child.kill());
  const waiting = new Map();
  const lines = createInterface({ input: child.stdout });
  t.after(() => lines.close());
  let serial = 0, diagnostics = '';
  child.stderr.on('data', (bytes) => { diagnostics += bytes; });
  lines.on('line', (line) => {
    const result = JSON.parse(line);
    waiting.get(result.id)?.(result);
    waiting.delete(result.id);
  });
  const call = (method, params = {}) => new Promise((resolve) => {
    const id = ++serial;
    waiting.set(id, resolve);
    child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
  });
  const tool = async (name, args = {}) => (await call('tools/call', { name, arguments: args })).result;
  const value = async (name, args = {}) => {
    const result = await tool(name, args);
    assert.ok(!result.isError, JSON.stringify(result));
    return JSON.parse(result.content[0].text);
  };
  const initialized = await call('initialize', { protocolVersion: '2024-11-05', capabilities: {}, clientInfo: { name: 'external-stdio-host', version: '1' } });
  assert.equal(initialized.result.serverInfo.name, '@ffclip-com/videocut');
  const listed = await call('tools/list');
  for (const name of ['list_voices', 'install_tts_model', 'get_tts_model_status', 'cancel_tts_model_install', 'synthesize_speech', 'get_tts_status', 'cancel_tts'])
    assert.ok(listed.result.tools.some((item) => item.name === name), name);
  for (const name of ['install_tts_model', 'synthesize_speech'])
    assert.deepEqual(listed.result.tools.find((item) => item.name === name).inputSchema.properties.dtype.enum, ['fp32']);
  const catalog = await value('list_voices');
  assert.equal(catalog.defaultVoice, 'zf_001');
  assert.ok(catalog.voices.some((voice) => voice.id === 'zf_001' && voice.language === 'zh'));
  assert.deepEqual(catalog.installedDtypes, []);
  assert.equal((await value('get_tts_model_status')).state, 'idle');
  const session = await value('create_session');
  assert.equal((await value('get_tts_status', { id: session.id })).state, 'idle');
  const missing = await tool('synthesize_speech', { id: session.id, version: 0, text: '你好，这是外部宿主调用。' });
  assert.equal(missing.isError, true);
  assert.ok(['BROWSER_REQUIRED', 'TTS_MODEL_REQUIRED'].includes(missing.structuredContent.code), JSON.stringify(missing));
  assert.equal(missing.structuredContent.previewUrl, session.previewUrl);
  const invalid = await tool('install_tts_model', { dtype: 'invalid', voices: ['zf_001'] });
  assert.equal(invalid.isError, true);
  assert.deepEqual((await call('ping')).result, {});
  assert.doesNotMatch(diagnostics, /Python 3 is required|pip install|kokoro_onnx/);
});
