import { cp, mkdir, readdir, readFile, writeFile, lstat } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { resolve, join } from 'node:path';

// Export current source only. Never copy .git or the private website/history.
const root = fileURLToPath(new URL('../', import.meta.url));
const [product, outputArg] = process.argv.slice(2);
if (!['ffclip-plugins', 'ffvideo-plugins'].includes(product) || !outputArg)
  throw new Error('Usage: node scripts/export-open-source.mjs <ffclip-plugins|ffvideo-plugins> <empty-output-directory>');
const output = resolve(outputArg);
try {
  if ((await readdir(output)).length) throw new Error('Output directory must be empty');
} catch (error) { if (error.code !== 'ENOENT') throw error; }
await mkdir(output, { recursive: true });
const common = ['packages', 'src', 'scripts', 'native', '.gitignore', '.editorconfig',
  '.prettierrc.yaml', '.prettierignore', 'package.json', 'pnpm-lock.yaml',
  'tsconfig.json', 'LICENSE', 'THIRD_PARTY_NOTICES.md'];
const inputs = product === 'ffclip-plugins'
  ? [...common, 'bin', 'plugins', 'tests', 'docs', '.github', 'index.html', 'vite.config.ts', 'README.md', 'publish-npm.command']
  : [...common, 'ffvideo', 'bin/open-preview.mjs'];
const blocked = new Set(['.git', 'ffclip', '.local', '.cache', 'node_modules', '.idea', '.vscode', 'certs', '.DS_Store']);
const credentials = /(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,}|AKIA[A-Z0-9]{16}|-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----|sk-(?:proj-)?[A-Za-z0-9_-]{32,})/;
let count = 0, total = 0;
async function copy(relative) {
  const base = relative.split('/').at(-1);
  if (blocked.has(base) || /^\.env(?:\.|$)/.test(base) || /\.(?:tgz|pem|key|p12|pfx|log)$/.test(base)) return;
  if (base === 'dist' && relative !== 'packages/text-wasm/dist') return;
  const source = join(root, relative);
  const info = await lstat(source);
  if (info.isSymbolicLink()) throw new Error(`Refusing symlink: ${relative}`);
  if (info.isDirectory()) {
    for (const name of (await readdir(source)).sort()) await copy(`${relative}/${name}`);
    return;
  }
  if (!info.isFile()) throw new Error(`Refusing special file: ${relative}`);
  const bytes = await readFile(source);
  if (!bytes.includes(0) && credentials.test(bytes.toString('utf8')))
    throw new Error(`Potential credential in ${relative}; review before publication`);
  if (bytes.length >= 100 * 1024 * 1024) throw new Error(`File exceeds GitHub limit: ${relative}`);
  await mkdir(join(output, relative, '..'), { recursive: true });
  await cp(source, join(output, relative));
  count++; total += bytes.length;
}
for (const relative of inputs) await copy(relative);
if (product === 'ffvideo-plugins') {
  const pkg = JSON.parse(await readFile(join(output, 'package.json'), 'utf8'));
  const tooling = { name: 'ffvideo-plugins', private: true, version: '0.2.0', type: 'module', license: 'MIT',
    repository: { type: 'git', url: 'git+https://github.com/425776024/ffvideo-plugins.git' },
    engines: pkg.engines,
    scripts: { build: 'npm --prefix ffvideo run build', dev: 'npm --prefix ffvideo run dev',
      start: 'npm --prefix ffvideo start', test: 'npm --prefix ffvideo test' },
    devDependencies: pkg.devDependencies };
  await writeFile(join(output, 'package.json'), JSON.stringify(tooling, null, 2) + '\n');
  await writeFile(join(output, 'README.md'), `# ffvideo-plugins\n\nTurn topics into local video drafts with narration, captions and agent feedback.\n\n## 从源码运行\n\nNode.js 22 或更高版本：\n\n\`\`\`sh\ngit clone https://github.com/425776024/ffvideo-plugins.git\ncd ffvideo-plugins\nnpm install\nnpm run build\nnode ffvideo/dist/bin/ffvideo.mjs --open\n\`\`\`\n\n[完整使用说明](ffvideo/README.md) · [宿主插件](ffvideo/plugins/README.md) · [MIT 许可证](LICENSE) · [第三方声明](THIRD_PARTY_NOTICES.md)\n\n\`ffvideo/\` 保存产品源码、插件、技能和测试。\`packages/\`、\`src/\`、\`scripts/\` 与 \`native/\` 保存从 [ffclip-plugins](https://github.com/425776024/ffclip-plugins) 复用的共享引擎及构建支持，保持既有相对导入，使本仓库可独立构建。文本 WASM 同时包含源码、预编译运行时和哈希记录，重编译见 [text-wasm](packages/text-wasm/README.md)。\n\n官网 \`ffclip/\` 源码、开发者本地数据和旧 Git 历史未包含在本仓库，也不在 MIT 授权范围内。模型权重另行下载并遵循各自许可。\n`);
}
console.log(JSON.stringify({ product, output, files: count, bytes: total, websiteIncluded: false, historyIncluded: false }));
