import { useState } from 'preact/compat';
import { useZrp6 } from '../App';
import { store } from '../mqtt';
import { Card, Field, Empty } from '../ui';

/** Every code seen or named, newest first. Codes are fixed per button, so a
 *  button only has to be pressed once to be known. */
export function knownButtons(s: ReturnType<typeof useZrp6>): string[] {
  const seen: string[] = [];
  for (const p of s.presses) if (!seen.includes(p.code)) seen.push(p.code);
  for (const c of Object.keys(s.names)) if (!seen.includes(c)) seen.push(c);
  return seen;
}

function NameField({ code, name }: { code: string; name: string }) {
  const [v, setV] = useState(name);
  const commit = () => v !== name && store.setName(code, v);
  return (
    <Field value={v} placeholder="Name this button"
           onInput={(e) => setV(e.currentTarget.value)}
           onBlur={commit}
           onKeyDown={(e: any) => e.key === 'Enter' && e.currentTarget.blur()} />
  );
}

export function RemotePanel() {
  const s = useZrp6();
  const buttons = knownButtons(s);
  const rulesFor = (code: string) => Object.values(s.rules).filter((r) => r.code === code).length;

  return (
    <>
      <Card title="Buttons" note={`${buttons.length} known`}>
        {buttons.length === 0
          ? <Empty>Press a button on the RTI remote.</Empty>
          : (
            <ul class="divide-y divide-edge">
              {buttons.map((code) => (
                <li key={code} class="flex items-center gap-3 py-2">
                  <div class="w-48 shrink-0"><NameField code={code} name={s.names[code] ?? ''} /></div>
                  <span class="flex-1 truncate font-mono text-xs text-muted">{code}</span>
                  <span class="text-xs text-muted">
                    {rulesFor(code) ? `${rulesFor(code)} rule${rulesFor(code) > 1 ? 's' : ''}` : ''}
                  </span>
                </li>
              ))}
            </ul>
          )}
      </Card>

      <Card title="Live">
        {s.presses.length === 0
          ? <Empty>Presses appear here as they arrive.</Empty>
          : (
            <ul class="divide-y divide-edge">
              {s.presses.map((p, i) => (
                <li key={i} class="flex items-center justify-between gap-3 py-2 text-sm">
                  <span class="font-medium">{s.names[p.code] ?? ''}</span>
                  <span class="flex-1 truncate font-mono text-xs text-muted">{p.code}</span>
                  <span class="text-xs text-muted">{new Date(p.at).toLocaleTimeString()}</span>
                </li>
              ))}
            </ul>
          )}
      </Card>
    </>
  );
}
