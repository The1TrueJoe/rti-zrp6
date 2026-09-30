/* The icons this UI uses, inline.
 *
 * lucide-react is 33 MB installed and pulls a module per icon through the
 * bundler. These are the same 24x24 stroke paths, and they cost bytes rather
 * than kilobytes — which matters when the bundle lives in the board's Flash.
 */
type P = { size?: number; class?: string };
const svg = (d: preact.JSX.Element, p: P) => (
  <svg width={p.size ?? 16} height={p.size ?? 16} viewBox="0 0 24 24" fill="none"
       stroke="currentColor" stroke-width="2" stroke-linecap="round"
       stroke-linejoin="round" class={p.class}>{d}</svg>
);

export const Cpu = (p: P) => svg(<><rect x="4" y="4" width="16" height="16" rx="2" /><rect x="9" y="9" width="6" height="6" /><path d="M9 2v2M15 2v2M9 20v2M15 20v2M2 9h2M2 15h2M20 9h2M20 15h2" /></>, p);
export const Toggle = (p: P) => svg(<><rect x="1" y="5" width="22" height="14" rx="7" /><circle cx="8" cy="12" r="3" /></>, p);
export const Radio = (p: P) => svg(<><circle cx="12" cy="12" r="2" /><path d="M4.9 19.1a10 10 0 0 1 0-14.2M19.1 4.9a10 10 0 0 1 0 14.2M7.8 16.2a6 6 0 0 1 0-8.4M16.2 7.8a6 6 0 0 1 0 8.4" /></>, p);
export const Terminal = (p: P) => svg(<><path d="m4 17 6-6-6-6M12 19h8" /></>, p);
export const Zap = (p: P) => svg(<><path d="M13 2 3 14h9l-1 8 10-12h-9l1-8z" /></>, p);
export const Alert = (p: P) => svg(<><path d="M12 9v4M12 17h.01M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z" /></>, p);
export const Plus = (p: P) => svg(<><path d="M5 12h14M12 5v14" /></>, p);
export const Trash = (p: P) => svg(<><path d="M3 6h18M8 6V4h8v2M19 6l-1 14H6L5 6" /></>, p);
export const Grip = (p: P) => svg(<><circle cx="9" cy="6" r="1" /><circle cx="15" cy="6" r="1" /><circle cx="9" cy="12" r="1" /><circle cx="15" cy="12" r="1" /><circle cx="9" cy="18" r="1" /><circle cx="15" cy="18" r="1" /></>, p);
export const Upload = (p: P) => svg(<><path d="M12 16V4M7 9l5-5 5 5M4 20h16" /></>, p);
export const User = (p: P) => svg(<><circle cx="12" cy="8" r="4" /><path d="M4 21a8 8 0 0 1 16 0" /></>, p);
export const Globe = (p: P) => svg(<><circle cx="12" cy="12" r="10" /><path d="M2 12h20M12 2a15 15 0 0 1 0 20 15 15 0 0 1 0-20" /></>, p);
export const Close = (p: P) => svg(<><path d="M18 6 6 18M6 6l12 12" /></>, p);
