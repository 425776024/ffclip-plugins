/** Preserve codec priming for the decoder while moving audible sample zero to timeline zero. */
export function audioPacketTiming(startFrame, packetFrames, encoderDelay, totalFrames, sampleRate) {
  for (const value of [startFrame, packetFrames, encoderDelay, totalFrames, sampleRate])
    if (!Number.isSafeInteger(value) || value < 0)
      throw new RangeError('Invalid audio sample timing');
  if (!packetFrames || !sampleRate) throw new RangeError('Empty audio packet');
  const begin = startFrame - encoderDelay;
  if (begin >= totalFrames) return null;
  return {
    timestamp: begin / sampleRate,
    duration: Math.min(packetFrames, totalFrames - begin) / sampleRate
  };
}

/** Match a deterministic probe, not silence thresholds or OS-specific AAC delay guesses. */
export function measureAudioDelay(
  reference,
  decoded,
  markerStart,
  markerFrames,
  maximumDelay = 4096
) {
  const score = (delay, stride) => {
    let dot = 0,
      x2 = 0,
      y2 = 0;
    for (let i = 0; i < markerFrames; i += stride) {
      const x = reference[markerStart + i],
        y = decoded[markerStart + i + delay] ?? 0;
      dot += x * y;
      x2 += x * x;
      y2 += y * y;
    }
    return dot / Math.sqrt(x2 * y2 || 1);
  };
  // Full-resolution lag search with a sparse sample set retains single-sample precision.
  let delay = 0,
    correlation = -Infinity;
  for (let candidate = 0; candidate <= maximumDelay; candidate++) {
    const value = score(candidate, 8);
    if (value > correlation) {
      delay = candidate;
      correlation = value;
    }
  }
  correlation = score(delay, 1);
  if (correlation < 0.9 || decoded.length < reference.length + delay)
    throw new Error(
      `AAC 编码延迟校准失败（相关度 ${correlation.toFixed(3)}），请改用 WebM 或本地编码器。`
    );
  return { delay, correlation };
}
