import { useState } from 'preact/compat';
import { useZrp6 } from '../App';
import { store } from '../mqtt';
import { Card, Button, Field, Select, Row } from '../ui';

const quad = (v: string) =>
  /^\d{1,3}(\.\d{1,3}){3}$/.test(v) && v.split('.').every((x) => +x < 256);

export function NetworkPanel() {
  const s = useZrp6();
  const n = s.net;
  const [mode, setMode] = useState(n.mode === 'static' ? 'static' : 'dhcp');
  const [ip, setIp] = useState(n.ip ?? '');
  const [mask, setMask] = useState(n.netmask ?? '255.255.255.0');
  const [gw, setGw] = useState(n.gateway ?? '');
  const [done, setDone] = useState('');
  const bad = mode === 'static' && ![ip, mask, gw].every(quad);
  const host = `zrp6-${(n.mac ?? '').split(':').slice(-2).join('')}`;

  /* A wrong static address makes the box unreachable until it is fixed over
   * JTAG, so that one is asked twice. */
  const apply = () => {
    if (mode === 'dhcp') {
      if (!confirm('Switch to DHCP? The box saves this and restarts, and is off the network for about ten seconds.')) return;
      store.pub('net/set', 'dhcp');
      setDone(`Restarting. The router gives it an address; look for ${host} in its list.`);
    } else {
      if (!confirm(`Give the box the fixed address ${ip}, netmask ${mask}, gateway ${gw}? It saves this and restarts.`)) return;
      if (!confirm(`Are you sure? If ${ip} is wrong for this network, the box becomes unreachable and can only be recovered over JTAG.`)) return;
      store.pub('net/set', `static ${ip} ${mask} ${gw}`);
      setDone(`Restarting at http://${ip}/`);
    }
  };

  return (
    <>
      <Card title="Address" note={n.mac}>
        <div class="divide-y divide-edge">
          <Row k="Mode" v={n.mode} />
          <Row k="IP address" v={n.ip} />
          <Row k="Netmask" v={n.netmask} />
          <Row k="Gateway" v={n.gateway} />
        </div>
        {n.mode === 'fallback' && (
          <p class="mt-3 text-sm text-warm">
            No DHCP server answered, so the box is on its fallback address. It keeps asking once a minute.
          </p>
        )}
      </Card>

      <Card title="Change">
        <div class="mb-3 flex flex-wrap gap-2">
          <Select value={mode} onChange={(e) => setMode(e.currentTarget.value)}>
            <option value="dhcp">DHCP</option>
            <option value="static">Fixed address</option>
          </Select>
          {mode === 'static' && (
            <>
              <div class="w-36"><Field value={ip} placeholder="IP address" onInput={(e) => setIp(e.currentTarget.value)} /></div>
              <div class="w-36"><Field value={mask} placeholder="Netmask" onInput={(e) => setMask(e.currentTarget.value)} /></div>
              <div class="w-36"><Field value={gw} placeholder="Gateway" onInput={(e) => setGw(e.currentTarget.value)} /></div>
            </>
          )}
        </div>
        {bad && <p class="mb-3 text-sm text-alarm">Each address is four numbers 0-255, e.g. 10.0.0.77.</p>}
        {done && <p class="mb-3 text-sm text-live">{done}</p>}
        <Button tone="accent" onClick={apply} disabled={bad || !s.connected}>Save and restart</Button>
      </Card>

      <Card title="Restart">
        <p class="mb-3 text-sm text-muted">The box is off the network for about five seconds. This page reconnects by itself.</p>
        <Button onClick={() => confirm('Restart the ZRP-6 now?') && store.pub('system/restart', '1')}
                disabled={!s.connected}>Restart box</Button>
      </Card>
    </>
  );
}
