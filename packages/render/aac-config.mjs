/** Accept both raw AudioSpecificConfig and the ES_Descriptor emitted by Apple's encoder. */
export function normalizeAacDecoderConfig(config) {
  const description = config?.description;
  if (!description) return config;
  const bytes = ArrayBuffer.isView(description)
    ? new Uint8Array(description.buffer, description.byteOffset, description.byteLength)
    : new Uint8Array(description);
  if (bytes[0] !== 0x03) return config;

  const descriptor = (offset, limit) => {
    if (offset >= limit) throw new Error('AAC 描述封装不完整');
    const tag = bytes[offset++];
    let length = 0,
      terminated = false;
    for (let i = 0; i < 4 && offset < limit; i++) {
      const value = bytes[offset++];
      length = length * 128 + (value & 0x7f);
      if (!(value & 0x80)) {
        terminated = true;
        break;
      }
    }
    if (!terminated || offset + length > limit) throw new Error('AAC 描述封装长度无效');
    return { tag, begin: offset, end: offset + length };
  };
  const es = descriptor(0, bytes.length);
  if (es.end !== bytes.length || es.end - es.begin < 3) throw new Error('AAC ES 描述无效');
  let offset = es.begin + 3;
  const flags = bytes[es.begin + 2];
  if (flags & 0x80) offset += 2; // dependsOn_ES_ID
  if (flags & 0x40) {
    if (offset >= es.end) throw new Error('AAC ES URL 描述不完整');
    offset += 1 + bytes[offset];
  }
  if (flags & 0x20) offset += 2; // OCR_ES_Id
  if (offset > es.end) throw new Error('AAC ES 描述不完整');
  while (offset < es.end) {
    const child = descriptor(offset, es.end);
    offset = child.end;
    if (child.tag !== 0x04) continue;
    if (child.end - child.begin < 13 || bytes[child.begin] !== 0x40)
      throw new Error('AAC 解码描述无效');
    let nestedOffset = child.begin + 13;
    while (nestedOffset < child.end) {
      const nested = descriptor(nestedOffset, child.end);
      nestedOffset = nested.end;
      if (nested.tag === 0x05 && nested.end - nested.begin >= 2)
        return { ...config, description: bytes.slice(nested.begin, nested.end) };
    }
  }
  throw new Error('AAC 描述未包含 AudioSpecificConfig');
}
