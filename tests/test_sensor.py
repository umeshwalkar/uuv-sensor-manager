#!/usr/bin/env python3
"""
test_senosr.py — Dummy TCP server that simulates a sensor device.

Listens on a configurable TCP port.  When a client (senosr_manager) connects,
it first drains any init commands the manager sends, then streams one Sensor
data packet per interval second.

Supported output formats (--format):
  properitory     "param1 param2 param3"           (3-field properitory)
  nmea            "$PSGDS,param1,param2,param3*HH" (NMEA PSGDS sentence)
 
"""

import argparse
import math
import random
import signal
import socket
import sys
import time
from datetime import datetime, timezone

running = True


# ── signal handling ───────────────────────────────────────────────────────────

def _sig(signum, frame):
    global running
    running = False

signal.signal(signal.SIGINT,  _sig)
signal.signal(signal.SIGTERM, _sig)


# ── realistic dummy data generator ───────────────────────────────────────────

def make_values(t: float) -> dict:
    """Generate random values over time."""
    # Sound velocity: 1490–1510 m/s with gentle sinusoidal variation
    param1      = 1500.0 + 8.0 * math.sin(t / 30.0) + random.gauss(0, 0.05)
    
    # Pressure: 5–40 dbar (5–40 m depth), slowly increasing then decreasing
    param2 = 20.0 + 15.0 * math.sin(t / 60.0) + random.gauss(0, 0.02)
    param2 = max(0.5, param2)
    
    # Temperature: 10–22 °C
    param3    = 16.0 + 5.0 * math.sin(t / 45.0) + random.gauss(0, 0.01)
    
    # param1 = random.gauss(0, 0.001)
    # param2 = random.gauss(0, 0.001)    
    # param3 = random.gauss(0, 0.001)
    
    return dict(
        param1=param1, param2=param2, param3=param3
    )


# ── packet builders ───────────────────────────────────────────────────────────

def _nmea_checksum(body: str) -> str:
    cs = 0
    for c in body:
        cs ^= ord(c)
    return f"{cs:02X}"


def build_properitory(v: dict) -> str:
    """Format 1 — 3-field: param1 param2 param3"""
    return f"{v['param1']:.3f} {v['param2']:.3f} {v['param3']:.3f}\r\n"


def build_nmea(v: dict) -> str:
    """Format 2 — $PSGDS,param1,param2,param3*HH"""
    body = (f"PSGDS,{v['param1']:07.3f},"
            f"{v['param2']:07.3f},{v['param3']:06.3f}")
    return f"${body}*{_nmea_checksum(body)}\r\n"




FORMAT_BUILDERS = {
    "properitory":     build_properitory,
    "nmea":            build_nmea,
 
}

CYCLE_ORDER = ["properitory", "nmea"]


# ── init-command drain ────────────────────────────────────────────────────────

def drain_init_commands(conn: socket.socket, drain_sec: float = 1.5) -> bool:
    """
    Read and display any init commands sent by senosr_manager after connect.
    Returns False if the connection is broken during the drain window.
    """
    conn.settimeout(0.1)
    print("[test_senosr] Draining init commands for {:.1f}s...".format(drain_sec))
    deadline = time.monotonic() + drain_sec
    buf = b""
    while time.monotonic() < deadline:
        try:
            chunk = conn.recv(256)
            if not chunk:
                print("[test_senosr] Client closed connection during drain")
                return False
            buf += chunk
        except socket.timeout:
            pass
        except (ConnectionResetError, BrokenPipeError, OSError):
            return False

    if buf:
        text = buf.decode("ascii", errors="replace")
        for cmd in text.replace("\r\n", "\n").replace("\r", "\n").splitlines():
            cmd = cmd.strip()
            if cmd:
                print(f"[test_senosr]   <- Init cmd: {cmd!r}")
    else:
        print("[test_senosr]   (no init commands received)")
    return True


# ── client session ────────────────────────────────────────────────────────────

def handle_client(conn: socket.socket, addr, fmt: str, interval: float):
    print(f"[test_senosr] Client connected: {addr}  format={fmt}")

    if not drain_init_commands(conn):
        return

    conn.settimeout(interval + 0.5)
    t0    = time.monotonic()
    cycle = 0

    while running:
        if fmt == "cycle":
            effective_fmt = CYCLE_ORDER[cycle % len(CYCLE_ORDER)]
            cycle += 1
        else:
            effective_fmt = fmt

        v      = make_values(time.monotonic() - t0)
        packet = FORMAT_BUILDERS[effective_fmt](v)

        try:
            conn.sendall(packet.encode("ascii"))
        except (BrokenPipeError, ConnectionResetError, OSError):
            print(f"[test_senosr] Client {addr} disconnected")
            break

        print(f"[test_senosr] [{effective_fmt:>15s}] {packet.strip()}")
        time.sleep(interval)

    print(f"[test_senosr] Session ended: {addr}")


# ── server loop ───────────────────────────────────────────────────────────────

def run_server(host: str, port: int, fmt: str, interval: float):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((host, port))
        srv.listen(1)
        srv.settimeout(1.0)
        print(f"[test_senosr] Listening on {host}:{port}  format={fmt}  interval={interval}s")
        print(f"[test_senosr] Press Ctrl+C to stop\n")

        while running:
            try:
                conn, addr = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break

            with conn:
                handle_client(conn, addr, fmt, interval)

            if running:
                print("[test_senosr] Waiting for next client...\n")

    print("[test_senosr] Server stopped")


# ── entry point ───────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(
        description="Dummy TCP SVP server for testing senosr_manager",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Format examples (one packet):
  properitory     09.812 20.571 1504.164  
  nmea            $PSGDS,0009.812,1504.164,20.571*HH
  cycle           rotates through all four formats
        """,
    )
    p.add_argument("--host",     default="0.0.0.0",
                   help="TCP bind address (default: 0.0.0.0)")
    p.add_argument("--port",     type=int, default=4006,
                   help="TCP port to listen on (default: 4006)")
    p.add_argument("--interval", type=float, default=1.0,
                   help="Seconds between packets (default: 1.0)")
    p.add_argument("--format",   default="properitory",
                   choices=["properitory", "properitory-dash", "nmea", "bathy2", "cycle"],
                   help="SVP output format to simulate (default: properitory)")
    args = p.parse_args()

    try:
        run_server(args.host, args.port, args.format, args.interval)
    except Exception as exc:
        print(f"[test_senosr] Fatal: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
