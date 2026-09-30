#!/usr/bin/env node
// The Control tab (build/stage/www/html/control.html) against the real
// driver.lua, through the Director stand-in in harness.js. What the driver
// publishes to the box is read back off its network binding.
//
//   node test/ui/control_test.js build/stage/driver.lua [lua|luajit]
'use strict';

const H = require('./harness');
const { check, settle, drainPushes, findButton, inputs } = H;
const PAGE = H.page('control.html');

const box = (topic, payload) => H.lua.exec(`m.IO.OnMessage(fromhex("${H.hex(topic)}"), fromhex("${H.hex(payload)}"), true) return true`);
const row = (tbody, i) => H.byId[tbody].children[i];
const cells = (tr) => tr.children.map((td) => td.textContent);
async function flush(ms) { await settle(); await H.lua.send('TICK ' + (ms || 100)); await drainPushes(); await settle(); }
function published(topic) { return H.lua.publishes().filter((p) => p.topic === topic).map((p) => p.payload); }
async function sendFrom(tr, text, escapes) {
  const [input, esc] = inputs(tr);
  input.value = text;
  if (esc && esc.getAttribute('type') === 'checkbox') esc.checked = escapes;
  findButton(tr, 'Send').click();
  await flush();
}

(async () => {
  console.log(`Control tab, built page (${H.LUA})`);
  H.lua = new H.Lua(); await H.lua.ready;
  H.loadPage(PAGE); await settle();
  check('rows for 3 relays + 3 triggers, 3 contacts, 7 IR ports, 2 RS-232 ports',
    H.byId.outputs.children.length === 6 && H.byId.contacts.children.length === 3 && H.byId.irPorts.children.length === 7 && H.byId.serialPorts.children.length === 2);
  check('before the box answers: not connected, states unknown', /Waiting|Connecting|Not/.test(H.byId.status.textContent) && cells(row('contacts', 0))[1] === '?', H.byId.status.textContent);

  await H.connect();
  await box('relay/1/state', 'ON'); await box('trigger/2/state', 'OFF'); await box('sense/1/state', 'CLOSED'); await box('sense/2/state', 'OPEN');
  await box('ir/3/mode', 'serial 9600 rs232'); await box('ir/1/mode', 'ir'); await box('serial/1/config', '19200 8N1');
  await box('firmware/version', '2026-09-29T20:00Z');
  await drainPushes(); await settle();
  check('header: connected, box address, mode, firmware', H.byId.status.textContent === 'Connected'
    && H.byId.boxInfo.textContent.includes('127.0.0.1:1883') && H.byId.boxInfo.textContent.includes('Direct to box (P2P)')
    && H.byId.boxInfo.textContent.includes('firmware 2026-09-29T20:00Z'), H.byId.boxInfo.textContent);
  check('relay and trigger states', cells(row('outputs', 0)).slice(0, 2).join('|') === 'Relay 1|ON' && cells(row('outputs', 4)).slice(0, 2).join('|') === 'Trigger 2 (12 V)|OFF');
  check('contact states', cells(row('contacts', 0)).join('|') === 'Contact 1|CLOSED' && cells(row('contacts', 1)).join('|') === 'Contact 2|OPEN');
  check('IR modes shown; IR 7 is HIGH OUT', cells(row('irPorts', 2))[1] === 'serial 9600 rs232' && cells(row('irPorts', 6))[0] === 'IR 7 (HIGH OUT)');
  check('an IR-mode port gets a code box, a serial-mode port a text box with escapes',
    inputs(row('irPorts', 0)).length === 1 && inputs(row('irPorts', 2)).length === 2);
  check('RS-232 config shown', cells(row('serialPorts', 0))[1] === '19200 8N1');

  findButton(row('outputs', 1), 'On').click(); await flush();
  check('Relay 2 On -> relay/2/set ON', published('zrp6/relay/2/set').join() === 'ON');
  check('feedback shown', H.byId.feedback.textContent === 'Sent: relay 2 on');
  findButton(row('outputs', 3), 'Pulse').click(); await flush(); await flush(600);
  check('Trigger 1 Pulse -> ON, then OFF after the pulse time', published('zrp6/trigger/1/set').join() === 'ON,OFF');
  findButton(row('outputs', 0), 'Off').click(); await flush();
  check('Relay 1 Off', published('zrp6/relay/1/set').join() === 'OFF');

  await sendFrom(row('irPorts', 0), 'nec 0x04 0x08', false);
  check('IR 1 Send -> ir/1/send', published('zrp6/ir/1/send').join() === 'nec 0x04 0x08');
  await sendFrom(row('irPorts', 2), 'PWR\\r', true);
  await sendFrom(row('irPorts', 2), 'A\\r', false);
  check('IR 3 (serial) Send -> ir/3/tx, escapes on and off', published('zrp6/ir/3/tx').join('|') === 'PWR\r|A\\r', JSON.stringify(published('zrp6/ir/3/tx')));
  await sendFrom(row('serialPorts', 1), 'HELLO\\x0d', true);
  check('RS-232 2 Send -> serial/2/tx with \\xNN', published('zrp6/serial/2/tx').join() === 'HELLO\r');

  await box('serial/1/rx', 'OK\r\n'); await drainPushes(); await settle();
  check('RS-232 1 shows the last data received', H.byId.serialPorts.children[0].textContent.includes('Received: OK\\r\\n'), H.byId.serialPorts.children[0].textContent);

  const typing = inputs(row('irPorts', 1))[0];
  typing.value = '0000 006D half typed';
  await box('sense/3/state', 'CLOSED'); await box('relay/3/state', 'ON'); await drainPushes(); await settle();
  check('live pushes update states without wiping a half-typed code', typing.value === '0000 006D half typed' && cells(row('contacts', 2))[1] === 'CLOSED' && inputs(row('irPorts', 1))[0] === typing);

  await box('ir/1/mode', 'serial 19200 ttl'); await drainPushes(); await settle();
  check('a port switched to serial on the box switches its send box', cells(row('irPorts', 0))[1] === 'serial 19200 ttl' && inputs(row('irPorts', 0)).length === 2);

  await H.lua.send('OFFLINE'); await drainPushes(); await settle();
  findButton(row('outputs', 0), 'On').click(); await flush();
  check('while the box is unreachable a button says so', H.byId.feedback.textContent === 'Not connected to the box' && H.byId.feedback.className.includes('err'), H.byId.feedback.textContent);
  check('and the header shows it', H.byId.dot.className === 'dot bad');

  H.lua.quit();
  H.finish();
})().catch((e) => { console.error(e); process.exit(1); });
