import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { resolve, join } from 'node:path';
export const root = fileURLToPath(new URL('../', import.meta.url));
const workspaceTools = resolve(root, '../../.local/text-wasm-tools');
export const toolsRoot = resolve(process.env.VIDEOCUT_WASM_TOOLS || (existsSync(workspaceTools) ? workspaceTools : join(root, '.cache/tools')));
const workspaceBuild = resolve(root, '../../.local/text-wasm-build');
export const buildRoot = resolve(process.env.VIDEOCUT_WASM_BUILD || (existsSync(workspaceBuild) ? workspaceBuild : join(root, '.cache/build')));
export const lock = { emscripten: '4.0.9', emsdk: '3bcf1dcd01f040f370e10fe673a092d9ed79ebb5', skia: '933b272d039add454e6491efafd2db84a18a92dd' };
export function run(command, args, options = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, { cwd: root, stdio: 'inherit', ...options });
    child.once('error', reject);
    child.once('exit', (code, signal) => code === 0 ? resolve() : reject(new Error(`${command} failed (${code ?? signal})`)));
  });
}
