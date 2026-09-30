// One action in the button editor: a room, one of that room's Control4
// commands (with its parameters), a source to select, or "programming event
// only". What it saves is exactly what the driver replays:
//   {kind:'command', room, roomName, command, params, label}
//   {kind:'source', room, roomName, source, label}
//   {kind:'event'}

import { el, select } from '../dom.js';
import { store, roomName } from '../store.js';
import * as catalog from '../catalog.js';

const EVENT = '@event';
const SOURCE = '@source';
const TYPED = '@typed';

function parseTyped(text) {
  const params = {};
  String(text || '').split(',').forEach((pair) => {
    const m = pair.match(/^\s*([\w]+)\s*=\s*(.*?)\s*$/);
    if (m) params[m[1]] = m[2];
  });
  return params;
}

function typedText(params) {
  return Object.keys(params || {}).map((k) => k + '=' + params[k]).join(', ');
}

/**
 * @param {object} action a saved action, or {} for a new one
 * @returns {{el: HTMLElement, collect: () => object, redrawRooms: () => void}}
 */
export function actionRow(action) {
  const a = Object.assign({ kind: 'command', params: {} }, action || {});
  const row = el('div', { class: 'action-row' });
  let roomSel = null;
  const whatCell = el('span', { class: 'what' });
  const paramCell = el('span', { class: 'params' });
  let whatSel = null;
  let specs = null; // CommandSpecs for the current room, or null

  function roomOptions() {
    const opts = store.rooms.map((r) => ({ value: r.id, label: r.name }));
    if (a.room && !store.rooms.some((r) => r.id === Number(a.room))) {
      opts.push({ value: a.room, label: (a.roomName || 'Room ' + a.room) + ' (saved)' });
    }
    return opts;
  }

  function currentWhat() {
    if (a.kind === 'event') return EVENT;
    if (a.kind === 'source') return SOURCE;
    return a.command || '';
  }

  function drawParams() {
    paramCell.innerHTML = '';
    const what = whatSel ? whatSel.value : '';
    if (what === SOURCE) {
      const waitSel = select([], '', roomSel.value ? 'Loading sources...' : 'Pick a room first');
      paramCell.appendChild(waitSel);
      if (!roomSel.value) return;
      catalog.loadSources(Number(roomSel.value), (list) => {
        if (whatSel.value !== SOURCE) return;
        const s = select(list.map((x) => ({ value: x.id, label: x.name + ' (' + x.type + ')' })), a.source,
          list.length ? 'Source...' : 'No sources in this room');
        s.setAttribute('data-role', 'source');
        s.onchange = () => { a.source = s.value; };
        paramCell.innerHTML = '';
        paramCell.appendChild(s);
      });
    } else if (what === TYPED) {
      paramCell.appendChild(el('input', { placeholder: 'Command, e.g. ROOM_OFF', value: a.kind === 'command' ? (a.command || '') : '', 'data-role': 'typed-command' }));
      paramCell.appendChild(el('input', { placeholder: 'Parameters: NAME=value, ...', value: typedText(a.params), 'data-role': 'typed-params' }));
    } else if (what && what !== EVENT) {
      const spec = (specs || []).find((c) => c.command === what);
      const params = spec ? spec.params : Object.keys(a.params || {}).map((name) => ({ name, label: name, kind: 'text' }));
      params.forEach((p) => {
        const saved = a.command === what && a.params ? a.params[p.name] : undefined;
        let input;
        if (p.kind === 'list') {
          input = select(p.options, saved, p.label + '...');
        } else if (p.kind === 'number') {
          input = el('input', { type: 'number', min: p.min, max: p.max, value: saved != null ? saved : (p.min != null ? p.min : '') });
        } else {
          input = el('input', { value: saved != null ? saved : '' });
        }
        input.setAttribute('data-param', p.name);
        paramCell.appendChild(el('label', { text: p.label }, [input]));
      });
    }
  }

  function drawWhat() {
    whatCell.innerHTML = '';
    const room = Number(roomSel.value) || null;
    const opts = [{ value: EVENT, label: 'Programming event only' }];
    if (room) opts.push({ value: SOURCE, label: 'Select source...', group: 'Helpers' });
    if (specs) {
      specs.forEach((c) => opts.push({ value: c.command, label: c.label, group: 'Control4 commands for this room' }));
    } else if (room) {
      opts.push({ value: TYPED, label: 'Type a command...', group: 'Helpers' });
    }
    let value = currentWhat();
    if (a.kind === 'command' && a.command && room && !opts.some((o) => o.value === a.command)) {
      if (specs) opts.push({ value: a.command, label: (a.label || a.command) + ' (saved)', group: 'Saved' });
      else value = TYPED;
    }
    if (!room && value !== EVENT) value = '';
    whatSel = select(opts, value, room ? 'Action...' : 'Pick a room first');
    whatSel.onchange = () => {
      if (whatSel.value === EVENT) a.kind = 'event';
      else if (whatSel.value === SOURCE) a.kind = 'source';
      else { a.kind = 'command'; if (whatSel.value !== TYPED) a.command = whatSel.value; }
      drawParams();
    };
    whatCell.appendChild(whatSel);
    drawParams();
  }

  function loadRoom() {
    const room = Number(roomSel.value);
    if (!room) { specs = null; drawWhat(); return; }
    whatCell.innerHTML = '';
    whatCell.appendChild(select([], '', 'Loading commands...'));
    paramCell.innerHTML = '';
    catalog.loadCommands(room, (list) => {
      if (Number(roomSel.value) !== room) return;
      specs = list;
      drawWhat();
    });
  }

  function drawRooms() {
    const fresh = select(roomOptions(), a.room, 'Room...');
    fresh.onchange = () => { a.room = fresh.value ? Number(fresh.value) : null; a.source = null; loadRoom(); };
    if (roomSel) row.replaceChild(fresh, roomSel); else row.appendChild(fresh);
    roomSel = fresh;
  }

  drawRooms();
  row.appendChild(whatCell);
  row.appendChild(paramCell);
  row.appendChild(el('button', { class: 'danger', text: 'Remove', onclick: () => row.parentNode && row.parentNode.removeChild(row) }));
  loadRoom();

  return {
    el: row,
    redrawRooms() { drawRooms(); },
    /** @returns {object} the action, or {error} */
    collect() {
      const what = whatSel ? whatSel.value : '';
      const room = Number(roomSel.value) || null;
      if (what === EVENT) return { kind: 'event' };
      if (!room) return { error: 'pick a room' };
      const base = { room, roomName: roomName(room) || a.roomName || '' };
      if (what === SOURCE) {
        const s = paramCell.querySelector('[data-role]');
        if (!s || !s.value) return { error: 'pick a source' };
        const opt = Array.prototype.find.call(s.children, (o) => o.value === s.value);
        return Object.assign(base, { kind: 'source', source: Number(s.value), label: opt ? opt.textContent.replace(/ \((watch|listen)\)$/, '') : '' });
      }
      if (what === TYPED) {
        const cmd = paramCell.querySelector('[data-role="typed-command"]');
        const prm = paramCell.querySelector('[data-role="typed-params"]');
        const command = String(cmd && cmd.value || '').trim().toUpperCase();
        if (!/^[A-Z0-9_]+$/.test(command)) return { error: 'type a command (A-Z, 0-9, _)' };
        return Object.assign(base, { kind: 'command', command, params: parseTyped(prm && prm.value), label: command });
      }
      if (!what) return { error: 'pick an action' };
      const params = {};
      paramCell.querySelectorAll('[data-param]').forEach((input) => {
        if (input.value !== '') params[input.getAttribute('data-param')] = input.value;
      });
      const spec = (specs || []).find((c) => c.command === what);
      return Object.assign(base, { kind: 'command', command: what, params, label: spec ? spec.label : (a.label || what) });
    },
  };
}
