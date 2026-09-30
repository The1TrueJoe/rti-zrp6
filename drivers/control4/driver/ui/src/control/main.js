// The Control tab: live state of the box's I/O and buttons to drive it.
// Talks to the driver exactly like the Remote Buttons tab (director.js):
// UI_* requests through UIRequest, CONTROL_DATA pushed back on every change.

import * as director from '../director.js';
import { $, banner } from '../dom.js';
import * as panels from './panels.js';

let feedbackTimer = null;

function feedback(text, isError) {
  const f = $('feedback');
  f.textContent = text;
  f.className = 'feedback' + (isError ? ' err' : '') + (text ? ' show' : '');
  clearTimeout(feedbackTimer);
  feedbackTimer = setTimeout(() => { f.className = 'feedback'; }, 4000);
}

director.onMessage((command, data) => {
  if (command === 'CONTROL_DATA') {
    panels.renderHeader(data);
    panels.renderOutputs('relay', data.relays);
    panels.renderOutputs('trigger', data.triggers);
    panels.renderContacts(data.contacts);
    panels.renderIr(data.ir);
    panels.renderSerial(data.serial);
  } else if (command === 'SENT') {
    feedback('Sent: ' + data.what, false);
  } else if (command === 'ERROR') {
    feedback(data.message, true);
  }
});

if (!director.available()) {
  $('status').textContent = 'Not connected';
  banner('Open this page from the "Control" tab of the RTI ZRP-6 I/O driver in Composer Pro.');
} else {
  director.connectSocket(() => director.send('UI_CONTROL_GET_STATE'));
  director.send('UI_CONTROL_GET_STATE').catch((e) => banner('Could not reach the controller: ' + e.message));
  // Without the live socket, ask now and then.
  setInterval(() => { if (!director.isSocketLive()) director.send('UI_CONTROL_GET_STATE').catch(() => {}); }, 3000);
}
