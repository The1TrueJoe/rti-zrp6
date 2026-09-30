// Rooms, room commands and room sources: from Director first, from the
// driver (C4:GetDevices / the room's UI requests) when Director's REST API is
// not reachable. Results are cached in the store; callers pass a callback.

import * as director from './director.js';
import { commandSpecs } from './commands.js';
import { store, changed } from './store.js';

const waiting = { rooms: [], sources: {} };

function roomsReady(rooms, from) {
  store.rooms = rooms;
  store.roomsFrom = from;
  const fns = waiting.rooms;
  waiting.rooms = [];
  fns.forEach((fn) => fn(rooms));
  changed();
}

/** @param {boolean} refresh @param {(rooms:any[]) => void} [fn] */
export function loadRooms(refresh, fn) {
  if (store.rooms.length && !refresh) { if (fn) fn(store.rooms); return; }
  if (fn) waiting.rooms.push(fn);
  director.getRooms()
    .then((rooms) => {
      if (rooms.length) roomsReady(rooms, 'director');
      else director.send('UI_GET_ROOMS', { refresh: true });
    })
    .catch(() => director.send('UI_GET_ROOMS', { refresh: true }));
}

/** The driver's fallback answer (ROOMS_DATA). */
export function driverRooms(rooms) {
  if (store.roomsFrom === 'director' && store.rooms.length) return;
  roomsReady((rooms || []).map((r) => ({ id: Number(r.id), name: String(r.name) })), 'driver');
}

/**
 * Director's commands for a room, as CommandSpecs; null if Director could
 * not be asked (the picker then offers a typed command).
 * @param {number} roomId @param {(specs:any[]|null) => void} fn
 */
export function loadCommands(roomId, fn) {
  if (roomId in store.commands) { fn(store.commands[roomId]); return; }
  director.getRoomCommands(roomId)
    .then((list) => commandSpecs(list))
    .catch(() => null)
    .then((specs) => {
      store.commands[roomId] = specs && specs.length ? specs : null;
      fn(store.commands[roomId]);
    });
}

function sourcesReady(roomId, list) {
  store.sources[roomId] = list;
  const fns = waiting.sources[roomId] || [];
  waiting.sources[roomId] = [];
  fns.forEach((fn) => fn(list));
}

/** @param {number} roomId @param {(sources:any[]) => void} fn */
export function loadSources(roomId, fn) {
  if (store.sources[roomId]) { fn(store.sources[roomId]); return; }
  (waiting.sources[roomId] = waiting.sources[roomId] || []).push(fn);
  if (waiting.sources[roomId].length > 1) return;
  director.getRoomSources(roomId)
    .then((list) => {
      if (list.length) sourcesReady(roomId, list);
      else director.send('UI_GET_SOURCES', { room: roomId });
    })
    .catch(() => director.send('UI_GET_SOURCES', { room: roomId }));
}

/** The driver's fallback answer (SOURCES_DATA). */
export function driverSources(roomId, list) {
  sourcesReady(Number(roomId), list || []);
}

export function reset() {
  store.rooms = [];
  store.roomsFrom = '';
  store.commands = {};
  store.sources = {};
}
