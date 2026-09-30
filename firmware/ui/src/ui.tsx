import type { ComponentChildren } from 'preact';

export function Card({ title, note, children }:
  { title: string; note?: string; children: ComponentChildren }) {
  return (
    <section class="mb-5 rounded-2xl border border-edge bg-panel p-5">
      <div class="mb-4 flex items-baseline justify-between gap-4">
        <h2 class="text-sm font-semibold tracking-tight">{title}</h2>
        {note && <span class="font-mono text-xs text-muted">{note}</span>}
      </div>
      {children}
    </section>
  );
}

export function Toggle({ label, on, onChange, hint }:
  { label: string; on: boolean; onChange: (v: boolean) => void; hint?: string }) {
  return (
    <button
      onClick={() => onChange(!on)}
      class="flex w-full items-center justify-between rounded-xl border border-edge bg-raised px-4 py-3 text-left transition hover:border-accent/40"
    >
      <span>
        <span class="block text-sm">{label}</span>
        {hint && <span class="block font-mono text-xs text-muted">{hint}</span>}
      </span>
      <span class={`relative h-6 w-11 rounded-full transition ${on ? 'bg-live' : 'bg-edge'}`}>
        <span class={`absolute top-0.5 h-5 w-5 rounded-full bg-panel shadow transition-all ${on ? 'left-[22px]' : 'left-0.5'}`} />
      </span>
    </button>
  );
}

/** A reading, not a control. Deliberately not a Toggle: a contact input that
 *  looks like a switch invites someone to try to click it. */
export function Reading({ label, value, live, hint }:
  { label: string; value: string; live?: boolean; hint?: string }) {
  return (
    <div class="flex items-center justify-between rounded-xl border border-edge bg-raised px-4 py-3">
      <span>
        <span class="block text-sm">{label}</span>
        {hint && <span class="block font-mono text-xs text-muted">{hint}</span>}
      </span>
      <span class="flex items-center gap-2 text-sm">
        <span class={`h-1.5 w-1.5 rounded-full ${live ? 'bg-live' : 'bg-edge'}`} />
        <span class={live ? 'text-ink' : 'text-muted'}>{value}</span>
      </span>
    </div>
  );
}

export function Button({ children, onClick, tone = 'normal', disabled }:
  { children: ComponentChildren; onClick?: () => void; tone?: 'normal' | 'accent'; disabled?: boolean }) {
  const base = 'rounded-lg px-3.5 py-2 text-sm font-medium transition disabled:opacity-40';
  return (
    <button disabled={disabled} onClick={onClick}
      class={tone === 'accent'
        ? `${base} bg-accent text-white hover:opacity-90`
        : `${base} border border-edge bg-raised hover:border-accent/40`}>
      {children}
    </button>
  );
}

/** A label and a value, for the read-only facts in a Card. */
export function Row({ k, v }: { k: string; v?: string }) {
  return (
    <div class="flex justify-between gap-4 py-2 text-sm">
      <span class="text-muted">{k}</span>
      <span class="font-mono text-xs">{v ?? '—'}</span>
    </div>
  );
}

export function Field(props: preact.JSX.IntrinsicElements['input']) {
  return (
    <input {...props}
      class={`w-full rounded-lg border border-edge bg-raised px-3 py-2 font-mono text-sm outline-none placeholder:text-muted focus:border-accent ${props.className ?? ''}`} />
  );
}

export function Select(props: preact.JSX.IntrinsicElements['select']) {
  return (
    <select {...props}
      class={`rounded-lg border border-edge bg-raised px-3 py-2 text-sm outline-none focus:border-accent ${props.className ?? ''}`} />
  );
}

export function Empty({ children }: { children: ComponentChildren }) {
  return <p class="rounded-xl border border-dashed border-edge px-4 py-6 text-center text-sm text-muted">{children}</p>;
}

export function Modal({ title, onClose, children }:
  { title: string; onClose: () => void; children: ComponentChildren }) {
  return (
    <div class="fixed inset-0 z-50 flex items-start justify-center overflow-y-auto bg-black/40 p-4 pt-[8vh]"
         onClick={(e) => e.target === e.currentTarget && onClose()}
         onKeyDown={(e: any) => e.key === 'Escape' && onClose()}>
      <div class="w-full max-w-xl rounded-2xl border border-edge bg-panel p-5 shadow-xl">
        <div class="mb-4 flex items-center justify-between">
          <h2 class="text-sm font-semibold tracking-tight">{title}</h2>
          <button onClick={onClose} class="text-muted hover:text-ink" title="Close">
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor"
                 stroke-width="2" stroke-linecap="round"><path d="M18 6 6 18M6 6l12 12" /></svg>
          </button>
        </div>
        {children}
      </div>
    </div>
  );
}
