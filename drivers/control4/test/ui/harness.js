// Shared by the tab tests: runs a BUILT tab page (build/stage/www/html/*.html)
// against the real driver.lua, with a stand-in for Composer's webview and
// Director in between.
//
// Director is played the way the pages use it:
//   GET  /api/v1/rooms, /api/v1/items/<room>/commands, /api/v1/agents/ui_configuration,
//        /api/v1/items?tree=false                      -> test/fixtures/director/*.json
//   POST /api/v1/items/<driver>/commands {command, tParams:{DATA}} -> the driver's UIRequest
//   C4:SendDataToUI -> socket.io event {data:{devicecommand:{command, params:{param:[...]}}}}
// The driver runs in test/integration/harness.lua (the mock C4) and is driven
// over its line protocol. The DOM is a small shim: enough for these pages.
'use strict';

const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const ROOT = path.resolve(__dirname, '..', '..');
const H = {};
module.exports = H;

H.DRIVER = path.resolve(process.argv[2] || path.join(ROOT, 'build/stage/driver.lua'));
H.LUA = process.argv[3] || 'lua';
H.page = (name) => fs.readFileSync(path.join(path.dirname(H.DRIVER), 'www/html', name), 'utf8');
const FIX = (name) => JSON.parse(fs.readFileSync(path.join(ROOT, 'test/fixtures/director', name), 'utf8'));
const hex = (s) => Buffer.from(s).toString('hex');
H.hex = hex;

// ---- the driver, over the harness's line protocol ------------------------------------

class Lua {
  constructor() {
    this.p = spawn(H.LUA, [path.join(ROOT, 'test/integration/harness.lua'), H.DRIVER], { stdio: ['pipe', 'pipe', 'inherit'] });
    this.buf = ''; this.waiters = []; this.lines = [];
    this.sent = []; // MQTT bytes the driver wrote to its network binding (hex lines decoded)
    this.p.stdout.on('data', (d) => {
      this.buf += d.toString();
      let i;
      while ((i = this.buf.indexOf('\n')) >= 0) {
        const line = this.buf.slice(0, i);
        this.buf = this.buf.slice(i + 1);
        if (line === 'END') {
          const w = this.waiters.shift(); const got = this.lines; this.lines = [];
          if (w) w(got);
        } else {
          if (line.startsWith('SEND ')) this.sent.push(Buffer.from(line.slice(5), 'hex'));
          this.lines.push(line);
        }
      }
    });
    this.ready = new Promise((r) => this.waiters.push(r));
  }
  send(line) { return new Promise((r) => { this.waiters.push(r); this.p.stdin.write(line + '\n'); }); }
  async exec(code) {
    const out = await this.send('EXEC ' + hex(code));
    const res = out.find((l) => l.startsWith('RESULT '));
    const text = res ? Buffer.from(res.slice(7), 'hex').toString() : '';
    if (text.startsWith('ERROR:')) throw new Error(text);
    return text;
  }
  /** PUBLISH packets the driver has sent: [{topic, payload}] */
  publishes() {
    const out = [];
    for (const buf of this.sent) {
      let pos = 0;
      while (pos < buf.length) {
        const first = buf[pos];
        let len = 0, mult = 1, i = pos + 1, b;
        do { b = buf[i++]; len += (b & 127) * mult; mult *= 128; } while (b & 128);
        if ((first >> 4) === 3) {
          const tl = buf[i] * 256 + buf[i + 1];
          out.push({ topic: buf.slice(i + 2, i + 2 + tl).toString(), payload: buf.slice(i + 2 + tl, i + len).toString('binary') });
        }
        pos = i + len;
      }
    }
    return out;
  }
  quit() { this.p.stdin.end('QUIT\n'); }
}
H.Lua = Lua;

/** Brings the driver's MQTT connection up (no real socket: CONNACK is fed in). */
H.connect = async function () {
  await H.lua.send('TICK 600');
  await H.lua.send('ONLINE');
  await H.lua.send('DATA 20020000'); // CONNACK, accepted
  await H.lua.send('TICK 100');
};

// ---- DOM shim ---------------------------------------------------------------------------

class El {
  constructor(tag) {
    this.tagName = tag.toUpperCase();
    this.children = []; this.attrs = {}; this.style = {}; this.listeners = {};
    this.parentNode = null; this._text = ''; this._value = ''; this.className = '';
    this.disabled = false; this.selected = false; this.checked = false;
  }
  appendChild(c) { if (c.parentNode) c.parentNode.removeChild(c); c.parentNode = this; this.children.push(c); return c; }
  removeChild(c) { this.children = this.children.filter((x) => x !== c); c.parentNode = null; return c; }
  replaceChild(n, o) { const i = this.children.indexOf(o); if (n.parentNode) n.parentNode.removeChild(n); this.children[i] = n; n.parentNode = this; o.parentNode = null; return o; }
  set innerHTML(v) { this.children.forEach((c) => { c.parentNode = null; }); this.children = []; this._text = ''; }
  get textContent() { return this._text + this.children.map((c) => c.textContent).join(''); }
  set textContent(v) { this.innerHTML = ''; this._text = String(v); }
  setAttribute(k, v) { this.attrs[k] = String(v); if (k === 'value') this._value = String(v); }
  getAttribute(k) { return k in this.attrs ? this.attrs[k] : null; }
  addEventListener(t, fn) { (this.listeners[t] = this.listeners[t] || []).push(fn); }
  dispatch(t, e) { (this.listeners[t] || []).forEach((f) => f(e || {})); }
  focus() {}
  click() { this.dispatch('click'); if (this.onclick) this.onclick({}); }
  options() { return this.all().filter((e) => e.tagName === 'OPTION'); }
  get value() {
    if (this.tagName === 'OPTION') return 'value' in this.attrs ? this.attrs.value : this.textContent;
    if (this.tagName !== 'SELECT') return this._value;
    const opts = this.options();
    const sel = opts.filter((o) => o.selected).pop() || opts[0];
    return sel ? sel.value : '';
  }
  set value(v) {
    if (this.tagName !== 'SELECT') { this._value = String(v); return; }
    this.options().forEach((o) => { o.selected = (o.value === String(v)); });
  }
  all() { return [this].concat(...this.children.map((c) => c.all())); }
  matches(q) {
    const m = /^\[([\w-]+)(?:="([^"]*)")?\]$/.exec(q);
    if (!m) return false;
    const v = this.getAttribute(m[1]);
    return v !== null && (m[2] === undefined || v === m[2]);
  }
  querySelectorAll(q) { return this.all().slice(1).filter((e) => e.matches(q)); }
  querySelector(q) { return this.querySelectorAll(q)[0] || null; }
}

H.byId = {};
function freshDom(html) {
  H.byId = {};
  for (const m of html.matchAll(/<(\w+)([^>]*?)\sid="(\w+)"([^>]*)>/g)) {
    const e = new El(m[1]);
    const attrs = m[2] + m[4];
    const cls = /class="([^"]*)"/.exec(attrs); if (cls) e.className = cls[1];
    if (/style="display:none"/.test(attrs)) e.style.display = 'none';
    H.byId[m[3]] = e;
  }
}
const document = { getElementById: (id) => H.byId[id], createElement: (t) => new El(t) };

// ---- Director stand-in --------------------------------------------------------------------

H.env = { socketHandler: null, restCarriesAnswer: false, socketWorks: true, directorReadable: true };
H.confirms = [];
H.restLog = [];

/** Delivers the driver's SendDataToUI calls to the page's socket subscription. */
H.drainPushes = async function () {
  const json = await H.lua.exec(
    'local t = m.JSON.Array({}) for i = (__n or 0) + 1, #C4.dataToUI do local p = C4.dataToUI[i] ' +
    't[#t + 1] = {command = p.command, json = p.params.json} end __n = #C4.dataToUI return m.JSON.Encode(t)');
  if (!H.env.socketHandler) return;
  for (const p of JSON.parse(json || '[]')) {
    H.env.socketHandler([{ data: { devicecommand: { command: p.command, params: { param: [{ name: 'json', value: { static: p.json } }] } } } }]);
  }
};

const GETS = {
  '/api/v1/rooms': () => FIX('rooms.json'),
  '/api/v1/items?tree=false': () => FIX('items.json'),
  '/api/v1/agents/ui_configuration': () => FIX('ui_configuration.json'),
  '/api/v1/items/14/commands': () => FIX('room-14-commands.json'),
};

async function fetchStub(url, opts) {
  const u = new URL(url);
  const pathq = u.pathname + u.search;
  const reply = (obj, status) => ({ status: status || 200, json: async () => obj, text: async () => (typeof obj === 'string' ? obj : JSON.stringify(obj)) });
  if (opts && opts.headers && opts.headers.Authorization !== 'Bearer tok') throw new Error('no token');
  if (u.pathname === '/socket.io/socket.io.js') {
    if (!H.env.socketWorks) throw new Error('no socket.io');
    return reply('window.io = { connect: function () { return window.__fakeSocket(); } };');
  }
  if (u.pathname === '/api/v1/items/42/datatoui') return reply({ subscriptionId: 7 });
  if (u.pathname === '/api/v1/items/42/commands' && opts.method === 'POST') {
    const body = JSON.parse(opts.body);
    const answer = await H.lua.exec(`return env.UIRequest(fromhex("${hex(body.command)}"), {DATA = fromhex("${hex(body.tParams.DATA)}")}) or ""`);
    setTimeout(H.drainPushes, 0);
    return reply(H.env.restCarriesAnswer ? answer : {});
  }
  H.restLog.push(pathq);
  if (H.env.directorReadable && GETS[pathq]) return reply(GETS[pathq]());
  return reply({ error: 'not found' }, H.env.directorReadable ? 404 : 401);
}

const window = {
  location: { search: '?apihost=controller.local&driverid=42&apitoken=tok' },
  __fakeSocket() {
    return {
      io: { engine: { id: 'eng1' } },
      once(ev, fn) { if (ev === 'connect') setTimeout(fn, 0); },
      on(ev, fn) { if (ev === '7') H.env.socketHandler = fn; },
      emit() {},
    };
  },
};

H.settle = async () => { for (let i = 0; i < 40; i++) await new Promise((r) => setTimeout(r, 5)); };

/** Runs a built page's script against the shim. */
H.loadPage = function (html) {
  freshDom(html);
  const script = /<script>([\s\S]*)<\/script>\s*<\/body>/.exec(html)[1];
  const quiet = { log() {}, error() {} };
  const fn = new Function('window', 'document', 'fetch', 'btoa', 'confirm', 'prompt', 'alert', 'setInterval', 'clearInterval', 'setTimeout', 'clearTimeout', 'eval', 'console', script);
  fn(window, document, fetchStub, (s) => Buffer.from(s, 'binary').toString('base64'),
    (msg) => { H.confirms.push(msg); return true; }, () => 'Den remote', () => {}, () => 0, () => {},
    setTimeout, clearTimeout, (code) => { new Function('window', code)(window); }, quiet);
};

// ---- checks and DOM helpers -----------------------------------------------------------------

H.passed = 0; H.failed = 0;
H.check = function (name, cond, detail) {
  if (cond) { H.passed++; console.log('  ok    ' + name); } else { H.failed++; console.log('  FAIL  ' + name + (detail ? ' ' + detail : '')); }
};
H.finish = function () {
  console.log(`\n${H.passed} passed, ${H.failed} failed`);
  process.exit(H.failed ? 1 : 0);
};
H.findButton = (root, text) => root.all().find((e) => e.tagName === 'BUTTON' && e.textContent === text);
H.selects = (row) => row.all().filter((e) => e.tagName === 'SELECT');
H.inputs = (row) => row.all().filter((e) => e.tagName === 'INPUT');
H.optionLabels = (sel) => sel.options().map((o) => o.textContent);
H.choose = (sel, value) => { sel.value = String(value); if (sel.onchange) sel.onchange(); };
