#!/usr/bin/env python3
"""Turn the built UI into one gzipped blob for main Flash page 0.

Vite emits three files. A browser fetching three files needs three sockets and
this board has four in total, so they are inlined into a single index.html
first — which also compresses better than three streams do separately.

The blob the firmware expects (see src/http.c):

    0  'Z','U','I','1'
    4  length, big endian
    6  reserved
    8  gzip data
"""
import gzip
import pathlib
import re
import sys

dist = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])
limit = (int(sys.argv[3]) if len(sys.argv) > 3 else 32768) - 8

html = (dist / "index.html").read_text()

def inline(pattern, fmt):
    global html
    def sub(m):
        name = m.group(1).split("/")[-1].split("?")[0]
        body = (dist / name).read_text()
        return fmt % body
    html = re.sub(pattern, sub, html)

# type="module" must survive the inlining. Vite's script is a module, and a
# module runs only once the page has been parsed; a plain <script> in <head>
# runs immediately, before <div id="root"> exists, and renders nothing. The
# first version of this dropped the attribute, and the page the box served was
# blank from the day it was built — it had only ever been checked through the
# dev server, which does not inline.
inline(r'<script[^>]*src="([^"]+)"[^>]*></script>', '<script type="module">%s</script>')
inline(r'<link[^>]*rel="stylesheet"[^>]*href="([^"]+)"[^>]*>', "<style>%s</style>")
html = re.sub(r'<link[^>]*rel="modulepreload"[^>]*>', "", html)

if '<script type="module">' not in html or re.search(r'<script>(?!\s*</script>)', html):
    sys.exit("pack: the app script is not a module — the page would render nothing")
if "src=" in html or 'rel="stylesheet"' in html:
    sys.exit("pack: something is still referenced externally:\n" + html[:400])

# mtime=0 so an unchanged UI produces an unchanged image and the diff of a
# firmware build stays meaningful.
blob = gzip.compress(html.encode(), compresslevel=9, mtime=0)
if len(blob) > limit:
    sys.exit(f"pack: {len(blob)} bytes will not fit below the high code ({limit})")

out.write_bytes(b"ZUI1" + len(blob).to_bytes(2, "big") + b"\0\0" + blob)
print(f"ui: {len(html)} bytes of HTML -> {len(blob)} gzipped "
      f"({100 - 100 * len(blob) // len(html)}% smaller), "
      f"{limit - len(blob)} bytes spare in the page")
