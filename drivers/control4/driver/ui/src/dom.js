// DOM helpers. No framework: the tab is a handful of lists and two dialogs.

/** @param {string} id */
export const $ = (id) => document.getElementById(id);

/**
 * Creates an element. attrs: text, class, on<event> handlers, anything else
 * as an attribute.
 * @param {string} tag
 * @param {Object<string, any>} [attrs]
 * @param {(Node|null|undefined)[]} [children]
 */
export function el(tag, attrs, children) {
  const n = document.createElement(tag);
  Object.keys(attrs || {}).forEach((k) => {
    const v = attrs[k];
    if (v === undefined || v === null) return;
    if (k === 'text') n.textContent = v;
    else if (k === 'class') n.className = v;
    else if (k.slice(0, 2) === 'on') n.addEventListener(k.slice(2), v);
    else n.setAttribute(k, v);
  });
  (children || []).forEach((c) => { if (c) n.appendChild(c); });
  return n;
}

/**
 * A <select>. options: [{value, label, group?}]; groups become <optgroup>s.
 * @param {{value:string, label:string, group?:string}[]} options
 * @param {string|number|null|undefined} value
 * @param {string} [placeholder]
 */
export function select(options, value, placeholder) {
  const s = el('select');
  if (placeholder) s.appendChild(el('option', { value: '', text: placeholder }));
  const groups = {};
  options.forEach((o) => {
    const opt = el('option', { value: String(o.value), text: o.label });
    if (String(o.value) === String(value)) opt.selected = true;
    if (o.group) {
      if (!groups[o.group]) groups[o.group] = s.appendChild(el('optgroup', { label: o.group }));
      groups[o.group].appendChild(opt);
    } else {
      s.appendChild(opt);
    }
  });
  return s;
}

export function open(id) { $(id).className = 'overlay open'; }
export function close(id) { $(id).className = 'overlay'; }
export function isOpen(id) { return $(id).className.indexOf('open') >= 0; }

export function banner(text) {
  const b = $('banner');
  b.textContent = text || '';
  b.style.display = text ? 'block' : 'none';
}
