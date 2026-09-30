// The Remote Buttons tab: wiring. See director.js for how it talks to
// Control4 and the driver, catalog.js for rooms/commands/sources, views/ for
// the pieces of the page.

import * as director from './director.js';
import * as catalog from './catalog.js';
import { store, subscribe } from './store.js';
import { $, banner } from './dom.js';
import * as lists from './views/lists.js';
import * as learn from './views/learn.js';
import * as editor from './views/editor.js';

director.onMessage((command, data) => {
  if (command === 'STATE_DATA') {
    store.driver = data;
    lists.render();
    learn.update();
  } else if (command === 'ROOMS_DATA') {
    catalog.driverRooms(data.rooms);
  } else if (command === 'SOURCES_DATA') {
    catalog.driverSources(data.room, data.sources);
  } else if (command === 'SAVED') {
    store.selectedRemote = data.remote;
    editor.saved();
  } else if (command === 'ERROR') {
    if (!editor.showError(data.message)) banner(data.message);
  }
});

subscribe(() => editor.roomsChanged());

lists.init();
learn.init();
editor.init();

if (!director.available()) {
  $('status').textContent = 'Not connected';
  banner('Open this page from the "Remote Buttons" tab of the RTI ZRP-6 I/O driver in Composer Pro.');
} else {
  director.connectSocket(() => director.send('UI_GET_STATE'));
  director.send('UI_GET_STATE').catch((e) => banner('Could not reach the controller: ' + e.message));
  catalog.loadRooms(false);
  // Without the live socket, ask now and then.
  setInterval(() => { if (!director.isSocketLive()) director.send('UI_GET_STATE').catch(() => {}); }, 3000);
}
