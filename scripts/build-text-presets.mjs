// Reproducible authored native packages, including antialiased bubble artwork.
// No copied texture/video or font bytes are used. Run after editing preset-designs.
import { mkdir, writeFile, readFile } from 'node:fs/promises';
import { deflateSync } from 'node:zlib';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { composeRecipe } from '../packages/text-wasm/src/recipes.mjs';
import { presetDesigns, authoredRecipes } from '../packages/text-wasm/src/preset-designs.mjs';

const root = new URL('../packages/text-wasm/presets/templates/', import.meta.url);
const rgba = (hex, alpha = 1) => [
  ...[1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16) / 255),
  alpha
];
const solid = (hex, alpha) => ({ kind: 'solid', color: rgba(hex, alpha) });
const binding = (material) => ({ kind: 'literal', material });
const insets = (left, top, right = left, bottom = top) => ({ left, top, right, bottom });
const gradient = (colors) =>
  colors.length === 1
    ? solid(colors[0])
    : {
        kind: 'linear_gradient',
        stops: colors.map((hex, i) => ({ offset: i / (colors.length - 1), color: rgba(hex) })),
        start: [0.5, 0],
        end: [0.5, 1],
        spread: 'clamp',
        sampling: 'rgba8_lut_256',
        coordinates: { space: 'grapheme', outset: 0, scale: 1 }
      };
const references = new URL('../packages/text-wasm/fixtures/templates/', import.meta.url);
const readJson = async (base, file) =>
  JSON.parse(
    await readFile(new URL(`com.videocut.text.qt-type.${base}/${file}`, references), 'utf8')
  );
async function inherited(design, base) {
  const source = design.source;
  const documents = {};
  for (const file of [
    'composition.json',
    'animation.ir.json',
    'effect.program.json',
    'manifest.json'
  ])
    documents[file] = await readJson(source, file);
  const c = documents['composition.json'];
  // Preserve typography, placement, material kinds, mapping, offsets, stroke
  // widths, padding, nine-slice caps and graph topology. Only paint changes.
  const paintLayers = (layers) => {
    for (const [i, layer] of layers.entries()) {
      const material = layer.material.material || layer.material.fallback;
      if (material.kind === 'solid')
        material.color = rgba(
          layer.type === 'fill'
            ? design.fill[0]
            : i % 2
              ? design.outline || design.fill[0]
              : design.depth || design.fill.at(-1),
          material.color[3]
        );
      if (material.kind === 'linear_gradient')
        material.stops.forEach(
          (stop, j) => (stop.color = rgba(design.fill[j % design.fill.length], stop.color[3]))
        );
      if (layer.strokes) paintLayers(layer.strokes);
    }
  };
  for (const paragraph of c.document.paragraphs)
    for (const run of paragraph.runs) paintLayers(run.style.materials.layers);
  c.display.fallback_name = design.name;
  c.display.name_key = design.id;
  c.bindings.forEach((b) => {
    if (b.id === 'content') b.default = design.text;
  });
  const manifest = documents['manifest.json'];
  manifest.presentation.fallback_name = design.name;
  manifest.presentation.name_key = design.id;
  manifest.presentation.fidelity = 'redesigned';
  manifest.preview.fixture_text = design.text;
  const remap = (value) =>
    Array.isArray(value)
      ? value.map(remap)
      : value && typeof value === 'object'
        ? Object.fromEntries(Object.entries(value).map(([k, v]) => [k, remap(v)]))
        : typeof value === 'string'
          ? value.replaceAll(`qt-type.${source}`, `qt-type.${base}`)
          : value;
  return remap(documents);
}
// New animation variants use the original per-letter program attachment and
// execution graph. Their effects change through the same native program IR.
function motionProgram(base, kind) {
  const instructions = [];
  const emit = (opcode, input_registers = [], immediates = [], input = null) => {
    const output_register = instructions.length;
    instructions.push({ opcode, output_register, input_registers, immediates, input });
    return output_register;
  };
  const constant = (n) => emit('constant', [], [n]);
  const op = (name, ...args) => emit(name, args);
  const p = emit('input', [], [], 'progress'),
    index = emit('input', [], [], 'unit_index'),
    count = emit('input', [], [], 'unit_count');
  const zero = constant(0),
    one = constant(1),
    pi = constant(Math.PI);
  const unit = op('divide', index, op('maximum', one, op('subtract', count, one)));
  const phase = op(
    'minimum',
    one,
    op(
      'maximum',
      zero,
      op('divide', op('subtract', p, op('multiply', unit, constant(0.18))), constant(0.82))
    )
  );
  const remain = op('power', op('subtract', one, phase), constant(3));
  const oscillation = op(
    'sine',
    op(
      'multiply',
      op('add', p, op('multiply', unit, constant(0.16))),
      op('multiply', pi, constant(2))
    )
  );
  const outputs = [],
    output = (name, register_index) =>
      outputs.push({ output: name, register_index, transform_index: 0 });
  if (!['wave', 'breathe', 'swing'].includes(kind))
    output('opacity', op('minimum', one, op('multiply', phase, constant(5))));
  if (kind === 'bounce')
    output(
      'offset_y',
      op(
        'multiply',
        op('sine', op('multiply', phase, op('multiply', pi, constant(3)))),
        op('multiply', remain, constant(-140))
      )
    );
  if (kind === 'slide') output('offset_x', op('multiply', remain, constant(-220)));
  if (kind === 'spring') {
    const scale = op(
      'add',
      op('subtract', one, op('multiply', remain, constant(0.72))),
      op(
        'multiply',
        op('sine', op('multiply', phase, op('multiply', pi, constant(3)))),
        op('multiply', remain, constant(0.35))
      )
    );
    output('scale_x', scale);
    output('scale_y', scale);
  }
  if (kind === 'wave') output('offset_y', op('multiply', oscillation, constant(32)));
  if (kind === 'breathe') {
    const pulse = op(
      'subtract',
      constant(0.5),
      op(
        'multiply',
        op('cosine', op('multiply', p, op('multiply', pi, constant(2)))),
        constant(0.5)
      )
    );
    const scale = op('add', one, op('multiply', pulse, constant(0.045)));
    output('scale_x', scale);
    output('scale_y', scale);
    output('opacity', op('add', constant(0.78), op('multiply', pulse, constant(0.22))));
  }
  if (kind === 'swing') output('rotation_z', op('multiply', oscillation, constant(8)));
  return {
    format: 'videocut.text-effect-program',
    programs: [
      {
        program_id: `com.videocut.text.qt-type.${base}.program.controller`,
        random_seed: 50,
        stages: [
          {
            stage_id: `authored-${kind}`,
            kind: 'per_unit',
            register_count: instructions.length,
            instructions,
            outputs
          }
        ]
      }
    ]
  };
}

// Small dependency-free rasterizer for our own vector geometry. 2x2 samples
// per output pixel keep edges smooth; nine-slice preserves the designed caps.
const polygon = (points) => (x, y) => {
  let inside = false;
  for (let i = 0, j = points.length - 1; i < points.length; j = i++) {
    const [a, b] = points[i],
      [c, d] = points[j];
    if (b > y !== d > y && x < ((c - a) * (y - b)) / (d - b) + a) inside = !inside;
  }
  return inside;
};
const rect =
  (l, t, r, b, radius = 0) =>
  (x, y) => {
    const dx = Math.max(l + radius - x, 0, x - r + radius),
      dy = Math.max(t + radius - y, 0, y - b + radius);
    return x >= l && x <= r && y >= t && y <= b && dx * dx + dy * dy <= radius * radius;
  };
const circle = (cx, cy, radius) => (x, y) => (x - cx) ** 2 + (y - cy) ** 2 < radius ** 2;
function artwork(d, width = 640, height = 288) {
  const layers = [],
    add = (mask, color) =>
      layers.push({ mask, color: rgba(color).map((c) => Math.round(c * 255)) });
  let body;
  // Both source packages reserve their center for stretching/tiling. Keep the
  // center rectangle continuously painted; transparent gutters or border marks
  // in that rectangle become repeated seams around the edited glyphs.
  if (d.shape === 'ribbon')
    body = polygon([
      [8, 12],
      [632, 12],
      [606, 144],
      [632, 276],
      [8, 276],
      [34, 144]
    ]);
  else if (d.shape === 'pixel')
    body = polygon([
      [28, 10],
      [612, 10],
      [612, 28],
      [630, 28],
      [630, 258],
      [612, 258],
      [612, 276],
      [28, 276],
      [28, 258],
      [10, 258],
      [10, 28],
      [28, 28]
    ]);
  else if (d.shape === 'note')
    body = polygon([
      [14, 14],
      [586, 14],
      [626, 49],
      [626, 274],
      [14, 274]
    ]);
  else if (d.shape === 'cloud') {
    const oval = (cx, cy, rx, ry, x, y) => ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 < 1;
    body = (x, y) =>
      rect(12, 36, 628, 252, 24)(x, y) ||
      [80, 200, 320, 440, 560].some(
        (cx) => oval(cx, 32, 77, 29, x, y) || oval(cx, 249, 77, 28, x, y)
      );
  } else if (d.shape === 'bracket') body = rect(12, 12, 628, 276, 4);
  else if (d.shape === 'signal')
    body = polygon([
      [12, 34],
      [34, 12],
      [606, 12],
      [628, 34],
      [628, 254],
      [606, 276],
      [34, 276],
      [12, 254]
    ]);
  else if (d.shape === 'frame') body = rect(12, 12, 628, 276, 4);
  else body = rect(12, 12, 628, 276, d.shape === 'pill' ? 126 : 46);
  const tail =
    d.shape === 'speech'
      ? polygon([
          [42, 259],
          [82, 264],
          [38, 286]
        ])
      : d.shape === 'cloud'
        ? circle(38, 281, 5)
        : () => false;
  const geometry = (x, y) =>
    (body(x, y) || tail(x, y)) &&
    !(d.shape === 'ticket' && (circle(12, 144, 22)(x, y) || circle(628, 144, 22)(x, y)));
  add((x, y) => geometry(x - 4, y - 8), d.accent);
  add(geometry, d.edge);
  add(
    (x, y) =>
      geometry(x, y) &&
      [
        [-3, 0],
        [3, 0],
        [0, -3],
        [0, 3]
      ].every(([dx, dy]) => geometry(x + dx, y + dy)),
    d.paper
  );
  if (d.shape === 'note') {
    add(
      polygon([
        [586, 14],
        [586, 50],
        [626, 50]
      ]),
      d.accent
    );
    add(rect(75, 9, 137, 22, 3), d.accent);
  }
  if (d.shape === 'signal') {
    for (let i = 0; i < 3; i++) add(rect(24 + i * 7, 70 - i * 11, 28 + i * 7, 90, 1), d.accent);
    add(rect(603, 197, 617, 204, 2), d.accent);
  }
  if (d.shape === 'ticket')
    for (let y = 31; y < 254; y += 17) add(rect(47, y, 50, y + 8), d.accent);
  if (d.shape === 'frame') {
    for (const x of [21, 593]) for (const y of [21, 262]) add(rect(x, y, x + 26, y + 4), d.accent);
    for (const x of [21, 615]) for (const y of [21, 240]) add(rect(x, y, x + 4, y + 26), d.accent);
  }
  if (d.shape === 'bracket') {
    for (const x of [23, 610]) add(rect(x, 26, x + 7, 260), d.accent);
    for (const x of [23, 594]) for (const y of [26, 254]) add(rect(x, y, x + 23, y + 6), d.accent);
  }
  if (d.shape === 'pill') for (const x of [33, 606]) add(circle(x, 144, 7), d.accent);
  return png(
    width,
    height,
    layers.map((layer) => ({
      ...layer,
      mask: (x, y) => layer.mask((x * 640) / width, (y * 288) / height)
    }))
  );
}
const crcTable = Array.from({ length: 256 }, (_, n) => {
  for (let k = 0; k < 8; k++) n = n & 1 ? 0xedb88320 ^ (n >>> 1) : n >>> 1;
  return n >>> 0;
});
function chunk(kind, data) {
  const type = Buffer.from(kind),
    payload = Buffer.concat([type, data]);
  let crc = 0xffffffff;
  for (const byte of payload) crc = crcTable[(crc ^ byte) & 255] ^ (crc >>> 8);
  const size = Buffer.alloc(4),
    checksum = Buffer.alloc(4);
  size.writeUInt32BE(data.length);
  checksum.writeUInt32BE((crc ^ 0xffffffff) >>> 0);
  return Buffer.concat([size, payload, checksum]);
}
function png(width, height, layers) {
  const raw = Buffer.alloc(height * (width * 4 + 1));
  for (let y = 0; y < height; y++)
    for (let x = 0; x < width; x++) {
      const color = [0, 0, 0, 0];
      for (const dx of [0.25, 0.75])
        for (const dy of [0.25, 0.75]) {
          let sample = [0, 0, 0, 0];
          for (const layer of layers)
            if (layer.mask(x + dx, y + dy))
              sample =
                typeof layer.color === 'function' ? layer.color(x + dx, y + dy) : layer.color;
          for (let c = 0; c < 3; c++) color[c] += (sample[c] * sample[3]) / 255 / 4;
          color[3] += sample[3] / 4;
        }
      const offset = y * (width * 4 + 1) + 1 + x * 4;
      // Unassociated PNG RGB, including the antialiased edge.
      for (let c = 0; c < 3; c++)
        raw[offset + c] = color[3] ? Math.round((color[c] * 255) / color[3]) : 0;
      raw[offset + 3] = Math.round(color[3]);
    }
  const header = Buffer.alloc(13);
  header.writeUInt32BE(width);
  header.writeUInt32BE(height, 4);
  header[8] = 8;
  header[9] = 6;
  return Buffer.concat([
    Buffer.from('89504e470d0a1a0a', 'hex'),
    chunk('IHDR', header),
    chunk('IDAT', deflateSync(raw)),
    chunk('IEND', Buffer.alloc(0))
  ]);
}
// Material textures keep the original dimensions and mapping/atlas contract.
// These are new geometric motifs, never recolored reference pixels.
function materialArtwork(width, height, design, index) {
  const layers = [],
    add = (mask, hex) => layers.push({ mask, color: rgba(hex).map((c) => Math.round(c * 255)) });
  const cx = width / 2,
    cy = height / 2;
  const rgb = (hex) => rgba(hex).map((c) => Math.round(c * 255));
  const shade = (hex, amount) =>
    rgb(hex).map((c, i) => (i === 3 ? c : Math.max(0, Math.min(255, c + amount))));
  const surface = (mask, color) => layers.push({ mask, color });
  if (design.id === 'pattern-flower') {
    if ([1, 5, 6].includes(index)) {
      // These are real 2x2 / 4x4 atlas tiles, not a small icon in a full texture.
      // Folded mint paper panels make a coherent cut-paper title surround.
      const columns = index === 5 ? 4 : 2,
        cell = width / columns;
      const colors = ['#6bd3bb', '#2e858e', '#b5edce', '#528eac'];
      surface(
        () => true,
        (x, y) => {
          const col = Math.floor(x / cell),
            row = Math.floor(y / cell);
          const u = (x % cell) / cell,
            v = (y % cell) / cell;
          const color = colors[(col + row + index) % colors.length];
          return shade(color, u + v < 1 ? 10 : -13);
        }
      );
      add((x, y) => Math.abs((x % cell) + (y % cell) - cell) < 3, '#dbf7db');
    } else {
      const r = 39 + index * 2;
      add((x, y) => Math.abs(x - cx) + Math.abs(y - cy) < r, '#2a7182');
      add((x, y) => Math.abs(x - cx) + Math.abs(y - cy) < r - 6, '#b9edcc');
      add((x, y) => Math.abs(x - cx) + Math.abs(y - cy) < r - 14 && y < cy, '#6fcbb5');
    }
  } else if (design.id === 'flower-sunset') {
    // Continuous warm molten bands suit thick flame lettering and all of this
    // source package's glyph-distance-field textures. No cropped mascot stamp.
    const colors = ['#f6a535', '#e46627', '#ffe6a0', '#ff8133'];
    surface(
      () => true,
      (x, y) => {
        const flow = Math.sin(x * 0.027 + Math.sin(y * 0.018) * 2.4);
        return shade(colors[index], flow * 20 + Math.sin(y * 0.13 + x * 0.02) * 4);
      }
    );
    add((x, y) => Math.sin(x * 0.027 + Math.sin(y * 0.018) * 2.4) > 0.97, '#ffeac0');
  } else if (design.id === 'flower-paper' && index === 1) {
    // Dry marker underline for the original paper package's horizontal accent.
    add((x, y) => x > 54 && x < 455 && Math.abs(y - 256) < 18 + 3 * Math.sin(x * 0.17), '#bc7959');
    add((x, y) => x > 72 && x < 438 && Math.abs(y - 254) < 8 + Math.sin(x * 0.11) * 3, '#dcac87');
    add((x, y) => x > 72 && x < 438 && Math.abs(y - 245) < 1.5 && Math.sin(x * 0.4) > 0, '#f4d6b5');
  } else if (design.id === 'flower-pop') {
    // Comic burst silhouette and regular halftone printing belong to one theme.
    const edge = (x, y, inset = 0) => {
      const angle = Math.atan2(y - cy, x - cx);
      const radius = width * (0.36 + 0.055 * Math.cos(12 * angle));
      return Math.hypot(x - cx, y - cy) < radius - inset;
    };
    add(edge, '#3a2549');
    add((x, y) => edge(x, y, 9), '#fff1bd');
    add((x, y) => edge(x, y, 17), '#f26b8b');
    add((x, y) => edge(x, y, 22) && Math.hypot((x % 25) - 12.5, (y % 25) - 12.5) < 3, '#e14c79');
    add((x, y) => edge(x, y, 40) && y < cy - 90, '#fa9cac');
  } else if (design.id === 'flower-ink') {
    if (index === 0) {
      const body = (x, y) =>
        rect(20, 20, width - 20, height - 20, 70)(x, y) &&
        x > 24 + 5 * Math.sin(y * 0.19) &&
        x < width - 24 + 5 * Math.sin(y * 0.23) &&
        y > 24 + 5 * Math.sin(x * 0.18) &&
        y < height - 24 + 5 * Math.sin(x * 0.14);
      surface(body, (x, y) => shade('#253d3a', 3 * Math.sin(x * 0.36) + 3 * Math.sin(y * 0.19)));
      add((x, y) => body(x, y) && Math.abs(y - 60 - 5 * Math.sin(x * 0.03)) < 2, '#708e7d');
    } else {
      surface(
        () => true,
        (x, y) => shade('#f5efda', 2 * Math.sin(x * 0.7) + 2 * Math.sin(y * 0.51))
      );
      add((x, y) => (x + 0.3 * y) % 83 < 0.7, '#e3dbc5');
    }
  } else if (design.id === 'flower-frost' && index === 1) {
    // Ice bevel around the original four-cell outline atlas.
    const outer = rect(24, 24, width - 24, height - 24, 24),
      inner = rect(41, 41, width - 41, height - 41, 16);
    surface(
      (x, y) => outer(x, y) && !inner(x, y),
      (x, y) => shade('#89b7d3', (width - x - y) * 0.075)
    );
    add((x, y) => outer(x, y) && !rect(29, 29, width - 29, height - 29, 20)(x, y), '#dff9ff');
  } else if (design.id === 'flower-gold' && index === 2) {
    const edge = (x, y) => Math.hypot(x - cx, y - cy) < width * 0.4;
    add(edge, '#a97534');
    add(circle(cx, cy, width * 0.38), '#dfba75');
    add(circle(cx, cy, width * 0.36), '#bc975c');
  } else {
    throw new Error(`Missing scene-specific artwork: ${design.id}/asset-${index}`);
  }
  return png(width, height, layers);
}
const assetSize = async (source, file) => {
  const bytes = await readFile(new URL(`com.videocut.text.qt-type.${source}/${file}`, references));
  return [bytes.readUInt32BE(16), bytes.readUInt32BE(20)];
};
async function packedVideo(path, design, metadata) {
  const { render_size: size, fps, frame_count: frames } = metadata;
  const proc = spawn(
    process.env.VIDEOCUT_FFMPEG || 'ffmpeg',
    [
      '-y',
      '-v',
      'error',
      '-f',
      'rawvideo',
      '-pixel_format',
      'rgb24',
      '-video_size',
      `${size * 2}x${size}`,
      '-framerate',
      String(fps),
      '-i',
      'pipe:0',
      '-an',
      '-c:v',
      'libx264',
      '-preset',
      'fast',
      '-crf',
      '16',
      '-pix_fmt',
      'yuv420p',
      path
    ],
    { stdio: ['pipe', 'ignore', 'pipe'] }
  );
  let error = '';
  proc.stderr.on('data', (data) => (error += data));
  const complete = once(proc, 'close');
  const colors = design.fill.map((hex) =>
    rgba(hex)
      .slice(0, 3)
      .map((v) => v * 255)
  );
  // Each glyph keeps its packed-alpha animated follower at the same authored
  // position/scale. Fresh flowing rings and sparks replace the reference stars.
  for (let frame = 0; frame < frames; frame++) {
    const data = Buffer.alloc(size * size * 6),
      t = frame / (frames - 1);
    for (let y = 0; y < size; y++)
      for (let x = 0; x < size; x++) {
        const dx = x - size * 0.5,
          dy = y - size * 0.5,
          r = Math.hypot(dx, dy),
          angle = Math.atan2(dy, dx);
        const radius = size * (0.21 + 0.045 * Math.sin(angle * 3 + t * 6.28));
        const distance = Math.abs(r - radius);
        let a = Math.max(0, 1 - distance / 7) * 0.72;
        const orbit = (angle + t * 6.28 + Math.PI * 4) % (Math.PI * 2);
        if (distance < 19 && orbit < 0.2) a = Math.max(a, (1 - distance / 19) * (1 - orbit / 0.2));
        const shimmer =
          Math.pow(Math.max(0, Math.sin(angle * 17 + t * 9)), 12) *
          Math.max(0, 1 - Math.abs(r - size * 0.33) / 12);
        a = Math.max(a, shimmer * 0.82);
        a *= Math.sin(Math.PI * Math.min(1, t * 1.08));
        const color = colors[(Math.floor((angle + Math.PI) * 3) + (frame % 2)) % colors.length];
        const alphaOffset = (y * size * 2 + x) * 3,
          colorOffset = alphaOffset + size * 3;
        for (let c = 0; c < 3; c++) {
          data[alphaOffset + c] = Math.round(a * 255);
          data[colorOffset + c] = Math.round(color[c] * a);
        }
      }
    if (!proc.stdin.write(data)) await once(proc.stdin, 'drain');
  }
  proc.stdin.end();
  const [code] = await complete;
  if (code !== 0) throw new Error(`Authored follower encoding failed: ${error}`);
}
const written = new Map();
async function save(design, base, options = {}) {
  const dir = new URL(`com.videocut.text.qt-type.${base}/`, root);
  await mkdir(dir, { recursive: true });
  let documents = await inherited(design, base);
  const m = documents['manifest.json'];
  const assets = new Map();
  for (const file of m.files.filter((f) => f.role === 'resource')) {
    const path = new URL(file.path, dir);
    await mkdir(new URL('./', path), { recursive: true });
    if (file.path.endsWith('.png')) {
      const [w, h] = await assetSize(design.source, file.path);
      let data;
      if (design.category === 'bubble') data = artwork(design, w, h);
      else {
        const authored = new URL(
          `${design.id}/${file.path.replace('assets/', '')}`,
          new URL('../packages/text-wasm/presets/artwork/', import.meta.url)
        );
        try {
          data = await readFile(authored);
        } catch (error) {
          if (error.code !== 'ENOENT') throw error;
          data = materialArtwork(w, h, design, Number(file.path.match(/(\d+)\.png$/)?.[1] || 0));
        }
        if (data.readUInt32BE(16) !== w || data.readUInt32BE(20) !== h)
          throw new Error(`${design.id}: replacement dimensions must stay ${w}x${h}`);
      }
      assets.set(file.path, { bytes: data, mediaType: file.media_type });
    } else if (file.path.endsWith('.mp4')) {
      const meta = await readJson(design.source, 'assets/asset-001.json');
      await packedVideo(path.pathname, design, meta.meta.videocut_packed_alpha);
      assets.set(file.path, { bytes: await readFile(path), mediaType: file.media_type });
      const length = assets.get(file.path).bytes.length;
      meta.meta.videocut_packed_alpha.byte_length = length;
      meta.meta.videocut_resource_closure.resources.forEach((r) => (r.byte_length = length));
      assets.set('assets/asset-001.json', {
        bytes: Buffer.from(JSON.stringify(meta, null, 2) + '\n'),
        mediaType: 'application/json'
      });
    }
  }
  if (!design.base && design.category === 'animation') {
    documents['effect.program.json'] = motionProgram(base, design.animation);
    const loop = ['wave', 'breathe', 'swing'].includes(design.animation);
    const driver = documents['animation.ir.json'].animation.layers[0].time_driver;
    if (loop) {
      driver.kind = 'loop_phase';
      driver.playback = 'loop';
    }
  }
  if (!design.base && design.category === 'flower' && design.backdrop) {
    const backdropDesign = {
      ...presetDesigns.find((d) => d.category === 'bubble' && d.source === design.backdrop),
      paper: design.fill[0],
      edge: design.depth,
      fill: [design.depth],
      accent: design.outline
    };
    const bg = await inherited(backdropDesign, design.backdrop);
    const [w, h] = await assetSize(design.backdrop, 'assets/asset-000.png');
    const backdropAssets = new Map([
      ['assets/asset-000.png', { bytes: artwork(backdropDesign, w, h), mediaType: 'image/png' }]
    ]);
    const motion = await inherited(
      { ...design, source: 'anim-lua-letter-transform' },
      'anim-lua-letter-transform'
    );
    const { bundle } = await composeRecipe(
      { base: design.source, backdrop: design.backdrop, animation: 'anim-lua-letter-transform' },
      async (part) =>
        part === design.source
          ? {
              composition: documents['composition.json'],
              animation: documents['animation.ir.json'],
              effectProgram: documents['effect.program.json'],
              assets
            }
          : part === design.backdrop
            ? {
                composition: bg['composition.json'],
                animation: bg['animation.ir.json'],
                effectProgram: bg['effect.program.json'],
                assets: backdropAssets
              }
            : {
                composition: motion['composition.json'],
                animation: motion['animation.ir.json'],
                effectProgram: motion['effect.program.json'],
                assets: new Map()
              }
    );
    documents['composition.json'] = bundle.composition;
    documents['animation.ir.json'] = bundle.animation;
    documents['effect.program.json'] = bundle.effectProgram;
    m.files.push({
      path: 'backdrop/assets/asset-000.png',
      role: 'resource',
      media_type: 'image/png'
    });
  }
  for (const [file, asset] of assets) {
    const path = new URL(file, dir);
    await mkdir(new URL('./', path), { recursive: true });
    await writeFile(path, asset.bytes);
  }
  for (const [file, value] of Object.entries(documents))
    await writeFile(new URL(file, dir), JSON.stringify(value, null, 2) + '\n');
  written.set(base, { documents, assets });
  console.log(`${design.id}: inherited ${design.source}, ${assets.size} visual assets`);
}
// Components first; the two historical recipes continue to combine them at runtime.
await save(
  {
    ...presetDesigns.find((d) => d.id === 'bubble-mint'),
    id: 'marquee-backing',
    shape: 'pill',
    paper: '#80454b',
    edge: '#f5c790',
    accent: '#b06866',
    source: 'bubble-nine-slice'
  },
  'bubble-nine-slice'
);
await save(
  {
    ...presetDesigns.find((d) => d.id === 'bubble-note'),
    id: 'collage-backing',
    shape: 'frame',
    paper: '#d7f1db',
    edge: '#28676c',
    accent: '#7dbeaa',
    source: 'bubble-tile'
  },
  'bubble-tile'
);
await save(
  { ...presetDesigns[0], source: 'anim-lua-letter-transform', category: 'animation' },
  'anim-lua-letter-transform'
);
for (const [i, design] of presetDesigns.entries()) await save(design, authoredRecipes[i].base);
console.log(
  'Generated 30 resource-backed presets while preserving the inherited template structure.'
);
