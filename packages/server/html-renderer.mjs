import { readFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { Chromium, findChromium } from './chromium.mjs';
import { parse, serialize } from 'parse5';
import { HtmlFrameCache } from './html-frame-cache.mjs';

const runtimeFile = fileURLToPath(new URL('../../dist/gsap-runtime.js', import.meta.url));
let gsapSource;
async function gsap() {
  if (!gsapSource) {
    gsapSource = await readFile(runtimeFile, 'utf8').catch(async () =>
      readFile(fileURLToPath(import.meta.resolve('gsap/dist/gsap.min.js')), 'utf8')
    );
    gsapSource = gsapSource.replace(/\/\/[#@]\s*sourceMappingUR[L]=.*$/gm, '');
  }
  return gsapSource;
}
const hash = (value) => createHash('sha256').update(JSON.stringify(value)).digest('hex');
const animationClock = `(() => {
  window.__videocutErrors=[];
  const rememberError=message=>{
    if(window.__videocutErrors.length<16)window.__videocutErrors.push(String(message));
  };
  addEventListener('error',event=>rememberError(event.message||('HTML 资源加载失败：'+(event.target?.src||event.target?.href||''))),true);
  addEventListener('unhandledrejection',event=>{rememberError(event.reason?.message||event.reason||'HTML 初始化失败');event.preventDefault();});
  addEventListener('securitypolicyviolation',event=>rememberError('HTML 资源被阻止，请将外部资源打包为 data URL：'+event.blockedURI));
  window.__videocutCheckErrors=()=>{if(window.__videocutErrors.length)throw Error(window.__videocutErrors.join('\\n'));};
  window.variables=window.__videocutVariables;
  window.__videocutAnimations=new Set();
  window.__videocutRememberAnimations=()=>{
    for(const animation of document.getAnimations()){
      window.__videocutAnimations.add(animation);animation.pause();
    }
  };
  // Reuse DOM frames only when every supported visual input is represented by
  // this state. Bitmap drawing, animated images, native timelines and custom
  // elements stay on the capture path rather than risking a stale static frame.
  window.__videocutRasterState=()=>{
    if(window.__videocutAnimations.size || document.fonts.status!=='loaded' || document.fonts.size ||
      document.querySelector('canvas,img,image,feImage,input,textarea,select,animate,animateMotion,animateTransform,set'))return null;
    const elements=[...document.querySelectorAll('*')];
    if(elements.some(e=>e.shadowRoot||e.localName.includes('-')||e.hasAttribute('is')))return null;
    let rules;
    try{rules=[...document.styleSheets].flatMap(sheet=>[...sheet.cssRules].map(rule=>rule.cssText)).join('\\n');}
    catch{return null;}
    if(rules.includes('paint(') || [...rules.matchAll(/url\\(\\s*(['\"]?)(.*?)\\1\\s*\\)/g)].some(match=>!match[2].startsWith('#')))return null;
    const scroll=elements.flatMap((e,i)=>e.scrollLeft||e.scrollTop?[[i,e.scrollLeft,e.scrollTop]]:[]);
    return JSON.stringify([document.documentElement.outerHTML,rules,scroll]);
  };
  const originalAnimate=Element.prototype.animate;
  Element.prototype.animate=function(...args){
    const animation=originalAnimate.apply(this,args);
    window.__videocutAnimations.add(animation);animation.pause();return animation;
  };
  new MutationObserver(window.__videocutRememberAnimations).observe(document,{childList:true,subtree:true});
  // GSAP's automatic 2D/3D promotion changes Chromium's text and shadow
  // rasterization after a later tween is rewound. Keep ordinary 2D seeks stable;
  // authored 3D transforms and explicit force3D:true still remain available.
  window.gsap.config({force3D:false});
  window.gsap.globalTimeline.autoRemoveChildren=false;
  window.gsap.globalTimeline.pause(0);window.gsap.ticker.sleep();
})();`;
function walk(node, visitor) {
  visitor(node);
  for (const child of [...(node.childNodes || [])]) walk(child, visitor);
}

// Read authoring HTML as data; remove navigation containers and network GSAP
// references. The installed GSAP is injected before any authored script executes.
async function documentFor(content) {
  const tree = parse(content.html);
  let head;
  walk(tree, (node) => {
    if (node.tagName === 'head') head = node;
    if (['iframe', 'object', 'embed', 'audio', 'video', 'base'].includes(node.tagName))
      throw new Error('HTML 动画仅支持图形；音视频请放入现有轨道，禁止嵌入页面。');
    if (node.tagName === 'meta' && node.attrs.some((a) => a.name === 'http-equiv'))
      node.parentNode.childNodes = node.parentNode.childNodes.filter((child) => child !== node);
    if (node.tagName === 'script') {
      const src = node.attrs.find((a) => a.name === 'src')?.value;
      if (src && /(?:^|\/)gsap(?:\.min)?\.js(?:[?#]|$)/i.test(src))
        node.parentNode.childNodes = node.parentNode.childNodes.filter((child) => child !== node);
      else if (src && !src.startsWith('data:'))
        throw new Error('HTML 脚本必须内联或使用本地 HTML 导入打包。');
    }
  });
  const injected = parse(
    `<head><meta http-equiv="Content-Security-Policy" content="default-src data: blob:; script-src 'unsafe-inline' 'unsafe-eval' data: blob:; style-src 'unsafe-inline' data:; connect-src 'none'; frame-src 'none'; object-src 'none'; base-uri 'none'; form-action 'none'"><style>html,body{margin:0;width:100%;height:100%;overflow:hidden}html{background:${content.transparent ? 'transparent' : '#000'}}</style><script>${await gsap()}</script><script>window.__videocutVariables=${JSON.stringify(content.variables || {}).replace(/</g, '\\u003c')};window.__timelines={};${animationClock}</script></head>`
  );
  const injectedHead = injected.childNodes
    .find((n) => n.tagName === 'html')
    .childNodes.find((n) => n.tagName === 'head');
  for (const node of injectedHead.childNodes) node.parentNode = head;
  head.childNodes.unshift(...injectedHead.childNodes);
  return serialize(tree);
}

const initialize = `async () => {
  window.__videocutRememberAnimations();
  if (window.__videocutReady) await window.__videocutReady;
  if (document.querySelector('audio,video,iframe,object,embed')) throw Error('HTML 动画不能包含音视频或嵌入页面');
  await document.fonts.ready;
  await Promise.all(Array.from(document.images).map(image => image.decode().catch(() => {throw Error('HTML 图片读取失败，请将资源打包为 data URL')})));
  window.gsap.globalTimeline.pause();window.gsap.ticker.sleep();
  window.__videocutRememberAnimations();
  await new Promise(resolve=>setTimeout(resolve,0));window.__videocutCheckErrors();
  return true;
}`;

export { documentFor, initialize };

export function htmlTickExpression(content, time) {
  return `(async () => {
        const time=${time}/120000;
        window.__videocutTime=time;
        const timelines=[...new Set(Object.values(window.__timelines||{}))];
        // The global timeline also owns ordinary gsap.to() calls. Keep completed
        // children attached so an absolute backward seek can render them again.
        window.gsap.globalTimeline.totalTime(time+0.000001,true);window.gsap.globalTimeline.totalTime(time,false);
        for (const timeline of timelines) {
          if (!timeline || typeof timeline.totalTime!=='function') throw Error('无效 GSAP 时间线');
          timeline.pause();timeline.totalTime(time+0.000001,true);timeline.totalTime(time,false);
        }
        const tick=window.tick||window.__videocut?.tick;
        if(tick) await tick(time,{width:${content.width},height:${content.height},duration:${content.duration}/120000,variables:window.__videocutVariables});
        // A finished fill:none animation drops out of document.getAnimations().
        // Its retained Animation object must still receive backward seek times.
        window.__videocutRememberAnimations();
        for (const animation of window.__videocutAnimations) {
          if(animation.effect?.target && !animation.effect.target.isConnected){window.__videocutAnimations.delete(animation);continue;}
          animation.pause();animation.currentTime=time*1000;
        }
        window.gsap.ticker.sleep();
        if(document.querySelector('audio,video,iframe,object,embed')) throw Error('HTML 动画不能包含音视频或嵌入页面');
        document.body.getBoundingClientRect();
        await new Promise(resolve=>setTimeout(resolve,0));window.__videocutCheckErrors();
        return window.__videocutRasterState();
      })()`;
}

/** Persistent isolated pages are shared by preview and export, keyed by frozen source. */
export class HtmlFrameRenderer {
  pages = new Map();
  frames = new HtmlFrameCache();
  pageQueue = Promise.resolve();
  activePages = new Set();
  states = new Map();
  identities = new WeakMap();
  closed = false;
  constructor({ concurrency = 2, browserFactory = () => new Chromium().start() } = {}) {
    if (!Number.isSafeInteger(concurrency) || concurrency < 1 || concurrency > 2)
      throw new RangeError('HTML capture concurrency must be 1 or 2');
    this.browserFactory = browserFactory;
    // Export already requests two exact successors. Independent DOMs let those
    // captures overlap without a later seek changing an earlier screenshot.
    // Ordinary sequential preview requests keep using the first lane.
    this.lanes = Array.from({ length: concurrency }, (_, index) => ({
      index,
      pending: 0,
      queue: Promise.resolve()
    }));
  }
  async capabilities() {
    const available = await findChromium().then(
      () => true,
      () => false
    );
    return { available, runtime: 'chromium', gsap: '3.15.0', alpha: true, clock: 120000 };
  }
  capture(content, time, signal) {
    if (this.closed) return Promise.reject(new Error('HTML 渲染器已关闭'));
    const lane = this.lanes.reduce((best, next) => (next.pending < best.pending ? next : best));
    lane.pending++;
    // A lane still serializes seek + screenshot on its own document. At most
    // two captures run, and an aborted queued request is checked before setup.
    const result = lane.queue
      .then(() => this.captureFrame(content, time, signal, lane.index))
      .finally(() => lane.pending--);
    lane.queue = result.catch(() => {});
    return result;
  }
  async captureFrame(content, time, signal, lane) {
    for (let attempt = 0; attempt < 2; attempt++) {
      try {
        return await this.frame(content, time, signal, lane);
      } catch (error) {
        if (signal?.aborted) throw new DOMException('HTML 帧已过期', 'AbortError');
        if (
          attempt ||
          this.closed ||
          !['CHROMIUM_TIMEOUT', 'CHROMIUM_DISCONNECTED'].includes(error.code)
        )
          throw error;
        await this.recover(error.browser || this.browser);
      }
    }
  }
  recover(browser) {
    // Both lanes can fail on the same transport. The page lock retires it only
    // once, and a peer cannot close a replacement browser started meanwhile.
    const result = this.pageQueue.then(async () => {
      if (!browser || browser !== this.browser || this.closed) return;
      this.browser = undefined;
      this.pages.clear();
      this.states.clear();
      await browser.close();
    });
    this.pageQueue = result.catch(() => {});
    return result;
  }
  async evaluate(sessionId, expression, browser = this.browser) {
    const result = await browser.send(
      'Runtime.evaluate',
      { expression, awaitPromise: true, returnByValue: true, timeout: 10000 },
      sessionId
    );
    if (result.exceptionDetails)
      throw new Error(
        result.exceptionDetails.exception?.description || result.exceptionDetails.text
      );
    return result.result.value;
  }
  page(content, key, signal) {
    // Creation and LRU eviction share one lock; captures on ready pages do not.
    const result = this.pageQueue.then(() => {
      if (signal?.aborted) throw new DOMException('HTML 帧已过期', 'AbortError');
      if (this.closed) throw new Error('HTML 渲染器已关闭');
      return this.openPage(content, key);
    });
    this.pageQueue = result.catch(() => {});
    return result;
  }
  async openPage(content, key) {
    if (this.pages.has(key)) {
      const value = this.pages.get(key);
      this.pages.delete(key);
      this.pages.set(key, value);
      return value;
    }
    if (!this.browser) this.browser = await this.browserFactory();
    const browser = this.browser;
    if (this.closed) {
      await browser.close();
      throw new Error('HTML 渲染器已关闭');
    }
    let contextId;
    // Bound DOM/GPU residency. A recently used clip remains ready for backward seeks.
    if (this.pages.size >= 8) {
      const entry = [...this.pages].find(([key]) => !this.activePages.has(key));
      if (!entry) throw new Error('HTML 渲染页面正在使用，无法释放');
      const [oldKey, old] = entry;
      this.pages.delete(oldKey);
      browser.listeners.delete(old.listener);
      // Reset the target/global scope but reuse the isolated context. Repeated
      // edits no longer churn Chromium's browser-context storage partitions.
      await browser.send('Target.closeTarget', { targetId: old.targetId });
      contextId = old.contextId;
    }
    contextId ??= (await browser.send('Target.createBrowserContext', {})).browserContextId;
    let listener;
    try {
      const { targetId } = await browser.send('Target.createTarget', {
        url: 'about:blank',
        browserContextId: contextId
      });
      const { sessionId } = await browser.send('Target.attachToTarget', {
        targetId,
        flatten: true
      });
      const resourceErrors = [],
        requests = new Map();
      listener = (event) => {
        if (event.sessionId !== sessionId) return;
        const { requestId } = event.params || {};
        if (event.method === 'Network.requestWillBeSent')
          requests.set(requestId, event.params.request.url);
        if (event.method === 'Network.loadingFailed') {
          const source = requests.get(requestId) || '未知资源';
          if (resourceErrors.length < 16)
            resourceErrors.push(
              'HTML 资源加载失败：' +
                (source.startsWith('data:') ? '内嵌 data URL' : source.slice(0, 512)) +
                ' (' +
                event.params.errorText +
                ')'
            );
          requests.delete(requestId);
        }
        if (event.method === 'Network.loadingFinished') requests.delete(requestId);
      };
      browser.listeners.add(listener);
      await browser.send('Page.enable', {}, sessionId);
      await browser.send('Network.enable', {}, sessionId);
      await browser.send(
        'Network.setBlockedURLs',
        { urls: ['http://*', 'https://*', 'file://*'] },
        sessionId
      );
      await browser.send(
        'Emulation.setDeviceMetricsOverride',
        { width: content.width, height: content.height, deviceScaleFactor: 1, mobile: false },
        sessionId
      );
      await browser.send(
        'Emulation.setDefaultBackgroundColorOverride',
        { color: { r: 0, g: 0, b: 0, a: content.transparent ? 0 : 1 } },
        sessionId
      );
      const { frameTree } = await browser.send('Page.getFrameTree', {}, sessionId);
      await browser.send(
        'Page.setDocumentContent',
        { frameId: frameTree.frame.id, html: await documentFor(content) },
        sessionId
      );
      await this.evaluate(sessionId, `(${initialize})()`, browser);
      if (resourceErrors.length) throw new Error(resourceErrors.join('\n'));
      const page = {
        browser,
        sessionId,
        contextId,
        targetId,
        time: -1,
        resourceErrors,
        listener
      };
      this.pages.set(key, page);
      return page;
    } catch (error) {
      browser.listeners.delete(listener);
      await browser
        .send('Target.disposeBrowserContext', { browserContextId: contextId })
        .catch(() => {});
      throw error;
    }
  }
  async frame(content, time, signal, lane = 0) {
    const check = () => {
      if (signal?.aborted) throw new DOMException('HTML 帧已过期', 'AbortError');
    };
    check();
    const sourceKey = hash(content),
      key = `${sourceKey}:${lane}`,
      started = performance.now();
    time = Math.max(0, Math.min(content.duration - 1, Math.round(time)));
    const frameKey = `${sourceKey}:${time}`;
    let page;
    this.activePages.add(key);
    try {
      page = await this.page(content, key, signal);
      check();
      const checkResources = () => {
        if (page.resourceErrors.length) throw new Error(page.resourceErrors.join('\n'));
      };
      checkResources();
      const cached = this.frames.get(frameKey);
      if (cached)
        return {
          png: cached,
          time,
          ms: performance.now() - started,
          cached: true,
          stateKey: this.identities.get(cached)
        };
      const rasterState = await this.evaluate(
        page.sessionId,
        htmlTickExpression(content, time),
        page.browser
      );
      check();
      checkResources();
      const stateKey = rasterState === null ? undefined : hash([sourceKey, rasterState]),
        stateCacheKey = `${sourceKey}:state`;
      if (stateKey && this.states.get(sourceKey) === stateKey) {
        const png = this.frames.get(stateCacheKey);
        if (png) {
          this.frames.set(frameKey, png);
          page.time = time;
          return {
            png,
            time,
            ms: performance.now() - started,
            cached: true,
            reused: true,
            stateKey
          };
        }
      }
      const result = await page.browser.send(
        'Page.captureScreenshot',
        {
          format: 'png',
          fromSurface: true,
          captureBeyondViewport: false,
          optimizeForSpeed: true
        },
        page.sessionId
      );
      check();
      checkResources();
      const png = Buffer.from(result.data, 'base64');
      this.frames.set(frameKey, png);
      if (stateKey) {
        this.identities.set(png, stateKey);
        this.states.delete(sourceKey);
        this.states.set(sourceKey, stateKey);
        this.frames.set(stateCacheKey, png);
        while (this.states.size > 8) {
          const oldest = this.states.keys().next().value;
          this.states.delete(oldest);
          this.frames.delete(`${oldest}:state`);
        }
      }
      page.time = time;
      return { png, time, ms: performance.now() - started, cached: false, stateKey };
    } catch (error) {
      if (page && error.name !== 'AbortError') {
        if (this.pages.get(key) === page) this.pages.delete(key);
        page.browser.listeners.delete(page.listener);
        await page.browser
          .send('Target.disposeBrowserContext', { browserContextId: page.contextId })
          .catch(() => {});
      }
      throw error;
    } finally {
      this.activePages.delete(key);
    }
  }
  close() {
    return (this.closing ||= this.shutdown());
  }
  async shutdown() {
    this.closed = true;
    // Stop a stalled command before draining queues; startup finishing after
    // close is also retired by openPage rather than leaving an orphan process.
    await this.browser?.close();
    await Promise.all(this.lanes.map((lane) => lane.queue));
    await this.pageQueue;
    this.pages.clear();
    this.frames.clear();
    this.states.clear();
  }
}
