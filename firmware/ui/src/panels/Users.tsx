import { useEffect, useState } from 'preact/compat';
import { useZrp6 } from '../App';
import { store } from '../mqtt';
import { Card, Button, Field, Empty } from '../ui';

/** Every user can do everything, including managing users. The box keeps
 *  only a salted hash of each password, so there is nothing to show but names. */
const NAME = /^[A-Za-z0-9._-]{1,15}$/;
const MAX = 8;

function said(v: string | null): [string, boolean] | null {
  if (!v) return null;
  const [what, who] = v.split(' ');
  return ({
    saving: ['Saving…', true],
    saved: [`Saved ${who}.`, true],
    deleted: [`Deleted ${who}.`, true],
    invalid: ['The box refused that name or password.', false],
    full: [`The box already holds ${MAX} users. Delete one first.`, false],
    last: ['That is the only user left, and the last user cannot be deleted.', false],
    busy: ['The box is busy saving something else. Try again in a moment.', false],
    error: ['The box could not write that to Flash. Try again.', false],
  } as Record<string, [string, boolean]>)[what] ?? [v, false];
}

export function UsersPanel() {
  const s = useZrp6();
  const [name, setName] = useState('');
  const [pass, setPass] = useState('');
  const users = s.users ?? [];
  const bytes = new TextEncoder().encode(pass).length;
  const err = name && !NAME.test(name) ? 'Names are 1-15 characters of A-Z a-z 0-9 . _ -'
    : bytes > 32 ? 'Passwords are at most 32 bytes.' : '';
  const note = said(s.userState);

  useEffect(() => { if (s.connected) store.listUsers(); }, [s.connected]);

  const save = () => { store.setUser(name, pass); setPass(''); };
  const drop = (n: string) =>
    confirm(`Delete user ${n}?${n === s.user ? ' You are signed in as this user: the next time this page reconnects it will ask you to sign in again.' : ''}`)
    && store.setUser(n, '');

  return (
    <>
      <Card title="Users" note={s.users ? `${users.length} of ${MAX}` : undefined}>
        {users.length === 0
          ? <Empty>{s.users ? 'No users listed.' : 'Asking the box…'}</Empty>
          : (
            <ul class="divide-y divide-edge">
              {users.map((n) => (
                <li key={n} class="flex items-center gap-3 py-2">
                  <span class="flex-1 font-mono text-sm">{n}</span>
                  {n === s.user && <span class="text-xs text-muted">you</span>}
                  <Button onClick={() => { setName(n); setPass(''); }}>Change password</Button>
                  <button onClick={() => drop(n)} class="text-xs text-muted hover:text-alarm">Delete</button>
                </li>
              ))}
            </ul>
          )}
        {note && <p class={`mt-3 text-sm ${note[1] ? 'text-muted' : 'text-alarm'}`}>{note[0]}</p>}
      </Card>

      <Card title="Add a user or change a password">
        <div class="flex gap-2">
          <Field value={name} placeholder="Name" autocomplete="off"
                 onInput={(e) => setName(e.currentTarget.value)} />
          <Field type="password" value={pass} placeholder="Password" autocomplete="new-password"
                 onInput={(e) => setPass(e.currentTarget.value)} />
          <Button tone="accent" onClick={save} disabled={!name || !pass || !!err}>Save</Button>
        </div>
        {err && <p class="mt-2 text-sm text-alarm">{err}</p>}
        <p class="mt-3 text-xs text-muted">
          An existing name gets the new password; a new name is added. Every user can do
          everything, including this. Nothing is encrypted on the network, so keep the box on a
          trusted one.
        </p>
      </Card>
    </>
  );
}
