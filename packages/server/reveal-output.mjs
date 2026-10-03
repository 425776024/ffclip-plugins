import { execFile, spawn } from 'node:child_process';
import { dirname, win32 } from 'node:path';
import { promisify } from 'node:util';

export function revealCommand(path, platform = process.platform) {
  if (platform === 'darwin') return ['open', ['-R', path]];
  if (platform === 'win32') return ['explorer.exe', [`/select,${win32.normalize(path)}`]];
  return ['xdg-open', [dirname(path)]];
}

// The caller must authorize and resolve the completed export before reaching here.
export async function revealOutput(path) {
  const [command, args] = revealCommand(path);
  if (process.platform === 'win32') {
    // Explorer delegates to an existing process and has no reliable exit status.
    await new Promise((resolve, reject) => {
      const child = spawn(command, args, { detached: true, stdio: 'ignore', windowsHide: true });
      child.once('error', reject);
      child.once('spawn', () => {
        child.unref();
        resolve();
      });
    });
  } else {
    await promisify(execFile)(command, args, { timeout: 10000 });
  }
}
