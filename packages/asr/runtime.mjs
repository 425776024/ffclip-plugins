export const ASR_SAMPLE_RATE = 16000;
/** A bounded 30 s PCM window with 4 s overlap; ownership intervals partition the source. */
export function audioWindows(begin, end) {
  if (
    !Number.isFinite(begin) ||
    !Number.isFinite(end) ||
    begin < 0 ||
    end <= begin ||
    end - begin > 3600
  )
    throw new RangeError('Invalid ASR audio range');
  const windows = [];
  for (let start = begin; start < end; start += 26) {
    const stop = Math.min(end, start + 30),
      last = stop === end;
    windows.push({
      begin: start,
      end: stop,
      keepBegin: start === begin ? begin : start + 2,
      keepEnd: last ? end : start + 28
    });
    if (last) break;
  }
  return windows;
}
/** Timestamp-preserving channel mix, with area averaging for sample-rate reduction. */
export function mixMono16k(output, block, begin) {
  const { data, timestamp, sampleRate, numberOfFrames } = block;
  if (!data.length || sampleRate <= 0 || !Number.isFinite(timestamp))
    throw new Error('Invalid decoded audio');
  const first = Math.max(0, Math.ceil((timestamp - begin) * ASR_SAMPLE_RATE - 1e-7));
  const last = Math.min(
    output.length,
    Math.ceil((timestamp + numberOfFrames / sampleRate - begin) * ASR_SAMPLE_RATE - 1e-7)
  );
  const ratio = sampleRate / ASR_SAMPLE_RATE;
  for (let index = first; index < last; index++) {
    const position = (begin + index / ASR_SAMPLE_RATE - timestamp) * sampleRate;
    let value = 0;
    for (const channel of data) {
      if (ratio > 1) {
        const from = Math.max(0, position),
          to = Math.min(numberOfFrames, position + ratio);
        let sum = 0;
        for (let frame = Math.floor(from); frame < Math.ceil(to); frame++)
          sum += channel[frame] * (Math.min(to, frame + 1) - Math.max(from, frame));
        value += to > from ? sum / (to - from) : 0;
      } else {
        const frame = Math.max(0, Math.min(numberOfFrames - 1, Math.floor(position))),
          fraction = Math.max(0, position - frame);
        value +=
          channel[frame] * (1 - fraction) +
          channel[Math.min(frame + 1, numberOfFrames - 1)] * fraction;
      }
    }
    value /= data.length;
    if (!Number.isFinite(value)) throw new Error('Decoded audio contains non-finite samples');
    output[index] = Math.max(-1, Math.min(1, value));
  }
}
export function isSilent(samples) {
  let energy = 0,
    peak = 0;
  for (const sample of samples) {
    energy += sample * sample;
    peak = Math.max(peak, Math.abs(sample));
  }
  return peak < 0.001 && Math.sqrt(energy / Math.max(1, samples.length)) < 0.0001;
}
/** Keep model-provided word timestamps; overlap ownership prevents duplicated boundary words. */
export function windowWords(result, window, sourceBegin) {
  const words = [];
  const duration = window.end - window.begin;
  for (const chunk of result.chunks || []) {
    if (!chunk.text?.trim() || !Array.isArray(chunk.timestamp)) continue;
    const [start, stop] = chunk.timestamp;
    if (!Number.isFinite(start) || (stop !== null && !Number.isFinite(stop))) continue;
    const from = window.begin + Math.max(0, Math.min(duration, start));
    const to = window.begin + Math.max(0, Math.min(duration, stop ?? duration));
    const midpoint = (from + to) / 2;
    if (to <= from || midpoint < window.keepBegin || midpoint >= window.keepEnd) continue;
    words.push({ text: chunk.text, start: from - sourceBegin, end: to - sourceBegin });
  }
  if (result.text?.trim() && !result.chunks?.length)
    throw new Error('Whisper returned text without verified timestamps');
  return words;
}
export function subtitleSegments(words) {
  const result = [];
  let current;
  for (const word of words) {
    const previousEnd = result.at(-1)?.end ?? 0;
    const start = Math.max(previousEnd, current?.end ?? 0, word.start);
    if (word.end <= start) continue;
    const text = word.text;
    if (
      current &&
      (start - current.end > 0.7 ||
        word.end - current.start > 6 ||
        Array.from(current.text + text).length > 28)
    ) {
      result.push(current);
      current = undefined;
    }
    if (!current) current = { text: text.trimStart(), start, end: word.end };
    else {
      current.text += text;
      current.end = word.end;
    }
    if (/[。！？.!?]\s*$/.test(text)) {
      result.push(current);
      current = undefined;
    }
  }
  if (current) result.push(current);
  return result
    .map((segment) => ({ ...segment, text: segment.text.trim() }))
    .filter((segment) => segment.text);
}
