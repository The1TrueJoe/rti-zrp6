import { defineConfig } from 'vite';
import preact from '@preact/preset-vite';
import tailwindcss from '@tailwindcss/vite';

/* This bundle is programmed into the board's Flash and served by an 8032, so
 * size is a hard constraint rather than a preference. Preact instead of React,
 * a hand-written MQTT client instead of mqtt.js, and inline SVG instead of an
 * icon package, together take it from 184 KB gzipped to a fraction of that. */
export default defineConfig({
  plugins: [preact(), tailwindcss()],
  server: { host: true, port: 5273 },
  build: {
    target: 'es2020',
    cssCodeSplit: false,
    reportCompressedSize: true,
    rollupOptions: {
      output: {
        /* One file each. The server is a state machine in 8051 assembly's
         * spiritual neighbourhood; every extra request is another thing it has
         * to get right. */
        inlineDynamicImports: true,
        entryFileNames: 'app.js',
        assetFileNames: 'app.[ext]',
      },
    },
    minify: 'terser',
    terserOptions: {
      compress: { passes: 3, unsafe: true, drop_console: true },
      format: { comments: false },
    },
  },
});
