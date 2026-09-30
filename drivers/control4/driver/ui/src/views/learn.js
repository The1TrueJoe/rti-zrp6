// The learn dialog: arm the driver, wait for a press, then open the editor.

import { $, open, close, isOpen } from '../dom.js';
import { store } from '../store.js';
import * as director from '../director.js';
import { openEditor } from './editor.js';

let waiting = false;
let timer = null;

function showStep(armed) {
  $('learnStart').style.display = armed ? 'none' : '';
  $('learnWait').style.display = armed ? '' : 'none';
  $('learnGo').style.display = armed ? 'none' : '';
}

/** Called on every STATE_DATA. */
export function update() {
  if (!isOpen('learnDlg')) return;
  const learn = store.driver.learn;
  if (learn.armed) {
    showStep(true);
    let left = learn.remaining;
    $('learnCountdown').textContent = left + ' s left';
    clearInterval(timer);
    timer = setInterval(() => {
      left = Math.max(0, left - 1);
      $('learnCountdown').textContent = left + ' s left';
    }, 1000);
  } else if (waiting) {
    waiting = false;
    clearInterval(timer);
    if (learn.captured) {
      close('learnDlg');
      openEditor(learn.captured);
    } else {
      $('learnCountdown').textContent = 'No button was pressed.';
      $('learnGo').style.display = '';
      $('learnGo').textContent = 'Try again';
    }
  }
}

export function init() {
  $('learnBtn').onclick = () => {
    showStep(false);
    $('learnGo').textContent = 'Start';
    $('learnName').value = '';
    waiting = false;
    open('learnDlg');
    $('learnName').focus();
  };
  $('learnGo').onclick = () => {
    waiting = true;
    showStep(true);
    $('learnCountdown').textContent = '';
    director.send('UI_LEARN', { name: $('learnName').value });
  };
  $('learnCancel').onclick = () => {
    waiting = false;
    clearInterval(timer);
    director.send('UI_CANCEL_LEARN');
    close('learnDlg');
  };
}
