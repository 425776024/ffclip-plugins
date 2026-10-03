import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { inflateSync } from 'node:zlib';
import { mkdtemp, mkdir, readFile, writeFile, rm, realpath } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, relative, isAbsolute } from 'node:path';
import { HtmlFrameRenderer } from '../packages/server/html-renderer.mjs';
import { Chromium, findChromium } from '../packages/server/chromium.mjs';
import { importHtml } from '../packages/server/html-import.mjs';
import { ticks, validateHtmlContent } from '../packages/core/project.mjs';

const html = (source, options = {}) => ({
  html: source,
  width: 160,
  height: 120,
  duration: ticks(4),
  transparent: true,
  ...options
});

// Chrome PNG screenshots are 8-bit RGB/RGBA. Decode all PNG scanline filters so
// assertions inspect real raster pixels without requiring an external codec.
function pixels(png) {
  assert.equal(png.subarray(0, 8).toString('hex'), '89504e470d0a1a0a');
  let width, height, channels;
  const idat = [];
  for (let offset = 8; offset < png.length;) {
    const length = png.readUInt32BE(offset),
      type = png.toString('ascii', offset + 4, offset + 8),
      data = png.subarray(offset + 8, offset + 8 + length);
    if (type === 'IHDR') {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      assert.equal(data[8], 8);
      assert.equal(data[12], 0);
      channels = { 2: 3, 6: 4 }[data[9]];
      assert.ok(channels, `Unsupported screenshot color type ${data[9]}`);
    }
    if (type === 'IDAT') idat.push(data);
    offset += length + 12;
  }
  const raw = inflateSync(Buffer.concat(idat)),
    stride = width * channels,
    decoded = Buffer.alloc(stride * height);
  const paeth = (a, b, c) => {
    const p = a + b - c,
      pa = Math.abs(p - a),
      pb = Math.abs(p - b),
      pc = Math.abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
  };
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)];
    assert.ok(filter <= 4);
    for (let x = 0; x < stride; x++) {
      const index = y * stride + x,
        left = x >= channels ? decoded[index - channels] : 0,
        up = y ? decoded[index - stride] : 0,
        corner = y && x >= channels ? decoded[index - stride - channels] : 0,
        value = [0, left, up, Math.floor((left + up) / 2), paeth(left, up, corner)][filter];
      decoded[index] = raw[y * (stride + 1) + 1 + x] + value;
    }
  }
  const rgba = Buffer.alloc(width * height * 4);
  for (let i = 0; i < width * height; i++) {
    decoded.copy(rgba, i * 4, i * channels, i * channels + 3);
    rgba[i * 4 + 3] = channels === 4 ? decoded[i * channels + 3] : 255;
  }
  return {
    width,
    height,
    rgba,
    at: (x, y) => [...rgba.subarray((y * width + x) * 4, (y * width + x + 1) * 4)]
  };
}

let chromium;
try {
  chromium = await findChromium();
} catch {}
const chromeOptions = {
  skip: !chromium && 'Chrome/Chromium unavailable; raster runtime was not tested',
  timeout: 60000
};

test(
  'real Chromium retires a failed context transport and both capture lanes recover exact pixels',
  chromeOptions,
  async (t) => {
    const browsers = [];
    const renderer = new HtmlFrameRenderer({
      browserFactory: async () => {
        const browser = await new Chromium().start();
        browsers.push(browser);
        if (browsers.length === 1) {
          const send = browser.socket.send.bind(browser.socket);
          // Suppress this packet to exercise the production 15-second CDP
          // timeout on a real transport, followed by a real browser restart.
          browser.socket.send = (message) => {
            if (JSON.parse(message).method !== 'Target.createBrowserContext') send(message);
          };
        }
        return browser;
      }
    });
    t.after(() => renderer.close());
    const content = html(
      '<div id="dot" style="position:absolute;top:20px;width:20px;height:20px;background:rgba(255,0,0,.5)"></div><script>window.tick=t=>dot.style.left=(t*80)+"px"</script>'
    );
    const frames = await Promise.all([
      renderer.capture(content, ticks(0.25)),
      renderer.capture(content, ticks(0.5))
    ]);
    assert.equal(browsers.length, 2, 'Two lanes share one replacement process');
    assert.equal(browsers[0].failure.code, 'CHROMIUM_TIMEOUT');
    assert.equal(browsers[0].failure.method, 'Target.createBrowserContext');
    assert.ok(
      browsers[0].process.exitCode !== null || browsers[0].process.signalCode !== null,
      'The failed owned process exited'
    );
    for (const [index, time] of [0.25, 0.5].entries()) {
      const image = pixels(frames[index].png);
      assert.equal(frames[index].time, ticks(time));
      assert.deepEqual(image.at(time * 80 + 5, 25), [255, 0, 0, 128]);
      assert.deepEqual(image.at(time * 80 - 5, 25), [0, 0, 0, 0]);
    }
  }
);

test(
  'real Chromium context reuse resets script globals and keeps contexts/targets bounded during source churn',
  chromeOptions,
  async (t) => {
    const renderer = new HtmlFrameRenderer();
    t.after(() => renderer.close());
    for (let batch = 0; batch < 12; batch++) {
      const contents = [batch * 2, batch * 2 + 1].map((index) =>
        html(
          '<div id="dot" style="position:absolute;top:20px;width:20px;height:20px;background:red"></div><script>window.counter=(window.counter||0)+1;window.tick=()=>dot.style.left=variables.left+"px"</script>',
          { variables: { left: 20 + index } }
        )
      );
      const frames = await Promise.all(contents.map((content) => renderer.capture(content, 0)));
      for (const [index, frame] of frames.entries())
        assert.deepEqual(pixels(frame.png).at(25 + batch * 2 + index, 25), [255, 0, 0, 255]);
    }
    const { browserContextIds } = await renderer.browser.send('Target.getBrowserContexts');
    assert.equal(browserContextIds.length, 8);
    assert.equal(renderer.pages.size, 8);
    const { targetInfos } = await renderer.browser.send('Target.getTargets');
    // Chrome also reports its own omnibox/browser_ui targets per context.
    // Only authored page targets must match the renderer's live DOM slots.
    const targets = targetInfos.filter(
      (target) => target.type === 'page' && browserContextIds.includes(target.browserContextId)
    );
    assert.equal(targets.length, 8, 'An evicted DOM target is closed before its context is reused');
    assert.deepEqual(
      targets.map((target) => target.targetId).sort(),
      [...renderer.pages.values()].map((page) => page.targetId).sort()
    );
    for (const page of renderer.pages.values())
      assert.equal(await renderer.evaluate(page.sessionId, 'window.counter'), 1);
  }
);

test('HTML capture bounds concurrency and serializes each independent lane', async () => {
  assert.throws(() => new HtmlFrameRenderer({ concurrency: 3 }), RangeError);
  const renderer = new HtmlFrameRenderer();
  const active = new Set(),
    started = [],
    release = [];
  renderer.frame = async (_content, time, _signal, lane) => {
    assert.equal(active.has(lane), false, 'A document cannot seek during its screenshot');
    active.add(lane);
    started.push({ time, lane });
    await new Promise((resolve) => release.push(resolve));
    active.delete(lane);
    return time;
  };
  const pending = [0, 1, 2].map((time) => renderer.capture({}, time));
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(started, [
    { time: 0, lane: 0 },
    { time: 1, lane: 1 }
  ]);
  assert.equal(active.size, 2);
  release[0]();
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(started[2], { time: 2, lane: 0 });
  release[1]();
  release[2]();
  assert.deepEqual(await Promise.all(pending), [0, 1, 2]);
  await renderer.close();
  await assert.rejects(renderer.capture({}, 3), /已关闭/);
});

test(
  'concurrent Chromium captures keep exact times, alpha and queued cancellation',
  chromeOptions,
  async (t) => {
    const renderer = new HtmlFrameRenderer();
    t.after(() => renderer.close());
    const content =
      html(`<style>#dot{position:absolute;top:20px;width:20px;height:20px;background:rgba(255,0,0,.5)}</style>
    <div id="dot"></div><script>window.calls=[];window.tick=t=>{calls.push(t);dot.style.left=(t*80)+'px'};</script>`);
    const times = [0.25, 0.5, 0.75, 1];
    const requests = times.map((time) => renderer.capture(content, ticks(time)));
    const cancelled = new AbortController();
    const discarded = assert.rejects(renderer.capture(content, ticks(2.5), cancelled.signal), {
      name: 'AbortError'
    });
    cancelled.abort();
    const frames = await Promise.all(requests);
    await discarded;
    assert.equal(renderer.pages.size, 2);
    for (let i = 0; i < frames.length; i++) {
      const image = pixels(frames[i].png),
        left = times[i] * 80;
      assert.equal(frames[i].time, ticks(times[i]));
      assert.deepEqual(image.at(left + 5, 25), [255, 0, 0, 128]);
      assert.deepEqual(image.at(left - 5, 25), [0, 0, 0, 0]);
    }
    const calls = (
      await Promise.all(
        [...renderer.pages.values()].map((page) =>
          renderer.evaluate(page.sessionId, 'window.calls')
        )
      )
    )
      .flat()
      .sort((a, b) => a - b);
    assert.deepEqual(calls, times, 'An aborted queued frame never executes its authored tick');
  }
);

test(
  'unchanged DOM frames reuse pixels across times and CSSOM changes invalidate them',
  chromeOptions,
  async (t) => {
    const renderer = new HtmlFrameRenderer();
    t.after(() => renderer.close());
    const content =
      html(`<style>#dot{position:absolute;width:20px;height:20px;background:red}</style><div id="dot"></div>
    <script>window.tick=t=>{if(t>=2)document.styleSheets[1].cssRules[0].style.background='blue'};</script>`);
    const first = await renderer.capture(content, 0);
    const held = await renderer.capture(content, ticks(1));
    assert.equal(held.reused, true);
    assert.equal(held.png, first.png);
    assert.equal(held.stateKey, first.stateKey);
    const changed = await renderer.capture(content, ticks(2));
    assert.equal(changed.cached, false);
    assert.notEqual(changed.stateKey, first.stateKey);
    assert.deepEqual(pixels(changed.png).at(5, 5), [0, 0, 255, 255]);
  }
);

test(
  'Chromium seeks registered, detached and global GSAP timelines deterministically',
  chromeOptions,
  async (t) => {
    const renderer = new HtmlFrameRenderer();
    t.after(() => renderer.close());
    const content =
      html(`<style>.box{position:absolute;left:0;width:20px;height:20px;background:#ff0000}#global{top:40px}#detached{top:80px}</style>
    <div class="box" id="registered"></div><div class="box" id="global"></div><div class="box" id="detached"></div>
    <script>
      window.__timelines.main=gsap.timeline({paused:true}).to('#registered',{x:80,duration:1,ease:'none'});
      const globalTween=gsap.to('#global',{x:80,duration:1,ease:'none'});gsap.globalTimeline.add(globalTween,0);
      const detached=gsap.timeline({paused:true}).to('#detached',{x:80,duration:1,ease:'none'});
      gsap.globalTimeline.remove(detached);window.__timelines.detached=detached;
    </script>`);
    const forward = pixels((await renderer.capture(content, ticks(0.5))).png);
    for (const y of [10, 50, 90]) {
      assert.deepEqual(forward.at(45, y), [255, 0, 0, 255], `GSAP at half-time row ${y}`);
      assert.equal(forward.at(5, y)[3], 0);
    }
    const repeated = await renderer.capture(content, ticks(0.5));
    assert.equal(repeated.cached, true);
    assert.deepEqual(pixels(repeated.png).rgba, forward.rgba);
    await renderer.capture(content, ticks(1.5));
    const backward = pixels((await renderer.capture(content, ticks(0.5))).png);
    assert.deepEqual(
      backward.rgba,
      forward.rgba,
      'A backward seek re-renders the exact forward pixels'
    );
    const zero = pixels((await renderer.capture(content, 0)).png);
    for (const y of [10, 50, 90]) {
      assert.deepEqual(zero.at(5, y), [255, 0, 0, 255]);
      assert.equal(zero.at(45, y)[3], 0);
    }
  }
);

test(
  'Chromium production SKILL entrance is pixel-identical after forward and backward seeks',
  chromeOptions,
  async (t) => {
    const renderer = new HtmlFrameRenderer();
    t.after(() => renderer.close());
    const source = await readFile(
      new URL(
        '../plugins/videocut-local/skills/motion-templates/assets/gsap-lower-third.html',
        import.meta.url
      ),
      'utf8'
    );
    for (const [variant, authorSource] of [
      ['production', source],
      ['runtime-default', source.replace(/gsap\.defaults\(\{\s*force3D:\s*false\s*\}\);?/g, '')]
    ]) {
      const content = html(authorSource, {
        width: 1920,
        height: 1080,
        duration: ticks(5),
        variables: {
          title: '同一条时间轴，更多表达',
          subtitle: '原生花字与 HTML · 自由剪辑',
          eyebrow: 'SKILL MOTION TEST',
          accent: '#f3d36a'
        }
      });
      const first = pixels((await renderer.capture(content, ticks(0.2))).png);
      await renderer.capture(content, ticks(2));
      await renderer.capture(content, ticks(4.8));
      renderer.frames.clear(); // Exercise the DOM renderer, independently of retained PNGs.
      const backward = pixels((await renderer.capture(content, ticks(0.2))).png);
      const signature = (image) => createHash('sha256').update(image.rgba).digest('hex');
      assert.equal(
        signature(backward),
        signature(first),
        'Every RGBA pixel in the first entrance frame survives a later exit and backward seek'
      );
      assert.deepEqual(first.at(0, 0), [0, 0, 0, 0]);
      assert.ok(
        first.rgba.some((value, index) => index % 4 === 3 && value),
        'The entrance frame contains visible template pixels'
      );
      const repeated = await renderer.capture(content, ticks(0.2));
      assert.equal(repeated.cached, true);
      assert.equal(signature(pixels(repeated.png)), signature(first));
      t.diagnostic(
        JSON.stringify({
          template: 'gsap-lower-third.html',
          variant,
          sampleTimes: [0.2, 2, 4.8, 0.2],
          rgbaDigest: signature(first),
          matching: true
        })
      );
    }
  }
);

test(
  'Chromium explicit authored 3D transforms still render with the deterministic 2D default',
  chromeOptions,
  async (t) => {
    const renderer = new HtmlFrameRenderer();
    t.after(() => renderer.close());
    const content =
      html(`<style>body{perspective:400px}#box{position:absolute;left:30px;top:30px;width:80px;height:60px;background:#ff8800}</style><div id="box"></div><script>
      window.__timelines.threeD=gsap.timeline({paused:true}).fromTo('#box',{rotationY:0,z:0},{rotationY:60,z:40,force3D:true,duration:1,ease:'none'});
      </script>`);
    const image = pixels((await renderer.capture(content, ticks(0.5))).png),
      page = [...renderer.pages.values()][0];
    const transform = await renderer.evaluate(
      page.sessionId,
      "getComputedStyle(document.querySelector('#box')).transform"
    );
    assert.match(transform, /^matrix3d\(/);
    assert.ok(
      image.rgba.some((value, index) => index % 4 === 3 && value),
      'Explicit 3D content produces raster pixels'
    );
    const before = createHash('sha256').update(image.rgba).digest('hex');
    await renderer.capture(content, ticks(1.5));
    renderer.frames.clear();
    const backward = pixels((await renderer.capture(content, ticks(0.5))).png);
    assert.equal(createHash('sha256').update(backward.rgba).digest('hex'), before);
  }
);

test(
  'Chromium rewinds finite CSS and WAAPI animations after their unfilled completion',
  chromeOptions,
  async (t) => {
    const renderer = new HtmlFrameRenderer();
    t.after(() => renderer.close());
    const content = html(`<style>
    .box{position:absolute;left:0;width:20px;height:20px;background:#00ff00}#waapi{top:40px}
    @keyframes move{from{transform:translateX(0)}to{transform:translateX(80px)}}
    #css{animation:move 1s linear 0s 1 normal none}
    </style><div class="box" id="css"></div><div class="box" id="waapi"></div>
    <script>document.querySelector('#waapi').animate([{transform:'translateX(0)'},{transform:'translateX(80px)'}],{duration:1000,fill:'none'});</script>`);
    const half = pixels((await renderer.capture(content, ticks(0.5))).png);
    for (const y of [10, 50]) assert.deepEqual(half.at(45, y), [0, 255, 0, 255]);
    const after = pixels((await renderer.capture(content, ticks(2))).png);
    for (const y of [10, 50]) assert.deepEqual(after.at(5, y), [0, 255, 0, 255]);
    const backward = pixels((await renderer.capture(content, ticks(0.5))).png);
    assert.deepEqual(backward.rgba, half.rgba);
    const delayed = html(
      content.html
        .replace('move 1s', 'move .02s')
        .replace('duration:1000', 'duration:20')
        .replace(
          '</script>',
          'window.__videocutReady=new Promise(resolve=>setTimeout(resolve,120));</script>'
        )
    );
    const first = pixels((await renderer.capture(delayed, ticks(0.01))).png);
    for (const y of [10, 50])
      assert.deepEqual(
        first.at(45, y),
        [0, 255, 0, 255],
        'Short animation survives asynchronous initialization'
      );
  }
);

test(
  'Chromium tick renders Canvas, template variables and transparent pixels',
  chromeOptions,
  async (t) => {
    const renderer = new HtmlFrameRenderer();
    t.after(() => renderer.close());
    const source = `<canvas width="160" height="120"></canvas><script>
    window.tick=(time,context)=>{const c=document.querySelector('canvas').getContext('2d');c.clearRect(0,0,160,120);c.fillStyle=window.variables.color;c.fillRect(time*40,10,20,20);};
    </script>`;
    const content = html(source, { variables: { color: '#3366ff' } });
    const frame = await renderer.capture(content, ticks(1)),
      image = pixels(frame.png);
    assert.equal(image.width, 160);
    assert.equal(image.height, 120);
    assert.deepEqual(image.at(45, 15), [51, 102, 255, 255]);
    assert.deepEqual(image.at(5, 5), [0, 0, 0, 0]);
    const changed = pixels(
      (await renderer.capture({ ...content, variables: { color: '#ff6600' } }, ticks(1))).png
    );
    assert.deepEqual(changed.at(45, 15), [255, 102, 0, 255]);
    const opaque = pixels(
      (await renderer.capture({ ...content, transparent: false }, ticks(1))).png
    );
    assert.deepEqual(opaque.at(5, 5), [0, 0, 0, 255]);
    assert.equal((await renderer.capture(content, ticks(6))).time, ticks(4) - 1);
    assert.equal((await renderer.capture(content, -1)).time, 0);
    const abort = new AbortController();
    abort.abort();
    await assert.rejects(renderer.capture(content, 0, abort.signal), { name: 'AbortError' });
  }
);

test(
  'Chromium surfaces initialization/tick errors and prevents embedded media/network scripts',
  chromeOptions,
  async (t) => {
    const renderer = new HtmlFrameRenderer();
    t.after(() => renderer.close());
    await assert.rejects(
      renderer.capture(html('<script>throw Error("authored script failed")</script>'), 0),
      /authored script failed/
    );
    await assert.rejects(
      renderer.capture(html('<link rel="stylesheet" href="https://example.com/theme.css">'), 0),
      /资源(?:被阻止|加载失败)/
    );
    await assert.rejects(
      renderer.capture(
        html(
          '<script>window.tick=()=>document.body.style.backgroundImage="url(https://example.com/photo.jpg)";</script>'
        ),
        0
      ),
      /资源(?:被阻止|加载失败)/
    );
    await assert.rejects(
      renderer.capture(html('<script>window.tick=()=>{throw Error("broken tick")}</script>'), 0),
      /broken tick/
    );
    await assert.rejects(
      renderer.capture(
        html('<script>window.__videocutReady=Promise.reject(Error("broken setup"))</script>'),
        0
      ),
      /broken setup/
    );
    await assert.rejects(
      renderer.capture(html('<script src="https://example.com/app.js"></script>'), 0),
      /内联/
    );
    await assert.rejects(renderer.capture(html('<video></video>'), 0), /音视频/);
    const dynamic = html(
      '<script>window.tick=()=>document.body.innerHTML="<video></video>"</script>'
    );
    await assert.rejects(renderer.capture(dynamic, 0), /音视频/);
    assert.equal(renderer.pages.size, 0, 'Broken pages are not retained');
    const safe = await renderer.capture(
      html('<div style="width:10px;height:10px;background:red"></div>'),
      0
    );
    assert.deepEqual(pixels(safe.png).at(5, 5), [255, 0, 0, 255]);
  }
);

test('HTML import freezes local scripts/styles/images and validates self-contained resources', async (t) => {
  const directory = await realpath(await mkdtemp(join(tmpdir(), 'videocut-html-import-test-')));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const allowed = async (path) => {
    const canonical = await realpath(path),
      rel = relative(directory, canonical);
    if (rel === '..' || rel.startsWith('../') || isAbsolute(rel))
      throw new Error('Outside authorized root');
    return canonical;
  };
  await mkdir(join(directory, 'styles'));
  await writeFile(
    join(directory, 'dot.svg'),
    '<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><path fill="red" d="M0 0h20v20H0z"/></svg>'
  );
  await writeFile(
    join(directory, 'styles', 'theme.css'),
    '.logo{background-image:url("../dot.svg");width:20px;height:20px}'
  );
  await writeFile(
    join(directory, 'motion.js'),
    'window.tick=(t)=>document.body.dataset.time=String(t)'
  );
  const path = join(directory, 'clip.html');
  await writeFile(
    path,
    '<link rel="stylesheet" href="styles/theme.css"><div class="logo"></div><img src="dot.svg"><script src="https://cdn.jsdelivr.net/npm/gsap/dist/gsap.min.js"></script><script src="motion.js"></script>'
  );
  const imported = await importHtml(
    path,
    { width: 160, height: 120, duration: ticks(2), transparent: true },
    allowed
  );
  assert.doesNotThrow(() => validateHtmlContent(imported.html));
  assert.match(imported.html.html, /data:image\/svg\+xml;base64,/);
  assert.match(imported.html.html, /window\.tick/);
  assert.doesNotMatch(
    imported.html.html,
    /src="motion\.js"|href="styles\/theme\.css"|cdn\.jsdelivr/
  );
  await writeFile(path, '<img src="https://example.com/remote.png">');
  await assert.rejects(importHtml(path, {}, allowed), /本地授权/);
  await writeFile(path, '<img srcset="dot.svg 1x">');
  await assert.rejects(importHtml(path, {}, allowed), /srcset/);
  await writeFile(path, '<style>@import "styles/theme.css";</style>');
  await assert.rejects(importHtml(path, {}, allowed), /@import/);
  await writeFile(path, '<audio src="dot.svg"></audio>');
  await assert.rejects(importHtml(path, {}, allowed), /音视频/);
});

test(
  'HTML capture preserves original pixels, DOM viewport and alpha on cached seeks',
  chromeOptions,
  async (t) => {
    const renderer = new HtmlFrameRenderer();
    t.after(() => renderer.close());
    const content = html(
      `<style>#dot{position:absolute;top:20px;width:20px;height:20px;background:rgba(255,0,0,.5)}</style><div id="dot"></div><script>window.tick=()=>{dot.style.left=(innerWidth/2)+'px';};</script>`
    );
    const full = pixels((await renderer.capture(content, ticks(1))).png);
    assert.equal(full.width, 160);
    assert.equal(full.height, 120);
    assert.deepEqual(full.at(85, 25), [255, 0, 0, 128]);
    assert.deepEqual(full.at(45, 25), [0, 0, 0, 0]);
    const nativeAgain = await renderer.capture(content, ticks(1));
    assert.equal(nativeAgain.cached, true);
    assert.deepEqual(pixels(nativeAgain.png).rgba, full.rgba);
  }
);

test('Chromium warmed capture latency at preview and full HD extents', chromeOptions, async (t) => {
  const renderer = new HtmlFrameRenderer();
  t.after(() => renderer.close());
  for (const [width, height] of [
    [640, 360],
    [1920, 1080]
  ]) {
    const content = html(
      `<style>main{font:700 52px sans-serif;color:white;padding:30px;background:linear-gradient(120deg,#3344ee,#8822aa)}</style>
      <main>VideoCut HTML</main><script>window.__timelines.title=gsap.timeline({paused:true}).fromTo('main',{x:0,opacity:0.3},{x:100,opacity:1,duration:2,ease:'none'});</script>`,
      { width, height }
    );
    for (let index = 0; index < 6; index++) await renderer.capture(content, ticks(index / 30));
    const samples = [];
    for (let index = 6; index < 18; index++) {
      const frame = await renderer.capture(content, ticks(index / 30));
      assert.equal(frame.cached, false);
      samples.push(frame.ms);
    }
    const ordered = [...samples].sort((a, b) => a - b),
      average = samples.reduce((a, b) => a + b, 0) / samples.length,
      p95 = ordered[Math.ceil(ordered.length * 0.95) - 1];
    t.diagnostic(
      JSON.stringify({
        width,
        height,
        warmedSamples: samples.length,
        averageMs: +average.toFixed(2),
        p95Ms: +p95.toFixed(2),
        scope: 'Chromium seek and PNG capture only'
      })
    );
  }
});

test(
  'retained original HTML frames reuse backward seeks and identical sources without recapture',
  chromeOptions,
  async (t) => {
    const renderer = new HtmlFrameRenderer();
    t.after(() => renderer.close());
    const content = html(
      '<div style="color:red">Exact</div><script>window.tick=t=>document.body.style.opacity=String(.25+t/4)</script>'
    );
    const first = await renderer.capture(content, ticks(0.2));
    await renderer.capture(content, ticks(0.8));
    const same = await renderer.capture(structuredClone(content), ticks(0.2));
    assert.equal(same.cached, true);
    assert.equal(same.png, first.png);
    assert.equal(pixels(same.png).width, content.width);
    const changed = await renderer.capture(
      { ...content, variables: { changed: true } },
      ticks(0.2)
    );
    assert.equal(changed.cached, false);
    const aborted = new AbortController();
    aborted.abort();
    await assert.rejects(renderer.capture(content, ticks(0.2), aborted.signal), {
      name: 'AbortError'
    });
    renderer.frames.clear();
    const recaptured = await renderer.capture(content, ticks(0.2));
    assert.equal(recaptured.cached, false);
    assert.deepEqual(pixels(recaptured.png).rgba, pixels(first.png).rgba);
  }
);
