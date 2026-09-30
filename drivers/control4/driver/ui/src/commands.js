// Turns Director's command list for a room into what the action picker shows.
//
// Director lists commands as [{command, display, params: [{name, type,
// values: [{id, name}]...}]}] (the shape pyControl4's get_item_commands users
// read: c4-mcp control4_gateway.py, ctrlable-control4 keypad.py). A remote
// press is a single event, so START_x/STOP_x pairs become one "tap" entry
// that sends both - the driver adds the STOP_x when it replays START_x.

/**
 * @typedef {{name:string, label:string, kind:'number'|'list'|'text', min?:number, max?:number,
 *   options?:{value:string,label:string}[]}} ParamSpec
 * @typedef {{command:string, label:string, params:ParamSpec[], tap:boolean}} CommandSpec
 */

function prettify(command) {
  return command.toLowerCase().replace(/_/g, ' ').replace(/\b\w/g, (c) => c.toUpperCase());
}

function numberOr(v, fallback) {
  const n = Number(v);
  return Number.isFinite(n) ? n : fallback;
}

/** @returns {ParamSpec} */
export function paramSpec(p) {
  const name = String(p.name || p.id || 'VALUE');
  const type = String(p.type || '').toUpperCase();
  const label = String(p.display || p.label || prettify(name));
  const values = Array.isArray(p.values) ? p.values : Array.isArray(p.items) ? p.items : null;
  if (values && values.length) {
    return {
      name, label, kind: 'list',
      options: values.map((v) => (v && typeof v === 'object')
        ? { value: String(v.id ?? v.value ?? v.name), label: String(v.name ?? v.display ?? v.value ?? v.id) }
        : { value: String(v), label: String(v) }),
    };
  }
  if (type.includes('BOOL')) {
    return { name, label, kind: 'list', options: [{ value: 'true', label: 'true' }, { value: 'false', label: 'false' }] };
  }
  if (/RANGED|INT|NUMBER|FLOAT|LEVEL|PERCENT/.test(type)) {
    return { name, label, kind: 'number', min: numberOr(p.minimum ?? p.min, undefined), max: numberOr(p.maximum ?? p.max, undefined) };
  }
  return { name, label, kind: 'text' };
}

/**
 * @param {object[]} list Director's command list
 * @returns {CommandSpec[]}
 */
export function commandSpecs(list) {
  const names = new Set(list.map((c) => String(c.command || c.name || '').toUpperCase()));
  const out = [];
  const seen = new Set();
  list.forEach((c) => {
    const command = String(c.command || c.name || '').toUpperCase();
    if (!/^[A-Z0-9_]+$/.test(command) || seen.has(command)) return;
    const stopOf = command.match(/^STOP_(.+)$/);
    if (stopOf && names.has('START_' + stopOf[1])) return; // folded into its START_
    seen.add(command);
    const startOf = command.match(/^START_(.+)$/);
    const tap = Boolean(startOf && names.has('STOP_' + startOf[1]));
    const base = String(c.display || c.label || prettify(tap ? startOf[1] : command));
    out.push({
      command,
      label: tap ? base.replace(/^start\s+/i, '') + ' (tap)' : base,
      params: (Array.isArray(c.params) ? c.params : []).map(paramSpec),
      tap,
    });
  });
  out.sort((a, b) => a.label.toLowerCase() < b.label.toLowerCase() ? -1 : 1);
  return out;
}
