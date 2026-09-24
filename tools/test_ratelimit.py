#!/usr/bin/env python3
"""Rate-limiter test: 100 rapid connections from one IP, expect rejections.

Starts a RAW echo backend + lb (--rate R --burst B), opens N connections as
fast as possible from 127.0.0.1, sends a small payload on each and counts:
  accepted = full echo received
  rejected = connection closed/reset/empty before echo (limiter closed it)

Usage:
  python3 tools/test_ratelimit.py
  python3 tools/test_ratelimit.py --n 100 --rate 10 --burst 5

Exit 0 (PASS) when some connections are rejected AND some accepted.
"""
import argparse
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LB_BIN = os.path.join(ROOT, "build", "lb")
ECHO_SERVER = os.path.join(ROOT, "tools", "echo_server.py")


def wait_port(host: str, port: int, timeout: float = 10.0) -> bool:
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with socket.create_connection((host, port), timeout=1):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def one_attempt(host: str, port: int, payload: bytes) -> bool:
    """True = accepted (echo ok), False = rejected/failed."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(3)
    try:
        s.connect((host, port))
        s.sendall(payload)
        data = b""
        while len(data) < len(payload):
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
        return data == payload
    except (OSError, TimeoutError):
        return False
    finally:
        s.close()


def main() -> int:
    ap = argparse.ArgumentParser(description="rate limiter 100-conn test")
    ap.add_argument("--n", type=int, default=100)
    ap.add_argument("--rate", type=float, default=10)
    ap.add_argument("--burst", type=float, default=5)
    ap.add_argument("--proxy-port", type=int, default=8081)
    ap.add_argument("--backend-port", type=int, default=9001)
    args = ap.parse_args()

    if not os.path.exists(LB_BIN):
        print(f"build {LB_BIN} first", file=sys.stderr)
        return 1

    backend = subprocess.Popen([sys.executable, ECHO_SERVER, str(args.backend_port), "--raw"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    lb_log = open(f"/tmp/opencode/ratelimit-lb.log", "w")
    lb = subprocess.Popen(
        [LB_BIN, "--listen", str(args.proxy_port),
         "--backends", f"127.0.0.1:{args.backend_port}",
         "--workers", "2", "--rate", str(args.rate), "--burst", str(args.burst)],
        stdout=subprocess.DEVNULL, stderr=lb_log)
    try:
        if not wait_port("127.0.0.1", args.backend_port):
            print("FAIL: backend did not start", flush=True)
            return 1
        if not wait_port("127.0.0.1", args.proxy_port):
            print("FAIL: lb did not start", flush=True)
            return 1
        # NOTE: the two wait_port probes above each consumed one token.
        # Re-fill: pause so the bucket is full before the measured burst.
        time.sleep(args.burst / max(args.rate, 1e-6) + 0.5)

        accepted = 0
        rejected = 0
        payload = b"ping-rl"
        for _ in range(args.n):
            if one_attempt("127.0.0.1", args.proxy_port, payload):
                accepted += 1
            else:
                rejected += 1
        print(f"connections={args.n} accepted={accepted} rejected={rejected} "
              f"(rate={args.rate}/s burst={args.burst})", flush=True)
        lb_log.flush()
        with open("/tmp/opencode/ratelimit-lb.log") as f:
            logged = sum(1 for line in f if "rate limited" in line)
        print(f"lb logged rejections: {logged}", flush=True)
        if rejected > 0 and accepted > 0:
            print("PASS", flush=True)
            return 0
        print("FAIL: expected a mix of accepted and rejected", flush=True)
        return 1
    finally:
        for p in (lb, backend):
            if p is not None:
                p.terminate()
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill()
        lb_log.close()


if __name__ == "__main__":
    sys.exit(main())
