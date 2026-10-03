import { readFile, realpath } from 'node:fs/promises';
import { join } from 'node:path';
import {
  createProject,
  addAsset,
  addHtmlClip,
  addText,
  identity,
  ticks,
  validateProject
} from '../core/project.mjs';
import { probe } from './media.mjs';

const content = {
  zh: {
    name: 'ffclip · 让灵感成为作品',
    titles: ['成为作品', '由你掌控', '从这里开始'],
    variables: {
      locale: 'zh',
      edition: '一部关于创作的短片',
      chapter1: '01 — 灵感的起点',
      chapter2: '02 — 创作的层次',
      chapter3: '03 — 属于你的作品',
      headline1: '让灵感',
      headline2: '每一层',
      headline3: '你的第一支短片',
      description1: '从一句话，到一段值得分享的故事。',
      description2: '画面、文字与声音，在此相遇。',
      description3: '本地预览 · 自由编辑 · 即刻出发',
      animation: 'HTML 动画',
      titles: '可编辑花字',
      voiceover: '语音配音',
      captions: '逐句字幕',
      footer: '灵感没有终点，创作从这里开始。'
    },
    tracks: ['HTML 动画', '连续旁白', '渐变花字', '逐句字幕', '海岸画面']
  },
  en: {
    name: 'ffclip · Ideas into stories',
    titles: ['into stories.', 'is yours.', 'begins here.'],
    variables: {
      locale: 'en',
      edition: 'A SHORT FILM ABOUT CREATING',
      chapter1: '01 — WHERE IDEAS BEGIN',
      chapter2: '02 — LAYERS OF EXPRESSION',
      chapter3: '03 — YOUR NEXT STORY',
      headline1: 'Turn ideas',
      headline2: 'Every layer',
      headline3: 'Your next story',
      description1: 'One prompt. A story worth sharing.',
      description2: 'Picture, words and sound come together.',
      description3: 'LOCAL PREVIEW · FREE TO EDIT · READY TO GO',
      animation: 'HTML animation',
      titles: 'Editable titles',
      voiceover: 'Voiceover',
      captions: 'Captions',
      footer: 'Ideas keep moving. Your story starts here.'
    },
    tracks: [
      'HTML animation',
      'Continuous voiceover',
      'Gradient titles',
      'Captions',
      'Coastal picture'
    ]
  }
};

const curve = (points) => ({
  timeDomain: 'itemLocal',
  keyframes: points.map(([time, value, interpolation = 'linear']) => ({
    id: identity('keyframe'),
    time: ticks(time),
    value,
    interpolation
  }))
});

/** Create an independent editable copy with a closed set of packaged media assets. */
export async function createStarterProject(staticDir, { locale = 'zh', name } = {}) {
  if (!Object.hasOwn(content, locale)) throw new Error('示例语言必须是 zh 或 en');
  if (name !== undefined && (typeof name !== 'string' || !name.trim() || name.length > 200))
    throw new Error('作品名称需要 1–200 个字符');
  const copy = content[locale];
  const assetPath = await realpath(join(staticDir, 'starter', `narration-${locale}.wav`));
  const imagePath = await realpath(join(staticDir, 'starter', 'landscape.jpg'));
  const [sourceHtml, timing, asset, picture, imageBytes] = await Promise.all([
    readFile(join(staticDir, 'starter', 'starter.html'), 'utf8'),
    readFile(join(staticDir, 'starter', `captions-${locale}.json`), 'utf8'),
    probe(assetPath),
    probe(imagePath),
    readFile(imagePath)
  ]);
  const project = createProject(name?.trim() || copy.name);
  project.canvas = { width: 1280, height: 720 };
  const image = addAsset(project, picture);
  image.name = copy.tracks[4];
  project.timeline.tracks[0].name = copy.tracks[4];
  image.placement.end = image.clip.source.end = ticks(18);
  image.clip.visual.fitPolicy = 'cover';
  image.clip.automation = {
    'visual.scaleX': curve([
      [0, 1.02],
      [18, 1.1]
    ]),
    'visual.scaleY': curve([
      [0, 1.02],
      [18, 1.1]
    ]),
    'visual.positionX': curve([
      [0, 0],
      [18, -24]
    ])
  };
  addHtmlClip(project, {
    name: copy.tracks[0],
    html: {
      html: sourceHtml.replace('__LANDSCAPE_DATA__', imageBytes.toString('base64')),
      width: 1280,
      height: 720,
      duration: ticks(18),
      transparent: true,
      variables: { ...copy.variables, accent: '#a4e0d2', highlight: '#efc19a' }
    }
  });
  const audio = addAsset(project, asset);
  audio.name = copy.tracks[1];
  project.timeline.tracks.at(-1).name = copy.tracks[1];
  let titleTrack;
  for (const [index, title] of copy.titles.entries()) {
    const item = addText(project, {
      content: title,
      start: ticks(index * 6),
      length: ticks(6),
      template: {
        id: index === 0 ? 'flower-frost' : 'flower-gold',
        version: 1,
        style: { fontSize: locale === 'en' ? (index === 2 ? 140 : 128) : index === 1 ? 146 : 156 }
      },
      trackId: titleTrack
    });
    item.clip.visual.positionX =
      index === 2 ? 0 : index === 1 ? (locale === 'en' ? -408 : -383) : -358;
    item.clip.visual.positionY = 98;
    item.clip.automation = {
      'visual.opacity': curve([
        [0, 0],
        [0.32, 0],
        [0.95, 1, 'easeOut'],
        [5.5, 1],
        [5.94, index === 2 ? 1 : 0]
      ]),
      'visual.positionY': curve([
        [0, item.clip.visual.positionY + 22],
        [0.95, item.clip.visual.positionY, 'easeOut']
      ]),
      'visual.scaleX': curve([
        [0, 0.96],
        [1.15, 1, 'easeOut']
      ]),
      'visual.scaleY': curve([
        [0, 0.96],
        [1.15, 1, 'easeOut']
      ])
    };
    titleTrack ||= project.timeline.tracks[0].id;
    project.timeline.tracks[0].name = copy.tracks[2];
  }
  let captionTrack;
  for (const segment of JSON.parse(timing)) {
    const item = addText(project, {
      content: segment.text,
      start: ticks(segment.start),
      length: ticks(segment.end - segment.start),
      fontSize: locale === 'en' ? 27 : 30,
      trackId: captionTrack
    });
    item.clip.visual.positionY = 254;
    captionTrack ||= project.timeline.tracks[0].id;
    project.timeline.tracks[0].name = copy.tracks[3];
  }
  return validateProject(project);
}
