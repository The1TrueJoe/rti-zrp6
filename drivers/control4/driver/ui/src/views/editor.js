// The button editor dialog: name, code, and the list of actions.

import { $, open, close, isOpen } from '../dom.js';
import { store } from '../store.js';
import * as director from '../director.js';
import * as catalog from '../catalog.js';
import { actionRow } from './actionRow.js';

let editing = null;
let rows = [];

function addRow(action) {
  const r = actionRow(action);
  rows.push(r);
  $('editActions').appendChild(r.el);
}

function note() {
  $('catalogNote').textContent = store.roomsFrom === 'driver'
    ? 'Director could not be asked for rooms, so the driver\'s room list is shown; commands may have to be typed.'
    : '';
}

/** @param {string} code */
export function openEditor(code) {
  const btn = store.driver.buttons.find((b) => b.code === code);
  editing = code;
  $('editTitle').textContent = btn ? 'Edit ' + btn.remoteName + ' / ' + btn.name : 'New button';
  $('editName').value = btn ? btn.name : '';
  $('editCode').textContent = code;
  $('editErr').textContent = '';
  $('editActions').innerHTML = '';
  rows = [];
  catalog.loadRooms(false, () => {
    if (editing !== code) return;
    $('editActions').innerHTML = '';
    rows = [];
    (btn && btn.actions.length ? btn.actions : [{}]).forEach((a) => addRow(JSON.parse(JSON.stringify(a))));
    note();
  });
  open('editDlg');
}

/** Called when rooms arrive after the rows were drawn (driver fallback). */
export function roomsChanged() {
  if (!isOpen('editDlg')) return;
  rows.forEach((r) => r.redrawRooms());
  note();
}

export function saved() {
  close('editDlg');
}

export function showError(message) {
  if (!isOpen('editDlg')) return false;
  $('editErr').textContent = message;
  return true;
}

export function init() {
  $('addAction').onclick = () => addRow({});
  $('refreshRooms').onclick = () => {
    catalog.reset();
    catalog.loadRooms(true, () => roomsChanged());
  };
  $('editCancel').onclick = () => close('editDlg');
  $('editSave').onclick = () => {
    const live = rows.filter((r) => r.el.parentNode);
    const actions = [];
    for (let i = 0; i < live.length; i++) {
      const a = live[i].collect();
      if (a.error) { $('editErr').textContent = 'Action ' + (i + 1) + ': ' + a.error + '.'; return; }
      actions.push(a);
    }
    $('editErr').textContent = '';
    director.send('UI_SAVE_BUTTON', { code: editing, name: $('editName').value, actions });
  };
}
