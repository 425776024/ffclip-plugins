import { basename } from 'node:path';
import { fileURLToPath } from 'node:url';
import { run, probe } from './media.mjs';

// Read file references only. Plain text is never interpreted as a local path.
const macScript = `
ObjC.import('AppKit');
const classes = $.NSArray.arrayWithObject($.NSURL.class);
const options = $.NSDictionary.dictionaryWithObjectForKey(true, $.NSPasteboardURLReadingFileURLsOnlyKey);
const urls = $.NSPasteboard.generalPasteboard.readObjectsForClassesOptions(classes, options);
const paths = [];
if (urls) for (let i = 0; i < urls.count; i++) {
  const url = urls.objectAtIndex(i);
  if (url.isFileURL) paths.push(ObjC.unwrap(url.path));
}
JSON.stringify(paths);`;
const windowsScript = `
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
Add-Type -AssemblyName System.Windows.Forms
$files = @([System.Windows.Forms.Clipboard]::GetFileDropList())
ConvertTo-Json -InputObject $files -Compress`;

export async function readClipboardFiles({ platform = process.platform, execute = run } = {}) {
  const options = { timeout: 5000, maxOutput: 512 * 1024 };
  let paths;
  if (platform === 'darwin') {
    paths = JSON.parse(
      await execute('/usr/bin/osascript', ['-l', 'JavaScript', '-e', macScript], options)
    );
  } else if (platform === 'win32') {
    paths = JSON.parse(
      await execute(
        'powershell.exe',
        ['-NoProfile', '-NonInteractive', '-STA', '-Command', windowsScript],
        options
      )
    );
  } else if (platform === 'linux') {
    let text;
    try {
      text = await execute('wl-paste', ['--no-newline', '--type', 'text/uri-list'], options);
    } catch {
      try {
        text = await execute(
          'xclip',
          ['-selection', 'clipboard', '-target', 'text/uri-list', '-o'],
          options
        );
      } catch {
        throw new Error('无法读取文件剪贴板，请复制文件后重试；Linux 需要 wl-paste 或 xclip');
      }
    }
    paths = text.split(/\r?\n/).flatMap((line) => {
      if (!line.startsWith('file:')) return [];
      try {
        return [fileURLToPath(line)];
      } catch {
        return [];
      }
    });
  } else {
    throw new Error('当前系统不支持文件粘贴，请从素材库导入');
  }
  if (
    !Array.isArray(paths) ||
    paths.some((path) => typeof path !== 'string' || !path || path.includes('\0'))
  )
    throw new Error('文件剪贴板格式无效');
  paths = [...new Set(paths)];
  if (paths.length > 100) throw new Error('一次最多粘贴 100 个文件');
  return paths;
}

/** Probe each file through the same root fence as ordinary imports.
 * @param {{allowed: (path: string) => Promise<string>, ffprobe?: string, readFiles?: typeof readClipboardFiles}} options
 * @returns {Promise<{assets: import('../core/types.js').Asset[], skipped: {name: string, error: string}[]}>} */
export async function importClipboardMedia({ allowed, ffprobe, readFiles = readClipboardFiles }) {
  const assets = [],
    skipped = [],
    seen = new Set();
  for (const file of await readFiles()) {
    try {
      const path = await allowed(file);
      if (seen.has(path)) continue;
      seen.add(path);
      assets.push(await probe(path, ffprobe));
    } catch (error) {
      skipped.push({ name: basename(file), error: error.message });
    }
  }
  return { assets, skipped };
}
