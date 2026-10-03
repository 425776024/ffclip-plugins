import { spawn } from 'node:child_process';
import { extname, basename } from 'node:path';
import { stat, open } from 'node:fs/promises';
import { createReadStream } from 'node:fs';
import { createHash } from 'node:crypto';
import { Input, FilePathSource, ALL_FORMATS } from 'mediabunny';
import { identity, ticks } from '../core/project.mjs';
import { containerTrackIndices, jpegOrientation } from './media-metadata.mjs';

export function run(
  command,
  args,
  { input, timeout = 30000, binary = false, maxOutput = 4 * 1024 * 1024, signal } = {}
) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, {
      stdio: ['pipe', 'pipe', 'pipe'],
      windowsHide: true,
      signal
    });
    let stdout = Buffer.alloc(0),
      stderr = '',
      timedOut = false;
    const timer = setTimeout(() => {
      timedOut = true;
      child.kill('SIGKILL');
    }, timeout);
    child.stdout.on('data', (data) => {
      if (stdout.length + data.length > maxOutput) {
        clearTimeout(timer);
        child.kill('SIGKILL');
        reject(new Error(`${command}: 输出超过 ${maxOutput} 字节限制`));
        return;
      }
      stdout = Buffer.concat([stdout, data]);
    });
    child.stderr.on('data', (data) => {
      stderr = (stderr + data).slice(-16384);
    });
    child.once('error', (error) => {
      clearTimeout(timer);
      reject(new Error(`${command}: ${error.message}`));
    });
    child.once('close', (code) => {
      clearTimeout(timer);
      if (timedOut) reject(new Error(`${command} 执行超时`));
      else if (code !== 0) reject(new Error(`${command}: ${stderr || stdout || `退出码 ${code}`}`));
      else resolve(binary ? stdout : stdout.toString('utf8'));
    });
    child.stdin.on('error', () => {});
    child.stdin.end(input);
  });
}

const imageExtensions = new Set(['.jpg', '.jpeg', '.png', '.webp', '.bmp']);
export const mediaExtensions = new Set([
  ...imageExtensions,
  '.mp4',
  '.mov',
  '.m4v',
  '.webm',
  '.mkv',
  '.mp3',
  '.wav',
  '.m4a',
  '.aac',
  '.flac',
  '.ogg'
]);
export const mime = {
  '.mp4': 'video/mp4',
  '.mov': 'video/quicktime',
  '.m4v': 'video/mp4',
  '.webm': 'video/webm',
  '.mkv': 'video/x-matroska',
  '.mp3': 'audio/mpeg',
  '.wav': 'audio/wav',
  '.m4a': 'audio/mp4',
  '.aac': 'audio/aac',
  '.flac': 'audio/flac',
  '.ogg': 'audio/ogg',
  '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.png': 'image/png',
  '.webp': 'image/webp',
  '.bmp': 'image/bmp',
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript',
  '.mjs': 'text/javascript',
  '.json': 'application/json',
  '.wasm': 'application/wasm',
  '.ttf': 'font/ttf',
  '.otf': 'font/otf',
  '.txt': 'text/plain; charset=utf-8',
  '.md': 'text/plain; charset=utf-8',
  '.css': 'text/css',
  '.svg': 'image/svg+xml'
};

function codecName(codec) {
  if (codec === 'avc') return 'h264';
  if (codec === 'ulaw' || codec === 'alaw') return `pcm_${codec}`;
  if (/^pcm-[suf]\d+$/.test(codec || ''))
    return codec.replace('pcm-', 'pcm_') + (codec.endsWith('8') ? '' : 'le');
  if (codec?.startsWith('pcm-')) return codec.replace('pcm-', 'pcm_');
  return codec;
}

// Pure JavaScript metadata is always the first path. An external tool is used
// only when a caller explicitly configures its executable as a fallback.
export async function probe(path, command) {
  try {
    return await probeContainer(path);
  } catch (error) {
    if (error.code === 'MEDIA_SIGNAL_UNSUPPORTED' || typeof command !== 'string' || !command.trim())
      throw error;
    return probeExternal(path, command);
  }
}

async function probeExternal(path, command) {
  const extension = extname(path).toLowerCase();
  if (!mediaExtensions.has(extension)) throw new Error('不支持的素材类型');
  const file = await stat(path);
  if (!file.isFile()) throw new Error('素材不是文件');
  const info = JSON.parse(
    await run(command, ['-v', 'error', '-show_streams', '-show_format', '-of', 'json', path])
  );
  const streams = info.streams || [];
  const video = streams.find((s) => s.codec_type === 'video' && !s.disposition?.attached_pic);
  const audio = streams.find((s) => s.codec_type === 'audio');
  if (['smpte2084', 'arib-std-b67'].includes(video?.color_transfer))
    throw new Error('当前 SDR 编辑管线不支持 HDR 素材，请使用 SDR 素材');
  const isImage = imageExtensions.has(extension);
  if (!video && !audio) throw new Error('文件没有可用的音视频轨道');
  const length = Number(info.format?.duration || video?.duration || audio?.duration);
  if (!isImage && (!Number.isFinite(length) || length <= 0)) throw new Error('无法读取素材时长');
  const rotation = Number(
    video?.side_data_list?.find((s) => s.rotation !== undefined)?.rotation ||
      video?.tags?.rotate ||
      0
  );
  const swapped = Math.abs(rotation % 180) === 90;
  return {
    id: identity('asset'),
    path,
    name: basename(path),
    size: file.size,
    sourceIdentity: `${file.dev}:${file.ino}:${file.size}:${file.mtimeMs}`,
    kind: isImage ? 'image' : video ? 'video' : 'audio',
    duration: ticks(isImage ? 5 : length),
    width: video ? (swapped ? video.height : video.width) : 0,
    height: video ? (swapped ? video.width : video.height) : 0,
    hasAudio: Boolean(audio),
    firstTimestamp: Number(info.format?.start_time) || 0,
    streams
  };
}

// Metadata parsing only. No native decoder or external media process is used.
export async function probeContainer(path) {
  const extension = extname(path).toLowerCase();
  if (!mediaExtensions.has(extension)) throw new Error('不支持的素材类型');
  const file = await stat(path);
  if (!file.isFile()) throw new Error('素材不是文件');
  const asset = {
    id: identity('asset'),
    path,
    name: basename(path),
    size: file.size,
    sourceIdentity: `${file.dev}:${file.ino}:${file.size}:${file.mtimeMs}`,
    duration: ticks(5),
    width: 0,
    height: 0,
    hasAudio: false,
    kind: 'image',
    firstTimestamp: 0,
    streams: []
  };
  if (imageExtensions.has(extension)) {
    let orientation = 1;
    const handle = await open(path, 'r');
    try {
      const bytes = Buffer.alloc(Math.min(file.size, 1024 * 1024));
      await handle.read(bytes, 0, bytes.length, 0);
      [asset.width, asset.height] = imageDimensions(bytes);
      orientation = jpegOrientation(bytes);
    } finally {
      await handle.close();
    }
    asset.streams = [
      {
        index: 0,
        codec_type: 'video',
        codec_name: extension.slice(1),
        width: asset.width,
        height: asset.height,
        disposition: { attached_pic: 0 },
        ...(orientation !== 1 ? { exif_orientation: orientation } : {})
      }
    ];
    if (orientation >= 5) [asset.width, asset.height] = [asset.height, asset.width];
    return asset;
  }
  const input = new Input({
    source: new FilePathSource(path, { maxCacheSize: 4 * 1024 * 1024 }),
    formats: ALL_FORMATS
  });
  try {
    const tracks = await input.getTracks();
    const video = await input.getPrimaryVideoTrack();
    const audio = await input.getPrimaryAudioTrack();
    if (!video && !audio) throw new Error('文件没有可用的音视频轨道');
    // Serialize initial packet discovery; concurrent first/duration reads race
    // the shared FLAC demuxer initialization in Mediabunny 1.61.0.
    const first = Math.max(0, await input.getFirstTimestamp());
    const end = await input.computeDuration();
    if (!Number.isFinite(end - first) || end <= first) throw new Error('无法读取素材时长');
    asset.firstTimestamp = first;
    asset.duration = ticks(end - first);
    asset.kind = video ? 'video' : 'audio';
    asset.hasAudio = Boolean(audio);
    if (video) {
      asset.width = await video.getDisplayWidth();
      asset.height = await video.getDisplayHeight();

      if (await video.hasHighDynamicRange())
        throw Object.assign(new Error('当前 SDR 编辑管线不支持 HDR 素材，请使用 SDR 素材'), {
          code: 'MEDIA_SIGNAL_UNSUPPORTED'
        });
    }
    const streamIndices = await containerTrackIndices(path, extension, file.size);
    const selectedTracks = [
      video,
      audio,
      ...tracks.filter((track) => track !== video && track !== audio)
    ].filter(Boolean);
    for (const track of selectedTracks) {
      const ordinal = tracks.indexOf(track);
      if (!track.isAudioTrack() && !track.isVideoTrack()) continue;
      const index = streamIndices ? streamIndices.get(track.id) : ordinal;
      if (!Number.isSafeInteger(index)) throw new Error('容器轨道不能映射到原生 stream index');
      const firstTimestamp = await track.getFirstTimestamp();
      const endTimestamp = await track.computeDuration();
      const resolution = await track.getTimeResolution();
      const disposition = await track.getDisposition();
      const stream = {
        index,
        id: track.id,
        codec_type: track.type,
        codec_name:
          codecName(await track.getCodec()) ||
          String((await track.getInternalCodecId()) || 'unknown'),
        start_time: String(firstTimestamp),
        duration: String(endTimestamp - Math.max(0, firstTimestamp)),
        time_base: `1/${resolution}`,
        start_pts: Math.round(firstTimestamp * resolution),
        duration_ts: Math.round((endTimestamp - Math.max(0, firstTimestamp)) * resolution),
        disposition: { default: disposition.default ? 1 : 0, attached_pic: 0 }
      };
      if (track.isVideoTrack()) {
        const ratio = await track.getPixelAspectRatio(),
          rotation = await track.getRotation();
        const rates = await track.computeFrameRateMetrics({ targetPacketCount: 256 });
        Object.assign(stream, {
          width: await track.getCodedWidth(),
          height: await track.getCodedHeight(),
          display_width: await track.getDisplayWidth(),
          display_height: await track.getDisplayHeight(),
          sample_aspect_ratio: `${ratio.num}:${ratio.den}`,
          side_data_list: [{ rotation: rotation > 180 ? 360 - rotation : -rotation }],
          display_rotation: rotation,
          color_space: await track.getColorSpace(),
          frame_rate_metrics: rates // bounded timestamp sample; not a whole-file CFR guarantee
        });
      } else
        Object.assign(stream, {
          sample_rate: await track.getSampleRate(),
          channels: await track.getNumberOfChannels()
        });
      asset.streams.push(stream);
    }

    return asset;
  } finally {
    input.dispose();
  }
}

export function imageDimensions(b) {
  let width = 0,
    height = 0;
  if (b.length >= 24 && b.subarray(0, 8).equals(Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]))) {
    width = b.readUInt32BE(16);
    height = b.readUInt32BE(20);
  } else if (b.length >= 26 && b.toString('ascii', 0, 2) === 'BM') {
    width = Math.abs(b.readInt32LE(18));
    height = Math.abs(b.readInt32LE(22));
  } else if (
    b.length >= 30 &&
    b.toString('ascii', 0, 4) === 'RIFF' &&
    b.toString('ascii', 8, 12) === 'WEBP'
  ) {
    const kind = b.toString('ascii', 12, 16);
    if (kind === 'VP8X') {
      width = 1 + b.readUIntLE(24, 3);
      height = 1 + b.readUIntLE(27, 3);
    } else if (kind === 'VP8 ') {
      width = b.readUInt16LE(26) & 16383;
      height = b.readUInt16LE(28) & 16383;
    } else if (kind === 'VP8L') {
      const bits = b.readUInt32LE(21);
      width = (bits & 16383) + 1;
      height = ((bits >>> 14) & 16383) + 1;
    }
  } else if (b.length > 4 && b[0] === 255 && b[1] === 216) {
    let at = 2;
    while (at + 8 < b.length) {
      if (b[at++] !== 255) continue;
      const marker = b[at++];
      if (marker === 255 || marker === 216) continue;
      if (marker === 217 || marker === 218) break;
      const n = b.readUInt16BE(at);
      if (n < 2 || at + n > b.length) break;
      if ([192, 193, 194, 195, 197, 198, 199, 201, 202, 203, 205, 206, 207].includes(marker)) {
        height = b.readUInt16BE(at + 3);
        width = b.readUInt16BE(at + 5);
        break;
      }
      at += n;
    }
  }
  if (!width || !height || width > 65536 || height > 65536) throw new Error('无法读取图片尺寸');
  return [width, height];
}

export async function fingerprint(path) {
  const hash = createHash('sha256');
  for await (const chunk of createReadStream(path)) hash.update(chunk);
  return `sha256:${hash.digest('hex')}`;
}
