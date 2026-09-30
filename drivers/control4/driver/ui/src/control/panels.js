// The Control tab's rows. Each row is built once and then only updated, so a
// live push (a contact changing, a relay answering) never wipes what the
// installer is typing into a Send box.

import { $, el } from '../dom.js';
import * as director from '../director.js';

/** @typedef {{n:number, state:string}} OutputState */

function badge(state) {
  const cls = state === 'ON' || state === 'CLOSED' ? 'badge on' : state === '?' ? 'badge unknown' : 'badge';
  return el('span', { class: cls, text: state });
}

function setBadge(cell, state) {
  cell.innerHTML = '';
  cell.appendChild(badge(state));
}

// ---- relays and triggers ---------------------------------------------------------

const outputRows = {};

function outputRow(kind, n) {
  const stateCell = el('td', { class: 'state' });
  const act = (action) => () => director.send('UI_SET_OUTPUT', { kind, n, action });
  const tr = el('tr', {}, [
    el('td', { text: (kind === 'relay' ? 'Relay ' : 'Trigger ') + n + (kind === 'trigger' ? ' (12 V)' : '') }),
    stateCell,
    el('td', { class: 'row-actions' }, [
      el('button', { text: 'On', onclick: act('ON') }),
      el('button', { text: 'Off', onclick: act('OFF') }),
      el('button', { text: 'Pulse', onclick: act('PULSE') }),
    ]),
  ]);
  $('outputs').appendChild(tr);
  return { update: (state) => setBadge(stateCell, state) };
}

/** @param {'relay'|'trigger'} kind @param {OutputState[]} list */
export function renderOutputs(kind, list) {
  list.forEach((o) => {
    const key = kind + o.n;
    if (!outputRows[key]) outputRows[key] = outputRow(kind, o.n);
    outputRows[key].update(o.state);
  });
}

// ---- contacts ----------------------------------------------------------------------

const contactRows = {};

/** @param {OutputState[]} list */
export function renderContacts(list) {
  list.forEach((c) => {
    if (!contactRows[c.n]) {
      const cell = el('td', { class: 'state' });
      $('contacts').appendChild(el('tr', {}, [el('td', { text: 'Contact ' + c.n }), cell]));
      contactRows[c.n] = cell;
    }
    setBadge(contactRows[c.n], c.state);
  });
}

// ---- send boxes ---------------------------------------------------------------------

/**
 * An input + (optional) escapes checkbox + Send button.
 * @param {string} placeholder
 * @param {boolean} withEscapes
 * @param {(text:string, escapes:boolean) => void} onSend
 */
function sendBox(placeholder, withEscapes, onSend) {
  const input = el('input', { placeholder, class: 'send-input' });
  const esc = el('input', { type: 'checkbox', title: 'Decode \\r \\n \\t \\\\ \\xNN' });
  esc.checked = true;
  const send = () => { if (input.value !== '') onSend(input.value, withEscapes ? Boolean(esc.checked) : false); };
  input.addEventListener('keydown', (e) => { if (e.key === 'Enter') send(); });
  const parts = [input];
  if (withEscapes) parts.push(el('label', { class: 'esc', text: 'escapes' }, [esc]));
  parts.push(el('button', { class: 'primary', text: 'Send', onclick: send }));
  return { el: el('span', { class: 'send' }, parts), input, esc };
}

// ---- IR ports -------------------------------------------------------------------------

const irRows = {};

function irRow(n, highOut) {
  const modeCell = el('td', { class: 'code' });
  const sendCell = el('td', { class: 'send-cell' });
  $('irPorts').appendChild(el('tr', {}, [el('td', { text: 'IR ' + n + (highOut ? ' (HIGH OUT)' : '') }), modeCell, sendCell]));
  const ir = sendBox('Pronto hex, or nec <addr> <cmd>', false, (text) => director.send('UI_SEND_IR', { port: n, code: text }));
  const serial = sendBox('Text to send', true, (text, escapes) => director.send('UI_SEND_SERIAL', { kind: 'ir', port: n, data: text, escapes }));
  let shown = null;
  return {
    update(p) {
      modeCell.textContent = p.mode;
      const want = p.serial ? serial : ir;
      if (shown !== want) {
        sendCell.innerHTML = '';
        sendCell.appendChild(want.el);
        shown = want;
      }
    },
  };
}

export function renderIr(list) {
  list.forEach((p) => {
    if (!irRows[p.n]) irRows[p.n] = irRow(p.n, p.highOut);
    irRows[p.n].update(p);
  });
}

// ---- RS-232 ports ---------------------------------------------------------------------

const serialRows = {};

function serialRow(n) {
  const configCell = el('td', { class: 'code' });
  const rx = el('div', { class: 'rx code' });
  const box = sendBox('Text to send', true, (text, escapes) => director.send('UI_SEND_SERIAL', { kind: 'serial', port: n, data: text, escapes }));
  $('serialPorts').appendChild(el('tr', {}, [el('td', { text: 'RS-232 ' + n }), configCell, el('td', { class: 'send-cell' }, [box.el, rx])]));
  return {
    update(p) {
      configCell.textContent = p.config;
      rx.textContent = p.rx ? 'Received: ' + p.rx + ' (' + p.rxAgo + ' s ago)' : 'Nothing received yet';
    },
  };
}

export function renderSerial(list) {
  list.forEach((p) => {
    if (!serialRows[p.n]) serialRows[p.n] = serialRow(p.n);
    serialRows[p.n].update(p);
  });
}

// ---- header ---------------------------------------------------------------------------

export function renderHeader(s) {
  $('dot').className = 'dot ' + (s.connected ? 'ok' : 'bad');
  $('status').textContent = s.connected ? 'Connected' : (s.status || 'Not connected');
  const parts = [];
  if (s.address) parts.push(s.address);
  parts.push(s.mode);
  if (s.firmware) parts.push('firmware ' + s.firmware);
  parts.push('driver ' + s.version);
  $('boxInfo').textContent = parts.join(' · ');
  $('pulseNote').textContent = 'Pulse = ' + s.pulseMs + ' ms (Relay Pulse Time)';
}
