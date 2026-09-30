import { useState } from 'preact/compat';
import { useZrp6 } from '../App';
import { store, BAUDS } from '../mqtt';
import { Card, Button, Field, Select } from '../ui';

/** RS-232 only. Ports 3 and 4 are RS-485 and are not wired into the bridge
 *  yet, so they are absent rather than shown broken. */
const PORTS = [1, 2];
const FORMATS = ['8N1', '8E1', '8O1', '7E1', '7O1', '8N2'];

/** Keyed on the box's serial/<n>/config, so it resets to what the box says. */
function Config({ n, cur }: { n: number; cur?: string }) {
  const [b, f] = (cur ?? '9600 8N1').split(' ');
  const [baud, setBaud] = useState(b);
  const [fmt, setFmt] = useState(f ?? '8N1');
  const want = `${baud} ${fmt}`;
  return (
    <div class="mb-3 flex gap-2">
      <Select value={baud} onChange={(e) => setBaud(e.currentTarget.value)}>
        {BAUDS.map((x) => <option key={x} value={x}>{x}</option>)}
      </Select>
      <Select value={fmt} onChange={(e) => setFmt(e.currentTarget.value)}>
        {FORMATS.map((x) => <option key={x}>{x}</option>)}
      </Select>
      <Button onClick={() => store.pub(`serial/${n}/config/set`, want)} disabled={want === cur}>Apply</Button>
    </div>
  );
}

export function SerialPanel() {
  const s = useZrp6();
  const [port, setPort] = useState(1);
  const [line, setLine] = useState('');

  const send = () => {
    if (!line) return;
    store.pub(`serial/${port}/tx`, line + '\r');
    setLine('');
  };

  return (
    <>
      <Card title="RS-232" note={s.serialCfg[port]}>
        <div class="mb-3 flex gap-2">
          {PORTS.map((n) => (
            <button key={n} onClick={() => setPort(n)}
              class={`rounded-lg px-3.5 py-2 text-sm transition ${
                port === n ? 'bg-accent/10 font-medium text-accent'
                           : 'border border-edge bg-raised text-muted hover:text-ink'}`}>
              Port {n}
            </button>
          ))}
        </div>
        <Config key={`${port} ${s.serialCfg[port]}`} n={port} cur={s.serialCfg[port]} />
        <div class="flex gap-2">
          <Field value={line} onChange={(e) => setLine(e.currentTarget.value)}
                 onKeyDown={(e: any) => e.key === 'Enter' && send()}
                 placeholder="Type a command and press Enter — CR is appended" />
          <Button tone="accent" onClick={send}>Send</Button>
        </div>
      </Card>

      <Card title={`Received on port ${port}`}>
        <pre class="max-h-80 overflow-auto whitespace-pre-wrap break-all rounded-xl border border-edge bg-raised p-3 font-mono text-xs leading-relaxed">
          {s.serial[port] || 'Nothing received yet.'}
        </pre>
      </Card>
    </>
  );
}
