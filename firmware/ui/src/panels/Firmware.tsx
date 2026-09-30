import { useEffect, useState } from 'preact/compat';
import { useZrp6 } from '../App';
import { store } from '../mqtt';
import { Card, Button, Row } from '../ui';

/** Where to POST: the box that serves this page, or the one the dev server
 *  points its broker at (VITE_MQTT_URL). /update takes firmware, /ui the page. */
function postUrl(path: string): string {
  const ws = import.meta.env.VITE_MQTT_URL as string | undefined;
  return ws ? ws.replace(/^ws/, 'http').replace(/\/?$/, path) : path;
}

type Kind = 'firmware' | 'page';

type Phase =
  | { k: 'idle' }
  | { k: 'bad'; msg: string }
  | { k: 'sending'; pct: number }
  | { k: 'writing' }
  | { k: 'done'; msg: string; from?: string }
  | { k: 'failed'; msg: string };

/** build/zrp6.bin (64 KB, "ZRP6") or build/zrp6-ui.bin ("ZUI2"). Checked
 *  here so a wrong file is refused in a second, not after a whole upload. */
async function check(f: File): Promise<Kind | string> {
  const magic = String.fromCharCode(...new Uint8Array(await f.slice(0, 4).arrayBuffer()));
  if (magic === 'ZRP6' && f.size === 65536) return 'firmware';
  if (magic === 'ZUI2' && f.size > 16 && f.size <= 32768) return 'page';
  return `${f.name} is neither build/zrp6.bin (firmware) nor build/zrp6-ui.bin (the web page).`;
}

export function FirmwarePanel() {
  const s = useZrp6();
  const fw = s.firmware;
  const [file, setFile] = useState<File | null>(null);
  const [kind, setKind] = useState<Kind>('firmware');
  const [ph, setPh] = useState<Phase>({ k: 'idle' });
  const noBoot = fw.state === 'no bootloader';

  // After a restart the page reconnects on its own; say so once it has.
  useEffect(() => {
    if (ph.k === 'done' && ph.from && s.connected && fw.version && fw.version !== ph.from)
      setPh({ k: 'done', msg: `Now running ${fw.version}.` });
  }, [s.connected, fw.version]);

  const pick = async (f: File | null) => {
    setFile(f);
    setPh({ k: 'idle' });
    if (f) {
      const k = await check(f);
      if (k === 'firmware' || k === 'page') setKind(k);
      else setPh({ k: 'bad', msg: k });
    }
  };

  /* A CRC mismatch means the box received different bytes than were sent —
   * on this board, the D4 bus fault. Nothing was installed, so it is simply
   * sent again, up to three times. */
  const send = (attempt = 1) => {
    if (!file) return;
    const from = fw.version;
    const x = new XMLHttpRequest();
    x.open('POST', postUrl(kind === 'page' ? '/ui' : '/update'));
    x.setRequestHeader('Authorization', store.auth());
    x.upload.onprogress = (e) => e.lengthComputable && setPh({ k: 'sending', pct: Math.round((100 * e.loaded) / e.total) });
    x.upload.onload = () => setPh({ k: 'writing' });
    x.onload = () => {
      const msg = x.responseText.trim();
      if (x.status === 422 && /CRC/.test(msg) && attempt < 3) send(attempt + 1);
      else if (x.status === 401)
        setPh({ k: 'failed', msg: 'The box did not accept your login for uploads. Nothing was installed. If the password was changed elsewhere, sign out and back in.' });
      else if (x.status !== 200) setPh({ k: 'failed', msg: msg || `HTTP ${x.status}` });
      else if (kind === 'page') setPh({ k: 'done', msg: 'Web page installed. Reload this page to use it.' });
      else if (/restarting/.test(msg))
        setPh({ k: 'done', from, msg: 'Installed. The box is restarting and installing it, which takes about 20 seconds. This page reconnects by itself.' });
      else setPh({ k: 'done', msg });
    };
    x.onerror = () => setPh({ k: 'failed', msg: 'The upload was cut off. Nothing was installed.' });
    setPh({ k: 'sending', pct: 0 });
    x.send(file);
  };

  const busy = ph.k === 'sending' || ph.k === 'writing';

  return (
    <>
      <Card title="Running">
        <div class="divide-y divide-edge">
          <Row k="Build" v={fw.version} />
          <Row k="State" v={fw.state} />
          <Row k="Previous image kept" v={fw.backup} />
          <Row k="Web page" v={fw.ui} />
        </div>
        <p class="mt-4 text-xs leading-relaxed text-muted">
          A new image is on trial until the first client connects to it. If none does, the box goes
          back to the previous image after its third restart.
        </p>
      </Card>

      <Card title="Update">
        {noBoot && (
          <p class="mb-4 rounded-xl border border-warm/30 p-3 text-sm text-warm">
            This box has no bootloader yet (v5 PLD). An upload is checked end to end but nothing is
            written.
          </p>
        )}
        <label class="block cursor-pointer rounded-xl border border-dashed border-edge px-4 py-6 text-center text-sm text-muted hover:border-accent">
          <input type="file" accept=".bin" class="hidden" disabled={busy}
                 onChange={(e) => pick(e.currentTarget.files?.[0] ?? null)} />
          {file ? <span class="font-mono text-ink">{file.name}</span> : 'Choose build/zrp6.bin (firmware) or build/zrp6-ui.bin (web page)'}
        </label>

        {ph.k === 'bad' && <p class="mt-3 text-sm text-alarm">{ph.msg}</p>}
        {ph.k === 'sending' && (
          <div class="mt-4 h-1.5 overflow-hidden rounded-full bg-raised">
            <div class="h-full bg-accent transition-all" style={{ width: `${ph.pct}%` }} />
          </div>
        )}
        {ph.k === 'writing' && <p class="mt-3 text-sm text-muted">Checking and writing it to Flash…</p>}
        {ph.k === 'done' && <p class="mt-3 text-sm text-live">{ph.msg}</p>}
        {ph.k === 'failed' && <p class="mt-3 text-sm text-alarm">{ph.msg}</p>}

        <div class="mt-4">
          <Button tone="accent" onClick={() => send()} disabled={!file || ph.k === 'bad' || busy}>
            {busy ? 'Uploading…' : 'Install'}
          </Button>
        </div>
      </Card>
    </>
  );
}
