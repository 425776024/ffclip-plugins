export function isSafari(userAgent?: string): boolean;
export function videoLatencyMode(codec: string, userAgent?: string): 'quality' | 'realtime';
export function selectEncoding(options: {
  format: 'mp4' | 'webm';
  localEncoder: boolean;
  videoSupported: boolean;
  audioSupported: boolean;
  preferredEncoding?: 'auto' | 'browser' | 'video-with-pcm' | 'frames-with-pcm';
}): 'browser' | 'video-with-pcm' | 'frames-with-pcm';
