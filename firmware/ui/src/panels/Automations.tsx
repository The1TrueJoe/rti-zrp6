import { useEffect, useState } from 'preact/compat';
import { Grip, Plus, Radio, Trash, Zap } from '../icons';
import { useZrp6 } from '../App';
import { store, RULE_SLOTS } from '../mqtt';
import { PORTS, describe, fromText, plan, scriptsOf, toText, type Step, type StepKind, type Write } from '../script';
import { Card, Button, Field, Select, Empty, Modal } from '../ui';

const KIND_LABEL: Record<StepKind, string> = { ir: 'Send IR', irtx: 'IR port serial', relay: 'Relay', trigger: '12 V trigger', serial: 'Serial' };
const portLabel = (k: StepKind, n: number) =>
  k === 'ir' || k === 'irtx' ? (n === 7 ? 'HIGH OUT' : `Port ${n}`) : k === 'serial' ? `Port ${n}` : `${n}`;
const blank = (kind: StepKind = 'ir'): Step =>
  ({ kind, port: PORTS[kind][0], payload: kind === 'relay' || kind === 'trigger' ? 'ON' : '' });

/* ---- one step ------------------------------------------------------------ */

function StepRow({ s, i, onChange, onDelete, onDrag, onDrop }: {
  s: Step; i: number; onChange: (s: Step) => void; onDelete: () => void;
  onDrag: (i: number) => void; onDrop: (i: number) => void;
}) {
  return (
    <div draggable onDragStart={() => onDrag(i)}
         onDragOver={(e) => e.preventDefault()} onDrop={() => onDrop(i)}
         class="flex items-center gap-2 rounded-xl border border-edge bg-raised p-2">
      <span class="cursor-grab px-1 text-muted" title="Drag to reorder"><Grip size={15} /></span>
      <span class="w-5 text-center font-mono text-xs text-muted">{i + 1}</span>
      <Select value={s.kind} onChange={(e) => onChange(blank(e.currentTarget.value as StepKind))}>
        {(Object.keys(KIND_LABEL) as StepKind[]).map((k) => <option key={k} value={k}>{KIND_LABEL[k]}</option>)}
      </Select>
      <Select value={String(s.port)} onChange={(e) => onChange({ ...s, port: +e.currentTarget.value })}>
        {PORTS[s.kind].map((n) => <option key={n} value={n}>{portLabel(s.kind, n)}</option>)}
      </Select>
      {s.kind === 'relay' || s.kind === 'trigger'
        ? (
          <Select value={s.payload} onChange={(e) => onChange({ ...s, payload: e.currentTarget.value })}>
            <option value="ON">On</option>
            <option value="OFF">Off</option>
          </Select>
        )
        : (
          <Field value={s.payload}
                 placeholder={s.kind === 'ir' ? 'nec 0x04 0x08, or Pronto hex' : 'Text to send'}
                 onInput={(e) => onChange({ ...s, payload: e.currentTarget.value })} />
        )}
      <button onClick={onDelete} title="Remove step" class="px-1 text-muted hover:text-alarm"><Trash size={15} /></button>
    </div>
  );
}

/* ---- the editor ---------------------------------------------------------- */

function Editor({ code: initial, onClose }: { code: string | null; onClose: () => void }) {
  const s = useZrp6();
  const [opened] = useState(Date.now());
  const [code, setCode] = useState(initial ?? '');
  const [name, setName] = useState(initial ? s.names[initial] ?? '' : '');
  const existing = initial ? (scriptsOf(s.rules).get(initial) ?? []).map((x) => x.step) : [];
  const [steps, setSteps] = useState<Step[]>(existing.length ? existing : [blank()]);
  const [drag, setDrag] = useState<number | null>(null);
  const [err, setErr] = useState('');

  // Listening: the first press to arrive after the editor opened is the button.
  const listening = !initial && !code;
  useEffect(() => {
    if (!listening) return;
    const p = s.presses[0];
    if (p && p.at >= opened) {
      setCode(p.code);
      setName(s.names[p.code] ?? '');
    }
  }, [s.presses, listening]);

  const move = (to: number) => {
    if (drag === null || drag === to) return;
    const next = [...steps];
    const [it] = next.splice(drag, 1);
    next.splice(to, 0, it);
    setSteps(next);
    setDrag(null);
  };
  const ready = !!code.trim() && steps.length > 0
    && steps.every((x) => x.kind === 'relay' || x.kind === 'trigger' || x.payload.trim());

  const save = () => {
    const w = plan(s.rules, code.trim(), steps, RULE_SLOTS);
    if (!w) { setErr(`Not enough free slots — the box holds ${RULE_SLOTS} steps in total.`); return; }
    if (name.trim() !== (s.names[code.trim()] ?? '')) store.setName(code.trim(), name);
    store.write(w);
    onClose();
  };
  const remove = () => {
    const w = plan(s.rules, code, [], RULE_SLOTS);
    if (w) store.write(w);
    onClose();
  };

  return (
    <Modal title={initial ? 'Edit script' : 'New script'} onClose={onClose}>
      <div class="space-y-4">
        {listening
          ? (
            <div class="flex flex-col items-center gap-2 rounded-xl border border-dashed border-accent/50 bg-accent/5 px-4 py-6 text-center">
              <Radio size={22} class="animate-pulse text-accent" />
              <div class="text-sm font-medium">Press a button on the remote</div>
              <div class="flex w-full max-w-xs gap-2 pt-2">
                <Field value={code} placeholder="or type a code"
                       onInput={(e) => setCode(e.currentTarget.value)} />
              </div>
            </div>
          )
          : (
            <div class="flex gap-2">
              <Field value={name} placeholder="Button name" onInput={(e) => setName(e.currentTarget.value)} />
              <Field value={code} onInput={(e) => setCode(e.currentTarget.value)} disabled={!!initial} />
            </div>
          )}

        {!listening && (
          <>
            <div class="space-y-2">
              {steps.map((st, i) => (
                <StepRow key={i} s={st} i={i}
                         onChange={(n) => setSteps(steps.map((x, j) => (j === i ? n : x)))}
                         onDelete={() => setSteps(steps.filter((_, j) => j !== i))}
                         onDrag={setDrag} onDrop={move} />
              ))}
              <Button onClick={() => setSteps([...steps, blank()])}>
                <span class="flex items-center gap-1.5"><Plus size={14} /> Add step</span>
              </Button>
            </div>
            {err && <p class="text-sm text-alarm">{err}</p>}
            <div class="flex flex-wrap gap-2 border-t border-edge pt-4">
              <Button tone="accent" onClick={save} disabled={!ready}>Save to the box</Button>
              {initial && <Button onClick={() => store.press(initial)}>Test saved</Button>}
              {initial && <Button onClick={remove}>Delete script</Button>}
              <Button onClick={onClose}>Cancel</Button>
            </div>
          </>
        )}
      </div>
    </Modal>
  );
}

/* ---- text view ----------------------------------------------------------- */

function TextView({ onClose }: { onClose: () => void }) {
  const s = useZrp6();
  const [text, setText] = useState(() => toText(scriptsOf(s.rules), s.names));
  const [err, setErr] = useState('');

  const apply = () => {
    try {
      const parsed = fromText(text);
      // The text is the whole set: buttons missing from it lose their steps.
      let rules = { ...s.rules };
      const writes: Write[] = [];
      const codes = new Set(parsed.map((p) => p.code));
      const doPlan = (code: string, steps: Step[]) => {
        const w = plan(rules, code, steps, RULE_SLOTS);
        if (!w) throw new Error(`not enough free slots for ${code} — the box holds ${RULE_SLOTS} steps`);
        for (const x of w) {
          if (x.text) { const [c, t, ...p] = x.text.split('|'); rules[x.slot] = { slot: x.slot, code: c, topic: t, payload: p.join('|') }; }
          else delete rules[x.slot];
        }
        writes.push(...w);
      };
      for (const code of new Set(Object.values(s.rules).map((r) => r.code))) if (!codes.has(code)) doPlan(code, []);
      for (const p of parsed) doPlan(p.code, p.steps);
      parsed.forEach((p) => p.name && store.setName(p.code, p.name));
      store.write(writes);
      onClose();
    } catch (e) {
      setErr((e as Error).message);
    }
  };

  return (
    <Modal title="Scripts as text" onClose={onClose}>
      <textarea value={text} onInput={(e) => setText(e.currentTarget.value)} spellcheck={false}
                class="h-80 w-full rounded-lg border border-edge bg-raised p-3 font-mono text-xs outline-none focus:border-accent" />
      {err && <p class="mt-2 text-sm text-alarm">{err}</p>}
      <div class="mt-4 flex gap-2">
        <Button tone="accent" onClick={apply}>Apply to the box</Button>
        <Button onClick={onClose}>Cancel</Button>
      </div>
    </Modal>
  );
}

/* ---- the page ------------------------------------------------------------ */

export function AutomationsPanel() {
  const s = useZrp6();
  const scripts = scriptsOf(s.rules);
  const used = Object.keys(s.rules).length;
  const [editing, setEditing] = useState<string | null | undefined>(undefined);
  const [asText, setAsText] = useState(false);

  return (
    <>
      <Card title="Scripts" note={`${used} of ${RULE_SLOTS} steps`}>
        {(s.writing > 0 || s.writeError) && (
          <div class={`mb-4 rounded-xl border p-3 text-sm ${s.writeError ? 'border-alarm/40 text-alarm' : 'border-warm/30 text-warm'}`}>
            {s.writeError ?? `Saving to the box — ${s.writing} left`}
          </div>
        )}
        {scripts.size === 0
          ? <Empty>No scripts yet.</Empty>
          : (
            <ul class="divide-y divide-edge">
              {[...scripts].map(([code, steps]) => {
                const recent = s.fired && Date.now() - s.fired.at < 4000 && steps.some((x) => x.slot === s.fired!.slot);
                return (
                  <li key={code} class="flex items-start gap-3 py-3">
                    <Zap size={15} class={`mt-0.5 shrink-0 ${recent ? 'text-live' : 'text-accent'}`} />
                    <button class="min-w-0 flex-1 text-left" onClick={() => setEditing(code)}>
                      <div class="truncate text-sm font-medium">{s.names[code] ?? code}</div>
                      {steps.map(({ slot, step }, i) => (
                        <div key={slot} class="truncate font-mono text-xs text-muted">{i + 1}. {describe(step)}</div>
                      ))}
                    </button>
                    <Button onClick={() => store.press(code)}>Test</Button>
                  </li>
                );
              })}
            </ul>
          )}
        <div class="mt-4 flex gap-2">
          <Button tone="accent" onClick={() => setEditing(null)} disabled={used >= RULE_SLOTS || s.writing > 0}>
            <span class="flex items-center gap-1.5"><Plus size={15} /> New script</span>
          </Button>
          <Button onClick={() => setAsText(true)} disabled={s.writing > 0}>Edit as text</Button>
        </div>
      </Card>
      {editing !== undefined && <Editor code={editing} onClose={() => setEditing(undefined)} />}
      {asText && <TextView onClose={() => setAsText(false)} />}
    </>
  );
}
