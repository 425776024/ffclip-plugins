/** Reflow the authored text box while retaining its center, typography and effects. */
export function applyTemplateLayout(bundle, width, canvas) {
  if (width === undefined) return bundle;
  const composition = structuredClone(bundle.composition);
  const reference = composition.placement;
  const scale = Math.min(
    canvas.width / reference.reference_width,
    canvas.height / reference.reference_height
  );
  const box = composition.document.layout_box;
  const next = width / scale;
  box.x += (box.width - next) / 2;
  box.width = next;
  box.sizing_mode = 'auto_height';
  box.clip_overflow = false;
  for (const paragraph of composition.document.paragraphs) {
    paragraph.style.wrap = 'word';
    delete paragraph.style.max_lines;
  }
  return { ...bundle, composition };
}

export function templateLayoutInfo(bundle, canvas) {
  const { placement, document } = bundle.composition;
  const scale = Math.min(
    canvas.width / placement.reference_width,
    canvas.height / placement.reference_height
  );
  const box = document.layout_box;
  return {
    width: box.width * scale,
    minimumWidth: Math.max(1, ((box.padding?.left || 0) + (box.padding?.right || 0) + 1) * scale)
  };
}

/** Editor width follows the layout box; decoration/ink height still follows the renderer. */
export function templateControlBounds(bundle, bounds, width, height) {
  const { placement, document } = bundle.composition;
  const scale = Math.min(width / placement.reference_width, height / placement.reference_height);
  const box = document.layout_box;
  return {
    ...bounds,
    x: (width - placement.reference_width * scale) / 2 + box.x * scale,
    width: box.width * scale
  };
}

/** Preserve explicit newlines, wrap words, and split overlong words at grapheme boundaries. */
export function wrapText(content, width, measure) {
  if (width === undefined) return content.split('\n');
  const words = new Intl.Segmenter(undefined, { granularity: 'word' });
  const graphemes = new Intl.Segmenter(undefined, { granularity: 'grapheme' });
  return content.split('\n').flatMap((paragraph) => {
    const lines = [];
    let line = '';
    for (const { segment } of words.segment(paragraph)) {
      if (measure(line + segment) <= width) {
        line += segment;
        continue;
      }
      if (line) {
        lines.push(line.trimEnd());
        line = '';
      }
      const token = segment.trimStart();
      for (const { segment: glyph } of graphemes.segment(token)) {
        if (line && measure(line + glyph) > width) {
          lines.push(line);
          line = '';
        }
        line += glyph;
      }
    }
    lines.push(line.trimEnd());
    return lines;
  });
}
