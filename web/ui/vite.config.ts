import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

/*
 * The contract version is declared once, in C++, and read out of that header
 * here. Two constants to keep in step is one too many: a bundle that expects a
 * version the server never serves reloads, comes back expecting the same thing,
 * and reloads again.
 */
function apiVersion(): number {
  const header = fileURLToPath(new URL('../server/ApiVersion.h', import.meta.url));
  const match = /API_VERSION\s*=\s*(\d+)/.exec(readFileSync(header, 'utf8'));

  if (!match?.[1]) throw new Error(`no API_VERSION in ${header}`);

  return Number(match[1]);
}

/*
 * The C++ position server listens on 8080. Proxying /api through Vite keeps
 * the browser on a single origin during development, so nothing depends on the
 * server's permissive CORS headers.
 */
export default defineConfig({
  plugins: [react()],
  define: {
    __API_VERSION__: apiVersion(),
  },
  server: {
    port: 5173,
    proxy: {
      '/api': {
        target: process.env.INITIUM_SERVER ?? 'http://127.0.0.1:8080',
        changeOrigin: true,
      },
    },
  },
});
