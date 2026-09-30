// Director client: everything the tab says to Control4.
//
// Composer opens the tab with ?apihost=&driverid=&apitoken=. Two channels:
//
// - To this driver: POST /api/v1/items/<driverid>/commands with
//   {command, tParams: {DATA: base64(JSON)}, async: false}; Director calls the
//   driver's UIRequest. The driver answers with C4:SendDataToUI, which arrives
//   over Director's socket.io DataToUI subscription. Same bridge as
//   finitelabs' shipping tab drivers (control4-finite-labs-essentials,
//   drivers/network_requests/www/html/index.html).
// - To Director itself, for the project's rooms and each room's commands:
//   GET /api/v1/rooms, GET /api/v1/items/<id>/commands, GET
//   /api/v1/agents/ui_configuration (pyControl4 director.py; captured shapes in
//   test/fixtures/director/).

/** @typedef {{id:number, name:string}} Room */
/** @typedef {{id:number, name:string, type:string}} Source */

const query = {};
window.location.search.replace(/[?&]+([^=&]+)=([^&]*)/gi, (m, k, v) => { query[k] = decodeURIComponent(v); });

export const apiHost = query.apihost;
export const driverId = query.driverid;
const apiToken = query.apitoken;

/** True when the page was opened by Composer with the Director parameters. */
export function available() {
  return Boolean(apiHost && driverId);
}

/**
 * One REST call to Director. Resolves with parsed JSON; rejects on HTTP errors.
 * @param {string} method
 * @param {string} path
 * @param {object} [body]
 */
export function rest(method, path, body) {
  const opts = { method, headers: { Authorization: 'Bearer ' + apiToken, 'Content-Type': 'application/json' } };
  if (body) opts.body = JSON.stringify(body);
  return fetch('https://' + apiHost + path, opts).then((r) => {
    if (r.status && (r.status < 200 || r.status >= 300)) throw new Error(path + ': HTTP ' + r.status);
    return r.json();
  });
}

function b64(text) {
  return btoa(unescape(encodeURIComponent(text)));
}

// ---- the driver ----------------------------------------------------------------

let onDriverMessage = () => {};
let socketLive = false;

/** @param {(command:string, data:any) => void} fn */
export function onMessage(fn) { onDriverMessage = fn; }

export function isSocketLive() { return socketLive; }

// The driver's UIRequest also returns {"_command", "data"} as JSON, so a
// REST reply that carries it counts as an answer when the socket is down.
function findAnswer(v, depth) {
  if (depth > 3 || v == null) return;
  if (typeof v === 'string') {
    if (v.indexOf('_command') < 0) return;
    try { findAnswer(JSON.parse(v), depth + 1); } catch (e) { /* not JSON */ }
    return;
  }
  if (typeof v !== 'object') return;
  if (typeof v._command === 'string') { onDriverMessage(v._command, v.data); return; }
  Object.keys(v).forEach((k) => findAnswer(v[k], depth + 1));
}

/**
 * Sends a UI_* request to this driver.
 * @param {string} command
 * @param {object} [params]
 */
export function send(command, params) {
  if (!available()) return Promise.resolve();
  return rest('POST', '/api/v1/items/' + driverId + '/commands', {
    command, tParams: { DATA: b64(JSON.stringify(params || {})) }, async: false,
  }).then((reply) => findAnswer(reply, 0));
}

function onSocketEvent(evt) {
  (Array.isArray(evt) ? evt : [evt]).forEach((item) => {
    const dc = item && item.data ? item.data.devicecommand : null;
    if (!dc || dc.command === 'LUA_OUTPUT') return;
    const params = {};
    if (dc.params && dc.params.param) {
      (Array.isArray(dc.params.param) ? dc.params.param : [dc.params.param]).forEach((p) => {
        if (p && p.name) params[p.name] = (p.value && p.value.static !== undefined) ? p.value.static : (p.value || '');
      });
    }
    let data = null;
    try { data = JSON.parse(params.json || 'null'); } catch (e) { return; }
    onDriverMessage(dc.command, data);
  });
}

/** Subscribes to the driver's SendDataToUI pushes over Director's socket.io. */
export function connectSocket(onLive) {
  if (!available()) return;
  const jwt = 'JWT=' + apiToken;
  fetch('https://' + apiHost + '/socket.io/socket.io.js?' + jwt, { headers: { Authorization: 'Bearer ' + apiToken } })
    .then((r) => r.text())
    .then((js) => {
      (0, eval)(js); // defines window.io
      const sock = window.io.connect('https://' + apiHost, { forceNew: true, query: jwt, transports: ['polling', 'websocket'] });
      sock.once('connect', () => {
        rest('GET', '/api/v1/items/' + driverId + '/datatoui?SubscriptionClient=' + sock.io.engine.id + '&coerce=false')
          .then((sub) => {
            if (!sub || !sub.subscriptionId) return;
            sock.on(String(sub.subscriptionId), onSocketEvent);
            sock.emit('startSubscription', String(sub.subscriptionId));
            socketLive = true;
            if (onLive) onLive();
          });
      });
    })
    .catch((e) => console.error('socket', e));
}

// ---- the project ------------------------------------------------------------------

function asList(v) {
  if (Array.isArray(v)) return v;
  if (v && typeof v === 'object') {
    for (const key of ['rooms', 'items', 'commands', 'source']) if (Array.isArray(v[key])) return v[key];
    return [v];
  }
  return [];
}

function isRoom(item) {
  return item && (item.typeName === 'room' || item.type === 8);
}

function roomsFrom(list) {
  const seen = {};
  const rooms = [];
  const walk = (item) => {
    if (!item || typeof item !== 'object') return;
    if (isRoom(item) && !seen[item.id] && !item.roomHidden) {
      seen[item.id] = true;
      rooms.push({ id: Number(item.id), name: String(item.name || item.roomName || 'Room ' + item.id), order: item.roomOrder || 0 });
    }
    asList(item.children).forEach(walk);
  };
  list.forEach(walk);
  rooms.sort((a, b) => a.name.toLowerCase() < b.name.toLowerCase() ? -1 : 1);
  return rooms;
}

/**
 * The project's rooms, from Director: /api/v1/rooms (flat list), else the
 * location tree (/api/v1/locations, rooms are type 8).
 * @returns {Promise<Room[]>}
 */
export function getRooms() {
  return rest('GET', '/api/v1/rooms')
    .then((v) => roomsFrom(asList(v)))
    .then((rooms) => rooms.length ? rooms : rest('GET', '/api/v1/locations').then((v) => roomsFrom(asList(v))))
    .catch(() => rest('GET', '/api/v1/locations').then((v) => roomsFrom(asList(v))));
}

/**
 * A room's commands as Director lists them: [{command, display, params: [...]}].
 * @param {number} roomId
 */
export function getRoomCommands(roomId) {
  const paths = ['/api/v1/items/' + roomId + '/commands', '/api/v1/locations/' + roomId + '/commands', '/api/v1/rooms/' + roomId + '/commands'];
  const attempt = (i) => rest('GET', paths[i])
    .then((v) => {
      const list = asList(v).filter((c) => c && typeof c === 'object' && (c.command || c.name));
      if (!list.length && i + 1 < paths.length) return attempt(i + 1);
      return list;
    })
    .catch((e) => { if (i + 1 < paths.length) return attempt(i + 1); throw e; });
  return attempt(0);
}

let itemNames = null;
function names() {
  if (itemNames) return Promise.resolve(itemNames);
  return rest('GET', '/api/v1/items?tree=false').then((items) => {
    itemNames = {};
    asList(items).forEach((it) => { if (it && it.id != null) itemNames[it.id] = it.name; });
    return itemNames;
  }).catch(() => ({}));
}

/**
 * The room's Watch and Listen sources, from the UI configuration agent
 * (experiences: {room_id, type: watch|listen, sources: {source: [{id}]}}).
 * @param {number} roomId
 * @returns {Promise<Source[]>}
 */
export function getRoomSources(roomId) {
  return Promise.all([rest('GET', '/api/v1/agents/ui_configuration'), names()]).then(([ui, nm]) => {
    const out = [];
    const seen = {};
    asList(ui && ui.experiences).forEach((exp) => {
      if (Number(exp.room_id) !== Number(roomId) || (exp.type !== 'watch' && exp.type !== 'listen')) return;
      asList(exp.sources && exp.sources.source).forEach((s) => {
        const key = s.id + exp.type;
        if (s.id == null || seen[key]) return;
        seen[key] = true;
        out.push({ id: Number(s.id), name: nm[s.id] || s.name || 'Device ' + s.id, type: exp.type });
      });
    });
    return out;
  });
}
