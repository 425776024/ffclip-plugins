/** Codec policy is independent of the browser probes and never requires a local binary. */
// Safari's quality AVC encoder buffers frames beyond its WebCodecs queue limit, preventing dequeue.
// https://github.com/Vanilagy/mediabunny/issues/541
export function isSafari(userAgent = '') {
  return /AppleWebKit\//.test(userAgent) && /Version\/.+Safari\//.test(userAgent);
}
export function videoLatencyMode(codec, userAgent = '') {
  return codec === 'avc' && isSafari(userAgent) ? 'realtime' : 'quality';
}

export function selectEncoding({
  format,
  localEncoder,
  videoSupported,
  audioSupported,
  preferredEncoding = 'auto'
}) {
  if (!videoSupported && !localEncoder)
    throw new Error(
      format === 'mp4'
        ? '当前浏览器无法以此分辨率编码 H.264，请选择 WebM 或降低分辨率。'
        : '当前浏览器无法以此分辨率编码 WebM，请降低分辨率或更换浏览器。'
    );
  if (!audioSupported && !localEncoder)
    throw new Error(
      format === 'mp4' ? '当前浏览器不支持 AAC 编码，请选择 WebM。' : '当前浏览器不支持 Opus 编码。'
    );
  const detected = !videoSupported
    ? 'frames-with-pcm'
    : !audioSupported
      ? 'video-with-pcm'
      : 'browser';
  const encoding = preferredEncoding === 'auto' ? detected : preferredEncoding;
  if (!['browser', 'video-with-pcm', 'frames-with-pcm'].includes(encoding))
    throw new Error('未知编码方案');
  if (encoding !== 'browser' && !localEncoder) throw new Error('此服务未提供本地编码器');
  if (encoding !== 'frames-with-pcm' && !videoSupported)
    throw new Error('所选方案需要浏览器视频编码能力');
  if (encoding === 'browser' && !audioSupported) throw new Error('所选方案需要浏览器音频编码能力');
  return encoding;
}
