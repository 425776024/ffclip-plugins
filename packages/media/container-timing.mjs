// Bounded EBML header parsing for the Matroska CodecDelay field that
// Mediabunny 1.61 does not yet apply to encoded packet timestamps.
export function matroskaCodecDelay(bytes, trackNumber) {
  function vint(offset, id = false) {
    if (offset >= bytes.length || !bytes[offset]) return null;
    let length = 1,
      mask = 128;
    while (!(bytes[offset] & mask)) {
      length++;
      mask >>= 1;
    }
    if (length > (id ? 4 : 8) || offset + length > bytes.length) return null;
    let value = id ? bytes[offset] : bytes[offset] & (mask - 1);
    let unknown = !id && value === mask - 1;
    for (let i = 1; i < length; i++) {
      value = value * 256 + bytes[offset + i];
      unknown &&= bytes[offset + i] === 255;
    }
    return { value, length, unknown };
  }
  function elements(start, end) {
    const result = [];
    for (let cursor = start; cursor < end;) {
      const id = vint(cursor, true);
      if (!id) break;
      const size = vint(cursor + id.length);
      if (!size) break;
      const begin = cursor + id.length + size.length;
      const finish = size.unknown ? end : begin + size.value;
      if (!Number.isSafeInteger(finish) || finish < begin) break;
      result.push({ id: id.value, begin, end: finish, complete: finish <= end });
      cursor = finish;
    }
    return result;
  }
  const unsigned = ({ begin, end }) => {
    if (end - begin > 8) throw new Error('Invalid Matroska CodecDelay integer');
    let value = 0;
    for (let i = begin; i < end; i++) value = value * 256 + bytes[i];
    if (!Number.isSafeInteger(value))
      throw new Error('Matroska CodecDelay exceeds exact integer range');
    return value;
  };
  const segment = elements(0, bytes.length).find((entry) => entry.id === 0x18538067);
  if (!segment) return null;
  const tracks = elements(segment.begin, Math.min(segment.end, bytes.length)).find(
    (entry) => entry.id === 0x1654ae6b
  );
  if (!tracks?.complete) return null;
  for (const entry of elements(tracks.begin, tracks.end)) {
    if (entry.id !== 0xae || !entry.complete) continue;
    const fields = elements(entry.begin, entry.end),
      number = fields.find((field) => field.id === 0xd7);
    if (!number?.complete || unsigned(number) !== trackNumber) continue;
    const delay = fields.find((field) => field.id === 0x56aa);
    return delay?.complete ? unsigned(delay) / 1e9 : 0;
  }
  return null;
}

export async function readMatroskaCodecDelay(url, trackNumber, signal) {
  for (const limit of [262144, 1048576, 4194304]) {
    const response = await fetch(url, { headers: { Range: `bytes=0-${limit - 1}` }, signal });
    if (!response.ok || !response.body) throw new Error('Cannot read Opus container timing');
    const reader = response.body.getReader(),
      chunks = [];
    let length = 0;
    try {
      while (length < limit) {
        const { done, value } = await reader.read();
        if (done) break;
        const part = value.subarray(0, limit - length);
        chunks.push(part);
        length += part.length;
      }
    } finally {
      await reader.cancel();
    }
    const bytes = new Uint8Array(length);
    let offset = 0;
    for (const part of chunks) {
      bytes.set(part, offset);
      offset += part.length;
    }
    const delay = matroskaCodecDelay(bytes, trackNumber);
    if (delay !== null) return delay;
    if (length < limit) break;
  }
  throw new Error('Opus container timing headers exceed the supported 4 MiB bound');
}
