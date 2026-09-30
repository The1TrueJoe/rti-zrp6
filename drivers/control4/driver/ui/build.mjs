// Builds the driver's Composer tabs, each into one self-contained HTML file.
//
//   node driver/ui/build.mjs <out dir> [--watch]
//
// For each page below, its entry module (and everything it imports) is
// bundled by esbuild into one IIFE, styles.css is bundled alongside, and both
// are inlined into the page's shell. Composer loads a tab as a single file
// (driver.xml: <tab name="Remote Buttons" file="html/index.html"/>, <tab
// name="Control" file="html/control.html"/>), so the output has no external
// references.
import * as esbuild from 'esbuild';
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const outDir = resolve(process.argv[2] || 'build/ui');

const PAGES = [
  { shell: 'shell.html', entry: 'src/main.js', out: 'index.html' },        // Remote Buttons
  { shell: 'control.html', entry: 'src/control/main.js', out: 'control.html' }, // Control
];
const watch = process.argv.includes('--watch');

// Composer's webview is Chromium-based (finitelabs' shipping tab pages use
// async/await and fetch in it); chrome80 keeps the output conservative.
const common = { bundle: true, write: false, target: ['chrome80'], logLevel: 'warning', legalComments: 'none' };

async function buildPage(page, style) {
  const js = await esbuild.build({ ...common, entryPoints: [join(here, page.entry)], format: 'iife' });
  const script = js.outputFiles[0].text.replace(/<\/script/gi, '<\\/script');
  const shell = readFileSync(join(here, page.shell), 'utf8');
  if (!shell.includes('/*STYLE*/') || !shell.includes('/*SCRIPT*/')) {
    throw new Error(page.shell + ' needs /*STYLE*/ and /*SCRIPT*/ placeholders');
  }
  // Function replacements: the bundle may contain `$&` and friends.
  const html = shell.replace('/*STYLE*/', () => style).replace('/*SCRIPT*/', () => script);
  const out = join(outDir, page.out);
  writeFileSync(out, html);
  console.log(`  OK  ${out} (${html.length} bytes)`);
}

async function build() {
  const css = await esbuild.build({ ...common, entryPoints: [join(here, 'styles.css')], loader: { '.css': 'css' } });
  const style = css.outputFiles[0].text.replace(/<\/style/gi, '<\\/style');
  mkdirSync(outDir, { recursive: true });
  for (const page of PAGES) await buildPage(page, style);
}

await build();
if (watch) {
  const { watch: fsWatch } = await import('node:fs');
  console.log('watching driver/ui for changes');
  let timer = null;
  fsWatch(here, { recursive: true }, () => {
    clearTimeout(timer);
    timer = setTimeout(() => build().catch((e) => console.error(e.message)), 100);
  });
}
