/** Apply per-clip edits before the shared WASM/GPU renderer loads its document. */
export function applyTemplateStyle(bundle, style) {
  if (!style || !Object.keys(style).length) return bundle;
  const composition = structuredClone(bundle.composition);
  const color =
    style.color &&
    [1, 3, 5].map((offset) => parseInt(style.color.slice(offset, offset + 2), 16) / 255);
  for (const paragraph of composition.document.paragraphs)
    for (const run of paragraph.runs) {
      if (style.fontSize !== undefined) run.style.font_size = style.fontSize;
      if (color)
        for (const layer of run.style.materials.layers)
          if (layer.type === 'fill')
            layer.material = { kind: 'literal', material: { kind: 'solid', color: [...color, 1] } };
    }
  return { ...bundle, composition };
}
