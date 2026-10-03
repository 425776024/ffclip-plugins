import { open, readdir, realpath } from 'node:fs/promises';
import { constants } from 'node:fs';
import { homedir } from 'node:os';
import { join, relative, isAbsolute, extname, win32 } from 'node:path';
import { createHash } from 'node:crypto';

const MAX_FILE_BYTES = 256 * 1024 * 1024;
const MAX_FACE_BYTES = 96 * 1024 * 1024;
const MAX_TABLES = 256;
const MAX_FACES = 256;
const align4 = (n) => Math.ceil(n / 4) * 4;
const hash = (value) => createHash('sha256').update(value).digest('hex');
const identity = (stat) => `${stat.dev}:${stat.ino}:${stat.size}:${stat.mtimeMs}`;
const contained = (root, path) => {
  const tail = relative(root, path);
  return tail === '' || (!tail.startsWith('..') && !isAbsolute(tail));
};

/** Standard font locations only. Options are for trusted embedding/tests, never HTTP input. */
export function standardFontDirectories(
  platform = process.platform,
  home = homedir(),
  env = process.env
) {
  if (platform === 'darwin')
    return [
      '/System/Library/Fonts',
      '/Library/Fonts',
      join(home, 'Library/Fonts'),
      // Recent macOS moves installed CJK fonts into the OS font asset store.
      '/System/Library/AssetsV2/com_apple_MobileAsset_Font7',
      '/System/Library/AssetsV2/com_apple_MobileAsset_Font8'
    ];
  if (platform === 'win32')
    return [
      win32.join(env.WINDIR || 'C:\\Windows', 'Fonts'),
      win32.join(
        env.LOCALAPPDATA || win32.join(home, 'AppData', 'Local'),
        'Microsoft',
        'Windows',
        'Fonts'
      )
    ];
  return [
    '/usr/share/fonts',
    '/usr/local/share/fonts',
    join(home, '.local/share/fonts'),
    join(home, '.fonts')
  ];
}

async function readAt(file, offset, length, fileSize) {
  if (
    !Number.isSafeInteger(offset) ||
    !Number.isSafeInteger(length) ||
    offset < 0 ||
    length < 0 ||
    offset + length > fileSize
  )
    throw new Error('Invalid font table bounds');
  const data = Buffer.alloc(length);
  let filled = 0;
  while (filled < length) {
    const { bytesRead } = await file.read(data, filled, length - filled, offset + filled);
    if (!bytesRead) throw new Error('Truncated font');
    filled += bytesRead;
  }
  return data;
}

async function readDirectory(file, offset, size) {
  const head = await readAt(file, offset, 12, size);
  const version = head.readUInt32BE(0);
  if (![0x00010000, 0x4f54544f, 0x74727565].includes(version))
    throw new Error('Unsupported sfnt font');
  const count = head.readUInt16BE(4);
  if (!count || count > MAX_TABLES) throw new Error('Invalid sfnt table count');
  const bytes = await readAt(file, offset + 12, count * 16, size);
  const tables = [],
    seen = new Set();
  let outputSize = 12 + count * 16;
  for (let i = 0; i < count; i++) {
    const p = i * 16,
      tag = bytes.toString('ascii', p, p + 4);
    const start = bytes.readUInt32BE(p + 8),
      length = bytes.readUInt32BE(p + 12);
    if (seen.has(tag) || !/^[\x20-\x7e]{4}$/.test(tag) || start + length > size)
      throw new Error('Invalid or duplicate font table');
    seen.add(tag);
    outputSize += align4(length);
    if (outputSize > MAX_FACE_BYTES) throw new Error('Font face exceeds size limit');
    tables.push({ tag, offset: start, length });
  }
  if (!seen.has('name') || !seen.has('head')) throw new Error('Font has no name or head table');
  return { version, tables, outputSize };
}

function decodeName(bytes, platform) {
  if (platform === 0 || platform === 3) {
    if (bytes.length % 2) return '';
    const copy = Buffer.from(bytes);
    copy.swap16();
    return copy.toString('utf16le');
  }
  // English family/PostScript names are ASCII in legacy Macintosh name records.
  return bytes.toString('latin1');
}

function namesFromTable(bytes) {
  if (bytes.length < 6) throw new Error('Invalid font names');
  const count = bytes.readUInt16BE(2),
    storage = bytes.readUInt16BE(4);
  if (6 + count * 12 > bytes.length || storage > bytes.length)
    throw new Error('Invalid font name records');
  const choices = new Map();
  for (let i = 0; i < count; i++) {
    const p = 6 + i * 12,
      platform = bytes.readUInt16BE(p),
      language = bytes.readUInt16BE(p + 4);
    const nameId = bytes.readUInt16BE(p + 6),
      length = bytes.readUInt16BE(p + 8),
      start = storage + bytes.readUInt16BE(p + 10);
    if (![1, 2, 6, 16, 17].includes(nameId) || start + length > bytes.length || length > 4096)
      continue;
    const value = decodeName(bytes.subarray(start, start + length), platform)
      .replace(/\0/g, '')
      .trim();
    if (!value || /[\x00-\x1f]/.test(value)) continue;
    const score =
      (platform === 3 ? 30 : platform === 0 ? 20 : 10) +
      (language === 0x409 ? 5 : language === 0 ? 4 : 0);
    if (!choices.has(nameId) || choices.get(nameId).score < score)
      choices.set(nameId, { score, value });
  }
  return {
    family: choices.get(16)?.value || choices.get(1)?.value || '',
    subfamily: choices.get(17)?.value || choices.get(2)?.value || 'Regular',
    postscriptName: choices.get(6)?.value || ''
  };
}

async function describeFace(file, directory, size) {
  const table = (tag) => directory.tables.find((entry) => entry.tag === tag);
  const name = table('name');
  if (name.length > 1024 * 1024) throw new Error('Font names exceed size limit');
  const names = namesFromTable(await readAt(file, name.offset, name.length, size));
  if (!names.family) throw new Error('Font has no family name');
  const os = table('OS/2');
  const data = os && os.length >= 64 ? await readAt(file, os.offset, 64, size) : null;
  const flags = data?.readUInt16BE(62) || 0;
  const weight = data?.readUInt16BE(4) || (/bold/i.test(names.subfamily) ? 700 : 400);
  return {
    ...names,
    weight,
    width: data?.readUInt16BE(6) || 5,
    slant:
      flags & 0x200
        ? 'oblique'
        : flags & 1 || /italic/i.test(names.subfamily)
          ? 'italic'
          : 'upright',
    format: directory.version === 0x4f54544f ? 'otf' : 'ttf',
    mediaType: directory.version === 0x4f54544f ? 'font/otf' : 'font/ttf'
  };
}

function checksum(bytes) {
  let value = 0;
  for (let i = 0; i < bytes.length; i += 4) {
    let word = 0;
    for (let j = 0; j < 4; j++) word = (word << 8) | (bytes[i + j] || 0);
    value = (value + (word >>> 0)) >>> 0;
  }
  return value;
}

/** Extract one TTC face without reading the collection into memory. Rebuild valid sfnt checksums. */
async function extractFace(file, directory, size) {
  const tables = directory.tables
    .filter(({ tag }) => tag !== 'DSIG')
    .sort((a, b) => (a.tag < b.tag ? -1 : a.tag > b.tag ? 1 : 0));
  const total = 12 + tables.length * 16 + tables.reduce((n, table) => n + align4(table.length), 0);
  const bytes = Buffer.alloc(total),
    power = 2 ** Math.floor(Math.log2(tables.length));
  bytes.writeUInt32BE(directory.version, 0);
  bytes.writeUInt16BE(tables.length, 4);
  bytes.writeUInt16BE(power * 16, 6);
  bytes.writeUInt16BE(Math.log2(power), 8);
  bytes.writeUInt16BE(tables.length * 16 - power * 16, 10);
  let offset = 12 + tables.length * 16,
    headOffset = null;
  for (let i = 0; i < tables.length; i++) {
    const table = tables[i],
      record = 12 + i * 16;
    const data = await readAt(file, table.offset, table.length, size);
    if (table.tag === 'head') {
      if (data.length < 12) throw new Error('Invalid font head');
      data.writeUInt32BE(0, 8);
      headOffset = offset;
    }
    bytes.write(table.tag, record, 4, 'ascii');
    bytes.writeUInt32BE(checksum(data), record + 4);
    bytes.writeUInt32BE(offset, record + 8);
    bytes.writeUInt32BE(data.length, record + 12);
    data.copy(bytes, offset);
    offset += align4(data.length);
  }
  bytes.writeUInt32BE((0xb1b0afba - checksum(bytes)) >>> 0, headOffset + 8);
  return bytes;
}

function selectFont(fonts, families) {
  for (const family of families) {
    const matches = fonts.filter((font) => font.family.toLowerCase() === family.toLowerCase());
    matches.sort(
      (a, b) =>
        (a.slant === 'upright' ? 0 : 1000) +
          Math.abs(a.weight - 400) -
          ((b.slant === 'upright' ? 0 : 1000) + Math.abs(b.weight - 400)) || a.bytes - b.bytes
    );
    if (matches.length) return matches[0].id;
  }
  return null;
}

/** No font bytes are bundled, downloaded, copied to a project, or read from caller-provided paths. */
export function createSystemFontService(options = {}) {
  const platform = options.platform || process.platform;
  const locatorPlatform =
    platform === 'darwin' ? 'macos' : platform === 'win32' ? 'windows' : platform;
  const rootPaths = options.roots || standardFontDirectories(platform, options.home, options.env);
  const registered = new Map(),
    pendingReads = new Map(),
    byteCache = new Map();
  let catalogPromise,
    cacheBytes = 0;
  const cacheLimit = 48 * 1024 * 1024;

  async function discover() {
    const roots = [
      ...new Set(
        (await Promise.all(rootPaths.map((path) => realpath(path).catch(() => null)))).filter(
          Boolean
        )
      )
    ];
    const seen = new Set(),
      warnings = [],
      fonts = [];
    let fileCount = 0,
      skipped = 0;
    async function scan(directory, depth) {
      if (depth > 5 || fileCount >= 5000) return;
      let entries;
      try {
        entries = await readdir(directory, { withFileTypes: true });
      } catch {
        return;
      }
      entries.sort((a, b) => a.name.localeCompare(b.name));
      for (const entry of entries) {
        if (fileCount >= 5000) break;
        const candidate = join(directory, entry.name);
        const path = await realpath(candidate).catch(() => null);
        if (!path || !roots.some((root) => contained(root, path)) || seen.has(path)) continue;
        seen.add(path);
        if (entry.isDirectory()) {
          await scan(path, depth + 1);
          continue;
        }
        if (!/\.(ttf|otf|ttc|otc)$/i.test(extname(entry.name))) continue;
        fileCount++;
        let file;
        try {
          file = await open(path, constants.O_RDONLY | (constants.O_NOFOLLOW || 0));
          const stat = await file.stat();
          if (!stat.isFile() || stat.size < 12 || stat.size > MAX_FILE_BYTES) continue;
          const head = await readAt(file, 0, 12, stat.size),
            collection = head.toString('ascii', 0, 4) === 'ttcf';
          const count = collection ? head.readUInt32BE(8) : 1;
          if (!count || count > MAX_FACES) throw new Error('Invalid font face count');
          const offsets = collection ? await readAt(file, 12, count * 4, stat.size) : null;
          for (let face = 0; face < count; face++) {
            const offset = offsets ? offsets.readUInt32BE(face * 4) : 0;
            const directory = await readDirectory(file, offset, stat.size);
            const metadata = await describeFace(file, directory, stat.size);
            const sourceIdentity = identity(stat),
              id = hash(`${path}\0${sourceIdentity}\0${face}`).slice(0, 32);
            const descriptor = {
              id,
              ...metadata,
              bytes: collection
                ? directory.outputSize -
                  directory.tables
                    .filter(({ tag }) => tag === 'DSIG')
                    .reduce((n, table) => n + 16 + align4(table.length), 0)
                : stat.size,
              sourceBytes: stat.size,
              faceIndex: 0,
              sourceFaceIndex: face,
              platform: locatorPlatform,
              identity: id
            };
            registered.set(id, { path, sourceIdentity, directory, collection, descriptor, roots });
            fonts.push(descriptor);
          }
        } catch {
          skipped++;
        } finally {
          await file?.close();
        }
      }
    }
    for (const root of roots)
      if (!seen.has(root)) {
        seen.add(root);
        await scan(root, 0);
      }
    fonts.sort(
      (a, b) =>
        a.family.localeCompare(b.family) ||
        a.weight - b.weight ||
        a.subfamily.localeCompare(b.subfamily)
    );
    const sans = selectFont(fonts, [
      'Arial',
      'Helvetica',
      'Segoe UI',
      'DejaVu Sans',
      'Liberation Sans',
      'Noto Sans',
      'FreeSans'
    ]);
    const cjk = selectFont(fonts, [
      'PingFang SC',
      'Microsoft YaHei',
      'Microsoft YaHei UI',
      'Noto Sans CJK SC',
      'Noto Sans SC',
      'Source Han Sans SC',
      'Heiti SC',
      'Hiragino Sans GB',
      'WenQuanYi Zen Hei',
      'WenQuanYi Micro Hei',
      'Droid Sans Fallback'
    ]);
    if (!sans)
      warnings.push(
        'No supported system sans font was found. Install a system sans font to render text.'
      );
    if (!cjk)
      warnings.push(
        'No Chinese system font was found. Chinese text requires a locally installed CJK font.'
      );
    if (skipped)
      warnings.push(`${skipped} unreadable or unsupported system font files were skipped.`);
    if (fileCount >= 5000) warnings.push('System font scan reached its 5000-file limit.');
    return { fonts, defaults: { sans, cjk }, warnings };
  }

  const catalog = () => (catalogPromise ||= discover());
  async function load(fontId) {
    await catalog();
    const record = registered.get(fontId);
    if (!record) throw Object.assign(new Error('Unknown system font'), { statusCode: 404 });
    const path = await realpath(record.path).catch(() => null);
    if (path !== record.path || !record.roots.some((root) => contained(root, path)))
      throw Object.assign(new Error('System font changed; restart the editor to refresh fonts'), {
        statusCode: 409
      });
    const file = await open(path, constants.O_RDONLY | (constants.O_NOFOLLOW || 0));
    try {
      const stat = await file.stat();
      if (!stat.isFile() || identity(stat) !== record.sourceIdentity)
        throw Object.assign(new Error('System font changed; restart the editor to refresh fonts'), {
          statusCode: 409
        });
      const cached = byteCache.get(fontId);
      if (cached) {
        byteCache.delete(fontId);
        byteCache.set(fontId, cached);
        return cached;
      }
      const bytes = record.collection
        ? await extractFace(file, record.directory, stat.size)
        : await readAt(file, 0, stat.size, stat.size);
      const value = {
        ...record.descriptor,
        bytes,
        mime: record.descriptor.mediaType,
        etag: `"${hash(bytes)}"`
      };
      if (bytes.length <= cacheLimit) {
        while (cacheBytes + bytes.length > cacheLimit && byteCache.size) {
          const oldest = byteCache.keys().next().value;
          cacheBytes -= byteCache.get(oldest).bytes.length;
          byteCache.delete(oldest);
        }
        byteCache.set(fontId, value);
        cacheBytes += bytes.length;
      }
      return value;
    } finally {
      await file.close();
    }
  }
  async function read(fontId) {
    if (typeof fontId !== 'string' || !/^[a-f0-9]{32}$/.test(fontId))
      throw Object.assign(new Error('Unknown system font'), { statusCode: 404 });
    if (!pendingReads.has(fontId))
      pendingReads.set(
        fontId,
        load(fontId).finally(() => pendingReads.delete(fontId))
      );
    return pendingReads.get(fontId);
  }
  return { catalog, read };
}

export const systemFonts = createSystemFontService();
