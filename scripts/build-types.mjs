import ts from 'typescript';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { join, resolve, relative, dirname } from 'node:path';
import { root } from './release-files.mjs';
async function writeChanged(path, content) {
  if (await readFile(path, 'utf8').catch(() => null) !== content) await writeFile(path, content);
}

/** Emit from implementation. Existing .d.mts files are outputs, never compiler inputs. */
export async function buildTypes(destination = join(root, 'packages')) {
  const options = {
    target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.NodeNext,
    moduleResolution: ts.ModuleResolutionKind.NodeNext, allowJs: true, checkJs: false,
    declaration: true, emitDeclarationOnly: true, skipLibCheck: true,
    strict: true, noImplicitAny: false, esModuleInterop: true,
    rootDir: root, outDir: join(root, '.local/declarations'),
    types: ['node'], lib: ['lib.es2022.d.ts', 'lib.dom.d.ts', 'lib.dom.iterable.d.ts']
  };
  const host = ts.createCompilerHost(options);
  const fileExists = host.fileExists.bind(host);
  host.fileExists = (path) => path.startsWith(join(root, 'packages')) && path.endsWith('.d.mts') &&
    (fileExists(path.replace(/\.d\.mts$/, '.mjs')) || fileExists(path.replace(/\.d\.mts$/, '.mts')))
    ? false : fileExists(path);
  const sources = ['core/project.mjs', 'client/index.mjs', 'server/index.mjs', 'core/history.mts', 'core/immutable.mts']
    .map((file) => join(root, 'packages', file));
  const program = ts.createProgram(sources, options, host);
  const output = new Map();
  const emission = program.emit(undefined, (path, content) => output.set(path, content));
  const diagnostics = [...ts.getPreEmitDiagnostics(program), ...emission.diagnostics];
  if (diagnostics.length) throw new Error(ts.formatDiagnosticsWithColorAndContext(diagnostics, {
    getCanonicalFileName: (p) => p, getCurrentDirectory: () => root, getNewLine: () => '\n'
  }));
  const generated = [];
  for (const [path, content] of output) {
    const file = relative(join(options.outDir, 'packages'), path);
    if (!/^(core\/[^/]+|client\/(?:index|types)|server\/(?:index|types))\.d\.[cm]?ts$/.test(file)) continue;
    const target = join(destination, file);
    await mkdir(dirname(target), { recursive: true });
    await writeChanged(target, '// Generated from implementation by scripts/build-types.mjs. Do not edit.\n' + content);
    generated.push(file);
  }
  // These small ES modules remain executable on every supported Node 22 version.
  for (const file of ['core/history', 'core/immutable']) {
    const source = await readFile(join(root, 'packages', file + '.mts'), 'utf8');
    const result = ts.transpileModule(source, { compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ESNext }, fileName: file + '.mts' });
    await writeChanged(join(root, 'packages', file + '.mjs'), '// Generated from ' + file + '.mts. Do not edit.\n' + result.outputText);
  }
  return generated;
}

if (process.argv[1] && resolve(process.argv[1]) === new URL(import.meta.url).pathname)
  console.log(`Generated ${(await buildTypes()).length} public declaration files from implementation.`);
