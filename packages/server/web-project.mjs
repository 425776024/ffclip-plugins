import { createReadStream } from 'node:fs';
import { lstat, mkdir, mkdtemp, readdir, readFile, writeFile, copyFile, rename, rm, realpath } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { join, dirname, extname } from 'node:path';
import { validateProject, clone } from '../core/project.mjs';

const format = 'videocut.web-project';
const maximumManifest = 32 * 1024 * 1024;
const safePath = (path) => typeof path === 'string' && /^[A-Za-z0-9_./-]+$/.test(path) &&
  !path.startsWith('/') && path.split('/').every((part) => part && part !== '.' && part !== '..');
const identity = (s) => `${s.dev}:${s.ino}:${s.size}:${s.mtimeMs}`;
async function hash(path) {
  const h = createHash('sha256');
  for await (const chunk of createReadStream(path)) h.update(chunk);
  return h.digest('hex');
}
async function regular(root, relative) {
  if (!safePath(relative)) throw new Error('作品包含无效的资源路径');
  let path = root;
  const parts = relative.split('/');
  for (let i = 0; i < parts.length; i++) {
    path = join(path, parts[i]);
    const s = await lstat(path);
    if (s.isSymbolicLink() || (i < parts.length - 1 ? !s.isDirectory() : !s.isFile()))
      throw new Error('作品资源必须是普通文件，不能包含符号链接');
  }
  return path;
}
async function files(root, prefix = '') {
  const result = [];
  for (const name of (await readdir(join(root, prefix))).sort()) {
    const relative = `${prefix}${name}`, info = await lstat(join(root, relative));
    if (!safePath(relative) || info.isSymbolicLink()) throw new Error('模板资源路径无效');
    if (info.isDirectory()) result.push(...await files(root, `${relative}/`));
    else if (info.isFile()) result.push(relative);
    else throw new Error('模板资源必须是普通文件');
  }
  return result;
}

/** A separate portable format, preserving the complete browser model and frozen template resources. */
export async function saveWebProject(project, destination, staticDir) {
  if (extname(destination) !== '.vcutweb') throw new Error('Web 作品需要 .vcutweb 扩展名');
  const stored = clone(validateProject(project));
  const staging = await mkdtemp(join(dirname(destination), '.videocut-save-'));
  const entries = {}, copied = new Map();
  try {
    async function copy(source, relative, expectedSize, expectedIdentity) {
      const before = await lstat(source);
      if (!before.isFile() || before.isSymbolicLink() ||
          (expectedSize !== undefined && before.size !== expectedSize) ||
          (expectedIdentity && identity(before) !== expectedIdentity))
        throw new Error('素材已变更，请重新导入后保存');
      const digest = await hash(source);
      if (identity(await lstat(source)) !== identity(before)) throw new Error('保存时素材发生变更');
      relative = relative(digest);
      if (!entries[relative]) {
        await mkdir(dirname(join(staging, relative)), { recursive: true });
        await copyFile(source, join(staging, relative));
        if (await hash(join(staging, relative)) !== digest || identity(await lstat(source)) !== identity(before))
          throw new Error('保存时素材发生变更');
        entries[relative] = { sha256: digest, size: before.size };
      }
      return relative;
    }
    for (const asset of stored.assets) {
      const source = asset.path;
      const relative = copied.get(source) || await copy(source,
        (digest) => `media/${digest}${extname(source).toLowerCase()}`, asset.size, asset.sourceIdentity);
      copied.set(source, relative);
      asset.path = relative;
      delete asset.sourceIdentity;
    }
    const templates = stored.timeline.tracks.flatMap((t) => t.items).map((i) => i.clip.text?.template).filter(Boolean);
    for (const t of templates) delete t.resourceBase;
    if (templates.length) {
      const templateRoot = join(staticDir, 'text-templates');
      for (const file of await files(templateRoot))
        await copy(await regular(templateRoot, file), () => `resources/text-templates/${file}`);
    }
    const manifest = { format, version: 1, project: stored, files: entries };
    const bytes = Buffer.from(JSON.stringify(manifest, null, 2));
    if (bytes.length > maximumManifest) throw new Error('作品数据超过 32 MB');
    await writeFile(join(staging, 'project.json'), bytes, { flag: 'wx' });
    // Reserve the destination exclusively; never overwrite an existing package.
    await mkdir(destination);
    try {
      for (const file of (await readdir(staging)).filter((file) => file !== 'project.json'))
        await rename(join(staging, file), join(destination, file));
      await rename(join(staging, 'project.json'), join(destination, 'project.json'));
    } catch (error) {
      await rm(destination, { recursive: true, force: true });
      throw error;
    }
    return { path: destination, format, files: Object.keys(entries).length };
  } finally {
    await rm(staging, { recursive: true, force: true });
  }
}

export async function openWebProject(root) {
  root = await realpath(root);
  const manifestPath = await regular(root, 'project.json');
  if ((await lstat(manifestPath)).size > maximumManifest) throw new Error('作品数据超过 32 MB');
  const manifest = JSON.parse(await readFile(manifestPath, 'utf8'));
  if (manifest.format !== format || manifest.version !== 1 || !manifest.files ||
      typeof manifest.files !== 'object' || Array.isArray(manifest.files) || Object.keys(manifest.files).length > 20000)
    throw new Error('Web 作品格式或版本不受支持');
  for (const [file, receipt] of Object.entries(manifest.files)) {
    if (!safePath(file) || !/^(media|resources\/text-templates)\//.test(file) ||
        !receipt || !/^[a-f0-9]{64}$/.test(receipt.sha256) || !Number.isSafeInteger(receipt.size) || receipt.size < 0)
      throw new Error('作品资源清单无效');
    const path = await regular(root, file);
    if ((await lstat(path)).size !== receipt.size || await hash(path) !== receipt.sha256)
      throw new Error(`作品资源校验失败：${file}`);
  }
  const project = clone(manifest.project);
  if (!Array.isArray(project?.assets)) throw new Error('作品素材列表无效');
  for (const asset of project.assets) {
    if (!manifest.files[asset.path] || !asset.path.startsWith('media/')) throw new Error('素材未包含在作品包中');
    asset.path = await regular(root, asset.path);
    asset.sourceIdentity = identity(await lstat(asset.path));
  }
  for (const track of project.timeline?.tracks || []) for (const { clip } of track.items || [])
    if (clip?.text?.template) delete clip.text.template.resourceBase;
  return { project: validateProject(project), root, files: manifest.files };
}

export async function webProjectResource(archive, relative) {
  const file = `resources/${relative}`;
  if (!archive?.files[file]) throw new Error('作品资源不存在');
  const path = await regular(archive.root, file), receipt = archive.files[file];
  if ((await lstat(path)).size !== receipt.size || await hash(path) !== receipt.sha256)
    throw new Error('作品资源在打开后发生变更');
  return path;
}
