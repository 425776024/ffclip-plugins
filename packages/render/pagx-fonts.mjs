/** Give each installed face a private, in-memory family name. PAGX 0.4.47's
 * viewer registers fallback fonts, whose family lookup ignores the style.
 * Unique families preserve real Regular/Bold outlines without faux bold.
 * The source system font and the editable PAGX document are never modified.
 */
export function aliasPagxFont(input, family) {
  const bytes = new Uint8Array(input),
    view = new DataView(bytes.buffer);
  if (bytes.length < 12 || !/^[A-Za-z0-9-]+$/.test(family)) throw Error('Invalid PAGX font alias');
  const count = view.getUint16(4);
  if (!count || count > 256 || 12 + count * 16 > bytes.length)
    throw Error('Invalid sfnt directory');
  const tables = [];
  const encoded = (value, unicode) => {
    const out = new Uint8Array(value.length * (unicode ? 2 : 1));
    for (let i = 0; i < value.length; i++)
      out[i * (unicode ? 2 : 1) + (unicode ? 1 : 0)] = value.charCodeAt(i);
    return out;
  };
  for (let i = 0; i < count; i++) {
    const p = 12 + i * 16,
      tag = String.fromCharCode(...bytes.subarray(p, p + 4));
    const offset = view.getUint32(p + 8),
      length = view.getUint32(p + 12);
    if (offset + length > bytes.length) throw Error('Invalid sfnt table');
    let data = bytes.slice(offset, offset + length);
    if (tag === 'name') {
      const names = new DataView(data.buffer);
      if (data.length < 6) throw Error('Invalid name table');
      const records = names.getUint16(2),
        storage = names.getUint16(4);
      if (6 + records * 12 > data.length || storage > data.length)
        throw Error('Invalid name records');
      const strings = [];
      let end = data.length;
      for (let j = 0; j < records; j++) {
        const q = 6 + j * 12,
          platform = names.getUint16(q),
          id = names.getUint16(q + 6);
        if (![1, 4, 6, 16, 21].includes(id) || ![0, 1, 3].includes(platform)) continue;
        const value = encoded(family, platform !== 1);
        if (end - storage + value.length > 65535) throw Error('PAGX font name table too large');
        names.setUint16(q + 8, value.length);
        names.setUint16(q + 10, end - storage);
        strings.push(value);
        end += value.length;
      }
      if (!strings.length) throw Error('PAGX font has no family names');
      const renamed = new Uint8Array(end);
      renamed.set(data);
      let cursor = data.length;
      for (const value of strings) {
        renamed.set(value, cursor);
        cursor += value.length;
      }
      data = renamed;
    }
    if (tag === 'head') new DataView(data.buffer).setUint32(8, 0);
    tables.push({ p, tag, data });
  }
  const align = (n) => Math.ceil(n / 4) * 4;
  const result = new Uint8Array(
    12 + count * 16 + tables.reduce((sum, t) => sum + align(t.data.length), 0)
  );
  result.set(bytes.subarray(0, 12 + count * 16));
  const output = new DataView(result.buffer);
  const checksum = (start, size) => {
    let sum = 0;
    for (let i = start; i < start + align(size); i += 4) sum = (sum + output.getUint32(i)) >>> 0;
    return sum;
  };
  let offset = 12 + count * 16,
    head;
  for (const t of tables) {
    result.set(t.data, offset);
    output.setUint32(t.p + 4, checksum(offset, t.data.length));
    output.setUint32(t.p + 8, offset);
    output.setUint32(t.p + 12, t.data.length);
    if (t.tag === 'head') head = offset;
    offset += align(t.data.length);
  }
  if (head === undefined) throw Error('PAGX font has no head table');
  output.setUint32(head + 8, (0xb1b0afba - checksum(0, result.length)) >>> 0);
  return result;
}
