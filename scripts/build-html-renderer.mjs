import { mkdir } from 'node:fs/promises';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
if (process.platform !== 'darwin') throw new Error('The native HTML renderer requires macOS');
const root = fileURLToPath(new URL('../', import.meta.url));
await mkdir(join(root, '.local/bin'), { recursive: true });
const result = spawnSync('xcrun', ['swiftc', '-O', join(root, 'native/html_renderer.swift'), '-o', join(root, '.local/bin/ffclip-html-renderer')], { stdio: 'inherit' });
process.exitCode = result.status ?? 1;
