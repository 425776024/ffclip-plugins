#!/usr/bin/env node
import { startServer } from '../packages/server/index.mjs';
import { openPreview } from './open-preview.mjs';
import { existsSync } from 'node:fs';
import { readFile } from 'node:fs/promises';

const options = { roots: [] };
let mcp = false;
let open = false;
try {
  const args = process.argv.slice(2);
  for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    if (arg === '--help' || arg === '-h') {
      process.stdout.write(
        'VideoCut — local video editor with portable project saving\n\nUsage: videocut-web [--root DIR] [--port 4318] [--project DIRECTORY.vcutweb|DIRECTORY.vcut] [--demo] [--demo-locale zh|en] [--open] [--native-bridge FILE] [--tts-model-dir DIR] [--asr-model-dir DIR] [--vision-model-dir DIR] [--mcp]\n\n--demo creates the bundled editable starter with HTML animation, styled titles, voiceover and captions; no AI model downloads. --demo-locale defaults to zh.\n--open opens the project preview in your default browser.\n--print-setup-skill prints the bundled initialize-demo SKILL without starting a server.\n--root can be repeated; defaults to the working directory. FFmpeg/FFprobe are optional local fallbacks, never downloaded.\n--mcp runs an MCP stdio adapter and its local preview server.\nPortable .vcutweb projects retain media, editable HTML and flower recipes without a native bridge. Native .vcut uses Project Format 1 and requires a compatible bridge.\n--tts-model-dir sets persistent Kokoro model storage (also VIDEOCUT_TTS_MODEL_DIR). ASR uses fixed Whisper Base, downloaded automatically on first transcription; cache can be set with --asr-model-dir or VIDEOCUT_ASR_MODEL_DIR. Vision uses FastVLM locally in the preview browser; first enable it in the initialization dialog. --vision-model-dir or VIDEOCUT_VISION_MODEL_DIR sets its persistent cache. Browser TTS uses WebGPU/WASM, requires no Python, and installs models only on explicit request.\n'
      );
      process.exit(0);
    }
    if (arg === '--print-setup-skill') {
      const root = new URL(
        existsSync(new URL('../package.json', import.meta.url)) ? '../' : '../../',
        import.meta.url
      );
      process.stdout.write(
        await readFile(
          new URL('plugins/videocut-local/skills/initialize-demo/SKILL.md', root),
          'utf8'
        )
      );
      process.exit(0);
    }
    if (arg === '--demo') {
      options.initialDemo ||= {};
      continue;
    }
    if (arg === '--open') {
      open = true;
      continue;
    }
    if (arg === '--mcp') {
      mcp = true;
      continue;
    }
    if (
      ![
        '--root',
        '--port',
        '--project',
        '--demo-locale',
        '--native-bridge',
        '--tts-model-dir',
        '--asr-model-dir',
        '--vision-model-dir'
      ].includes(arg) ||
      !args[i + 1]
    )
      throw new Error(`Unknown or incomplete option: ${arg}`);
    const value = args[++i];
    if (arg === '--root') options.roots.push(value);
    if (arg === '--port') {
      options.port = Number(value);
      if (!Number.isInteger(options.port) || options.port < 0 || options.port > 65535)
        throw new Error('Invalid port');
    }
    if (arg === '--project') options.initialProjectPath = value;
    if (arg === '--demo-locale') {
      if (!['zh', 'en'].includes(value)) throw new Error('Invalid demo locale; choose zh or en');
      options.initialDemo ||= {};
      options.initialDemo.locale = value;
    }
    if (arg === '--native-bridge') options.nativeBridge = value;
    if (arg === '--vision-model-dir') options.visionModelDir = value;
    if (arg === '--asr-model-dir') options.asrModelDir = value;
    if (arg === '--tts-model-dir') options.ttsModelDir = value;
  }
  if (!options.roots.length) delete options.roots;
  const server = await startServer(options);
  process.stderr.write(`VideoCut: ${server.initialSession?.previewUrl || server.url}\n`);
  if (open)
    await openPreview(server.initialSession?.previewUrl || server.url).catch((error) => {
      process.stderr.write(
        `Could not open the browser: ${error.message}. Open the preview URL above.\n`
      );
    });
  const close = async () => {
    await server.close();
    process.exit(0);
  };
  process.on('SIGINT', close);
  process.on('SIGTERM', close);
  if (mcp) {
    const { serveMcp } = await import('../packages/server/mcp.mjs');
    await serveMcp(server.url, close);
  }
} catch (error) {
  process.stderr.write(`${error.message}\n`);
  process.exitCode = 1;
}
