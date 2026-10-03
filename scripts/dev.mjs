import { spawn } from 'node:child_process';
import { startServer } from '../packages/server/index.mjs';
const server = await startServer({
  roots: process.argv.slice(2).length ? process.argv.slice(2) : [process.cwd()]
});
const vite = spawn(process.execPath, ['node_modules/vite/bin/vite.js'], { stdio: 'inherit' });
async function close() {
  vite.kill();
  await server.close();
}
process.on('SIGINT', close);
process.on('SIGTERM', close);
vite.on('exit', () => {
  server.close();
});
