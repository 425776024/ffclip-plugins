import { open } from 'node:fs/promises';

// Read container track directories without loading media payloads. Native Format 1
// stores a zero-based physical stream index; Mediabunny's public track list omits
// subtitle/unknown tracks, so its array position must not be used for MP4/MKV.
export async function containerTrackIndices(path, extension, fileSize) {
  if (!['.mp4', '.mov', '.m4v', '.mkv', '.webm'].includes(extension)) return null;
  const handle = await open(path, 'r');
  const bytes = async (at, count) => {
    if (at < 0 || count < 0 || at + count > fileSize)
      throw Error('Invalid container metadata range');
    const output = Buffer.alloc(count),
      result = await handle.read(output, 0, count, at);
    if (result.bytesRead !== count) throw Error('Truncated container metadata');
    return output;
  };
  try {
    const indices = new Map();
    if (['.mp4', '.mov', '.m4v'].includes(extension)) {
      const box = async (at, limit) => {
        const header = await bytes(at, 8);
        let size = header.readUInt32BE(0),
          offset = 8;
        if (size === 1) {
          size = Number((await bytes(at + 8, 8)).readBigUInt64BE());
          offset = 16;
        }
        if (size === 0) size = limit - at;
        if (!Number.isSafeInteger(size) || size < offset || at + size > limit)
          throw Error('Invalid ISO media box');
        return { type: header.toString('ascii', 4, 8), begin: at + offset, end: at + size };
      };
      let moov;
      for (let at = 0, count = 0; at + 8 <= fileSize && count++ < 100000;) {
        const row = await box(at, fileSize);
        if (row.type === 'moov') {
          moov = row;
          break;
        }
        at = row.end;
      }
      if (!moov) throw Error('MP4 track directory is unavailable');
      let ordinal = 0;
      for (let at = moov.begin; at + 8 <= moov.end;) {
        const track = await box(at, moov.end);
        at = track.end;
        if (track.type !== 'trak') continue;
        let found = false;
        for (let child = track.begin; child + 8 <= track.end;) {
          const row = await box(child, track.end);
          child = row.end;
          if (row.type !== 'tkhd') continue;
          const version = (await bytes(row.begin, 1))[0],
            offset = version === 1 ? 20 : 12;
          if (version !== 0 && version !== 1) throw Error('Unsupported MP4 track header');
          const identity = (await bytes(row.begin + offset, 4)).readUInt32BE();
          indices.set(identity, ordinal);
          found = true;
          break;
        }
        if (!found) throw Error('MP4 track has no identity');
        ordinal++;
      }
    } else {
      const vint = async (at, keepMarker) => {
        const first = (await bytes(at, 1))[0];
        let length = 1,
          marker = 128;
        while (length <= 8 && !(first & marker)) {
          marker >>= 1;
          length++;
        }
        if (length > 8 || (keepMarker && length > 4)) throw Error('Invalid EBML integer');
        const data = await bytes(at, length);
        let value = keepMarker ? first : first & (marker - 1);
        let unknown = !keepMarker && value === marker - 1;
        for (let i = 1; i < length; i++) {
          value = value * 256 + data[i];
          unknown = unknown && data[i] === 255;
        }
        if (!Number.isSafeInteger(value) && !unknown) throw Error('EBML value exceeds safe bounds');
        return { length, value, unknown };
      };
      const element = async (at, limit) => {
        const identity = await vint(at, true),
          size = await vint(at + identity.length, false);
        const begin = at + identity.length + size.length,
          end = size.unknown ? limit : begin + size.value;
        if (end > limit || end < begin) throw Error('Invalid EBML element size');
        return { id: identity.value, begin, end };
      };
      let segment;
      for (let at = 0, count = 0; at < fileSize && count++ < 1000;) {
        const row = await element(at, fileSize);
        if (row.id === 0x18538067) {
          segment = row;
          break;
        }
        at = row.end;
      }
      if (!segment) throw Error('Matroska segment is unavailable');
      let tracks;
      for (let at = segment.begin, count = 0; at < segment.end && count++ < 100000;) {
        const row = await element(at, segment.end);
        if (row.id === 0x1654ae6b) {
          tracks = row;
          break;
        }
        at = row.end;
      }
      if (!tracks) throw Error('Matroska track directory is unavailable');
      let ordinal = 0;
      for (let at = tracks.begin; at < tracks.end;) {
        const entry = await element(at, tracks.end);
        at = entry.end;
        if (entry.id !== 0xae) continue;
        let found = false;
        for (let child = entry.begin; child < entry.end;) {
          const row = await element(child, entry.end);
          child = row.end;
          if (row.id !== 0xd7) continue;
          const value = await bytes(row.begin, row.end - row.begin);
          if (!value.length || value.length > 6) throw Error('Unsupported Matroska track identity');
          indices.set(value.readUIntBE(0, value.length), ordinal);
          found = true;
          break;
        }
        if (!found) throw Error('Matroska track has no identity');
        ordinal++;
      }
    }
    return indices;
  } finally {
    await handle.close();
  }
}

/** TIFF orientation stored in a JPEG APP1 Exif segment, with bounded offsets. */
export function jpegOrientation(bytes) {
  if (bytes[0] !== 255 || bytes[1] !== 216) return 1;
  for (let at = 2; at + 4 <= bytes.length;) {
    if (bytes[at++] !== 255) continue;
    const marker = bytes[at++];
    if (marker === 255 || marker === 216) continue;
    if (marker === 217 || marker === 218) break;
    const size = bytes.readUInt16BE(at);
    if (size < 2 || at + size > bytes.length) break;
    if (marker === 225 && size >= 16 && bytes.toString('ascii', at + 2, at + 8) === 'Exif\0\0') {
      const start = at + 8,
        end = at + size,
        little = bytes.toString('ascii', start, start + 2) === 'II';
      if (!little && bytes.toString('ascii', start, start + 2) !== 'MM') return 1;
      const u16 = (offset) => (little ? bytes.readUInt16LE(offset) : bytes.readUInt16BE(offset));
      const u32 = (offset) => (little ? bytes.readUInt32LE(offset) : bytes.readUInt32BE(offset));
      if (start + 8 > end || u16(start + 2) !== 42) return 1;
      const directory = start + u32(start + 4);
      if (directory < start || directory + 2 > end) return 1;
      const count = u16(directory);
      for (let i = 0; i < count; i++) {
        const entry = directory + 2 + i * 12;
        if (entry + 12 > end) return 1;
        if (u16(entry) === 0x112 && u16(entry + 2) === 3 && u32(entry + 4) === 1) {
          const orientation = u16(entry + 8);
          return orientation >= 1 && orientation <= 8 ? orientation : 1;
        }
      }
    }
    at += size;
  }
  return 1;
}
