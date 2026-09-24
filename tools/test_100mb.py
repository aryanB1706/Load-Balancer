#!/usr/bin/env python3
"""100MB full-duplex checksum test through the TCP proxy.

Starts a RAW echo backend + lb proxy (or uses existing ones), sends N MB of
deterministic data through the proxy, half-closes, reads the echo back and
verifies length + sha256.

Usage:
  python3 tools/test_100mb.py --mb 100
  python3 tools/test_100mb.py --mb 1 --proxy-port 8081 --backend-port 9001
  python3 tools/test_100mb.py --mb 10 --no-spawn --proxy 127.0.0.1:8081

Exit 0 on PASS, 1 on FAIL.
"""
import argparse
import hashlib
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LB_BIN = os.path.join(ROOT, "build", "lb")
ECHO_SERVER = os.path.join(ROOT, "tools", "echo_server.py")

CHUNK = 64 * 1024


def gen_chunk(seed_off: int, n: int) -> bytes:
    # Deterministic pattern: byte i == (seed_off + i) % 256
    return bytes(((seed_off + i) & 0xFF) for i in range(n))


def wait_port(host: str, port: int, timeout: float = 10.0) -> bool:
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with socket.create_connection((host, port), timeout=1):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def run_test(proxy_host: str, proxy_port: int, total: int) -> int:
    # Full-duplex client: a sender thread streams while the main thread
    # receives concurrently. (Send-all-then-read deadlocks past ~10MB:
    # proxy 64KB buffers + kernel buffers fill, backend blocks in sendall,
    # client blocks in sendall — nobody reads. Classic TCP deadlock.)
    import threading

    block = os.urandom(1024 * 1024)
    h_send = hashlib.sha256()
    sent_tmp = 0
    while sent_tmp < total:
        n = min(len(block), total - sent_tmp)
        h_send.update(block if n == len(block) else block[:n])
        sent_tmp += n
    send_digest = h_send.hexdigest()

    h_recv = hashlib.sha256()
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(120)
    t0 = time.time()
    send_err: list = []

    def sender() -> None:
        try:
            sent = 0
            while sent < total:
                n = min(len(block), total - sent)
                s.sendall(block if n == len(block) else block[:n])
                sent += n
            s.shutdown(socket.SHUT_WR)  # half-close: done sending
        except OSError as e:
            send_err.append(e)

    try:
        s.connect((proxy_host, proxy_port))
        th = threading.Thread(target=sender, daemon=True)
        th.start()

        # Receive side concurrently: expect exactly `total` bytes back.
        recvd = 0
        while recvd < total:
            try:
                data = s.recv(128 * 1024)
            except TimeoutError:
                break
            if not data:
                break
            h_recv.update(data)
            recvd += len(data)
        th.join(timeout=120)
        t_recv = time.time()
    except (OSError, TimeoutError) as e:
        print(f"FAIL: socket error: {e}", flush=True)
        return 1
    finally:
        s.close()

    if send_err:
        print(f"FAIL: sender error: {send_err[0]}", flush=True)
        return 1
    ok_len = recvd == total
    ok_sum = send_digest == h_recv.hexdigest()
    dt = t_recv - t0
    print(f"sent={total} recvd={recvd} len_ok={ok_len}", flush=True)
    print(f"sha_send={send_digest}", flush=True)
    print(f"sha_recv={h_recv.hexdigest()} match={ok_sum}", flush=True)
    print(f"total={dt:.2f}s xput={total / max(dt, 1e-6) / 1e6:.1f} MB/s", flush=True)
    if ok_len and ok_sum:
        print("PASS", flush=True)
        return 0
    print("FAIL", flush=True)
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(description="100MB proxy checksum test")
    ap.add_argument("--mb", type=int, default=100, help="megabytes to send (default 100)")
    ap.add_argument("--proxy-port", type=int, default=8081)
    ap.add_argument("--backend-port", type=int, default=9001)
    ap.add_argument("--proxy", default=None, help="existing proxy host:port (implies --no-spawn)")
    ap.add_argument("--no-spawn", action="store_true", help="do not start backend/lb")
    args = ap.parse_args()

    total = args.mb * 1024 * 1024
    backend = None
    lb = None

    if args.proxy and not args.no_spawn:
        args.no_spawn = True

    try:
        if not args.no_spawn:
            if not os.path.exists(LB_BIN):
                print(f"build {LB_BIN} first: cmake -B build -S . && cmake --build build",
                      file=sys.stderr)
                return 1
            backend = subprocess.Popen(
                [sys.executable, ECHO_SERVER, str(args.backend_port), "--raw"])
            lb = subprocess.Popen(
                [LB_BIN, "--listen", str(args.proxy_port),
                 "--backends", f"127.0.0.1:{args.backend_port}", "--workers", "1"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if not wait_port("127.0.0.1", args.backend_port):
                print("FAIL: backend did not start", flush=True)
                return 1
            if not wait_port("127.0.0.1", args.proxy_port):
                print("FAIL: lb did not start", flush=True)
                return 1
            # Drain the probe connections' echoes: backend/lb are ready now.
            proxy_host, proxy_port = "127.0.0.1", args.proxy_port
        else:
            hp = (args.proxy or f"127.0.0.1:{args.proxy_port}").rsplit(":", 1)
            proxy_host, proxy_port = hp[0], int(hp[1])

        print(f"sending {args.mb}MB through {proxy_host}:{proxy_port} ...", flush=True)
        return run_test(proxy_host, proxy_port, total)
    finally:
        for p in (lb, backend):
            if p is not None:
                p.terminate()
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill()


if __name__ == "__main__":
    sys.exit(main())
