// Interactive MCP host simulator. Uses the installed plugin configuration, not source imports.
// Usage: node tests/helpers/plugin-host.mjs /installed/plugin/.mcp.json /evidence-directory
import { readFile, mkdir, appendFile, writeFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';

const [configPath, evidencePath] = process.argv.slice(2);
const evidence = resolve(evidencePath);
await mkdir(evidence, { recursive: true });
const config = JSON.parse(await readFile(configPath, 'utf8')).mcpServers.videocut;
const child = spawn(config.command, config.args, {
  cwd: config.cwd ? resolve(dirname(resolve(configPath)), config.cwd) : undefined,
  env: { ...process.env, ...config.env },
  stdio: ['pipe', 'pipe', 'pipe']
});
child.stderr.on('data', (data) => process.stderr.write(data));
const requests = new Map();
let counter = 0,
  current,
  journal = Promise.resolve();
const log = (direction, message) => {
  journal = journal.then(() =>
    appendFile(
      join(evidence, 'mcp-transcript.jsonl'),
      JSON.stringify({ at: new Date().toISOString(), direction, message }) + '\n'
    )
  );
};
createInterface({ input: child.stdout }).on('line', (line) => {
  const response = JSON.parse(line);
  log('received', response);
  const pending = requests.get(response.id);
  if (!pending) return;
  clearTimeout(pending.timer);
  requests.delete(response.id);
  if (response.error) pending.reject(new Error(JSON.stringify(response.error)));
  else pending.resolve(response.result);
});
child.on('exit', (code) => {
  for (const { reject, timer } of requests.values()) {
    clearTimeout(timer);
    reject(new Error(`MCP process exited: ${code}`));
  }
});
function rpc(method, params = {}) {
  const id = ++counter;
  const message = { jsonrpc: '2.0', id, method, params };
  log('sent', message);
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      requests.delete(id);
      reject(new Error(`Timed out: ${method}`));
    }, 60000);
    requests.set(id, { resolve, reject, timer });
    child.stdin.write(JSON.stringify(message) + '\n');
  });
}
async function tool(name, args = {}) {
  const result = await rpc('tools/call', { name, arguments: args });
  if (result.isError) throw new Error(result.content[0].text);
  const value = JSON.parse(result.content[0].text);
  if (value.project) {
    current = value;
    await writeFile(join(evidence, 'session.json'), JSON.stringify(value, null, 2));
  }
  return value;
}
function print(value) {
  if (value.project) {
    console.log(
      JSON.stringify({
        id: value.id,
        version: value.version,
        previewUrl: value.previewUrl,
        name: value.project.name,
        canvas: value.project.canvas,
        operations: value.operations,
        tracks: value.project.timeline.tracks.map((t) => ({
          id: t.id,
          type: t.type,
          items: t.items.map((i) => ({
            id: i.id,
            name: i.name,
            type: i.clip.type,
            source: i.clip.source,
            placement: i.placement,
            text: i.clip.text
          }))
        }))
      })
    );
  } else console.log(JSON.stringify(value));
}
print(
  await rpc('initialize', {
    protocolVersion: '2024-11-05',
    capabilities: {},
    clientInfo: { name: 'codex-installed-plugin-simulation', version: '1' }
  })
);
child.stdin.write(JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }) + '\n');
const listed = await rpc('tools/list');
print({
  tools: listed.tools.map((t) => t.name),
  installedConfig: configPath,
  runtime: config.args[0]
});
print(await tool('create_session'));
for await (const line of createInterface({ input: process.stdin })) {
  try {
    const request = JSON.parse(line);
    if (request.observeSeconds) {
      const samples = [];
      const until = Date.now() + Math.min(request.observeSeconds, 15) * 1000;
      while (Date.now() < until) {
        samples.push(await tool('get_preview_status', { id: current.id }));
        await new Promise((resolve) => setTimeout(resolve, 750));
      }
      await writeFile(
        join(evidence, `playback-${Date.now()}.json`),
        JSON.stringify(samples, null, 2)
      );
      print({ samples });
      continue;
    }
    const args = request.arguments || {};
    if (!['create_session', 'list_files', 'list_voices', 'install_tts_model', 'get_tts_model_status', 'cancel_tts_model_install'].includes(request.name)) args.id ??= current.id;
    if (
      ['edit_timeline', 'update_session', 'export_project', 'render_video', 'synthesize_speech'].includes(request.name)
    )
      args.version ??= current.version;
    print(await tool(request.name, args));
  } catch (error) {
    print({ error: error.message });
  }
}
await journal;
child.stdin.end();
