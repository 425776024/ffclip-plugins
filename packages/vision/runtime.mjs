export const VISION_MAX_FRAMES = 32;
export const VISION_MAX_SEGMENTS = 32;
export const VISION_MAX_SAMPLES = 128;
/** Cover the requested source range with bounded, equal intervals and ordered samples. */
export function planVideoSegments(
  begin,
  end,
  { segmentSeconds = 5, maxSegments = 12, framesPerSegment = 3 } = {}
) {
  if (
    !Number.isFinite(segmentSeconds) ||
    segmentSeconds < 1 ||
    segmentSeconds > 600 ||
    !Number.isInteger(maxSegments) ||
    maxSegments < 1 ||
    maxSegments > VISION_MAX_SEGMENTS ||
    !Number.isInteger(framesPerSegment) ||
    framesPerSegment < 1 ||
    framesPerSegment > 4
  )
    throw new RangeError('Invalid video segment sampling options');
  sampleTimes(begin, end, 1); // Validate the half-open source range too.
  const count = Math.min(maxSegments, Math.ceil((end - begin) / segmentSeconds));
  const segments = Array.from({ length: count }, (_, i) => {
    const startSeconds = begin + ((end - begin) * i) / count;
    const endSeconds = i === count - 1 ? end : begin + ((end - begin) * (i + 1)) / count;
    return {
      startSeconds,
      endSeconds,
      sampleTimes: sampleTimes(startSeconds, endSeconds, framesPerSegment)
    };
  });
  return {
    segments,
    sampling: {
      strategy: 'uniform-intervals',
      requestedSegmentSeconds: segmentSeconds,
      intervalSeconds: (end - begin) / count,
      framesPerSegment,
      totalSamples: count * framesPerSegment,
      coarsened: count < Math.ceil((end - begin) / segmentSeconds)
    }
  };
}

/** Give the model temporal context without changing the caller's requested output format. */
export function storyboardPrompt(prompt, segment) {
  return `The image is a chronological storyboard of sampled video frames from source interval ${segment.startSeconds.toFixed(3)} to ${segment.endSeconds.toFixed(3)} seconds. Use only the ${segment.sampleTimes.length} labeled panels; unlabeled areas are padding. Read panels left to right, then top to bottom. Panel labels are source timestamps, not text in the scene. Describe this interval using only the sampled visual evidence; do not assume unseen actions between frames. Follow the user's requested language and output format.\n\nUser request:\n${prompt}`;
}
/** Midpoint sampling stays inside a half-open source range, including one-frame jobs. */
export function sampleTimes(begin, end, count) {
  if (
    !Number.isFinite(begin) ||
    !Number.isFinite(end) ||
    begin < 0 ||
    end <= begin ||
    !Number.isInteger(count) ||
    count < 1 ||
    count > VISION_MAX_FRAMES
  )
    throw new RangeError('Invalid vision sampling range');
  return Array.from({ length: count }, (_, i) => begin + ((end - begin) * (i + 0.5)) / count);
}

/** Dependency injection keeps generation and prompt removal verifiable without downloading weights. */
export async function describeFrame({ processor, model, image, prompt, maxNewTokens }) {
  const text = processor.apply_chat_template([{ role: 'user', content: `<image>\n${prompt}` }], {
    add_generation_prompt: true
  });
  const inputs = await processor(image, text, { add_special_tokens: false });
  const output = await model.generate({
    ...inputs,
    max_new_tokens: maxNewTokens,
    do_sample: false
  });
  const result = processor
    .batch_decode(output.slice(null, [inputs.input_ids.dims.at(-1), null]), {
      skip_special_tokens: true
    })[0]
    ?.trim();
  if (!result) throw new Error('视觉模型返回了空描述');
  return result;
}
