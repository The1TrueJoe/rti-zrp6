#!/usr/bin/env python3
"""
rbb_bridge.py — present a serial remote_bitbang adapter to OpenOCD as TCP.

OpenOCD's `remote_bitbang` driver speaks TCP or a unix socket, not a serial
port. The ESP32 sketch in tools/esp32_remote_bitbang/ speaks the protocol over
its UART, so this sits in between.

    python3 tools/rbb_bridge.py --dev /dev/cu.usbserial-4 &
    openocd -f tools/openocd/zrp6.cfg

Latency matters more than throughput here: OpenOCD blocks on every 'R' (a TDO
sample), so each read costs a full round trip. TCP_NODELAY is set, and you
should also drop the FTDI latency timer (see --help) or every read waits for
the default 16 ms poll.
"""
import argparse
import socket
import sys
import threading

try:
    import serial
except ImportError:
    sys.exit("pyserial missing:  pip install pyserial")


def serve(ser, conn, quiet):
    """Pump one OpenOCD connection, then shut both directions down cleanly.

    The reader must be stoppable: a thread parked in a blocking serial read
    outlives the connection that spawned it, and then eats bytes belonging to
    the *next* one. So the port gets a short timeout and the thread polls a
    stop flag instead of blocking forever."""
    stop = threading.Event()

    def ser_to_tcp():
        while not stop.is_set():
            try:
                b = ser.read(max(1, ser.in_waiting))
            except Exception:
                break
            if not b:
                continue                      # timeout, just re-check stop
            try:
                conn.sendall(b)
            except Exception:
                break
        stop.set()

    t = threading.Thread(target=ser_to_tcp, daemon=True)
    t.start()
    try:
        while True:
            b = conn.recv(4096)
            if not b:
                break
            ser.write(b)
    except Exception as e:
        if not quiet:
            print("[tcp->ser] %s" % e, file=sys.stderr)
    finally:
        stop.set()
        try:
            conn.shutdown(socket.SHUT_RDWR)
        except Exception:
            pass
        conn.close()
        t.join(timeout=1.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dev", required=True, help="serial device, e.g. /dev/cu.usbmodem1203")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--port", type=int, default=4242, help="TCP port for OpenOCD")
    ap.add_argument("--once", action="store_true", help="exit after one connection")
    ap.add_argument("-q", "--quiet", action="store_true")
    a = ap.parse_args()

    # A real timeout is what makes the reader thread stoppable; do not use None.
    ser = serial.Serial(a.dev, a.baud, timeout=0.05)
    ser.reset_input_buffer()
    print("serial %s @ %d" % (a.dev, a.baud), flush=True)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", a.port))
    srv.listen(1)
    print("listening on 127.0.0.1:%d" % a.port, flush=True)

    while True:
        conn, addr = srv.accept()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print("openocd connected from %s:%d" % addr, flush=True)
        ser.reset_input_buffer()
        serve(ser, conn, a.quiet)
        print("openocd disconnected", flush=True)
        if a.once:
            break


if __name__ == "__main__":
    main()
