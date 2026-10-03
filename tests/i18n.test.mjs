import test from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';
import { mkdtemp, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import {
  browserLocale,
  resolveLocale,
  translate,
  translateMessage
} from '../src/editor/i18n/runtime.mjs';
import { english } from '../src/editor/i18n/messages.mjs';
import { readFile, readdir } from 'node:fs/promises';
import { createRequire } from 'node:module';
import ts from 'typescript';
import { parse as parseVue } from 'vue/compiler-sfc';

test('all literal interface messages have an English translation', async () => {
  const { baseParse, NodeTypes } = createRequire(import.meta.resolve('vue/compiler-sfc'))(
    '@vue/compiler-dom'
  );
  const missing = new Set();
  const scan = (code) => {
    const source = ts.createSourceFile('message.ts', code, ts.ScriptTarget.Latest, true);
    const check = (node) => {
      if (ts.isStringLiteral(node) || ts.isNoSubstitutionTemplateLiteral(node)) {
        if (node.text && /\p{Script=Han}/u.test(node.text) && !Object.hasOwn(english, node.text))
          missing.add(node.text);
      } else if (ts.isConditionalExpression(node)) {
        check(node.whenTrue);
        check(node.whenFalse);
      }
    };
    const visit = (node) => {
      if (
        ts.isCallExpression(node) &&
        node.expression.getText(source) === 'tr' &&
        node.arguments[0]
      )
        check(node.arguments[0]);
      ts.forEachChild(node, visit);
    };
    visit(source);
  };
  for (const name of (await readdir('src/editor')).filter((name) => name.endsWith('.vue'))) {
    const { descriptor } = parseVue(await readFile(`src/editor/${name}`, 'utf8'));
    scan(descriptor.scriptSetup?.content || '');
    if (!descriptor.template) continue;
    const visit = (node) => {
      if (node.type === NodeTypes.INTERPOLATION) scan(node.content.content);
      if (node.type === NodeTypes.ELEMENT)
        for (const prop of node.props)
          if (prop.type === NodeTypes.DIRECTIVE && prop.exp) scan(prop.exp.content);
      for (const child of node.children || []) visit(child);
    };
    visit(baseParse(descriptor.template.content));
  }
  assert.deepEqual([...missing], []);
});

test('Chinese variants and English follow the first preferred computer language', () => {
  for (const language of ['zh', 'zh-CN', 'zh-TW', 'zh-Hant', 'ZH_hans'])
    assert.equal(resolveLocale([language, 'en-US']), 'zh-CN');
  for (const language of ['en', 'en-US', 'en-GB', 'ja-JP', 'de-DE'])
    assert.equal(resolveLocale([language, 'zh-CN']), 'en');
  assert.equal(resolveLocale(['', 'zh-CN']), 'zh-CN');
  assert.equal(browserLocale({ language: 'zh-TW' }), 'zh-CN');
  assert.equal(browserLocale({ languages: [], language: 'en-US' }), 'en');
  assert.equal(browserLocale({}), 'en');
});

test('UI translations preserve interpolation values, including filenames and braces', () => {
  const name = '标题.mp4 {count}';
  assert.equal(
    translate('en', '{name} · 双击添加到时间轴', { name }),
    `${name} · Double-click to add to timeline`
  );
  assert.equal(translate('zh-CN', '{count} 轨道', { count: 3 }), '3 轨道');
  assert.equal(translate('en', '未注册的技术错误'), '未注册的技术错误');
  assert.equal(translate('en', undefined), '');
  assert.equal(
    translateMessage('en', '已导出：/作品/默认文字.mp4'),
    'Exported: /作品/默认文字.mp4'
  );
  assert.equal(translateMessage('en', '字号需要是有效数字。'), 'Font size must be a valid number.');
  assert.equal(translateMessage('en', '上内边距'), 'Top padding');
  assert.equal(
    translateMessage('en', '已粘贴 2 个素材到时间轴；跳过 1 个文件：字幕.txt'),
    'Pasted 2 media items into timeline; skipped 1 files: 字幕.txt'
  );
});

test('every translated message preserves its named parameters', () => {
  const tokens = (value) => [...value.matchAll(/\{(\w+)\}/g)].map((match) => match[1]).sort();
  for (const [source, translation] of Object.entries(english)) {
    assert.ok(translation.trim(), source);
    assert.deepEqual(tokens(translation), tokens(source), source);
  }
});

const compile = async (entry) => {
  const result = await build({
    entryPoints: [entry],
    bundle: true,
    platform: 'node',
    format: 'esm',
    write: false,
    logLevel: 'silent'
  });
  const directory = await mkdtemp(join(tmpdir(), 'videocut-i18n-'));
  const file = join(directory, 'fixture.mjs');
  await writeFile(file, result.outputFiles[0].text);
  try {
    return await import(pathToFileURL(file).href);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
};

test('languagechange updates reactive labels and document language and can be cleaned up', async () => {
  const original = Object.getOwnPropertyDescriptor(globalThis, 'navigator');
  const prior = Object.fromEntries(
    ['window', 'document', 'location'].map((key) => [key, globalThis[key]])
  );
  const preferences = { languages: ['zh-TW'], language: 'zh-TW' };
  Object.defineProperty(globalThis, 'navigator', { configurable: true, value: preferences });
  globalThis.window = new EventTarget();
  globalThis.document = { documentElement: { lang: '' }, title: '', createElement: () => ({}) };
  globalThis.location = { pathname: '/', search: '' };
  try {
    const { followSystemLanguage, useI18n } = await compile('src/editor/i18n.ts');
    const { locale, tr } = useI18n();
    const stop = followSystemLanguage();
    assert.equal(locale.value, 'zh-CN');
    assert.equal(tr('导出'), '导出');
    assert.equal(document.documentElement.lang, 'zh-CN');
    preferences.languages = ['en-US'];
    window.dispatchEvent(new Event('languagechange'));
    assert.equal(locale.value, 'en');
    assert.equal(tr('导出'), 'Export');
    assert.equal(document.title, 'VideoCut · Local editor');
    location.pathname = '/projects/用户标题';
    document.title = '用户标题 · VideoCut';
    preferences.languages = ['zh-CN'];
    window.dispatchEvent(new Event('languagechange'));
    assert.equal(tr('导出'), '导出');
    assert.equal(document.title, '用户标题 · VideoCut');
    stop();
    preferences.languages = ['en-US'];
    window.dispatchEvent(new Event('languagechange'));
    assert.equal(locale.value, 'zh-CN');
  } finally {
    if (original) Object.defineProperty(globalThis, 'navigator', original);
    else delete globalThis.navigator;
    for (const [key, value] of Object.entries(prior)) {
      if (value === undefined) delete globalThis[key];
      else globalThis[key] = value;
    }
  }
});

test('localizing a new built-in HTML preset preserves the original and animation code', async () => {
  const { localizeHtmlPreset } = await compile('src/editor/localize-html-preset.ts');
  const source = {
    html: '<html><body><h1>每一帧，都有故事</h1><p>自由剪辑，灵感成片</p><script>window.tick=t=>{window.frame=t;};</script></body></html>',
    variables: { title: '让想法\n闪耀全场', unknown: '自定义内容' },
    width: 1920,
    height: 1080,
    duration: 720000,
    transparent: true
  };
  const original = structuredClone(source);
  const localized = localizeHtmlPreset(source, 'en');
  assert.deepEqual(source, original);
  assert.match(localized.html, /Every frame tells a story/);
  assert.ok(localized.html.includes('<script>window.tick=t=>{window.frame=t;};</script>'));
  assert.equal(localized.variables.title, 'Let ideas\nshine bright');
  assert.equal(localized.variables.unknown, '自定义内容');
  for (const key of ['width', 'height', 'duration', 'transparent'])
    assert.equal(localized[key], source[key]);
  assert.deepEqual(localizeHtmlPreset(source, 'zh-CN'), source);
});

test('authored HTML labels stay distinct from localizable inspector labels', async () => {
  const { parseHtmlProperties } = await compile('src/editor/html-properties.ts');
  const fields = parseHtmlProperties({
    html: '<style>#named { color: red; }</style><h1 id="named" data-label="标题">用户的文字</h1><p>用户正文</p>',
    variables: { title: 'User title', 标题: 'Custom variable' },
    width: 640,
    height: 360,
    duration: 120000,
    transparent: true
  });
  assert.equal(fields.find((field) => field.value === '用户的文字').labelIsAuthored, true);
  assert.equal(fields.find((field) => field.value === '用户正文').labelIsAuthored, undefined);
  assert.equal(fields.find((field) => field.kind === 'color').contextIsAuthored, true);
  assert.equal(fields.find((field) => field.variable === '标题').labelIsAuthored, true);
  assert.equal(fields.find((field) => field.variable === 'title').labelIsAuthored, undefined);
});
