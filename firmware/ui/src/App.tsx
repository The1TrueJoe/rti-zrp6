import { useEffect, useState, useSyncExternalStore } from 'preact/compat';
import { Alert, Cpu, Globe, Radio, Terminal, Toggle, Upload, User, Zap } from './icons';
import { store, type State } from './mqtt';
import { Button, Field, Modal } from './ui';
import { OverviewPanel } from './panels/Overview';
import { ControlPanel } from './panels/Control';
import { IrPanel } from './panels/Ir';
import { SerialPanel } from './panels/Serial';
import { RemotePanel } from './panels/Remote';
import { AutomationsPanel } from './panels/Automations';
import { FirmwarePanel } from './panels/Firmware';
import { NetworkPanel } from './panels/Network';
import { UsersPanel } from './panels/Users';

export function useZrp6(): State {
  return useSyncExternalStore((cb) => store.watch(cb), () => store.state);
}

const DESTS = [
  { id: 'overview', label: 'Overview', icon: Cpu, render: () => <OverviewPanel /> },
  { id: 'control', label: 'Control', icon: Toggle, render: () => <ControlPanel /> },
  { id: 'ir', label: 'Infrared', icon: Radio, render: () => <IrPanel /> },
  { id: 'serial', label: 'Serial', icon: Terminal, render: () => <SerialPanel /> },
  { id: 'remote', label: 'Remote', icon: Radio, render: () => <RemotePanel /> },
  { id: 'automations', label: 'Automations', icon: Zap, render: () => <AutomationsPanel /> },
  { id: 'network', label: 'Network', icon: Globe, render: () => <NetworkPanel /> },
  { id: 'users', label: 'Users', icon: User, render: () => <UsersPanel /> },
  { id: 'firmware', label: 'Firmware', icon: Upload, render: () => <FirmwarePanel /> },
];

export default function App() {
  const s = useZrp6();
  const [at, setAt] = useState('overview');

  useEffect(() => {
    // The box serves this page and its MQTT WebSocket on the same port, so the
    // page's own origin is the broker. VITE_MQTT_URL points a dev server
    // (npm run dev) at a real box instead, e.g. ws://10.0.0.77/.
    const url = (import.meta.env.VITE_MQTT_URL as string) ||
      `ws://${location.host}/`;
    store.connect(url);
  }, []);

  if (!s.authed) return <Login s={s} />;
  const here = DESTS.find((d) => d.id === at) ?? DESTS[0];

  return (
    <div class="flex h-full">
      <nav class="flex w-52 shrink-0 flex-col border-r border-edge bg-panel">
        <div class="px-5 py-5">
          <div class="text-[15px] font-semibold tracking-tight">ZRP-6</div>
          <Link connected={s.connected} online={s.online} />
        </div>
        <div class="flex flex-col gap-0.5 px-2">
          {DESTS.map((d) => (
            <button
              key={d.id}
              onClick={() => setAt(d.id)}
              class={`flex items-center gap-2.5 rounded-lg px-3 py-2 text-left text-sm transition
                ${at === d.id
                  ? 'bg-accent/10 font-medium text-accent'
                  : 'text-muted hover:bg-raised hover:text-ink'}`}
            >
              <d.icon size={16} />
              {d.label}
            </button>
          ))}
        </div>
        <div class="mt-auto truncate px-5 py-4 text-xs text-muted">
          {s.user} · <button onClick={() => store.logout()} class="hover:text-ink">Sign out</button>
        </div>
      </nav>

      <main class="min-w-0 flex-1 overflow-y-auto">
        <div class="mx-auto max-w-4xl px-8 py-8">
          <h1 class="mb-6 text-xl font-semibold tracking-tight">{here.label}</h1>
          {s.error && (
            <div class="mb-5 flex items-start gap-2 rounded-xl border border-alarm/30 bg-alarm/5 p-3.5 text-sm text-alarm">
              <Alert size={16} class="mt-0.5 shrink-0" />
              <span class="font-mono text-xs">{s.error}</span>
            </div>
          )}
          {here.render()}
        </div>
      </main>
      <LinkModal link={s.link} />
    </div>
  );
}

/** Shown before the first successful login, and again whenever the box
 *  refuses one. A refusal is not retried: the same password would fail the
 *  same way, and each failure locks every login out for 2 s. */
function Login({ s }: { s: State }) {
  const [u, setU] = useState(s.user ?? '');
  const [p, setP] = useState('');
  const [keep, setKeep] = useState(false);
  const msg = s.link === 'denied' ? 'Wrong username or password.'
    : !s.user ? '' : s.link === 'failing' ? "Can't reach the ZRP-6. Still trying…" : 'Signing in…';

  return (
    <div class="flex h-full items-center justify-center p-4">
      <form class="w-full max-w-xs space-y-3 rounded-2xl border border-edge bg-panel p-5"
            onSubmit={(e) => { e.preventDefault(); u && p && store.login(u, p, keep); }}>
        <div class="text-[15px] font-semibold tracking-tight">ZRP-6</div>
        <Field value={u} placeholder="Username" autocomplete="username"
               onInput={(e) => setU(e.currentTarget.value)} />
        <Field type="password" value={p} placeholder="Password" autocomplete="current-password"
               onInput={(e) => setP(e.currentTarget.value)} />
        <label class="flex items-center gap-2 text-sm text-muted">
          <input type="checkbox" checked={keep} onChange={(e) => setKeep(e.currentTarget.checked)} />
          Remember me
        </label>
        {msg && <p class={`text-sm ${s.link === 'denied' ? 'text-alarm' : 'text-muted'}`}>{msg}</p>}
        <Button tone="accent" disabled={!u || !p}>Sign in</Button>
      </form>
    </div>
  );
}

/** The box has four network sockets in total — a hardware limit of its
 *  network chip — shared by every browser tab and MQTT client. When they run
 *  out, say so rather than leaving a red dot to be puzzled over. */
function LinkModal({ link }: { link: State['link'] }) {
  const [hidden, setHidden] = useState<string | null>(null);
  useEffect(() => { if (link === 'up') setHidden(null); }, [link]);
  if ((link !== 'bumped' && link !== 'failing') || hidden === link) return null;
  const close = () => setHidden(link);

  return link === 'bumped'
    ? (
      <Modal title="Connection handed over" onClose={close}>
        <p class="text-sm text-muted">
          The ZRP-6 has four network connections. MQTT clients such as a controller always
          get one, and the box keeps one free for new MQTT clients and one for loading this
          page. When it runs short, the oldest browser tab gives way — this one. Reconnecting
          will take a connection back from another tab if one is needed.
        </p>
        <div class="mt-5 flex gap-2">
          <Button tone="accent" onClick={() => store.reconnect()}>Reconnect here</Button>
          <Button onClick={close}>Not now</Button>
        </div>
      </Modal>
    )
    : (
      <Modal title="Can't reach the ZRP-6" onClose={close}>
        <p class="text-sm text-muted">
          This page keeps trying every two seconds. Usually one of these is the cause:
        </p>
        <ul class="mt-3 list-disc space-y-1.5 pl-5 text-sm text-muted">
          <li>The box is restarting. That takes about ten seconds.</li>
          <li>
            All four of its connections are in use, by other ZRP-6 tabs or MQTT clients such
            as a controller. Close a tab you don't need.
          </li>
          <li>The box is unplugged or off the network.</li>
        </ul>
        <div class="mt-5 flex gap-2">
          <Button onClick={close}>Dismiss</Button>
        </div>
      </Modal>
    );
}

/** Two separate facts, and conflating them hides the interesting failure:
 *  the browser can be attached to the broker while the box itself is gone. */
function Link({ connected, online }: { connected: boolean; online: boolean }) {
  const [text, tone] = !connected
    ? ['Not connected', 'bg-alarm']
    : online
      ? ['Online', 'bg-live']
      : ['Box offline', 'bg-warm'];
  return (
    <div class="mt-1.5 flex items-center gap-1.5 text-xs text-muted">
      <span class={`h-1.5 w-1.5 rounded-full ${tone}`} />
      {text}
    </div>
  );
}
