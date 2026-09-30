/* Scripts: what a button does, as an ordered list of steps.
 *
 * The box stores rules, not scripts — a rule is one button code and one
 * command, and when a button arrives the box runs every rule for it in slot
 * order. A script is simply that ordering made explicit: step n of a button's
 * script lives in the n-th lowest slot that holds a rule for that button.
 * Reordering steps therefore means rewriting slots, and this module works out
 * the smallest set of slot writes that turns one arrangement into another.
 *
 * Scripts also have a text form, so they can be written by hand or generated —
 * for instance from IR codes captured with a learner:
 *
 *     button 14.0.2.8.0.2.9  Power
 *       ir 1 nec 0x04 0x08
 *       relay 2 on
 *       serial 1 PWR ON\r
 *       irtx 3 POWER            <- text out of an IR port in serial mode (ir/3/tx)
 */
import type { Rule } from './mqtt';

export type StepKind = 'ir' | 'irtx' | 'relay' | 'trigger' | 'serial';
export type Step = { kind: StepKind; port: number; payload: string };

export const PORTS: Record<StepKind, number[]> = {
  ir: [1, 2, 3, 4, 5, 6, 7], irtx: [1, 2, 3, 4, 5, 6, 7], relay: [1, 2, 3], trigger: [1, 2, 3], serial: [1, 2],
};
const VERB: Record<StepKind, string> = { ir: 'send', irtx: 'tx', relay: 'set', trigger: 'set', serial: 'tx' };

/** irtx is an IR port's serial side: the same port, verb tx instead of send. */
export const stepTopic = (s: Step) =>
  `${s.kind === 'irtx' ? 'ir' : s.kind}/${s.port}/${VERB[s.kind]}`;

export function stepOf(r: Rule): Step | null {
  const [t, n, v] = r.topic.split('/');
  const k = t === 'ir' && v === 'tx' ? 'irtx' : t;
  if (!(k in PORTS)) return null;
  return { kind: k as StepKind, port: +n, payload: r.payload };
}

export function describe(s: Step): string {
  const p = s.payload.length > 32 ? `${s.payload.slice(0, 32)}…` : s.payload;
  if (s.kind === 'ir' || s.kind === 'irtx')
    return `IR ${s.port === 7 ? 'HIGH OUT' : s.port}${s.kind === 'irtx' ? ' serial' : ''} · ${p}`;
  if (s.kind === 'serial') return `Serial ${s.port} · ${p}`;
  return `${s.kind === 'relay' ? 'Relay' : 'Trigger'} ${s.port} ${p}`;
}

/** code -> its steps in run order, from the box's rule table. */
export function scriptsOf(rules: Record<number, Rule>): Map<string, { slot: number; step: Step }[]> {
  const out = new Map<string, { slot: number; step: Step }[]>();
  for (const r of Object.values(rules).sort((a, b) => a.slot - b.slot)) {
    const step = stepOf(r);
    if (!step) continue;
    if (!out.has(r.code)) out.set(r.code, []);
    out.get(r.code)!.push({ slot: r.slot, step });
  }
  return out;
}

export type Write = { slot: number; text: string };      // text '' deletes

/** The slot writes that make `code` run exactly `steps`, touching nothing that
 *  already matches. Returns null if there are not enough free slots. */
export function plan(rules: Record<number, Rule>, code: string, steps: Step[],
                     slots: number): Write[] | null {
  const mine = Object.values(rules).filter((r) => r.code === code)
    .map((r) => r.slot).sort((a, b) => a - b);
  const free = [];
  for (let n = 1; n <= slots; n++) if (!rules[n]) free.push(n);
  const use = [...mine];
  while (use.length < steps.length) {
    if (!free.length) return null;
    use.push(free.shift()!);
  }
  use.sort((a, b) => a - b);
  const writes: Write[] = [];
  use.forEach((slot, i) => {
    if (i >= steps.length) { writes.push({ slot, text: '' }); return; }
    const s = steps[i];
    const text = `${code}|${stepTopic(s)}|${s.payload}`;
    const cur = rules[slot];
    if (!cur || `${cur.code}|${cur.topic}|${cur.payload}` !== text) writes.push({ slot, text });
  });
  return writes;
}

/* ---- text form ---------------------------------------------------------- */

export function toText(scripts: Map<string, { step: Step }[]>, names: Record<string, string>): string {
  const lines: string[] = [];
  for (const [code, steps] of scripts) {
    lines.push(`button ${code}${names[code] ? `  ${names[code]}` : ''}`);
    for (const { step: s } of steps) {
      const v = s.kind === 'relay' || s.kind === 'trigger' ? s.payload.toLowerCase() : s.payload;
      lines.push(`  ${s.kind} ${s.port} ${v}`);
    }
    lines.push('');
  }
  return lines.join('\n');
}

export type Parsed = { code: string; name: string; steps: Step[] };

/** Throws with a line number on anything it does not understand — a script is
 *  going to be written into the box, so a silent guess is worse than an error. */
export function fromText(text: string): Parsed[] {
  const out: Parsed[] = [];
  let cur: Parsed | null = null;
  text.split('\n').forEach((raw, i) => {
    const line = raw.replace(/\s+$/, '');
    if (!line.trim() || line.trim().startsWith('#')) return;
    const at = `line ${i + 1}`;
    const m = /^button\s+(\S+)(?:\s+(.*))?$/.exec(line.trim());
    if (m && !/^\s/.test(raw)) {
      cur = { code: m[1], name: (m[2] ?? '').trim(), steps: [] };
      out.push(cur);
      return;
    }
    if (!cur) throw new Error(`${at}: a step before any "button" line`);
    const s = /^\s+(irtx|ir|relay|trigger|serial)\s+(\d)\s*(.*)$/.exec(raw);
    if (!s) throw new Error(`${at}: expected "ir|irtx|relay|trigger|serial <port> <value>"`);
    const kind = s[1] as StepKind, port = +s[2];
    if (!PORTS[kind].includes(port)) throw new Error(`${at}: ${kind} has no port ${port}`);
    let payload = s[3];
    if (kind === 'relay' || kind === 'trigger') {
      payload = payload.toUpperCase();
      if (payload !== 'ON' && payload !== 'OFF') throw new Error(`${at}: ${kind} takes on or off`);
    } else if (!payload) throw new Error(`${at}: ${kind} needs something to send`);
    cur.steps.push({ kind, port, payload });
  });
  return out;
}
