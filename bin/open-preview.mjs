import { execFile } from 'node:child_process';
import { promisify } from 'node:util';

export function browserCommand(url, platform = process.platform) {
  const parsed = new URL(url);
  if (parsed.protocol !== 'http:' || parsed.hostname !== '127.0.0.1')
    throw new Error('Only local VideoCut previews can be opened');
  if (platform === 'darwin') return ['open', [url]];
  if (platform === 'win32') return ['rundll32.exe', ['url.dll,FileProtocolHandler', url]];
  return ['xdg-open', [url]];
}

export async function openPreview(url) {
  const [command, args] = browserCommand(url);
  await promisify(execFile)(command, args, { windowsHide: true, timeout: 10000 });
}
