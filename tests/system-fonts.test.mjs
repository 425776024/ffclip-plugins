import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile, rm, symlink } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import {
  createSystemFontService,
  standardFontDirectories
} from '../packages/server/system-fonts.mjs';

async function temporary(t) {
  const root = await mkdtemp(join(tmpdir(), 'videocut-system-font-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  return root;
}

function sfnt(family, subfamily = 'Regular', weight = 400) {
  const records = [
    [1, family],
    [2, subfamily],
    [6, `${family.replaceAll(' ', '')}-${subfamily}`]
  ];
  const strings = records.map(([, value]) => Buffer.from(value, 'utf16le').swap16());
  const names = Buffer.alloc(6 + records.length * 12 + strings.reduce((n, s) => n + s.length, 0));
  names.writeUInt16BE(records.length, 2);
  names.writeUInt16BE(6 + records.length * 12, 4);
  let offset = 0;
  records.forEach(([id], i) => {
    const p = 6 + i * 12;
    names.writeUInt16BE(3, p);
    names.writeUInt16BE(1, p + 2);
    names.writeUInt16BE(0x409, p + 4);
    names.writeUInt16BE(id, p + 6);
    names.writeUInt16BE(strings[i].length, p + 8);
    names.writeUInt16BE(offset, p + 10);
    strings[i].copy(names, 6 + records.length * 12 + offset);
    offset += strings[i].length;
  });
  const os2 = Buffer.alloc(64);
  os2.writeUInt16BE(weight, 4);
  os2.writeUInt16BE(5, 6);
  const tables = [
    ['OS/2', os2],
    ['head', Buffer.alloc(54)],
    ['name', names]
  ];
  offset = 12 + tables.length * 16;
  const result = Buffer.alloc(
    offset + tables.reduce((n, [, b]) => n + Math.ceil(b.length / 4) * 4, 0)
  );
  result.writeUInt32BE(0x00010000);
  result.writeUInt16BE(tables.length, 4);
  tables.forEach(([tag, bytes], i) => {
    const p = 12 + i * 16;
    result.write(tag, p, 4, 'ascii');
    result.writeUInt32BE(offset, p + 8);
    result.writeUInt32BE(bytes.length, p + 12);
    bytes.copy(result, offset);
    offset += Math.ceil(bytes.length / 4) * 4;
  });
  return result;
}

function ttc(...faces) {
  const header = Buffer.alloc(12 + faces.length * 4);
  header.write('ttcf');
  header.writeUInt32BE(0x00010000, 4);
  header.writeUInt32BE(faces.length, 8);
  let base = header.length;
  faces = faces.map((face, index) => {
    const copy = Buffer.from(face);
    header.writeUInt32BE(base, 12 + index * 4);
    for (let i = 0; i < copy.readUInt16BE(4); i++) {
      const p = 12 + i * 16 + 8;
      copy.writeUInt32BE(copy.readUInt32BE(p) + base, p);
    }
    base += copy.length;
    return copy;
  });
  return Buffer.concat([header, ...faces]);
}

function checksum(bytes) {
  const padded = Buffer.alloc(Math.ceil(bytes.length / 4) * 4);
  bytes.copy(padded);
  let sum = 0;
  for (let i = 0; i < padded.length; i += 4) sum = (sum + padded.readUInt32BE(i)) >>> 0;
  return sum;
}

test('standard font directories cover OS and user locations without external commands', () => {
  assert.ok(standardFontDirectories('darwin', '/user').includes('/user/Library/Fonts'));
  assert.ok(
    standardFontDirectories('darwin').includes(
      '/System/Library/AssetsV2/com_apple_MobileAsset_Font8'
    )
  );
  assert.deepEqual(
    standardFontDirectories('win32', 'D:\\User', {
      WINDIR: 'C:\\Windows',
      LOCALAPPDATA: 'D:\\User\\AppData\\Local'
    }),
    ['C:\\Windows\\Fonts', 'D:\\User\\AppData\\Local\\Microsoft\\Windows\\Fonts']
  );
  assert.ok(standardFontDirectories('linux', '/user').includes('/user/.local/share/fonts'));
});

test('catalog selects actual regular families and TTC returns one checksummed standalone face', async (t) => {
  const root = await temporary(t);
  await writeFile(join(root, 'arial.ttf'), sfnt('Arial'));
  await writeFile(
    join(root, 'cjk.ttc'),
    ttc(sfnt('PingFang SC', 'Bold', 700), sfnt('PingFang SC'))
  );
  const service = createSystemFontService({ roots: [root], platform: 'darwin' });
  const catalog = await service.catalog();
  assert.equal(catalog.fonts.length, 3);
  const sans = catalog.fonts.find((f) => f.id === catalog.defaults.sans);
  const cjk = catalog.fonts.find((f) => f.id === catalog.defaults.cjk);
  assert.equal(sans.family, 'Arial');
  assert.equal(cjk.family, 'PingFang SC');
  assert.equal(cjk.postscriptName, 'PingFangSC-Regular');
  assert.equal(cjk.weight, 400);
  assert.equal(cjk.width, 5);
  assert.equal(cjk.faceIndex, 0);
  assert.equal(cjk.sourceFaceIndex, 1);
  assert.ok(cjk.bytes < cjk.sourceBytes);
  assert.ok(!JSON.stringify(catalog).includes(root));
  const [a, b] = await Promise.all([service.read(cjk.id), service.read(cjk.id)]);
  assert.equal(a, b);
  assert.equal(a.bytes.length, cjk.bytes);
  assert.equal(a.bytes.readUInt32BE(0), 0x00010000);
  assert.equal(checksum(a.bytes), 0xb1b0afba);
  for (let i = 0; i < a.bytes.readUInt16BE(4); i++) {
    const p = 12 + i * 16;
    const tag = a.bytes.toString('ascii', p, p + 4);
    const offset = a.bytes.readUInt32BE(p + 8),
      length = a.bytes.readUInt32BE(p + 12);
    assert.ok(offset + length <= a.bytes.length);
    assert.equal(offset % 4, 0);
    const table = Buffer.from(a.bytes.subarray(offset, offset + length));
    if (tag === 'head') table.writeUInt32BE(0, 8);
    assert.equal(checksum(table), a.bytes.readUInt32BE(p + 4));
  }
  assert.match(a.etag, /^"[0-9a-f]{64}"$/);
  assert.equal(a.mime, 'font/ttf');
});

test('read rejects arbitrary paths, unknown IDs, outside symlinks and modified registered files', async (t) => {
  const root = await temporary(t),
    fonts = join(root, 'fonts');
  await mkdir(fonts);
  const path = join(fonts, 'font.ttf');
  await writeFile(path, sfnt('Arial'));
  await writeFile(join(root, 'outside.ttf'), sfnt('PingFang SC'));
  await symlink(join(root, 'outside.ttf'), join(fonts, 'outside.ttf'));
  const service = createSystemFontService({ roots: [fonts] });
  const catalog = await service.catalog();
  assert.equal(catalog.fonts.length, 1);
  assert.equal(catalog.defaults.cjk, null);
  await assert.rejects(service.read(path), { statusCode: 404 });
  await assert.rejects(service.read('a'.repeat(32)), { statusCode: 404 });
  const fontId = catalog.defaults.sans;
  await service.read(fontId); // Also reject changes after a cached read.
  await writeFile(path, sfnt('Arial', 'Bold', 700));
  await assert.rejects(service.read(fontId), { statusCode: 409 });
});

test('malformed collection/table bounds are skipped without exposing file contents', async (t) => {
  const root = await temporary(t);
  const broken = ttc(sfnt('Arial'));
  broken.writeUInt32BE(0xfffffff0, 12);
  await writeFile(join(root, 'broken.ttc'), broken);
  const malicious = sfnt('Arial');
  malicious.writeUInt32BE(0xfffffff0, 20);
  await writeFile(join(root, 'broken.ttf'), malicious);
  const catalog = await createSystemFontService({ roots: [root] }).catalog();
  assert.equal(catalog.fonts.length, 0);
  assert.equal(catalog.defaults.sans, null);
  assert.ok(catalog.warnings.some((message) => message.includes('2 unreadable')));
});

test('local default fonts provide usable independent sfnt bytes when present', async (t) => {
  const service = createSystemFontService(),
    catalog = await service.catalog();
  if (!catalog.defaults.sans) {
    t.skip('No supported system fonts installed');
    return;
  }
  for (const id of new Set(Object.values(catalog.defaults).filter(Boolean))) {
    const descriptor = catalog.fonts.find((font) => font.id === id);
    const result = await service.read(id);
    assert.equal(result.bytes.length, descriptor.bytes);
    assert.notEqual(result.bytes.toString('ascii', 0, 4), 'ttcf');
    assert.ok([0x00010000, 0x4f54544f, 0x74727565].includes(result.bytes.readUInt32BE(0)));
    if (descriptor.sourceBytes !== descriptor.bytes)
      assert.equal(checksum(result.bytes), 0xb1b0afba);
  }
});
