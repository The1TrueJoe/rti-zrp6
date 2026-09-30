#!/usr/bin/env node
// The Remote Buttons tab (build/stage/www/html/index.html) against the real
// driver.lua, through the Director stand-in in harness.js.
//
//   node test/ui/page_test.js build/stage/driver.lua [lua|luajit]
'use strict';

const H = require('./harness');

const rowTexts = () => H.byId.buttons.children.map((tr) => tr.children.map((td) => td.textContent).join(' | '));
const rowsOf = () => H.byId.editActions.children;
const { check, settle, drainPushes, findButton, selects, optionLabels, choose } = H;
async function roomAndWhat(row, room, what) {
  choose(selects(row)[0], room); await settle();
  choose(selects(row)[1], what); await settle();
}
const PAGE = H.page('index.html');

(async () => {
  console.log(`Remote Buttons tab, built page (${H.LUA})`);
  H.lua = new H.Lua(); await H.lua.ready;
  H.loadPage(PAGE); await settle();
  check('page subscribes over socket.io and draws the state', H.env.socketHandler !== null && /driver/.test(H.byId.status.textContent), H.byId.status.textContent);

  // learn
  H.byId.learnBtn.click();
  H.byId.learnName.value = 'Evening';
  H.byId.learnGo.click(); await settle();
  check('driver is waiting for a press', (await H.lua.exec('return tostring(m.REMOTES.learn.armed)')) === 'true');
  await H.lua.exec('m.IO.OnMessage("remote/code", "p16.16.14.12.15:14.0.2.8", false) return true');
  await drainPushes(); await settle();
  check('press learned -> editor opens on it', H.byId.editDlg.className.includes('open') && H.byId.editCode.textContent === 'p16.16.14.12.15:14.0.2.8');

  // rooms and commands from Director
  const r1 = rowsOf()[0];
  check('rooms come from Director (GET /api/v1/rooms)', optionLabels(selects(r1)[0]).join(',') === 'Room...,Bathroom,Room', optionLabels(selects(r1)[0]).join(','));
  choose(selects(r1)[0], 14); await settle();
  const labels = optionLabels(selects(r1)[1]);
  check('the action list is the room\'s Director commands (GET /api/v1/items/14/commands)',
    labels.includes('Set Volume Level') && labels.includes('Play/Pause') && labels.includes('Room Off'), labels.join(','));
  check('START_/STOP_ pairs are one "tap" entry', labels.includes('Volume Up (tap)') && !labels.some((l) => /Stop Volume/.test(l)), labels.join(','));
  check('helpers kept: select source and event only', labels.includes('Select source...') && labels.includes('Programming event only'));
  choose(selects(r1)[1], 'SET_VOLUME_LEVEL'); await settle();
  const level = r1.querySelector('[data-param="LEVEL"]');
  check('a command\'s parameters become inputs (RANGED_INTEGER -> number 0-100)', level && level.getAttribute('type') === 'number' && level.getAttribute('max') === '100');
  level.value = '35';

  H.byId.addAction.click();
  const r2 = rowsOf()[1];
  await roomAndWhat(r2, 14, '@source'); await settle();
  const src = r2.querySelector('[data-role="source"]');
  check('sources come from Director (ui_configuration + item names)', src && optionLabels(src).includes('Cable TV (watch)') && optionLabels(src).includes('Digital Media (listen)'), src && optionLabels(src).join(','));
  choose(src, 36);

  H.byId.addAction.click();
  const r3 = rowsOf()[2];
  await roomAndWhat(r3, 14, 'START_VOL_UP');

  H.byId.addAction.click();
  const r4 = rowsOf()[3];
  choose(selects(r4)[0], 31); await settle();
  check('a room Director has no command list for offers a typed command', optionLabels(selects(r4)[1]).includes('Type a command...'), optionLabels(selects(r4)[1]).join(','));
  choose(selects(r4)[1], '@typed'); await settle();
  r4.querySelector('[data-role="typed-command"]').value = 'room_off';

  H.byId.editSave.click(); await settle();
  const saved = await H.lua.exec('local b = m.REMOTES.data.buttons["p16.16.14.12.15:14.0.2.8"] local t = {} for _, a in ipairs(b.actions) do '
    + 'local ps = {} for k, v in pairs(a.params or {}) do ps[#ps + 1] = k .. "=" .. v end '
    + 't[#t + 1] = a.kind .. ":" .. tostring(a.room) .. ":" .. tostring(a.command or a.source) .. ":" .. table.concat(ps, ";") .. ":" .. tostring(a.release) end return b.name .. " " .. table.concat(t, " ")');
  check('Save stores exactly the chosen commands', saved === 'Evening command:14:SET_VOLUME_LEVEL:LEVEL=35:nil source:14:36::nil command:14:START_VOL_UP::STOP_VOL_UP command:31:ROOM_OFF::nil', saved);
  check('editor closes on SAVED', !H.byId.editDlg.className.includes('open'));

  await H.lua.exec('C4.deviceCalls = {} m.IO.OnMessage("remote/code", "p16.16.14.12.15:14.0.2.8", false) return true');
  const ran = await H.lua.exec('local t = {} for _, c in ipairs(C4.deviceCalls) do local p = c.params or {} '
    + 't[#t + 1] = c.id .. ":" .. c.command .. (p.LEVEL and ("(" .. p.LEVEL .. ")") or "") .. (p.deviceid and ("(" .. p.deviceid .. ")") or "") end return table.concat(t, " ")');
  check('pressing it replays those commands', ran === '14:SET_VOLUME_LEVEL(35) 14:SELECT_VIDEO_DEVICE(36) 14:START_VOL_UP 14:STOP_VOL_UP 31:ROOM_OFF', ran);
  await drainPushes(); await settle();
  check('the button lists what it does', rowTexts().some((t) => t.includes('Set Volume Level (LEVEL=35) in Room; Select Cable TV in Room; Volume Up (tap) in Room; ROOM_OFF in Bathroom')), rowTexts().join(' / '));

  // editing shows what was saved
  findButton(H.byId.buttons, 'Edit').click(); await settle();
  const e1 = rowsOf()[0];
  check('Edit reopens with the saved room, command and parameter', selects(e1)[0].value === '14' && selects(e1)[1].value === 'SET_VOLUME_LEVEL' && e1.querySelector('[data-param="LEVEL"]').value === '35');
  H.byId.editCancel.click();

  // bench remote, rename, delete
  await H.lua.exec('m.IO.OnMessage("remote/code", "9.9.9", false) return true');
  await drainPushes(); await settle();
  findButton(H.byId.recent, 'Add').click(); await settle();
  H.byId.editName.value = 'Off';
  choose(selects(rowsOf()[0])[1], '@event');
  H.byId.editSave.click(); await settle();
  check('bench remote button (remote id "")', (await H.lua.exec('return m.REMOTES.data.buttons["9.9.9"].remote == "" and "yes" or "no"')) === 'yes');
  H.byId.renameRemote.click(); await settle();
  check('Rename remote', (await H.lua.exec('return m.REMOTES.data.remotes[""].name')) === 'Den remote');
  findButton(H.byId.buttons, 'Delete').click(); await settle();
  check('Delete', (await H.lua.exec('return tostring(m.REMOTES.data.buttons["9.9.9"] == nil)')) === 'true' && H.confirms.length === 1);
  H.lua.quit();

  // Director's REST API not readable: rooms from the driver (C4:GetDevices), typed commands
  H.lua = new H.Lua(); await H.lua.ready;
  H.env.directorReadable = false; H.env.socketHandler = null; H.restLog.length = 0;
  H.loadPage(PAGE); await settle();
  await H.lua.exec('m.IO.OnMessage("remote/code", "5.5", false) return true');
  await drainPushes(); await settle();
  findButton(H.byId.recent, 'Add').click(); await settle();
  const f1 = rowsOf()[0];
  check('fallback: rooms from the driver', optionLabels(selects(f1)[0]).join(',') === 'Room...,Bathroom,Den,Room', optionLabels(selects(f1)[0]).join(','));
  choose(selects(f1)[0], 14); await settle();
  choose(selects(f1)[1], '@source'); await settle();
  const fs1 = f1.querySelector('[data-role="source"]');
  check('fallback: sources from the room\'s UI request, through the driver', fs1 && fs1.options().some((o) => o.value === '36'), fs1 && optionLabels(fs1).join(','));
  H.lua.quit();

  // Without the socket: the page falls back to the REST reply.
  H.lua = new H.Lua(); await H.lua.ready;
  H.env.directorReadable = true; H.env.socketHandler = null; H.env.socketWorks = false; H.env.restCarriesAnswer = true;
  H.loadPage(PAGE); await settle();
  check('no socket: state arrives through the REST reply', /driver/.test(H.byId.status.textContent), H.byId.status.textContent);
  H.lua.quit();

  H.finish();
})().catch((e) => { console.error(e); process.exit(1); });
