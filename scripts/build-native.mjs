import { readFile, mkdir } from 'node:fs/promises';
import { resolve, join } from 'node:path';
import { spawn } from 'node:child_process';

// Reuse the exact compiler/includes/libraries of an already-built desktop checkout.
// Never configure, rebuild or modify that checkout from this repository.
const source = process.argv[2] || process.env.VIDEOCUT_SOURCE;
if (!source) throw new Error('Usage: npm run build:native -- /path/to/videoCut [build-directory]');
const build = resolve(process.argv[3] || join(source, 'cmake-build-debug'));
const commands = JSON.parse(await readFile(join(build, 'compile_commands.json'), 'utf8'));
const command = commands.find((c) => c.file.endsWith('/ProjectFormatDocumentCodec.cpp'));
if (!command)
  throw new Error('Build the current VideoCutProjectFormat target in the desktop checkout first');
const tokens = command.command
  .match(/(?:[^\s"']+|"[^"]*"|'[^']*')+/g)
  .map((t) => t.replace(/^['"]|['"]$/g, ''));
const compiler = tokens[0],
  flags = [];
for (let i = 1; i < tokens.length; i++) {
  const t = tokens[i];
  if (/^-(I|D|std=|mmacosx-version-min=)/.test(t)) flags.push(t);
  if (['-isystem', '-isysroot', '-arch'].includes(t)) flags.push(t, tokens[++i]);
}
const ninja = await readFile(join(build, 'build.ninja'), 'utf8');
const line = ninja
  .split('\n')
  .find((l) => /^build bin\/VideoCutClipAnimationPreviewSamples:/.test(l));
if (!line) throw new Error('Desktop build is missing the domain sample link target');
const libraries = line
  .split(' | ')[1]
  .split(' || ')[0]
  .split(/\s+/)
  .filter((p) => p.endsWith('.a'))
  .map((p) => resolve(build, p));
await mkdir('.local/bin', { recursive: true });
const args = [
  ...flags,
  '-I/opt/homebrew/include',
  '-O2',
  resolve('native/videocut_bridge.cpp'),
  resolve('packages/text-wasm/src/package_decoder.cpp'),
  '-o',
  resolve('.local/bin/videocut-bridge'),
  join(build, 'sdk/editor/libVideoCutProjectFormat.a'),
  join(build, 'sdk/editor/libVideoCutEditorApplication.a'),
  join(build, 'libVideoCutFxPreviewCore.a'),
  ...libraries,
  '-lz'
];
const child = spawn(compiler, args, { stdio: 'inherit' });
child.once('error', (e) => {
  throw e;
});
child.once('exit', (code) => {
  process.exitCode = code || 0;
  if (!code) console.log('Built .local/bin/videocut-bridge using the desktop format libraries.');
});
