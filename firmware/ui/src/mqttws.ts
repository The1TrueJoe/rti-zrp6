/* A minimal MQTT 3.1.1 client over WebSocket.
 *
 * mqtt.js is ~100 KB and carries TCP, TLS and WebSocket transports plus QoS 1
 * and 2 state machines. This page talks to one broker, over one transport, at
 * QoS 0 — and the bundle is programmed into the board's Flash, so the rest is
 * weight the 8032 would have to store and serve for nothing.
 */
type OnMessage = (topic: string, payload: string, retain: boolean) => void;

/** How the link is doing, beyond up/down:
 *  - 'bumped': the box closed us with code 4000 because a newer browser took
 *    the last free socket (ws.c make_room). No automatic retry — two tabs
 *    retrying would just take turns bumping each other.
 *  - 'failing': several attempts in a row have failed. The box is restarting,
 *    off the network, or every one of its four sockets is taken.
 *  - 'denied': CONNACK refused the login (return code 4 or 5). No retry —
 *    the same bad password would only fail again, and the box answers every
 *    failed login by refusing all logins for 2 s. */
export type Link = 'up' | 'connecting' | 'bumped' | 'failing' | 'denied';

/** Close code the box sends when it drops this tab to make room. */
const BUMPED = 4000;

const enc = new TextEncoder();
const dec = new TextDecoder();

function varint(n: number): number[] {
  const out: number[] = [];
  do {
    let d = n % 128;
    n = Math.floor(n / 128);
    if (n) d |= 0x80;
    out.push(d);
  } while (n);
  return out;
}

function str(s: string): number[] {
  const b = Array.from(enc.encode(s));
  return [b.length >> 8, b.length & 0xff, ...b];
}

export class MqttWs {
  private ws: WebSocket | null = null;
  private pid = 1;
  private timer: number | undefined;
  private retry: number | undefined;
  /** CONNACK accepted: until then the box takes nothing but the CONNECT. */
  private ready = false;
  private pending: string[] = [];
  private failures = 0;
  private heard = 0;

  constructor(
    private url: string,
    private user: string,
    private pass: string,
    private onMessage: OnMessage,
    private onState: (link: Link) => void,
  ) {}

  /** After being bumped, or on the user's say-so. */
  reconnect() {
    this.failures = 0;
    this.onState('connecting');
    this.connect();
  }

  connect() {
    let ws: WebSocket;
    try {
      ws = new WebSocket(this.url, 'mqtt');
    } catch {
      this.failed();
      return;
    }
    ws.binaryType = 'arraybuffer';
    this.ws = ws;

    ws.onopen = () => {
      /* clean session + username + password (0xC2), 60 s keepalive, client id
         from the page session so two tabs never collide on the broker */
      const id = 'ui-' + Math.random().toString(36).slice(2, 8);
      const body = [...str('MQTT'), 4, 0xc2, 0, 60, ...str(id), ...str(this.user), ...str(this.pass)];
      this.send([0x10, ...varint(body.length), ...body]);
      /* Ping every 5 s and give up after 15 s of silence. When the box
         reboots, the browser's TCP connection doesn't learn it is dead until
         it sends something, so a slow ping kept the page saying "Online"
         for half a minute after the box had gone. The box answers every
         ping, so silence really does mean the link is gone. */
      this.heard = Date.now();
      this.timer = self.setInterval(() => {
        if (Date.now() - this.heard > 15000) {
          this.close();             // a dead peer never finishes the close
          this.failed();
          return;
        }
        this.send([0xc0, 0]);
      }, 5000);
    };
    ws.onclose = (ev) => {
      this.cleanup();
      if (ev.code === BUMPED) this.onState('bumped');
      else this.failed();
    };
    ws.onmessage = (ev) => {
      this.heard = Date.now();
      this.read(new Uint8Array(ev.data as ArrayBuffer));
    };
  }

  /** Hang up for good: no close event, no retry. */
  close() {
    clearTimeout(this.retry);
    if (this.ws) { this.ws.onclose = null; this.ws.close(); }
    this.cleanup();
  }

  private cleanup() {
    if (this.timer) { clearInterval(this.timer); this.timer = undefined; }
    this.ws = null;
    this.ready = false;
  }
  private failed() {
    this.failures++;
    this.onState(this.failures >= 3 ? 'failing' : 'connecting');
    this.retry = self.setTimeout(() => this.connect(), 2000);
  }

  /** Any other refusal (1-3) is followed by the box hanging up, which retries. */
  private connack(rc: number) {
    if (rc === 4 || rc === 5) { this.close(); this.onState('denied'); return; }
    if (rc) return;
    this.ready = true;
    this.failures = 0;
    this.onState('up');
    this.pending.forEach((t) => this.subscribe(t));
    this.pending = [];
  }

  private send(bytes: number[]) {
    if (this.ws && this.ws.readyState === 1) this.ws.send(new Uint8Array(bytes));
  }

  private read(b: Uint8Array) {
    let i = 0;
    while (i < b.length) {
      const type = b[i] & 0xf0;
      const retain = (b[i] & 1) === 1;
      let mul = 1, rem = 0, j = i + 1;
      for (;;) {
        rem += (b[j] & 0x7f) * mul;
        mul *= 128;
        if (!(b[j++] & 0x80)) break;
      }
      if (type === 0x30) {
        const tl = (b[j] << 8) | b[j + 1];
        const topic = dec.decode(b.subarray(j + 2, j + 2 + tl));
        const payload = dec.decode(b.subarray(j + 2 + tl, j + rem));
        this.onMessage(topic, payload, retain);
      } else if (type === 0x20) this.connack(b[j + 1]);
      i = j + rem;
    }
  }

  subscribe(filter: string) {
    if (!this.ready) { this.pending.push(filter); return; }
    const id = this.pid++;
    const body = [id >> 8, id & 0xff, ...str(filter), 0];
    this.send([0x82, ...varint(body.length), ...body]);
  }

  publish(topic: string, payload: string, retain = false) {
    if (!this.ready) return;
    const body = [...str(topic), ...Array.from(enc.encode(payload))];
    this.send([0x30 | (retain ? 1 : 0), ...varint(body.length), ...body]);
  }
}
