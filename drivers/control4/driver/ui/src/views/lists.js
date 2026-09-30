// The remotes list, the selected remote's buttons, and recent presses.

import { $, el } from '../dom.js';
import { store } from '../store.js';
import * as director from '../director.js';
import { openEditor } from './editor.js';

function renderStatus(s) {
  $('dot').className = 'dot ' + (s.connected ? 'ok' : 'bad');
  $('status').textContent = (s.connected ? 'Connected to the ZRP-6' : (s.status || 'Not connected to the ZRP-6')) + ' - driver ' + s.version;
}

function renderRemotes(s) {
  const box = $('remotes');
  box.innerHTML = '';
  if (!s.remotes.length) box.appendChild(el('div', { class: 'empty', text: 'None yet' }));
  if (!s.remotes.some((r) => r.id === store.selectedRemote)) store.selectedRemote = s.remotes.length ? s.remotes[0].id : null;
  s.remotes.forEach((r) => {
    box.appendChild(el('div', {
      class: 'remote' + (r.id === store.selectedRemote ? ' sel' : ''),
      title: r.id === '' ? 'codes without a preamble (the bench remote)' : r.id,
      onclick: () => { store.selectedRemote = r.id; render(); },
    }, [el('span', { text: r.name }), el('span', { class: 'n', text: String(r.buttons) })]));
  });
  const remote = s.remotes.find((r) => r.id === store.selectedRemote);
  $('listTitle').textContent = remote ? remote.name : 'Buttons';
  $('renameRemote').style.display = remote ? '' : 'none';
}

function renderButtons(s) {
  const tbody = $('buttons');
  tbody.innerHTML = '';
  const list = s.buttons.filter((b) => b.remote === store.selectedRemote);
  $('noButtons').style.display = list.length ? 'none' : '';
  list.forEach((b) => {
    tbody.appendChild(el('tr', {}, [
      el('td', { text: b.name }),
      el('td', { class: 'code', text: b.code }),
      el('td', { class: 'acts', text: b.summary || 'programming event only' }),
      el('td', { class: 'row-actions' }, [
        el('button', { text: 'Edit', onclick: () => openEditor(b.code) }),
        el('button', { text: 'Test', title: 'Run its actions now', onclick: () => director.send('UI_TEST_BUTTON', { code: b.code }) }),
        el('button', {
          class: 'danger', text: 'Delete',
          onclick: () => { if (confirm('Forget "' + b.name + '"?')) director.send('UI_DELETE_BUTTON', { code: b.code }); },
        }),
      ]),
    ]));
  });
}

function renderRecent(s) {
  const ul = $('recent');
  ul.innerHTML = '';
  if (!s.recent.length) ul.appendChild(el('li', { class: 'muted', text: 'Press a button on any RTI remote.' }));
  s.recent.forEach((r) => {
    ul.appendChild(el('li', {}, [
      el('span', { class: 'lbl' }, [
        el('div', { text: r.known ? r.label : 'Not learned' }),
        el('div', { class: 'code', text: r.code + '  (' + r.ago + ' s ago)' }),
      ]),
      el('button', { text: r.known ? 'Edit' : 'Add', onclick: () => openEditor(r.code) }),
    ]));
  });
}

export function render() {
  const s = store.driver;
  if (!s) return;
  renderStatus(s);
  renderRemotes(s);
  renderButtons(s);
  renderRecent(s);
}

export function init() {
  $('renameRemote').onclick = () => {
    const remote = store.driver.remotes.find((r) => r.id === store.selectedRemote);
    if (!remote) return;
    const name = prompt('Name for this remote', remote.name);
    if (name) director.send('UI_RENAME_REMOTE', { id: remote.id, name });
  };
}
