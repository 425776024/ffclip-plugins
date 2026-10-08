/** Runs inside Chromium on the upstream flattened snapshot, not the authored DOM. */
export function normalizePagxSnapshot(html) {
  const doc = new DOMParser().parseFromString(html, 'text/html');
  const rules = new Map();
  for (const style of doc.querySelectorAll('style')) {
    const sheet = new CSSStyleSheet();
    sheet.replaceSync(style.textContent);
    for (const rule of sheet.cssRules)
      if (rule.type === CSSRule.KEYFRAMES_RULE) rules.set(rule.name, rule);
  }
  const extra = new Map();
  for (const span of doc.querySelectorAll('span[style]')) {
    const name = span.style.animationName;
    if (!name.startsWith('pagxAnim')) continue;
    let owner = span.parentElement;
    while (owner && owner.style.animationName !== name) owner = owner.parentElement;
    if (!owner) continue;
    // Upstream emits a measured text-line span with its host's computed style.
    // Applying its transform/opacity again doubles displacement and squares alpha.
    // Only inherited text paint needs an animation on that generated inner span.
    const rule = rules.get(name);
    if (!rule) throw Error('PAGX snapshot animation definition is missing');
    const frames = [...rule.cssRules]
      .map((frame) => {
        const paint = [...frame.style].filter((p) =>
          ['color', 'fill', 'stroke', 'stroke-dashoffset'].includes(p)
        );
        return paint.length
          ? `${frame.keyText}{${paint.map((p) => `${p}:${frame.style.getPropertyValue(p)}`).join(';')}}`
          : '';
      })
      .filter(Boolean);
    span.style.animationName = frames.length ? name + 'TextPaint' : 'none';
    if (frames.length) extra.set(name, `@keyframes ${name}TextPaint{${frames.join('')}}`);
  }
  if (extra.size) {
    const style = doc.createElement('style');
    style.textContent = [...extra.values()].join('\n');
    doc.head.append(style);
  }
  // The native importer consumes the XML-compatible HTML subset (self-closing
  // meta/img/br). Browser outerHTML would turn these into unclosed HTML tags.
  return new XMLSerializer()
    .serializeToString(doc)
    .replace(/ xmlns="http:\/\/www\.w3\.org\/1999\/xhtml"/g, '');
}
