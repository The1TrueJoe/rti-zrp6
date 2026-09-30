import { useState } from 'preact/compat';
import { useZrp6 } from '../App';
import { store, BAUDS } from '../mqtt';
import { Card, Button, Field, Select, Empty } from '../ui';

/** Ports 1-6 are the numbered emitter jacks. Port 7 is HIGH OUT, the
 *  high-current output — it is inverted in hardware and has no panel LED,
 *  which is why it is labelled rather than just numbered. */
const PORTS = [1, 2, 3, 4, 5, 6, 7];
const LABEL = (n: number) => (n === 7 ? '7 — HIGH OUT' : String(n));

type Saved = { name: string; port: number; code: string };
const KEY = 'zrp6.ir.library';
const load = (): Saved[] => { try { return JSON.parse(localStorage.getItem(KEY) ?? '[]'); } catch { return []; } };

/** One port's mode: IR, or transmit-only serial at a baud and polarity. The
 *  box answers ir/<n>/mode, and the row is keyed on that, so it resets to
 *  whatever the box says it is now. */
function PortMode({ n, cur }: { n: number; cur?: string }) {
  const [m, b, pol] = (cur ?? 'ir').split(' ');
  const [mode, setMode] = useState(m);
  const [baud, setBaud] = useState(b ?? '9600');
  const [p, setP] = useState(pol ?? 'rs232');
  const want = mode === 'ir' ? 'ir' : `serial ${baud} ${p}`;
  return (
    <li class="flex flex-wrap items-center gap-2 py-2">
      <span class="w-32 text-sm">Port {LABEL(n)}</span>
      <Select value={mode} onChange={(e) => setMode(e.currentTarget.value)}>
        <option value="ir">IR</option>
        <option value="serial">Serial out</option>
      </Select>
      {mode === 'serial' && (
        <>
          <Select value={baud} onChange={(e) => setBaud(e.currentTarget.value)}>
            {BAUDS.map((x) => <option key={x} value={x}>{x}</option>)}
          </Select>
          <Select value={p} onChange={(e) => setP(e.currentTarget.value)}>
            <option value="rs232">RS-232 (idle low)</option>
            <option value="ttl">TTL (idle high)</option>
          </Select>
        </>
      )}
      <span class="flex-1" />
      <Button onClick={() => store.pub(`ir/${n}/mode/set`, want)} disabled={want === cur}>Apply</Button>
    </li>
  );
}

export function IrPanel() {
  const s = useZrp6();
  const [port, setPort] = useState(6);
  const [code, setCode] = useState('nec 0x04 0x08');
  const [name, setName] = useState('');
  const [lib, setLib] = useState<Saved[]>(load);

  /* A port in serial mode ignores ir/<n>/send and takes text on ir/<n>/tx
   * instead — with a CR appended, as the Serial panel does. */
  const serial = (p: number) => s.irMode[p]?.startsWith('serial');
  const send = (p: number, c: string) =>
    serial(p) ? store.pub(`ir/${p}/tx`, c + '\r') : store.pub(`ir/${p}/send`, c);
  const save = () => {
    if (!name.trim()) return;
    const next = [...lib.filter((x) => x.name !== name.trim()),
                  { name: name.trim(), port, code }];
    setLib(next); localStorage.setItem(KEY, JSON.stringify(next)); setName('');
  };
  const drop = (n: string) => {
    const next = lib.filter((x) => x.name !== n);
    setLib(next); localStorage.setItem(KEY, JSON.stringify(next));
  };

  return (
    <>
      <Card title="Send a code" note={serial(port) ? s.irMode[port] : '38 kHz'}>
        <div class="mb-3 flex gap-2">
          <Select value={port} onChange={(e) => setPort(+e.currentTarget.value)}>
            {PORTS.map((n) => <option key={n} value={n}>Port {LABEL(n)}{serial(n) ? ' (serial)' : ''}</option>)}
          </Select>
          <Field value={code} onChange={(e) => setCode(e.currentTarget.value)}
                 placeholder={serial(port) ? 'Text to send — CR is appended' : 'nec 0x04 0x08   —or—   0000 0068 0000 0015 ...'} />
          <Button tone="accent" onClick={() => send(port, code)}>Send</Button>
        </div>
        <p class="text-xs text-muted">
          Two forms are accepted: <code class="font-mono">nec &lt;addr&gt; &lt;cmd&gt; [repeats]</code>,
          or Pronto hex pasted straight from a remote database — Pronto already
          expresses timings in carrier periods, so it goes out unmodified.
        </p>
      </Card>

      <Card title="Port modes">
        <ul class="divide-y divide-edge">
          {PORTS.map((n) => <PortMode key={`${n} ${s.irMode[n]}`} n={n} cur={s.irMode[n]} />)}
        </ul>
        <p class="mt-3 text-xs text-muted">
          Serial out turns a port into a transmit-only 8N1 serial line for devices with a 3.5 mm
          serial jack. 57600 and up are generated in software and are the least tolerant. The box
          remembers the mode.
        </p>
      </Card>

      <Card title="Library">
        <div class="mb-3 flex gap-2">
          <Field value={name} onChange={(e) => setName(e.currentTarget.value)}
                 placeholder="Name this code, e.g. TV power" />
          <Button onClick={save} disabled={!name.trim()}>Save</Button>
        </div>
        {lib.length === 0
          ? <Empty>No saved codes. Send one above, give it a name, and save it.</Empty>
          : (
            <ul class="divide-y divide-edge">
              {lib.map((x) => (
                <li key={x.name} class="flex items-center gap-3 py-2">
                  <span class="flex-1 text-sm">{x.name}</span>
                  <span class="font-mono text-xs text-muted">port {LABEL(x.port)}</span>
                  <Button onClick={() => send(x.port, x.code)}>Send</Button>
                  <button onClick={() => drop(x.name)}
                          class="text-xs text-muted hover:text-alarm">Remove</button>
                </li>
              ))}
            </ul>
          )}
      </Card>
    </>
  );
}
