import { spawn } from 'node:child_process';
import { readFile, writeFile, stat, mkdtemp, rm, access } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, join, basename, resolve, extname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { DOMParser, XMLSerializer } from '@xmldom/xmldom';
import { createPagxContent, PAGX_MAX_BYTES } from '../core/pagx.mjs';
import { validateHtmlContent } from '../core/project.mjs';
import { Chromium } from './chromium.mjs';
import { documentFor, initialize } from './html-renderer.mjs';
import { importHtml } from './html-import.mjs';
import { normalizePagxSnapshot } from './pagx-snapshot.mjs';
import capture from '../pagx/capture.json' with { type: 'json' };

const root = fileURLToPath(new URL('../../', import.meta.url));
const executableName = process.platform === 'win32' ? 'pagx.exe' : 'pagx';
async function executable() {
  const candidates = [
    process.env.VIDEOCUT_PAGX_CLI,
    join(root, 'dist/pagx-cli', `${process.platform}-${process.arch}`, executableName),
    join(root, '.local/pagx/build', executableName)
  ].filter(Boolean);
  for (const path of candidates)
    if (
      await access(path).then(
        () => true,
        () => false
      )
    )
      return path;
  throw Error(
    'PAGX 转换器尚未构建：运行 npm run build:pagx，或设置 VIDEOCUT_PAGX_CLI 指向支持 --capture-animations 的官方开源 CLI'
  );
}
async function cli(path, args, signal, diagnosticExit = false) {
  return new Promise((resolve, reject) => {
    const child = spawn(path, args, {
      signal,
      windowsHide: true,
      env: { ...process.env, PAGX_HTML_SNAPSHOT: '0' },
      stdio: ['ignore', 'pipe', 'pipe']
    });
    let output = '',
      errors = '';
    const timer = setTimeout(() => child.kill('SIGKILL'), 30000);
    child.stdout.on('data', (b) => {
      output = (output + b).slice(-262144);
    });
    child.stderr.on('data', (b) => {
      errors = (errors + b).slice(-16384);
    });
    child.once('error', (e) => {
      clearTimeout(timer);
      reject(e);
    });
    child.once('close', (code) => {
      clearTimeout(timer);
      if (code !== 0 && !(diagnosticExit && code === 1 && output.trim().startsWith('{')))
        reject(Error('PAGX CLI: ' + (errors || output || `退出码 ${code}`)));
      else resolve({ output, warnings: errors, code });
    });
  });
}
async function boundedRead(path) {
  if ((await stat(path)).size > PAGX_MAX_BYTES) throw Error('PAGX 和内嵌资源不能超过 4 MiB');
  return readFile(path);
}

/** Import only user-authorized resources; resulting projects contain no local file references. */
export async function importPagx(path, options, allowed) {
  path = await allowed(path);
  if (extname(path).toLowerCase() !== '.pagx') throw Error('请选择 .pagx 文件');
  let xml = (await boundedRead(path)).toString('utf8');
  if (/<!DOCTYPE|<!ENTITY/i.test(xml)) throw Error('PAGX 不允许外部实体或 DTD');
  const doc = new DOMParser({
    onError: (_level, message) => {
      throw Error(message);
    }
  }).parseFromString(xml, 'application/xml');
  for (const node of Array.from(doc.getElementsByTagName('*'))) {
    const attribute = ['ImagePattern', 'Glyph'].includes(node.tagName)
      ? 'image'
      : node.tagName === 'Image'
        ? 'source'
        : null;
    if (!attribute || !node.hasAttribute(attribute)) continue;
    const source = node.getAttribute(attribute);
    if (/^data:/i.test(source) || (attribute === 'image' && /^@\S+$/.test(source))) continue;
    if (!source || /^(?:[a-z]+:|\/\/)/i.test(source)) throw Error('PAGX 资源必须来自本地授权目录');
    const resource = await allowed(resolve(dirname(path), source));
    const bytes = await boundedRead(resource);
    const mime = {
      '.png': 'image/png',
      '.jpg': 'image/jpeg',
      '.jpeg': 'image/jpeg',
      '.webp': 'image/webp',
      '.gif': 'image/gif'
    }[extname(resource).toLowerCase()];
    if (!mime) throw Error('不支持的 PAGX 图片资源');
    node.setAttribute(attribute, `data:${mime};base64,${bytes.toString('base64')}`);
    xml = new XMLSerializer().serializeToString(doc);
    if (Buffer.byteLength(xml) > PAGX_MAX_BYTES) throw Error('PAGX 和内嵌资源不能超过 4 MiB');
  }
  return { name: basename(path), pagx: createPagxContent(xml, options) };
}

/** Conversion uses a browser once to extract geometry/channels; playback never calls it. */
export class PagxConverter {
  busy = false;
  controller;
  async capabilities() {
    const available = await executable().then(
      () => true,
      () => false
    );
    return {
      available: true,
      conversionAvailable: available,
      defaultRenderer: 'pagx',
      fallbackRenderer: 'html',
      experimental: true,
      renderer: 'pagx-wasm-webgl2',
      license: 'Apache-2.0',
      enterpriseRequired: false,
      converterRevision: capture.revision,
      maxConversionSeconds: 30,
      supported: ['CSS 2D', 'GSAP', 'anime.js 3', 'synchronous window.tick'],
      unsupported: [
        'canvas/WebGL animation',
        '3D',
        'interactive state machines',
        'async tick',
        'animated layout',
        'animated path control points / clip-path'
      ],
      videoExport: 'VideoCut compositor and encoder'
    };
  }
  async prepare(options, allowed) {
    if (!options || !['pagx', 'html'].includes(options.renderer || 'pagx'))
      throw Error('动画渲染格式无效');
    if (Boolean(options.html) === Boolean(options.path))
      throw Error('请选择 html 内容或本地 path 中的一种');
    // Authorization and malformed input are errors, never reasons to bypass validation.
    const source = options.path
      ? await importHtml(options.path, options, allowed)
      : { html: options.html, name: options.name || '动画' };
    validateHtmlContent(source.html);
    if (options.renderer === 'html')
      return {
        type: 'html-clip',
        ...source,
        conversion: { requested: 'html', renderer: 'html', fallback: false, warnings: [] }
      };
    try {
      const result = await this.convert({ html: source.html, name: source.name }, allowed);
      const loss = result.warnings.filter((w) =>
        /subset:[^\]]*(?:unsupported|dropped|invalid)/.test(w)
      );
      if (loss.length) throw Error(loss.join('\n'));
      return {
        type: 'pagx-clip',
        name: source.name,
        pagx: result.pagx,
        conversion: {
          requested: 'pagx',
          renderer: 'pagx',
          fallback: false,
          warnings: result.warnings,
          requiresVisualReview: true
        }
      };
    } catch (error) {
      return {
        type: 'html-clip',
        ...source,
        conversion: {
          requested: 'pagx',
          renderer: 'html',
          fallback: true,
          reason: error instanceof Error ? error.message : String(error),
          warnings: []
        }
      };
    }
  }
  async convert(options, allowed) {
    if (this.busy) throw Error('PAGX 转换正在进行，请稍后重试');
    this.busy = true;
    this.controller = new AbortController();
    const signal = this.controller.signal;
    let browser, directory;
    try {
      if (Boolean(options.html) === Boolean(options.path))
        throw Error('请选择 html 内容或本地 path 中的一种');
      const source = options.path
        ? await importHtml(options.path, options, allowed)
        : { html: options.html, name: options.name || 'PAGX 动画' };
      const content = validateHtmlContent(source.html);
      if (content.duration > 30 * 120000)
        throw Error('单次 PAGX 转换最长 30 秒，请拆成多个动画片段');
      const binary = await executable();
      const help = await cli(binary, ['import', '--help'], signal);
      if (!help.output.includes('--capture-animations'))
        throw Error(
          '此 PAGX CLI 缺少开源动画导入能力；请运行 npm run build:pagx，不能使用旧版 CLI 静默生成静态图'
        );
      directory = await mkdtemp(join(tmpdir(), 'videocut-pagx-'));
      browser = await new Chromium().start();
      const conversionBrowser = browser;
      signal.addEventListener(
        'abort',
        () => {
          void conversionBrowser.close();
        },
        { once: true }
      );
      const { targetId } = await browser.send('Target.createTarget', { url: 'about:blank' });
      const { sessionId } = await browser.send('Target.attachToTarget', {
        targetId,
        flatten: true
      });
      const send = (method, params = {}) => browser.send(method, params, sessionId);
      const evaluate = async (expression) => {
        const r = await send('Runtime.evaluate', {
          expression,
          awaitPromise: true,
          returnByValue: true
        });
        if (r.exceptionDetails)
          throw Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
        return r.result.value;
      };
      await send('Page.enable');
      await send('Network.enable');
      await send('Network.setBlockedURLs', { urls: ['http://*', 'https://*', 'file://*'] });
      await send('Emulation.setDeviceMetricsOverride', {
        width: content.width,
        height: content.height,
        deviceScaleFactor: 1,
        mobile: false
      });
      // setDocumentContent preserves this initialized global scope. Freeze timers before author scripts.
      await evaluate(capture.init);
      const { frameTree } = await send('Page.getFrameTree');
      await send('Page.setDocumentContent', {
        frameId: frameTree.frame.id,
        html: await documentFor(content)
      });
      await evaluate(
        `(${initialize.replace('setTimeout(resolve,0)', 'window.__pagxRealSetTimeout(resolve,0)')})()`
      );
      await evaluate(`(() => {
        if(document.querySelector('canvas')) throw Error('Canvas/WebGL 动画不能转换为可编辑 PAGX');
        if(document.querySelectorAll('*').length>500) throw Error('PAGX 转换最多支持 500 个 DOM 元素');
        document.body.style.width='${content.width}px';document.body.style.height='${content.height}px';
        window.__videocutDurationMs=${content.duration / 120};
        const baseline=new WeakMap();
        const remember=message=>{if(!window.__videocutErrors.includes(message)&&window.__videocutErrors.length<16)window.__videocutErrors.push(message);};
        const layoutProperties=['width','height','left','top','right','bottom','margin','padding','fontSize','fontFamily','fontWeight','lineHeight','letterSpacing','borderRadius','backgroundImage','backgroundPosition','backgroundSize','display','visibility','perspective','transformOrigin','zIndex','overflow','strokeWidth','strokeDasharray','backdropFilter'];
        let elements;
        window.__videocutSampleTime=ms=>{
          const t=ms/1000;
          for(const tl of new Set(Object.values(window.__timelines||{}))) {
            if(!tl||typeof tl.totalTime!=='function')throw Error('无效 GSAP 时间线');
            tl.pause();tl.totalTime(t+0.000001,true);tl.totalTime(t,false);
          }
          const tick=window.tick||window.__videocut?.tick;
          if(tick){const r=tick(t,{width:${content.width},height:${content.height},duration:${content.duration / 120000},variables:window.__videocutVariables});if(r?.then){window.__videocutErrors.push('异步 tick 暂不支持转换');}}
          window.__videocutRememberAnimations();
          for(const a of window.__videocutAnimations){a.pause();a.currentTime=ms;}
          const current=[...document.querySelectorAll('body *')].filter(e=>!['SCRIPT','STYLE','LINK','META'].includes(e.tagName));
          elements ||= current;
          if(current.length!==elements.length||current.some((e,i)=>e!==elements[i]))remember('动态增删 DOM 暂不支持 PAGX 转换');
          for(const e of current) {
            const s=getComputedStyle(e);
            if(s.transform!=='none'&&!new DOMMatrix(s.transform).is2D)remember('3D 变换暂不支持 PAGX 转换');
            const geometry=['d','transform','x','y','cx','cy','r','rx','ry','width','height','viewBox','points','src','href'];
            const text=[...e.childNodes].filter(n=>n.nodeType===Node.TEXT_NODE).map(n=>n.textContent).join('');
            const layout=JSON.stringify([...layoutProperties.map(p=>s[p]),...geometry.map(a=>e.getAttribute(a)),text]);
            if(baseline.has(e)&&baseline.get(e)!==layout)remember('检测到不支持的布局、字体、背景或 SVG 几何动画，请保留 HTML');
            else baseline.set(e,layout);
          }
          window.__videocutCheckErrors();
        };
      })()`);
      await evaluate(capture.pseudos);
      const captured = await evaluate(capture.capture);
      await evaluate('window.__videocutCheckErrors()');
      const snapshot = await evaluate(capture.snapshot);
      if (!snapshot?.html || Buffer.byteLength(snapshot.html) > 4 * 1024 * 1024)
        throw Error('PAGX HTML 快照无效或过大');
      const input = join(directory, 'scene.html'),
        output = join(directory, 'scene.pagx');
      await writeFile(
        input,
        await evaluate(`(${normalizePagxSnapshot.toString()})(${JSON.stringify(snapshot.html)})`)
      );
      await browser.close();
      browser = undefined;
      const converted = await cli(
        binary,
        [
          'import',
          '--input',
          input,
          '--output',
          output,
          '--format',
          'html',
          '--images',
          'embed',
          '--verbose'
        ],
        signal
      );
      const document = new DOMParser().parseFromString(
        (await boundedRead(output)).toString('utf8'),
        'application/xml'
      );
      // The HTML importer adds this flag to absolute-positioned root children.
      // It is redundant outside a layout container and triggers the CLI verifier.
      for (const layer of Array.from(document.getElementsByTagName('Layer'))) {
        if (layer.hasAttribute('includeInLayout') && !layer.parentNode.getAttribute?.('layout'))
          layer.removeAttribute('includeInLayout');
        for (const [edge, opposite, center] of [
          ['left', 'right', 'centerX'],
          ['top', 'bottom', 'centerY']
        ])
          if (
            layer.getAttribute(edge) === '0' &&
            !layer.hasAttribute(opposite) &&
            !layer.hasAttribute(center)
          )
            layer.removeAttribute(edge);
      }
      // The snapshot already splits a text leaf into measured visual lines.
      // Reflowing these at a different font metric wraps the last glyph again.
      for (const box of Array.from(document.getElementsByTagName('TextBox'))) {
        const text = box.getElementsByTagName('Text');
        if (
          text.length === 1 &&
          !/[\r\n]/.test(text[0].getAttribute('text') || text[0].textContent)
        )
          box.setAttribute('wordWrap', 'false');
      }
      const xml = new XMLSerializer().serializeToString(document);
      await writeFile(output, xml);
      const pagx = createPagxContent(xml, {
        duration: content.duration,
        transparent: content.transparent
      });
      if (captured.count > 0 && !xml.includes('<Animation '))
        throw Error('转换器丢失了动画，已拒绝静态结果');
      const verified = await cli(
        binary,
        ['verify', '--skip-render', '--skip-layout', '--json', output],
        signal,
        true
      );
      const diagnostics = JSON.parse(verified.output);
      if (
        !Array.isArray(diagnostics.diagnostics) ||
        diagnostics.diagnostics.some((d) => /^Element |^Schemas /i.test(d.message))
      )
        throw Error('PAGX 规范校验失败：' + verified.output);
      return {
        name: source.name.replace(/\.html?$/i, '.pagx'),
        pagx,
        capturedAnimations: captured.count,
        warnings: [
          ...converted.warnings.trim().split('\n').filter(Boolean),
          ...diagnostics.diagnostics.map((d) => d.message)
        ],
        diagnostics: { ok: diagnostics.ok, diagnostics: diagnostics.diagnostics },
        conversion: {
          experimental: true,
          requiresVisualReview: true,
          revision: capture.revision,
          sampleRate: 60,
          runtime: 'pagx-wasm-webgl2',
          enterpriseRequired: false
        }
      };
    } finally {
      await browser?.close();
      if (directory) await rm(directory, { recursive: true, force: true });
      this.controller = undefined;
      this.busy = false;
    }
  }
  close() {
    this.controller?.abort();
  }
}
