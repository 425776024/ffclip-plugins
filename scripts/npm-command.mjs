import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { dirname, join } from 'node:path';

const root = new URL('../', import.meta.url);

export function authorizationUrls(output) {
  const plain = output.replace(/\x1b\[[0-9;?]*[A-Za-z]/g, '');
  return [...plain.matchAll(/https:\/\/[^\s<>"']+/g)]
    .map(([url]) => url)
    .filter((url) => {
      try {
        const parsed = new URL(url);
        return (
          ['www.npmjs.com', 'npmjs.com'].includes(parsed.hostname) &&
          /^\/(?:login|auth)(?:\/|$)/.test(parsed.pathname)
        );
      } catch {
        return false;
      }
    });
}

export async function openAuthorization(url) {
  const command = process.platform === 'darwin' ? 'open' : 'xdg-open';
  try {
    await run(command, [url], { stdio: 'ignore' });
    console.log('\n已打开 npm 授权页面，请在浏览器完成授权，脚本会继续。');
  } catch {
    console.log(`\n浏览器未能自动打开，请打开上面的授权 URL：\n${url}`);
  }
}

function run(command, args, options) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, options);
    child.once('error', reject);
    child.once('close', (code, signal) => {
      if (code === 0) resolve();
      else reject(new Error(`${command} 执行失败（${signal || code}）。`));
    });
  });
}

export function runNpm(
  args,
  { capture = false, authorize = false, cli, opener = openAuthorization } = {}
) {
  cli ||= process.env.npm_execpath;
  if (!cli) {
    // Also support direct node invocation with a standard Node installation.
    const candidate = join(dirname(process.execPath), '../lib/node_modules/npm/bin/npm-cli.js');
    if (existsSync(candidate)) cli = candidate;
  }
  let command = cli ? process.execPath : 'npm';
  let commandArgs = cli ? [cli, ...args] : args;
  let automaticBrowser = false;
  if (authorize && ['darwin', 'linux'].includes(process.platform)) {
    // npm requires a real terminal for publish 2FA. script supplies one while we
    // observe its URL; --browser=false avoids npm's extra ENTER prompt.
    automaticBrowser = true;
    commandArgs.push('--browser=false');
    if (process.platform === 'darwin') {
      commandArgs = ['-q', '/dev/null', command, ...commandArgs];
    } else {
      const quote = (value) => `'${value.replaceAll("'", "'\\''")}'`;
      commandArgs = ['-q', '-e', '-c', [command, ...commandArgs].map(quote).join(' '), '/dev/null'];
    }
    command = 'script';
  }
  return new Promise((resolve, reject) => {
    let stdout = '';
    let stderr = '';
    let pending = '';
    const opened = new Set();
    const openings = [];
    const child = spawn(command, commandArgs, {
      cwd: root,
      stdio: capture
        ? ['ignore', 'pipe', 'pipe']
        : automaticBrowser
          ? [process.stdin.isTTY ? 'inherit' : 'ignore', 'pipe', 'pipe']
          : 'inherit'
    });
    function observe(chunk, stream) {
      const text = chunk.toString();
      if (stream === 'stdout') stdout += text;
      else stderr += text;
      if (!capture) process[stream].write(chunk);
      if (!automaticBrowser) return;
      pending += text;
      // Only open complete lines: streams may split an authorization URL.
      const end = pending.lastIndexOf('\n');
      if (end < 0) return;
      for (const url of authorizationUrls(pending.slice(0, end))) {
        if (opened.has(url)) continue;
        opened.add(url);
        openings.push(
          Promise.resolve()
            .then(() => opener(url))
            .catch(() => {
              console.log(`请手动打开授权页面：${url}`);
            })
        );
      }
      pending = pending.slice(end + 1);
    }
    child.stdout?.on('data', (chunk) => observe(chunk, 'stdout'));
    child.stderr?.on('data', (chunk) => observe(chunk, 'stderr'));
    child.once('error', reject);
    child.once('close', async (code, signal) => {
      await Promise.all(openings);
      if (code === 0) resolve({ stdout, stderr });
      else {
        const error = new Error(`npm ${args[0]} 执行失败（${signal || code}）。`);
        Object.assign(error, { stdout, stderr, code });
        reject(error);
      }
    });
  });
}
