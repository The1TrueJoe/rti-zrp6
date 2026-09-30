import { useZrp6 } from '../App';
import { Card, Reading } from '../ui';
import { TOPIC_BASE } from '../mqtt';

export function OverviewPanel() {
  const s = useZrp6();
  const relays = Object.values(s.relay).filter(Boolean).length;
  const triggers = Object.values(s.trigger).filter(Boolean).length;
  const closed = Object.entries(s.sense).filter(([, v]) => !v).length;

  return (
    <>
      <Card title="Box" note={TOPIC_BASE}>
        <div class="grid gap-2 sm:grid-cols-2">
          <Reading label="Connection" value={s.connected ? 'Connected' : 'Not connected'} live={s.connected} />
          <Reading label="Controller" value={s.online ? 'Online' : 'Offline'} live={s.online} />
          <Reading label="Address" value={s.net.ip ?? '—'} hint={s.net.mode} live={!!s.net.ip && s.net.mode !== 'fallback'} />
          <Reading label="Relays closed" value={`${relays} of 3`} live={relays > 0} />
          <Reading label="Triggers on" value={`${triggers} of 3`} live={triggers > 0} />
          <Reading label="Contacts closed" value={`${closed} of 3`} live={closed > 0} />
          <Reading label="Rules" value={String(Object.keys(s.rules).length)}
                   live={Object.keys(s.rules).length > 0} />
        </div>
      </Card>

      <Card title="Recent remote presses">
        {s.presses.length === 0
          ? <p class="text-sm text-muted">Nothing received yet. Press a button on the RTI remote.</p>
          : (
            <ul class="divide-y divide-edge">
              {s.presses.slice(0, 8).map((p, i) => (
                <li key={i} class="flex items-center justify-between py-2 text-sm">
                  <span class="font-mono">{s.names[p.code] ?? p.code}</span>
                  <span class="text-xs text-muted">
                    {new Date(p.at).toLocaleTimeString()}
                  </span>
                </li>
              ))}
            </ul>
          )}
      </Card>
    </>
  );
}
