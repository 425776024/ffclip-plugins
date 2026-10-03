import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, realpath, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { readClipboardFiles, importClipboardMedia } from '../packages/server/clipboard.mjs';
import { createProject, CommandHistory, ticks } from '../packages/core/project.mjs';

test('native clipboard readers keep file order, Unicode and literal shell characters', async () => {
  for (const platform of ['darwin', 'win32']) {
    const paths =
      platform === 'darwin'
        ? ['/tmp/中文 $(touch nope)\nimage.png', '/tmp/second.mov']
        : ['C:\\素材\\image.png', 'C:\\素材\\second.mov'];
    let call;
    const result = await readClipboardFiles({
      platform,
      execute: async (...args) => {
        call = args;
        return JSON.stringify([...paths, paths[0]]);
      }
    });
    assert.deepEqual(result, paths);
    assert.equal(call[2].timeout, 5000);
    assert.equal(call[0], platform === 'darwin' ? '/usr/bin/osascript' : 'powershell.exe');
    // Copied file names never become executable script input.
    for (const path of paths) assert.ok(!call[1].some((arg) => arg.includes(path)));
  }
});

test('Linux file URI clipboard ignores plain text, comments and remote URLs', async () => {
  const calls = [];
  const result = await readClipboardFiles({
    platform: 'linux',
    execute: async (command, args) => {
      calls.push({ command, args });
      if (command === 'wl-paste') throw new Error('not installed');
      return '# comment\r\nfile:///tmp/%E4%B8%AD%E6%96%87%20clip.mp4\r\n/tmp/plain.wav\r\nhttps://example.com/file.mp4\r\nfile://remote-host/a.mp4\r\nfile:///tmp/a%0Ab.png\r\n';
    }
  });
  assert.deepEqual(result, ['/tmp/中文 clip.mp4', '/tmp/a\nb.png']);
  assert.deepEqual(
    calls.map((call) => call.command),
    ['wl-paste', 'xclip']
  );
});

test('native clipboard rejects invalid lists and excessive files, supports empty clipboard', async () => {
  const read = (value) =>
    readClipboardFiles({ platform: 'darwin', execute: async () => JSON.stringify(value) });
  assert.deepEqual(await read([]), []);
  await assert.rejects(read('plain text'), /格式无效/);
  await assert.rejects(read(['/tmp/a\0.png']), /格式无效/);
  await assert.rejects(read(Array.from({ length: 101 }, (_, i) => `/tmp/${i}.png`)), /100/);
});

test('clipboard imports fence paths, skip invalid files and commit a reversible batch', async (t) => {
  const root = await realpath(await mkdtemp(join(tmpdir(), 'videocut-clipboard-')));
  t.after(() => rm(root, { recursive: true, force: true }));
  const first = join(root, '中文 one.png'),
    second = join(root, 'two.png');
  const png = Buffer.from(
    'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a3XcAAAAASUVORK5CYII=',
    'base64'
  );
  await writeFile(first, png);
  await writeFile(second, png);
  await writeFile(join(root, 'notes.txt'), 'plain text');
  const allowed = async (path) => {
    if (!path.startsWith(root + '/')) throw new Error('路径不在启动时授权的目录内');
    return realpath(path);
  };
  const { assets, skipped } = await importClipboardMedia({
    allowed,
    readFiles: async () => [first, first, '/outside/private.png', join(root, 'notes.txt'), second]
  });
  assert.deepEqual(
    assets.map((a) => a.name),
    ['中文 one.png', 'two.png']
  );
  assert.deepEqual(
    skipped.map((s) => s.name),
    ['private.png', 'notes.txt']
  );
  assert.match(skipped[0].error, /授权/);
  assert.match(skipped[1].error, /不支持/);
  const history = new CommandHistory(),
    project = createProject();
  let start = ticks(2);
  const operations = assets.map((asset) => {
    const op = { action: 'add_asset', asset, startSeconds: start / 120000 };
    start += asset.duration;
    return op;
  });
  const batch = history.prepare(project, operations);
  history.accept(batch);
  assert.equal(history.state.undoCount, 1);
  assert.deepEqual(
    batch.project.timeline.tracks.flatMap((track) => track.items.map((i) => i.placement.begin)),
    [ticks(2), ticks(7)]
  );
  const undo = history.prepare(batch.project, [{ action: 'undo' }]);
  history.accept(undo);
  assert.deepEqual(undo.project, project);
  const redo = history.prepare(undo.project, [{ action: 'redo' }]);
  history.accept(redo);
  assert.deepEqual(redo.project, batch.project);
});
