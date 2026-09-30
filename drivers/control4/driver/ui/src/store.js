// The page's state, and a tiny change notifier. The driver's STATE_DATA push
// replaces `driver`; rooms, commands and sources are caches of what Director
// (or, as a fallback, the driver) has told us.

/**
 * @typedef {{code:string, remote:string, remoteName:string, name:string, actions:object[], summary:string}} Button
 * @typedef {{version:string, connected:boolean, status:string,
 *   learn:{armed:boolean, remaining:number, captured?:string},
 *   remotes:{id:string, name:string, buttons:number}[], buttons:Button[],
 *   recent:{code:string, ago:number, known:boolean, label?:string, remote:string}[]}} DriverState
 */

export const store = {
  /** @type {DriverState|null} */
  driver: null,
  /** @type {{id:number, name:string}[]} */
  rooms: [],
  /** where rooms came from: 'director' | 'driver' | '' */
  roomsFrom: '',
  /** @type {Object<number, import('./commands.js').CommandSpec[]|null>} null = not available */
  commands: {},
  /** @type {Object<number, {id:number, name:string, type:string}[]>} */
  sources: {},
  selectedRemote: null,
};

const listeners = [];

/** @param {() => void} fn */
export function subscribe(fn) { listeners.push(fn); }

export function changed() { listeners.forEach((fn) => fn()); }

export function roomName(id) {
  const r = store.rooms.find((x) => x.id === Number(id));
  return r ? r.name : '';
}
