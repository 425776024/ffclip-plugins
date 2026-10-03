import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { join } from 'node:path';
import { root, pluginFiles, verifyDist, readRegular, isMain } from './release-files.mjs';

const exec = promisify(execFile);
export async function checkPackage(packageRoot = root) {
  const files = await verifyDist(packageRoot);
  const expected = [
    ...files,
    'package.json',
    'README.md',
    'LICENSE',
    ...pluginFiles.map((path) => `plugins/videocut-local/${path}`)
  ].sort();
  for (const path of expected) await readRegular(join(packageRoot, path));
  // --ignore-scripts prevents this prepack check from recursively invoking itself.
  const { stdout } = await exec('npm', ['pack', '--dry-run', '--ignore-scripts', '--json'], {
    cwd: packageRoot,
    maxBuffer: 8 * 1024 * 1024
  });
  const [pack] = JSON.parse(stdout);
  const actual = pack.files.map((file) => file.path).sort();
  if (JSON.stringify(actual) !== JSON.stringify(expected)) {
    const extra = actual.filter((path) => !expected.includes(path));
    const missing = expected.filter((path) => !actual.includes(path));
    throw new Error(
      `Unsafe npm package. Unexpected: ${extra.join(', ')}; missing: ${missing.join(', ')}`
    );
  }
  return pack;
}

if (isMain(import.meta.url)) {
  const pack = await checkPackage();
  console.log(
    `Verified ${pack.entryCount} npm files: built runtime, declarations and plugin metadata only.`
  );
}
