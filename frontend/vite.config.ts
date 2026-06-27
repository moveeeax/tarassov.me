import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import path from 'node:path';

// In dev, proxy /api/* to the backend so the SPA can call relative URLs
// and the cookies the backend sets land on the same origin. In prod the
// frontend container's nginx config does the same proxy_pass.
export default defineConfig({
  plugins: [react()],
  resolve: {
    alias: { '@': path.resolve(__dirname, './src') },
  },
  server: {
    port: 5173,
    proxy: {
      '/api': {
        target: 'http://localhost:8080',
        changeOrigin: true,
      },
    },
  },
  build: {
    outDir: 'dist',
    // No sourcemaps in the production bundle — they'd publish readable
    // source for the public fork. Dev/preview still get inline maps via
    // Vite's defaults. Override with VITE_SOURCEMAP=1 for local debugging.
    sourcemap: process.env.VITE_SOURCEMAP === '1',
  },
});
