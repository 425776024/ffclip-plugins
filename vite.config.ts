import { pagxRuntimeAssets } from './scripts/pagx-assets.mjs';
import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';
import { textTemplateAssets } from './scripts/text-assets.mjs';
import { ttsRuntimeAssets } from './scripts/tts-assets.mjs';
import { asrRuntimeAssets } from './scripts/asr-assets.mjs';

export default defineConfig({
  plugins: [vue(), textTemplateAssets(), ttsRuntimeAssets(), asrRuntimeAssets(), pagxRuntimeAssets()],
  publicDir: false,
  worker: { format: 'es' },
  build: { target: 'es2022', outDir: 'dist/web', sourcemap: false, minify: 'esbuild' },
  server: {
    host: '127.0.0.1',
    port: 5173,
    strictPort: true,
    proxy: {
      '/vision-models': {
        target: process.env.VIDEOCUT_SERVER || 'http://127.0.0.1:4318',
        changeOrigin: true,
        configure(proxy) {
          proxy.on('proxyReq', (req) => req.removeHeader('origin'));
        }
      },
      '/asr-models': {
        target: process.env.VIDEOCUT_SERVER || 'http://127.0.0.1:4318',
        changeOrigin: true,
        configure(proxy) {
          proxy.on('proxyReq', (req) => req.removeHeader('origin'));
        }
      },
      '/tts-models': {
        target: process.env.VIDEOCUT_SERVER || 'http://127.0.0.1:4318',
        changeOrigin: true,
        configure(proxy) {
          proxy.on('proxyReq', (req) => {
            req.removeHeader('origin');
          });
        }
      },
      '/api': {
        target: process.env.VIDEOCUT_SERVER || 'http://127.0.0.1:4318',
        changeOrigin: true,
        configure(proxy) {
          proxy.on('proxyReq', (req) => {
            req.removeHeader('origin');
          });
        }
      }
    }
  }
});
