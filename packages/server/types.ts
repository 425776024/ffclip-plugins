import type { Project } from '../core/types.js';
export interface ServerOptions {
  roots?: string[];
  ffprobe?: string;
  ffmpeg?: string;
  port?: number;
  staticDir?: string;
  nativeBridge?: string;
  /** Optional raw-pixel HTML video encoder. Without it the browser compositor is used. */
  htmlRenderer?: string;
  initialProject?: Project;
  initialProjectPath?: string;
  /** Open an editable built-in project with packaged voiceover and captions. */
  initialDemo?: { locale?: 'zh' | 'en'; name?: string };
  /** Persistent local Kokoro cache; downloaded only by an explicit install request. */
  ttsModelDir?: string;
  asrModelDir?: string;
  /** FastVLM cache and initialization preference; installation requires UI consent. */
  visionModelDir?: string;
}
