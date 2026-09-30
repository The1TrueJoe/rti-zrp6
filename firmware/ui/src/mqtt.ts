import { type Link, MqttWs } from './mqttws';
import type { Write } from './script';

/** A rule exactly as the box stores it: a remote button's code and one MQTT
 *  command the box itself accepts. Several rules may share a code; the box
 *  runs them in slot order. Nothing here runs rules — the box does, with or
 *  without this page open. */
export type Rule = { slot: number; code: string; topic: string; payload: string };

export const RULE_SLOTS = 14;
/** What an IR port in serial mode can do; the RS-232 ports take the same. */
export const BAUDS = [1200, 2400, 4800, 9600, 14400, 19200, 38400, 57600, 115200];

export type State = {
  /** who is signed in (or trying to); null when signed out */
  user: string | null;
  /** the box has accepted this login at least once */
  authed: boolean;
  connected: boolean;
  link: Link;
  online: boolean;
  relay: Record<number, boolean>;
  trigger: Record<number, boolean>;
  sense: Record<number, boolean>;
  serial: Record<number, string>;
  /** slot -> rule, as last listed by the box */
  rules: Record<number, Rule>;
  /** the box's answer to the last save or delete per slot */
  ruleState: Record<number, string>;
  /** remote presses, newest first */
  presses: { at: number; code: string }[];
  /** button nicknames — kept in this browser; the box has no room for them */
  names: Record<string, string>;
  fired: { at: number; slot: number } | null;
  stackFree: number | null;
  /** firmware/version, firmware/state, firmware/backup — docs/UPDATING.md */
  firmware: { version?: string; state?: string; backup?: string; ui?: string };
  /** net/mode, ip, netmask, gateway, mac */
  net: Record<string, string>;
  /** ir/<n>/mode and serial/<n>/config */
  irMode: Record<number, string>;
  serialCfg: Record<number, string>;
  /** users/list, and the box's answer to the last users/<name>/set */
  users: string[] | null;
  userState: string | null;
  /** slot writes still to be confirmed by the box, and what went wrong if one failed */
  writing: number;
  writeError: string | null;
  error: string | null;
};

const BASE = (import.meta.env.VITE_ZRP6_TOPIC as string) || 'zrp6';
const NAMES_KEY = 'zrp6.buttonNames';
const LOGIN_KEY = 'zrp6.login';

type Creds = { u: string; p: string; keep: boolean };

/** "Remember me" keeps the login in localStorage; otherwise it lives in
 *  sessionStorage and goes when the tab does. */
function stores(): Storage[] {
  try { return [localStorage, sessionStorage]; } catch { return []; }
}
function loadLogin(): Creds | null {
  for (const [i, st] of stores().entries()) {
    try { const v = JSON.parse(st.getItem(LOGIN_KEY) ?? 'null'); if (v) return { ...v, keep: !i }; } catch { /* */ }
  }
  return null;
}
function saveLogin(c: Creds | null) {
  stores().forEach((st, i) => {
    try {
      if (c && c.keep === !i) st.setItem(LOGIN_KEY, JSON.stringify({ u: c.u, p: c.p }));
      else st.removeItem(LOGIN_KEY);
    } catch { /* private window */ }
  });
}

function loadNames(): Record<string, string> {
  try { return JSON.parse(localStorage.getItem(NAMES_KEY) || '{}'); } catch { return {}; }
}

/** "code|topic|payload" — the payload may itself contain '|'. */
export function parseRule(slot: number, text: string): Rule | null {
  const a = text.indexOf('|');
  const b = a < 0 ? -1 : text.indexOf('|', a + 1);
  if (b < 0) return null;
  return { slot, code: text.slice(0, a), topic: text.slice(a + 1, b), payload: text.slice(b + 1) };
}

/** The broker is the source of truth and the page only renders it, so state
 *  lives here and components subscribe via useSyncExternalStore. */
class Store {
  private creds = loadLogin();
  private url = '';
  /** a password change in flight, so our own can be updated once saved */
  private pw: { n: string; p: string } | null = null;
  state: State = {
    user: this.creds?.u ?? null, authed: false,
    connected: false, link: 'connecting', online: false,
    relay: {}, trigger: {}, sense: {}, serial: {},
    rules: {}, ruleState: {}, presses: [], names: loadNames(),
    fired: null, stackFree: null, firmware: {}, net: {}, irMode: {}, serialCfg: {},
    users: null, userState: null, writing: 0, writeError: null, error: null,
  };
  /** The box saves one rule at a time and each save rewrites a Flash sector,
   *  so writes go out strictly one after another: the next is sent only when
   *  the box has answered the last. */
  private queue: Write[] = [];
  private subs = new Set<() => void>();
  private client: MqttWs | null = null;

  watch(cb: () => void) {
    this.subs.add(cb);
    return () => this.subs.delete(cb);
  }
  private emit() {
    this.state = { ...this.state };
    this.subs.forEach((f) => f());
  }

  connect(url: string) {
    if (this.url) return;
    this.url = url;
    if (this.creds) this.open();
  }

  private open() {
    this.client?.close();
    const c = new MqttWs(
      this.url, this.creds!.u, this.creds!.p,
      (topic, payload) => this.apply(topic, payload),
      (link) => {
        const s = this.state, up = link === 'up';
        s.connected = up;
        s.link = link;
        if (up) {
          s.authed = true;
          c.subscribe(`${BASE}/#`);
          this.listRules();
        } else if (link === 'denied') {
          s.authed = false;
          saveLogin(null);
        }
        this.emit();
      },
    );
    this.client = c;
    c.connect();
  }

  reconnect() { this.client?.reconnect(); }

  login(u: string, p: string, keep: boolean) {
    this.creds = { u, p, keep };
    saveLogin(this.creds);
    Object.assign(this.state, { user: u, authed: false, link: 'connecting' });
    this.open();
    this.emit();
  }
  logout() {
    this.client?.close();
    this.client = null;
    this.creds = null;
    saveLogin(null);
    Object.assign(this.state, { user: null, authed: false, connected: false, link: 'connecting', users: null, userState: null });
    this.emit();
  }
  /** HTTP Basic, for POST /update and /ui. btoa wants Latin-1, so the UTF-8
   *  bytes go through as char codes. */
  auth(): string {
    const c = this.creds;
    return 'Basic ' + btoa(String.fromCharCode(...new TextEncoder().encode(`${c?.u}:${c?.p}`)));
  }

  private apply(topic: string, v: string) {
    const p = topic.startsWith(`${BASE}/`) ? topic.slice(BASE.length + 1).split('/') : [];
    const s = this.state;
    if (p[0] === 'status') s.online = v === 'online';
    else if (p[0] === 'relay' && p[2] === 'state') s.relay[+p[1]] = v === 'ON';
    else if (p[0] === 'trigger' && p[2] === 'state') s.trigger[+p[1]] = v === 'ON';
    else if (p[0] === 'sense' && p[2] === 'state') s.sense[+p[1]] = v === 'OPEN';
    else if (p[0] === 'serial' && p[2] === 'rx') {
      const n = +p[1];
      s.serial[n] = ((s.serial[n] ?? '') + v).slice(-4000);
    } else if (p[0] === 'remote' && p[1] === 'code') {
      s.presses = [{ at: Date.now(), code: v }, ...s.presses].slice(0, 40);
    } else if (p[0] === 'rules' && p.length === 2 && /^\d+$/.test(p[1])) {
      const r = parseRule(+p[1], v);
      if (r) s.rules = { ...s.rules, [r.slot]: r };
    } else if (p[0] === 'rules' && p[2] === 'state') {
      const slot = +p[1];
      s.ruleState = { ...s.ruleState, [slot]: v };
      if (this.queue.length && this.queue[0].slot === slot) {
        this.queue.shift();
        if (v === 'saved' || v === 'deleted') this.next();
        else { s.writeError = `slot ${slot}: the box answered "${v}"`; this.queue = []; s.writing = 0; this.listRules(); }
      } else if (v === 'saved' || v === 'deleted') this.listRules();
    } else if (p[0] === 'rules' && p[1] === 'fired') {
      s.fired = { at: Date.now(), slot: +v };
    } else if (p[0] === 'debug' && p[1] === 'stackfree') {
      s.stackFree = +v;
    } else if (p[0] === 'firmware' && p.length === 2) {
      s.firmware = { ...s.firmware, [p[1]]: v };
    } else if (p[0] === 'net' && p.length === 2 && p[1] !== 'set') {
      s.net = { ...s.net, [p[1]]: v };
    } else if (p[0] === 'ir' && p[2] === 'mode' && p.length === 3) {
      s.irMode = { ...s.irMode, [+p[1]]: v };
    } else if (p[0] === 'serial' && p[2] === 'config' && p.length === 3) {
      s.serialCfg = { ...s.serialCfg, [+p[1]]: v };
    } else if (p[0] === 'users' && p[1] === 'list') {
      s.users = v ? v.split(',') : [];
    } else if (p[0] === 'users' && p[1] === 'state') {
      s.userState = v;
      const [what, who] = v.split(' ');
      const c = this.creds, w = this.pw;
      if (what === 'saved' && c && w && w.n === who && c.u === who) { c.p = w.p; saveLogin(c); }
      if (who) { this.pw = null; this.listUsers(); }
    } else return;
    this.emit();
  }

  pub(topic: string, payload: string) {
    this.client?.publish(`${BASE}/${topic}`, payload);
  }

  listUsers() { this.pub('users/get', ''); }
  /** Adds the user or changes its password; an empty password deletes it. */
  setUser(n: string, pass: string) {
    this.pw = { n, p: pass };
    this.state.userState = 'saving';
    this.pub(`users/${n}/set`, pass);
    this.emit();
  }

  listRules() {
    this.state.rules = {};
    this.pub('rules/get', '');
    this.emit();
  }
  saveRule(slot: number, code: string, topic: string, payload: string) {
    this.state.ruleState = { ...this.state.ruleState, [slot]: 'saving' };
    this.pub(`rules/${slot}/set`, `${code}|${topic}|${payload}`);
    this.emit();
  }
  deleteRule(slot: number) {
    this.state.ruleState = { ...this.state.ruleState, [slot]: 'deleting' };
    this.pub(`rules/${slot}/set`, '');
    this.emit();
  }
  /** Publishing a code is a press as far as the box is concerned: its rules run. */
  press(code: string) {
    this.pub('remote/code', code);
  }
  /** Send a planned set of slot writes, one at a time. */
  write(writes: Write[]) {
    if (!writes.length) return;
    this.queue.push(...writes);
    this.state.writeError = null;
    if (this.queue.length === writes.length) this.next();
    else { this.state.writing = this.queue.length; this.emit(); }
  }
  private next() {
    this.state.writing = this.queue.length;
    const w = this.queue[0];
    if (!w) { this.listRules(); return; }
    this.state.ruleState = { ...this.state.ruleState, [w.slot]: w.text ? 'saving' : 'deleting' };
    this.pub(`rules/${w.slot}/set`, w.text);
    this.emit();
  }
  freeSlot(): number | null {
    for (let n = 1; n <= RULE_SLOTS; n++) if (!this.state.rules[n]) return n;
    return null;
  }
  setName(code: string, name: string) {
    const names = { ...this.state.names };
    if (name.trim()) names[code] = name.trim(); else delete names[code];
    this.state.names = names;
    try { localStorage.setItem(NAMES_KEY, JSON.stringify(names)); } catch { /* private window */ }
    this.emit();
  }
}

export const store = new Store();
export const TOPIC_BASE = BASE;
