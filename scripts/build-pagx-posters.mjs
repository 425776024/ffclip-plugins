// Generate library posters with the same WASM worker used by preview/export.
import { spawn } from 'node:child_process';
const child = spawn(process.execPath, ['--test', 'tests/pagx-templates-browser.test.mjs'], {
  stdio: 'inherit',
  env: { ...process.env, PAGX_WRITE_POSTERS: '1' }
});
child.on('error', (error) => {
  console.error(error);
  process.exitCode = 1;
});
child.on('exit', (code) => {
  process.exitCode = code ?? 1;
});
