import { lstat, readFile, readdir, rename, rm, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { deflateRawSync } from 'node:zlib';
import { root, isMain } from './release-files.mjs';
import { runNpm } from './npm-command.mjs';

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

// Standard ZIP with UTF-8 names and raw DEFLATE, using only Node built-ins.
// The small static site does not require ZIP64 or a platform-specific zip tool.
export async function zipDirectory(directory) {
  const local = [];
  const central = [];
  let offset = 0;
  let count = 0;
  async function visit(relative = '') {
    for (const name of (await readdir(join(directory, relative))).sort()) {
      const path = relative ? `${relative}/${name}` : name;
      const info = await lstat(join(directory, path));
      if (info.isDirectory()) {
        await visit(path);
        continue;
      }
      if (!info.isFile()) throw new Error(`官网打包只接受普通文件：${path}`);
      const filename = Buffer.from(path);
      const bytes = await readFile(join(directory, path));
      const compressed = deflateRawSync(bytes);
      const header = Buffer.alloc(30);
      header.writeUInt32LE(0x04034b50, 0);
      header.writeUInt16LE(20, 4);
      header.writeUInt16LE(0x0800, 6);
      header.writeUInt16LE(8, 8);
      header.writeUInt16LE(33, 12); // 1980-01-01, deterministic ZIP timestamp.
      header.writeUInt32LE(crc32(bytes), 14);
      header.writeUInt32LE(compressed.length, 18);
      header.writeUInt32LE(bytes.length, 22);
      header.writeUInt16LE(filename.length, 26);
      local.push(header, filename, compressed);
      const entry = Buffer.alloc(46);
      entry.writeUInt32LE(0x02014b50, 0);
      // UNIX origin avoids older macOS unzip treating UTF-8 as DOS codepage bytes.
      entry.writeUInt16LE(0x0314, 4);
      header.copy(entry, 6, 4, 28);
      entry.writeUInt32LE((0o100644 << 16) >>> 0, 38);
      entry.writeUInt32LE(offset, 42);
      central.push(entry, filename);
      offset += header.length + filename.length + compressed.length;
      count++;
      if (count > 65535 || offset > 0xffffffff) throw new Error('官网归档超过普通 ZIP 限制。');
    }
  }
  await visit();
  const index = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(count, 8);
  end.writeUInt16LE(count, 10);
  end.writeUInt32LE(index.length, 12);
  end.writeUInt32LE(offset, 16);
  return { bytes: Buffer.concat([...local, index, end]), count };
}

export async function buildWebsite() {
  await runNpm(['--prefix', 'ffclip', 'run', 'build']);
  const directory = join(root, 'ffclip/dist');
  for (const name of [
    'index.html',
    'zh/index.html',
    'en/index.html',
    'robots.txt',
    'sitemap.xml'
  ]) {
    if (!(await lstat(join(directory, name))).isFile()) throw new Error(`官网缺少文件：${name}`);
  }
  const archive = join(root, 'ffclip/ffclip-website.zip');
  const temporary = `${archive}.${process.pid}.tmp`;
  try {
    const { bytes, count } = await zipDirectory(directory);
    await writeFile(temporary, bytes, { flag: 'wx' });
    await rename(temporary, archive);
    console.log(
      `\n官网打包完成：\n目录：${directory}\nZIP：${archive}\n共 ${count} 个文件，${(bytes.length / 1024 / 1024).toFixed(2)} MB，可直接上传静态托管。`
    );
  } finally {
    await rm(temporary, { force: true });
  }
}

if (isMain(import.meta.url)) {
  buildWebsite().catch((error) => {
    console.error(`官网打包失败：${error.message}`);
    process.exitCode = 1;
  });
}
