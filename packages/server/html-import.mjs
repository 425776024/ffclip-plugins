import { readFile } from 'node:fs/promises';
import { dirname, resolve, extname, basename } from 'node:path';
import { parse, serialize } from 'parse5';
import { mime } from './media.mjs';
import { validateHtmlContent } from '../core/project.mjs';

/** Freeze a local HTML document and its relative graphics resources into the DTO. */
export async function importHtml(path, options, allowed) {
  path = await allowed(path);
  if (!['.html', '.htm'].includes(extname(path).toLowerCase())) throw new Error('请选择 HTML 文件');
  let total = 0;
  const read = async (resource) => {
    resource = await allowed(resource);
    const bytes = await readFile(resource);
    total += bytes.length;
    if (total > 1024 * 1024) throw new Error('HTML 和资源合计不能超过 1 MiB');
    return bytes;
  };
  const tree = parse((await read(path)).toString('utf8'));
  const resource = async (value, directory) => {
    if (value.startsWith('data:') || value.startsWith('#')) return value;
    if (/^(?:[a-z]+:|\/\/)/i.test(value)) throw new Error('HTML 资源必须来自本地授权目录');
    const target = resolve(directory, decodeURIComponent(value.split(/[?#]/)[0]));
    const bytes = await read(target);
    const fragment = value.includes('#') ? '#' + value.split('#').slice(1).join('#') : '';
    return `data:${mime[extname(target).toLowerCase()] || 'application/octet-stream'};base64,${bytes.toString('base64')}${fragment}`;
  };
  const css = async (source, directory) => {
    if (/@import\b/i.test(source)) throw new Error('请将 CSS @import 合并到本地样式文件');
    const matches = [...source.matchAll(/url\(\s*(['"]?)(.*?)\1\s*\)/gi)];
    for (const match of matches) source = source.replace(match[0], `url("${await resource(match[2], directory)}")`);
    return source;
  };
  const textNode = (value, parentNode) => ({ nodeName: '#text', value, parentNode });
  const visit = async (node) => {
    const dir = dirname(path);
    if (node.tagName === 'script') {
      const src = node.attrs.find((a) => a.name === 'src');
      if (src && /(?:^|\/)gsap(?:\.min)?\.js(?:[?#]|$)/i.test(src.value)) {
        node.parentNode.childNodes = node.parentNode.childNodes.filter((child) => child !== node);
        return;
      }
      if (src && !src.value.startsWith('data:')) {
        if (/^(?:[a-z]+:|\/\/)/i.test(src.value)) throw new Error('HTML 脚本必须来自本地授权目录');
        const source = (await read(resolve(dir, decodeURIComponent(src.value.split(/[?#]/)[0])))).toString('utf8');
        node.attrs = node.attrs.filter((a) => a !== src);
        node.childNodes = [textNode(source.replace(/<\/script/gi, '<\\/script'), node)];
      }
    }
    if (node.tagName === 'link' && node.attrs.some((a) => a.name === 'rel' && a.value === 'stylesheet')) {
      const href = node.attrs.find((a) => a.name === 'href')?.value;
      if (!href || /^(?:[a-z]+:|\/\/)/i.test(href)) throw new Error('HTML 样式必须来自本地授权目录');
      const target = resolve(dir, decodeURIComponent(href.split(/[?#]/)[0]));
      node.tagName = node.nodeName = 'style'; node.attrs = [];
      node.childNodes = [textNode(await css((await read(target)).toString('utf8'), dirname(target)), node)];
    } else if (node.tagName === 'style') {
      node.childNodes = [textNode(await css(node.childNodes.map((n) => n.value || '').join(''), dir), node)];
    }
    for (const attr of node.attrs || []) {
      if (attr.name === 'style') attr.value = await css(attr.value, dir);
      if ((attr.name === 'src' && node.tagName !== 'script') || (['href', 'xlink:href'].includes(attr.name) && ['image', 'use'].includes(node.tagName)))
        attr.value = await resource(attr.value, dir);
      if (attr.name === 'srcset') throw new Error('HTML 导入请使用单一 img src，避免 srcset 不确定资源');
    }
    for (const child of [...(node.childNodes || [])]) await visit(child);
  };
  await visit(tree);
  const html = { html: serialize(tree), width: options.width ?? 1920, height: options.height ?? 1080,
    duration: options.duration ?? 600000, transparent: options.transparent ?? true,
    ...(options.variables ? { variables: options.variables } : {}) };
  validateHtmlContent(html);
  return { name: basename(path), html };
}
