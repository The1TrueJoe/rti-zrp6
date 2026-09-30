import { useZrp6 } from '../App';
import { store } from '../mqtt';
import { Card, Toggle, Reading } from '../ui';

const N = [1, 2, 3];

export function ControlPanel() {
  const s = useZrp6();
  return (
    <>
      <Card title="Relays" note="Port C bits 2-4, active high">
        <div class="grid gap-2">
          {N.map((n) => (
            <Toggle key={n} label={`Relay ${n}`} hint={`relay/${n}`}
              on={!!s.relay[n]}
              onChange={(v) => store.pub(`relay/${n}/set`, v ? 'ON' : 'OFF')} />
          ))}
        </div>
      </Card>

      <Card title="12 V triggers" note="P1.5-P1.7, active low">
        <div class="grid gap-2">
          {N.map((n) => (
            <Toggle key={n} label={`Trigger ${n}`} hint={`trigger/${n}`}
              on={!!s.trigger[n]}
              onChange={(v) => store.pub(`trigger/${n}/set`, v ? 'ON' : 'OFF')} />
          ))}
        </div>
      </Card>

      <Card title="Contact inputs" note="P3.0, Port C.7, P1.4">
        <div class="grid gap-2">
          {N.map((n) => (
            <Reading key={n} label={`Sense ${n}`} hint={`sense/${n}`}
              value={s.sense[n] === undefined ? '—' : s.sense[n] ? 'Open' : 'Closed'}
              live={s.sense[n] === false} />
          ))}
        </div>
      </Card>
    </>
  );
}
