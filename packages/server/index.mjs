/** @typedef {import('./types.js').ServerOptions} ServerOptions */
import { prepareNativeTemplates, resolveNativeTextFonts } from './native-templates.mjs';
import { createBrowserExportJob } from './template-export.mjs';
import { HtmlVideoCache, nativeHtmlPlan, createNativeHtmlExportJob } from './native-html-export.mjs';
import { createServer } from 'node:http';
import { createReadStream, existsSync } from 'node:fs';
import {
  realpath,
  readdir,
  stat,
  mkdir,
  writeFile,
  access,
  mkdtemp,
  cp,
  rm
} from 'node:fs/promises';
import { randomBytes } from 'node:crypto';
import { resolve, relative, isAbsolute, extname, join, dirname, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createProject, validateProject, clone, CommandHistory } from '../core/project.mjs';
import { probe, fingerprint, run, mediaExtensions, mime } from './media.mjs';
import { systemFonts } from './system-fonts.mjs';
import { HtmlFrameRenderer } from './html-renderer.mjs';
import { validateHtmlContent } from '../core/project.mjs';
import { importHtml } from './html-import.mjs';
import { KokoroModelStore, defaultTtsModelDir } from './tts-models.mjs';
import { createTtsRoutes } from './tts.mjs';
import { WhisperModelStore, defaultAsrModelDir } from './asr-models.mjs';
import { createAsrRoutes } from './asr.mjs';
import { VisionModelStore, defaultVisionModelDir } from './vision-models.mjs';
import {
  createVisionRoutes,
  createVisionDescriptionRoutes,
  assertVisionSource
} from './vision.mjs';
import { importClipboardMedia } from './clipboard.mjs';
import { saveWebProject, openWebProject, webProjectResource } from './web-project.mjs';
import { createStarterProject } from './starter-example.mjs';
import { createSoftwareUpdates } from './software-updates.mjs';
import { revealOutput } from './reveal-output.mjs';

const packageRoot = fileURLToPath(new URL('../../', import.meta.url));
const json = (res, status, data) => {
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Cache-Control': 'no-store'
  });
  res.end(JSON.stringify(data));
};
const within = (root, path) => {
  const rel = relative(root, path);
  return rel === '' || (!rel.startsWith('..' + sep) && rel !== '..' && !isAbsolute(rel));
};

/**
 * @param {import('./types.js').ServerOptions} [options]
 * @returns {Promise<{url: string; initialSession: import('../client/types.js').Snapshot | undefined; close(): Promise<void>}>}
 */
export async function startServer({
  roots = [process.cwd()],
  port = 4318,
  staticDir = join(packageRoot, 'dist/web'),
  nativeBridge = process.env.VIDEOCUT_NATIVE_BRIDGE ||
    join(packageRoot, '.local/bin/videocut-bridge'),
  htmlRenderer = process.env.VIDEOCUT_HTML_RENDERER,
  initialProject,
  initialProjectPath,
  initialDemo,
  ffprobe = process.env.FFPROBE,
  ffmpeg = process.env.FFMPEG || 'ffmpeg',
  ttsModelDir = process.env.VIDEOCUT_TTS_MODEL_DIR || defaultTtsModelDir(),
  asrModelDir = process.env.VIDEOCUT_ASR_MODEL_DIR || defaultAsrModelDir(),
  visionModelDir = process.env.VIDEOCUT_VISION_MODEL_DIR || defaultVisionModelDir()
} = {}) {
  roots = await Promise.all(roots.map((p) => realpath(resolve(p))));
  const token = randomBytes(32).toString('hex');
  const sessions = new Map();
  // Keep old names reserved so a saved link never attaches to a different cut.
  const previewPaths = new Map();
  const htmlFrames = new HtmlFrameRenderer();
  const htmlVideos = new HtmlVideoCache();
  const ttsModels = new KokoroModelStore({ directory: ttsModelDir });
  const asrModels = new WhisperModelStore({ directory: asrModelDir });
  const visionModels = new VisionModelStore({ directory: visionModelDir });
  const updates = await createSoftwareUpdates({
    packageRoot,
    prepareInstall: async () => {
      const saved = [];
      for (const s of sessions.values()) {
        const project = clone(s.project);
        await projectPaths(project);
        const name = project.name.replace(/[^\p{L}\p{N}_.-]/gu, '_').slice(0, 80) || 'VideoCut';
        const destination = join(
          roots[0],
          `${name}-update-${Date.now()}-${randomBytes(3).toString('hex')}.vcutweb`
        );
        const receipt = await saveWebProject(
          project,
          destination,
          s.resourceArchive ? join(s.resourceArchive.root, 'resources') : staticDir
        );
        s.projectPath = receipt.path;
        await attachWebResources(s);
        notify(s);
        saved.push({ name: project.name, path: receipt.path });
      }
      return saved;
    }
  });
  let origin = '';
  // Grant access to individual shipped narration files, never the package directory.
  const starterAssets = new Set();
  async function allowed(path) {
    if (typeof path !== 'string' || !isAbsolute(path)) throw new Error('请提供绝对路径');
    const canonical = await realpath(path);
    if (!roots.some((root) => within(root, canonical)) && !starterAssets.has(canonical))
      throw new Error('路径不在启动时授权的目录内；请使用 --root 添加目录');
    return canonical;
  }
  async function projectPaths(project) {
    for (const asset of project.assets) {
      const path = await allowed(asset.path);
      if (path !== asset.path) throw new Error('素材需要使用真实路径');
      if (!(await stat(path)).isFile() || !mediaExtensions.has(extname(path).toLowerCase()))
        throw new Error('素材路径不是可读媒体文件');
    }
  }
  async function openProject(requestedPath) {
    const path = await allowed(requestedPath);
    if ((await stat(path)).isDirectory() && extname(path) === '.vcutweb')
      return (await openWebProject(path)).project;
    if (!(await stat(path)).isDirectory() || extname(path) !== '.vcut')
      throw new Error('请选择 .vcutweb 或 .vcut 作品目录');
    await access(nativeBridge).catch(() => {
      throw new Error('请配置 Project Format 1 格式桥接器');
    });
    const project = validateProject(
      JSON.parse(await run(nativeBridge, ['import', path], { maxOutput: 32 * 1024 * 1024 }))
    );
    await projectPaths(project);
    if (
      project.timeline.tracks.some((track) =>
        track.items.some((item) => item.clip.text && !item.clip.text.template)
      )
    )
      resolveNativeTextFonts(project, await systemFonts.catalog());
    for (const asset of project.assets) {
      const info = await stat(asset.path);
      asset.sourceIdentity = `${info.dev}:${info.ino}:${info.size}:${info.mtimeMs}`;
    }
    const templates = project.timeline.tracks
      .flatMap((t) => t.items)
      .map((i) => i.clip.text?.template)
      .filter((t) => t?.packageDigest);
    if (templates.length) {
      const temporary = await mkdtemp(join(roots[0], '.videocut-template-check-'));
      try {
        const { bundles } = await prepareNativeTemplates(project, staticDir, temporary);
        const digests = JSON.parse(
          await run(nativeBridge, ['template-digests'], {
            input: JSON.stringify(bundles),
            timeout: 120000
          })
        );
        for (const t of templates)
          if (digests[t.id] !== t.packageDigest)
            throw new Error('作品文字模板资源版本与当前安装版本不匹配');
      } finally {
        await rm(temporary, { recursive: true, force: true });
      }
    }
    return project;
  }
  function previewPath(s) {
    const cleaned = s.project.name
      .normalize('NFC')
      .trim()
      .replace(/[^\p{L}\p{N}_.-]+/gu, '-')
      .replace(/^[.-]+|[.-]+$/g, '');
    const name = [...cleaned].slice(0, 80).join('') || '未命名作品';
    if (s.previewName !== name) {
      let path = `/projects/${encodeURIComponent(name)}`;
      for (let suffix = 2; previewPaths.has(path) && previewPaths.get(path) !== s.id; suffix++)
        path = `/projects/${encodeURIComponent(name)}-${suffix}`;
      previewPaths.set(path, s.id);
      s.previewName = name;
      s.previewPath = path;
    }
    return s.previewPath;
  }
  function createSession(project = createProject(), projectPath = null) {
    if (sessions.size >= 32) {
      const oldest = [...sessions.values()]
        .filter((s) => !s.clients.size && !s.exporting)
        .sort((a, b) => a.touched - b.touched)[0];
      if (oldest) {
        oldest.closed = true;
        oldest.ttsJob?.cancel();
        oldest.asrJob?.cancel();
        oldest.visionJob?.cancel();
        sessions.delete(oldest.id);
      } else throw new Error('会话数量已达上限，请关闭不用的会话');
    }
    const id = randomBytes(18).toString('hex');
    const session = {
      id,
      project: clone(project),
      projectPath,
      history: new CommandHistory(),
      version: 0,
      clients: new Set(),
      touched: Date.now(),
      exporting: false,
      renderJob: null,
      exportOutputs: new Set(),
      thumbnails: new Map(),
      previewSequence: 0,
      previewReports: new Map()
    };
    sessions.set(id, session);
    previewPath(session);
    return session;
  }
  function rememberOutput(s, path) {
    s.exportOutputs.add(path);
    if (s.exportOutputs.size > 32) s.exportOutputs.delete(s.exportOutputs.values().next().value);
  }
  async function initializeDemo(options) {
    const project = await createStarterProject(staticDir, options);
    for (const asset of project.assets) starterAssets.add(asset.path);
    const session = createSession(project);
    session.example = 'starter';
    return session;
  }
  async function attachWebResources(s) {
    if (!s.projectPath?.endsWith('.vcutweb')) return;
    s.resourceArchive = await openWebProject(s.projectPath);
    for (const track of s.project.timeline.tracks)
      for (const { clip } of track.items)
        if (clip.text?.template)
          clip.text.template.resourceBase = `/project-resources/${s.id}/text-templates/`;
  }
  const snapshot = (s) => {
    for (const track of s.project.timeline.tracks)
      for (const { clip } of track.items) {
        if (!clip.text?.template) continue;
        if (s.resourceArchive)
          clip.text.template.resourceBase = `/project-resources/${s.id}/text-templates/`;
        else delete clip.text.template.resourceBase;
      }
    return {
      id: s.id,
      version: s.version,
      project: s.project,
      history: s.history.state,
      previewUrl: `${origin}${decodeURI(previewPath(s))}`,
      projectPath: s.projectPath,
      ...(s.example ? { example: s.example } : {})
    };
  };
  const notify = (s) => {
    for (const res of s.clients) res.write(`data: ${JSON.stringify(snapshot(s))}\n\n`);
  };
  async function body(req) {
    let size = 0;
    const chunks = [];
    for await (const data of req) {
      size += data.length;
      if (size > 8 * 1024 * 1024) throw new Error('请求超过 8 MB');
      chunks.push(data);
    }
    return JSON.parse(Buffer.concat(chunks).toString() || '{}');
  }
  async function serveFile(req, res, path) {
    const info = await stat(path);
    if (!info.isFile()) return json(res, 404, { error: '文件不存在' });
    const headers = {
      'Content-Type': mime[extname(path).toLowerCase()] || 'application/octet-stream',
      'Accept-Ranges': 'bytes',
      'Cache-Control': 'no-store'
    };
    let start = 0,
      end = info.size - 1,
      status = 200;
    if (req.headers.range) {
      const match = /^bytes=(\d*)-(\d*)$/.exec(req.headers.range);
      if (!match || (!match[1] && !match[2])) {
        res.writeHead(416, { 'Content-Range': `bytes */${info.size}` });
        return res.end();
      }
      start = match[1] ? Number(match[1]) : Math.max(0, info.size - Number(match[2]));
      end = match[1] && match[2] ? Math.min(Number(match[2]), info.size - 1) : info.size - 1;
      if (!Number.isSafeInteger(start) || start > end || start >= info.size) {
        res.writeHead(416, { 'Content-Range': `bytes */${info.size}` });
        return res.end();
      }
      headers['Content-Range'] = `bytes ${start}-${end}/${info.size}`;
      status = 206;
    }
    headers['Content-Length'] = Math.max(0, end - start + 1);
    res.writeHead(status, headers);
    if (req.method === 'HEAD' || !info.size) return res.end();
    const stream = createReadStream(path, { start, end });
    stream.on('error', () => res.destroy());
    res.on('close', () => stream.destroy());
    stream.pipe(res);
  }
  const ttsRoutes = createTtsRoutes({
    models: ttsModels,
    roots,
    allowed,
    probe: (path) => probe(path, ffprobe),
    projectPaths,
    notify,
    snapshot,
    body,
    json
  });
  const asrRoutes = createAsrRoutes({ models: asrModels, snapshot, notify, body, json });
  const visionRoutes = createVisionRoutes({
    models: visionModels,
    snapshot,
    body,
    json,
    allowed,
    probe: (path) => probe(path, ffprobe)
  });
  const visionDescriptionRoutes = createVisionDescriptionRoutes({
    models: visionModels,
    sessions,
    snapshot,
    createSession,
    body,
    json,
    routes: visionRoutes,
    setupUrl: () => origin
  });
  const server = createServer(async (req, res) => {
    res.setHeader('X-Content-Type-Options', 'nosniff');
    res.setHeader('Referrer-Policy', 'no-referrer');
    res.setHeader('Cross-Origin-Resource-Policy', 'same-origin');
    try {
      if (req.headers.host !== new URL(origin).host)
        return json(res, 403, { error: '只允许本机服务地址' });
      if (
        (req.headers.origin && req.headers.origin !== origin) ||
        req.headers['sec-fetch-site'] === 'cross-site'
      )
        return json(res, 403, { error: '不允许跨站访问本地文件' });
      const url = new URL(req.url, origin);
      const resource = /^\/project-resources\/([a-f0-9]+)\/(.+)$/.exec(url.pathname);
      if (resource && ['GET', 'HEAD'].includes(req.method)) {
        const session = sessions.get(resource[1]);
        if (!session?.resourceArchive) return json(res, 404, { error: '作品资源不存在' });
        return await serveFile(
          req,
          res,
          await webProjectResource(session.resourceArchive, decodeURIComponent(resource[2]))
        );
      }
      if (url.pathname.startsWith('/vision-models/')) {
        if (!['GET', 'HEAD'].includes(req.method))
          return json(res, 405, { error: 'Method not allowed' });
        const prefix = '/vision-models/fastvlm-0.5b/';
        if (!url.pathname.startsWith(prefix)) return json(res, 404, { error: '模型资源不存在' });
        if ((await visionModels.consent()) !== 'enabled')
          return json(res, 409, { error: '视觉理解尚未启用', code: 'VISION_CONSENT_REQUIRED' });
        const file = await visionModels.verifiedFile(
          decodeURIComponent(url.pathname.slice(prefix.length))
        );
        if (!file)
          return json(res, 404, { error: '视觉模型尚未下载', code: 'VISION_MODEL_REQUIRED' });
        return await serveFile(req, res, file);
      }
      if (url.pathname.startsWith('/asr-models/')) {
        if (!['GET', 'HEAD'].includes(req.method))
          return json(res, 405, { error: 'Method not allowed' });
        const prefix = '/asr-models/whisper-base/';
        if (!url.pathname.startsWith(prefix)) return json(res, 404, { error: '模型资源不存在' });
        const file = await asrModels.verifiedFile(
          decodeURIComponent(url.pathname.slice(prefix.length))
        );
        if (!file) return json(res, 404, { error: '识别模型尚未下载', code: 'ASR_MODEL_REQUIRED' });
        return await serveFile(req, res, file);
      }
      if (url.pathname.startsWith('/tts-models/')) {
        if (!['GET', 'HEAD'].includes(req.method))
          return json(res, 405, { error: 'Method not allowed' });
        const prefix = '/tts-models/kokoro-v1.1-zh/';
        if (!url.pathname.startsWith(prefix)) return json(res, 404, { error: '模型资源不存在' });
        const path = decodeURIComponent(url.pathname.slice(prefix.length));
        const file = await ttsModels.verifiedFile(path);
        if (!file) return json(res, 404, { error: '模型资源尚未安装', code: 'TTS_MODEL_REQUIRED' });
        return await serveFile(req, res, file);
      }
      if (!url.pathname.startsWith('/api/')) {
        if (!['GET', 'HEAD'].includes(req.method))
          return json(res, 405, { error: 'Method not allowed' });
        const requested = resolve(staticDir, '.' + decodeURIComponent(url.pathname));
        if (!within(resolve(staticDir), requested)) return json(res, 403, { error: '无效路径' });
        const path =
          existsSync(requested) && (await stat(requested)).isFile()
            ? requested
            : join(staticDir, 'index.html');
        if (!existsSync(path)) return json(res, 503, { error: '请先运行 npm run build' });
        return await serveFile(req, res, path);
      }
      if (url.pathname === '/api/bootstrap' && req.method === 'GET')
        return json(res, 200, {
          token,
          roots,
          nativeExport: existsSync(nativeBridge),
          previewControl: true,
          htmlClips: await htmlFrames.capabilities()
        });
      // A same-origin font service exposes only opaque IDs discovered in standard
      // OS/user font directories. It never accepts a caller-supplied file path.
      if (url.pathname === '/api/fonts' && req.method === 'GET')
        return json(res, 200, await systemFonts.catalog());
      if (url.pathname.startsWith('/api/fonts/') && ['GET', 'HEAD'].includes(req.method)) {
        const id = url.pathname.slice('/api/fonts/'.length);
        try {
          const font = await systemFonts.read(id);
          if (req.headers['if-none-match'] === font.etag) return res.writeHead(304).end();
          res.writeHead(200, {
            'Content-Type': font.mime,
            'Content-Length': font.bytes.length,
            'Cache-Control': 'private, max-age=3600',
            ETag: font.etag
          });
          return res.end(req.method === 'HEAD' ? undefined : font.bytes);
        } catch (error) {
          return json(res, error.statusCode || 400, { error: error.message });
        }
      }
      if (
        req.headers.authorization !== `Bearer ${token}` &&
        url.searchParams.get('token') !== token
      )
        return json(res, 401, { error: '本地会话凭证失效，请重新连接' });
      if (url.pathname === '/api/updates' && req.method === 'GET')
        return json(res, 200, await updates.check(url.searchParams.get('refresh') === '1'));
      if (url.pathname === '/api/updates/install' && req.method === 'POST') {
        return json(
          res,
          202,
          await updates.install(() => {
            const activeJobs = [...sessions.values()].some(
              (s) =>
                s.exporting ||
                [s.ttsJob, s.asrJob, s.visionJob].some(
                  (job) => job && ['queued', 'running'].includes(job.status().state)
                )
            );
            if (activeJobs)
              throw Object.assign(new Error('请等待导出、配音或分析任务完成后再更新。'), {
                statusCode: 409
              });
          })
        );
      }
      if (updates.status().state === 'installing' && !['GET', 'HEAD'].includes(req.method))
        return json(res, 409, { error: '正在安装更新，请稍后再操作。' });
      if (url.pathname === '/api/asr/model' && req.method === 'GET')
        return json(res, 200, await asrModels.catalog());
      if (url.pathname === '/api/vision/model' && req.method === 'GET')
        return json(res, 200, await visionModels.catalog());
      if (
        req.method === 'POST' &&
        ['/api/vision/describe-image', '/api/vision/describe-video'].includes(url.pathname)
      )
        return await visionDescriptionRoutes(
          req,
          res,
          url.pathname.endsWith('image') ? 'image' : 'video'
        );
      if (url.pathname === '/api/vision/setup' && req.method === 'POST') {
        const data = await body(req);
        // MCP exposes only request-setup: acceptance belongs to the initialization dialog.
        if (data.action === 'request') {
          visionModels.promptRequested = true;
          for (const s of sessions.values())
            for (const client of s.clients)
              client.write('event: vision-setup-request\ndata: {}\n\n');
          return json(res, 200, { ...(await visionModels.catalog()), setupUrl: origin });
        }
        const status = await visionModels.decide(data.enabled);
        if (!data.enabled) for (const s of sessions.values()) s.visionJob?.cancel();
        return json(res, data.enabled ? 202 : 200, status);
      }
      if (url.pathname === '/api/vision/model/install' && req.method === 'DELETE')
        return json(res, 200, await visionModels.cancel());
      if (url.pathname === '/api/tts/catalog' && req.method === 'GET')
        return json(res, 200, await ttsModels.catalog());
      if (url.pathname === '/api/tts/model/install') {
        if (req.method === 'GET') return json(res, 200, ttsModels.status());
        if (req.method === 'POST') return json(res, 202, ttsModels.startInstall(await body(req)));
        if (req.method === 'DELETE') return json(res, 200, await ttsModels.cancel());
        return json(res, 405, { error: 'Method not allowed' });
      }
      if (url.pathname === '/api/html/import' && req.method === 'POST') {
        const data = await body(req);
        return json(res, 200, await importHtml(data.path, data, allowed));
      }
      if (url.pathname === '/api/html-frames' && req.method === 'POST') {
        const data = await body(req);
        validateHtmlContent(data.html);
        if (!Number.isSafeInteger(data.time) || data.time < 0)
          throw new Error('HTML 时间需要非负 120000 Hz 整数 ticks');
        if (data.rasterScale !== undefined)
          throw new Error('HTML 帧保持模板原始尺寸，不支持降低采样分辨率');
        const controller = new AbortController();
        const cancel = () => {
          if (!res.writableEnded) controller.abort();
        };
        res.on('close', cancel);
        try {
          const frame = await htmlFrames.capture(data.html, data.time, controller.signal);
          if (controller.signal.aborted) return;
          res.writeHead(200, {
            'Content-Type': 'image/png',
            'Content-Length': frame.png.length,
            'Cache-Control': 'no-store',
            'X-HTML-Time': String(frame.time),
            'X-HTML-Frame-Ms': frame.ms.toFixed(2),
            'X-HTML-Cached': String(frame.cached),
            ...(frame.stateKey ? { 'X-HTML-State': frame.stateKey } : {})
          });
          return res.end(frame.png);
        } finally {
          res.off('close', cancel);
        }
      }
      if (url.pathname === '/api/files' && req.method === 'GET') {
        const path = await allowed(url.searchParams.get('path') || roots[0]);
        const entries = [];
        for (const entry of await readdir(path, { withFileTypes: true })) {
          if (entry.name.startsWith('.')) continue;
          if (
            !entry.isDirectory() &&
            !(
              entry.isFile() &&
              (mediaExtensions.has(extname(entry.name).toLowerCase()) ||
                ['.html', '.htm'].includes(extname(entry.name).toLowerCase()))
            )
          )
            continue;
          entries.push({
            name: entry.name,
            path: join(path, entry.name),
            directory: entry.isDirectory()
          });
        }
        entries.sort(
          (a, b) => Number(b.directory) - Number(a.directory) || a.name.localeCompare(b.name)
        );
        const parent = dirname(path);
        return json(res, 200, {
          path,
          parent: roots.some((root) => within(root, parent)) ? parent : null,
          entries
        });
      }
      if (url.pathname === '/api/projects/open' && req.method === 'POST') {
        const data = await body(req);
        const path = await allowed(data.path);
        const project = await openProject(path);
        const session = createSession(project, path);
        await attachWebResources(session);
        return json(res, 201, snapshot(session));
      }
      if (url.pathname === '/api/assets' && req.method === 'POST') {
        const { path } = await body(req);
        return json(res, 200, await probe(await allowed(path), ffprobe));
      }
      if (url.pathname === '/api/clipboard/assets' && req.method === 'POST') {
        return json(res, 200, await importClipboardMedia({ allowed, ffprobe }));
      }
      if (url.pathname === '/api/examples/starter' && req.method === 'POST')
        return json(res, 201, snapshot(await initializeDemo(await body(req))));
      if (url.pathname === '/api/sessions' && req.method === 'POST') {
        const data = await body(req);
        const project = data.project ? validateProject(data.project) : createProject();
        await projectPaths(project);
        const templates = project.timeline.tracks
          .flatMap((t) => t.items)
          .map((i) => i.clip.text?.template)
          .filter((t) => t?.packageDigest);
        if (templates.length) {
          const temporary = await mkdtemp(join(roots[0], '.videocut-template-check-'));
          try {
            const { bundles } = await prepareNativeTemplates(project, staticDir, temporary);
            const digests = JSON.parse(
              await run(nativeBridge, ['template-digests'], {
                input: JSON.stringify(bundles),
                timeout: 120000
              })
            );
            for (const t of templates)
              if (digests[t.id] !== t.packageDigest)
                throw new Error('作品文字模板资源版本与当前安装版本不匹配');
          } finally {
            await rm(temporary, { recursive: true, force: true });
          }
        }
        return json(res, 201, snapshot(createSession(project)));
      }
      if (url.pathname === '/api/sessions' && req.method === 'GET')
        return json(
          res,
          200,
          [...sessions.values()].map((s) => ({
            id: s.id,
            name: s.project.name,
            version: s.version,
            previewUrl: snapshot(s).previewUrl,
            projectPath: s.projectPath
          }))
        );
      if (url.pathname === '/api/sessions/resolve' && req.method === 'GET') {
        const path = url.searchParams.get('previewPath') || '';
        const part = /^\/projects\/([^/]+)$/.exec(path)?.[1];
        const canonical = part ? `/projects/${encodeURIComponent(decodeURIComponent(part))}` : '';
        const s = sessions.get(previewPaths.get(canonical));
        if (!s) return json(res, 404, { error: '作品预览已结束，请通过 .vcut 目录重新打开' });
        s.touched = Date.now();
        return json(res, 200, snapshot(s));
      }
      const match =
        /^\/api\/sessions\/([a-f0-9]+)(?:\/(events|media|save|export|export-output|render|render-job|commands|open|preview|preview-status|vision|vision-media|vision-job(?:\/(?:claim|progress|heartbeat|fail|result))?|asr|asr-job(?:\/(?:claim|prepare|progress|heartbeat|fail|result))?|tts|tts-job(?:\/(?:claim|progress|heartbeat|fail|result))?))?$/.exec(
          url.pathname
        );
      const s = match && sessions.get(match[1]);
      if (!s) return json(res, 404, { error: '会话已结束，请新建作品' });
      s.touched = Date.now();
      if (match[2] === 'export-output') {
        if (!['GET', 'HEAD', 'POST'].includes(req.method))
          return json(res, 405, { error: 'Method not allowed' });
        const requested =
          req.method === 'POST' ? (await body(req)).path : url.searchParams.get('path');
        if (!s.exportOutputs.has(requested)) return json(res, 404, { error: '导出结果不存在' });
        const path = await allowed(requested);
        if (path !== requested) throw new Error('导出文件路径已变化，请重新导出');
        const info = await stat(path);
        if (req.method === 'POST') {
          await revealOutput(path);
          return json(res, 200, { opened: true });
        }
        if (!info.isFile() || !['.mp4', '.webm'].includes(extname(path)))
          return json(res, 400, { error: '此导出结果不是视频文件' });
        return await serveFile(req, res, path);
      }
      if (match[2] === 'vision-media' && ['GET', 'HEAD'].includes(req.method)) {
        const job = s.visionJob;
        if (
          !job ||
          url.searchParams.get('job') !== job.spec.id ||
          !['queued', 'running'].includes(job.status().state)
        )
          return json(res, 410, { error: '视觉媒体版本已释放' });
        const path = await allowed(job.spec.path);
        try {
          assertVisionSource(job.spec, await stat(path));
        } catch (error) {
          job.fail(error.message);
          return json(res, error.statusCode || 400, { error: error.message, code: error.code });
        }
        return await serveFile(req, res, path);
      }
      if (match[2]?.startsWith('vision')) return await visionRoutes(req, res, s, match[2]);
      if (match[2]?.startsWith('asr')) return await asrRoutes(req, res, url, s, match[2]);
      if (match[2]?.startsWith('tts')) return await ttsRoutes(req, res, url, s, match[2]);
      if (!match[2] && req.method === 'GET') return json(res, 200, snapshot(s));
      if (!match[2] && req.method === 'DELETE') {
        s.closed = true;
        for (const client of s.clients) client.end();
        s.renderJob?.fail(new Error('会话已关闭'));
        s.ttsJob?.cancel();
        s.asrJob?.cancel();
        s.visionJob?.cancel();
        sessions.delete(s.id);
        return json(res, 200, { closed: true });
      }
      if (!match[2] && req.method === 'PUT') {
        const data = await body(req);
        const project = validateProject(data.project);
        await projectPaths(project);
        // Check after async I/O, so two concurrent writers cannot both win.
        if (data.version !== s.version)
          return json(res, 409, {
            error: '作品已被其他窗口或插件修改，请基于最新版本重试',
            ...snapshot(s)
          });
        s.project = clone(project);
        s.history.clear();
        s.version++;
        notify(s);
        return json(res, 200, snapshot(s));
      }
      if (match[2] === 'preview' && req.method === 'GET') {
        for (const [key, report] of s.previewReports)
          if (Date.now() - report.receivedAt > 15000) s.previewReports.delete(key);
        return json(res, 200, {
          previewUrl: snapshot(s).previewUrl,
          sequence: s.previewSequence,
          connections: s.clients.size,
          clients: [...s.previewReports.values()]
        });
      }
      if (match[2] === 'preview' && req.method === 'POST') {
        const data = await body(req);
        if (!['play', 'pause', 'seek'].includes(data.action)) throw new Error('预览动作无效');
        if (data.action === 'seek' && data.timeSeconds === undefined)
          throw new Error('seek 需要 timeSeconds');
        if (
          data.timeSeconds !== undefined &&
          (typeof data.timeSeconds !== 'number' ||
            !Number.isFinite(data.timeSeconds) ||
            data.timeSeconds < 0 ||
            data.timeSeconds > 86400)
        )
          throw new Error('预览时间无效');
        const command = {
          sequence: ++s.previewSequence,
          action: data.action,
          timeSeconds: data.timeSeconds
        };
        for (const client of s.clients)
          client.write(`event: preview-control\ndata: ${JSON.stringify(command)}\n\n`);
        return json(res, 200, {
          ...command,
          deliveredTo: s.clients.size,
          previewUrl: snapshot(s).previewUrl
        });
      }
      if (match[2] === 'preview-status' && req.method === 'POST') {
        const data = await body(req);
        if (
          !/^[a-zA-Z0-9-]{1,80}$/.test(data.clientId) ||
          !Number.isFinite(data.timeSeconds) ||
          !Number.isInteger(data.version) ||
          !Number.isInteger(data.commandSequence) ||
          typeof data.playing !== 'boolean' ||
          !Array.isArray(data.media) ||
          data.media.length > 100
        )
          throw new Error('预览状态无效');
        if (s.previewReports.size >= 32 && !s.previewReports.has(data.clientId))
          s.previewReports.delete(s.previewReports.keys().next().value);
        s.previewReports.set(data.clientId, { ...data, receivedAt: Date.now() });
        return json(res, 200, { received: true });
      }
      if (match[2] === 'events' && req.method === 'GET') {
        res.writeHead(200, {
          'Content-Type': 'text/event-stream',
          'Cache-Control': 'no-cache',
          Connection: 'keep-alive'
        });
        s.clients.add(res);
        res.write(`data: ${JSON.stringify(snapshot(s))}\n\n`);
        const timer = setInterval(() => {
          s.touched = Date.now();
          res.write(': keepalive\n\n');
        }, 25000);
        res.on('close', () => {
          clearInterval(timer);
          s.clients.delete(res);
          if (!s.clients.size && !s.renderJob?.native) s.renderJob?.fail(new Error('渲染页面已关闭'));
          if (!s.clients.size) s.ttsJob?.cancel();
          if (!s.clients.size) s.asrJob?.cancel();
          if (!s.clients.size) s.visionJob?.cancel();
        });
        return;
      }
      if (match[2] === 'media' && ['GET', 'HEAD'].includes(req.method)) {
        const renderId = url.searchParams.get('job');
        if (renderId && renderId !== s.renderJob?.spec.id)
          return json(res, 410, { error: '导出媒体版本已释放' });
        const mediaProject = renderId ? s.renderJob.spec.project : s.project;
        const asset = mediaProject.assets.find((a) => a.id === url.searchParams.get('asset'));
        if (!asset) return json(res, 404, { error: '素材不存在' });
        if (url.searchParams.has('waveform') || url.searchParams.has('thumbnail'))
          return json(res, 410, { error: '媒体视觉由浏览器媒体 Worker 生成' });
        return await serveFile(req, res, await allowed(asset.path));
      }
      if (match[2] === 'commands' && req.method === 'POST') {
        const data = await body(req);
        if (data.version !== s.version)
          return json(res, 409, { error: '作品版本冲突', ...snapshot(s) });
        const edited = s.history.prepare(s.project, data.operations);
        await projectPaths(edited.project);
        if (data.version !== s.version)
          return json(res, 409, { error: '作品版本冲突', ...snapshot(s) });
        s.history.accept(edited);
        if (edited.patches.length) {
          s.project = edited.project;
          s.version++;
          notify(s);
        }
        return json(res, 200, {
          ...snapshot(s),
          operations: edited.operations,
          changes: edited.changes
        });
      }
      if (match[2] === 'render-job' && req.method === 'GET')
        return json(res, 200, s.renderJob?.status() || s.lastRender || { phase: 'idle' });
      if (match[2] === 'render-job' && req.method === 'POST') {
        const job = s.renderJob;
        if (!job || url.searchParams.get('job') !== job.spec.id)
          return json(res, 404, { error: '导出任务已结束' });
        const action = url.searchParams.get('action');
        if (action === 'claim') {
          const worker = job.claim();
          return worker
            ? json(res, 200, { worker, ...job.spec })
            : json(res, 409, { error: '导出已由其他窗口接管' });
        }
        if (action === 'cancel') {
          job.fail(new Error('用户取消导出'));
          return json(res, 200, { cancelled: true });
        }
        if (action === 'error') {
          job.check(req.headers['x-render-worker']);
          const data = await body(req);
          job.fail(new Error(String(data.error || '浏览器渲染失败').slice(0, 2000)));
          return json(res, 200, {});
        }
        const worker = req.headers['x-render-worker'];
        if (action === 'configure')
          return json(res, 200, await job.configure(worker, await body(req)));
        if (action === 'frame')
          return json(
            res,
            200,
            await job.frame(req, worker, Number(url.searchParams.get('index')))
          );
        if (action === 'audio')
          return json(
            res,
            200,
            await job.audio(
              req,
              worker,
              Number(url.searchParams.get('sampleOffset')),
              Number(url.searchParams.get('sampleRate')),
              Number(url.searchParams.get('channels')),
              Number(url.searchParams.get('sequence'))
            )
          );
        if (action === 'chunk')
          return json(
            res,
            200,
            await job.chunk(
              req,
              worker,
              Number(url.searchParams.get('position')),
              Number(url.searchParams.get('sequence'))
            )
          );
        if (action === 'progress') return json(res, 200, job.progress(worker, await body(req)));
        if (action === 'finish') return json(res, 200, await job.finish(worker, await body(req)));
        return json(res, 400, { error: '未知导出操作' });
      }
      if (['save', 'export', 'render'].includes(match[2]) && req.method === 'POST') {
        if (s.exporting) return json(res, 409, { error: '此会话正在导出，请等待完成' });
        const data = await body(req);
        if (s.exporting) return json(res, 409, { error: '此会话正在导出，请等待完成' });
        if (data.version !== s.version)
          return json(res, 409, { error: '作品版本已变化，请同步后再导出' });
        const project = clone(s.project);
        s.exporting = true;
        try {
          await projectPaths(project);
          const parent = await allowed(data.directory || roots[0]);
          const stamp = `${Date.now()}-${randomBytes(3).toString('hex')}`;
          const name = project.name.replace(/[^\p{L}\p{N}_.-]/gu, '_').slice(0, 80) || 'VideoCut';
          if (match[2] === 'save') {
            const receipt = await saveWebProject(
              project,
              join(parent, `${name}-${stamp}.vcutweb`),
              s.resourceArchive ? join(s.resourceArchive.root, 'resources') : staticDir
            );
            s.projectPath = receipt.path;
            await attachWebResources(s);
            rememberOutput(s, receipt.path);
            notify(s);
            return json(res, 200, { ...receipt, version: data.version });
          }
          if (match[2] === 'render') {
            const progress = (value) => {
              if (value.phase === 'complete') rememberOutput(s, value.path);
              s.lastRender = value;
              for (const listener of s.clients)
                listener.write(`event: render-progress\ndata: ${JSON.stringify(value)}\n\n`);
            };
            let nativeReady = false;
            if (htmlRenderer && nativeHtmlPlan(project, data.format || 'mp4')) {
              nativeReady = await access(htmlRenderer).then(() => run(ffmpeg, ['-version'], { timeout: 1500, maxOutput: 128 * 1024 })).then(() => true, () => false);
            }
            if (!nativeReady && !s.clients.size)
              return json(res, 409, {
                error: '请打开作品网页完成视频导出',
                previewUrl: snapshot(s).previewUrl,
                code: 'BROWSER_REQUIRED'
              });
            const job = nativeReady ? await createNativeHtmlExportJob(project, data.version, join(parent, `${name}-${stamp}`), progress, htmlRenderer, ffmpeg, htmlVideos) : await createBrowserExportJob(
              project,
              data.version,
              join(parent, `${name}-${stamp}`),
              data.format || 'mp4',
              progress,
              ffmpeg
            );
            s.renderJob = job;
            const disconnected = () => {
              if (!res.writableEnded) job.fail(new Error('导出请求已断开'));
            };
            res.on('close', disconnected);
            try {
              if (res.destroyed || (!job.native && !s.clients.size)) job.fail(new Error('导出网页或请求已断开'));
              else if (!job.native)
                for (const listener of s.clients)
                  listener.write(
                    `event: render-request\ndata: ${JSON.stringify({ id: job.spec.id })}\n\n`
                  );
              return json(res, 200, await job.completed);
            } finally {
              res.off('close', disconnected);
              s.renderJob = null;
              await job.dispose();
            }
          }
          if (project.timeline.tracks.some((track) => track.items.some((item) => item.clip.html)))
            throw new Error(
              'HTML 动画可以导出 MP4/WebM；当前原生 .vcut 格式桥接器尚未支持 HTML 片段。'
            );
          await access(nativeBridge).catch(() => {
            throw new Error(
              '尚未配置 VideoCut 原生格式桥接器，请通过 --native-bridge 指定适用于本机的预编译程序'
            );
          });
          for (const asset of project.assets) {
            if ((await stat(asset.path)).size !== asset.size)
              throw new Error(`素材已变更，请重新导入：${asset.name}`);
            asset.fingerprint = await fingerprint(asset.path);
            asset.nativeProbe = { streams: (await probe(asset.path, ffprobe)).streams };
          }
          const path = join(parent, `${name}-${stamp}.vcut`);
          const temporary = await mkdtemp(join(parent, '.videocut-export-'));
          let created = false;
          try {
            if (
              project.timeline.tracks.some((t) =>
                t.items.some((i) => i.clip.text?.layoutWidth !== undefined)
              )
            ) {
              const capabilities = JSON.parse(await run(nativeBridge, ['capabilities']));
              if (capabilities.textLayoutWidth !== 1)
                throw new Error('请更新原生桥接器以保存文字框宽度；也可使用完整 .vcutweb 保存');
            }
            if (project.timeline.tracks.some((t) => t.items.some((i) => i.clip.text?.template))) {
              const capabilities = JSON.parse(
                await run(nativeBridge, ['capabilities']).catch(() => {
                  throw new Error('请更新原生格式桥接器以支持复杂文字模板导出');
                })
              );
              if (
                project.timeline.tracks.some((t) =>
                  t.items.some((i) => i.clip.text?.template?.recipe)
                ) &&
                capabilities.customTextRecipes !== 1
              )
                throw new Error('请更新原生桥接器以保存自定义花字配方；也可使用完整 .vcutweb 保存');
              if (
                project.timeline.tracks.some((t) =>
                  t.items.some((i) => i.clip.text?.template?.style)
                ) &&
                capabilities.textTemplateStyles !== 1
              )
                throw new Error('请更新原生桥接器以保存花字颜色与字号；也可使用完整 .vcutweb 保存');
              if (capabilities.textTemplates !== 1)
                throw new Error('原生格式桥接器不支持复杂文字模板');
            }
            const {
              bundles,
              assets,
              project: preparedProject
            } = await prepareNativeTemplates(project, staticDir, temporary);
            const staged = join(temporary, 'project.vcut');
            const receipt = JSON.parse(
              await run(nativeBridge, ['export', staged], {
                input: JSON.stringify({
                  ...preparedProject,
                  templateBundles: bundles,
                  templateAssets: assets
                }),
                timeout: 120000
              })
            );
            if (assets.length) {
              await cp(
                join(staticDir, 'text-templates/LICENSES'),
                join(staged, 'assets/text/LICENSES'),
                { recursive: true }
              );
              await cp(
                join(staticDir, 'text-templates/NOTICE.md'),
                join(staged, 'assets/text/NOTICE.md')
              );
            }
            await mkdir(path);
            created = true;
            await cp(staged, path, { recursive: true, force: false, errorOnExist: true });
            s.projectPath = path;
            rememberOutput(s, path);
            notify(s);
            return json(res, 200, { ...receipt, path, version: data.version });
          } catch (error) {
            if (created) await rm(path, { recursive: true, force: true });
            throw error;
          } finally {
            await rm(temporary, { recursive: true, force: true });
          }
        } finally {
          s.exporting = false;
        }
      }
      return json(res, 405, { error: 'Method not allowed' });
    } catch (error) {
      if (!res.headersSent)
        json(res, error.statusCode || 400, {
          error: error.message,
          ...(error.code ? { code: error.code } : {}),
          ...(error.previewUrl ? { previewUrl: error.previewUrl } : {})
        });
      else res.destroy();
    }
  });
  if (initialDemo && (initialProject || initialProjectPath))
    throw new Error('--demo 与 --project 不能同时使用');
  if (initialProjectPath) {
    initialProjectPath = await allowed(initialProjectPath);
    initialProject = await openProject(initialProjectPath);
  }
  if (initialProject) {
    validateProject(initialProject);
    await projectPaths(initialProject);
  }
  server.requestTimeout = 65000;
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, '127.0.0.1', resolve);
  });
  origin = `http://127.0.0.1:${server.address().port}`;
  let initialSession;
  if (initialDemo) {
    try {
      initialSession = snapshot(await initializeDemo(initialDemo));
    } catch (error) {
      server.closeAllConnections();
      await new Promise((resolve) => server.close(resolve));
      throw error;
    }
  } else if (initialProject) {
    const session = createSession(initialProject, initialProjectPath);
    await attachWebResources(session);
    initialSession = snapshot(session);
  }
  const gc = setInterval(() => {
    for (const [id, s] of sessions)
      if (!s.clients.size && !s.exporting && Date.now() - s.touched > 6 * 60 * 60 * 1000) {
        s.closed = true;
        s.ttsJob?.cancel();
        s.asrJob?.cancel();
        s.visionJob?.cancel();
        sessions.delete(id);
      }
  }, 60000);
  gc.unref();
  return {
    url: origin,
    token,
    initialSession,
    async close() {
      clearInterval(gc);
      await updates.close();
      for (const s of sessions.values()) {
        s.closed = true;
        s.renderJob?.fail(new Error('服务已关闭'));
        s.ttsJob?.cancel();
        s.asrJob?.cancel();
        s.visionJob?.cancel();
        for (const res of s.clients) res.end();
      }
      server.closeAllConnections();
      await ttsModels.cancel();
      await asrModels.cancel();
      await visionModels.cancel();
      await htmlFrames.close();
      htmlVideos.clear();
      await new Promise((resolve) => server.close(resolve));
    }
  };
}
