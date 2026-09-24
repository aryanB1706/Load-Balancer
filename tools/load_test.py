#!/usr/bin/env python3
"""Load test: 1000+ concurrent connections through the proxy.

Each connection sends a deterministic payload and reads back the exact echo
from a RAW backend, verifying bytes. Reports throughput, failures, and (in
spawn mode) lb open-fd counts before/after to catch leaks.

Usage:
  python3 tools/load_test.py --conns 1200
  python3 tools/load_test.py --conns 1500 --payload 4096 --workers 4
  python3 tools/load_test.py --no-spawn --proxy 127.0.0.1:8081 --conns 1200

Exit 0 (PASS) when all echoes verify and fd count is stable.
"""
import argparse
import asyncio
import os
import resource
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LB_BIN = os.path.join(ROOT, "build", "lb")
ECHO_SERVER = os.path.join(ROOT, "tools", "echo_server.py")


def raise_fds() -> None:
    try:
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        want = min(65536, hard)
        if soft < want:
            resource.setrlimit(resource.RLIMIT_NOFILE, (want, hard))
    except (OSError, ValueError):
        pass


def wait_port(host: str, port: int, timeout: float = 15.0) -> bool:
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with socket.create_connection((host, port), timeout=1):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def fd_count(pid: int) -> int:
    try:
        return len(os.listdir(f"/proc/{pid}/fd"))
    except OSError:
        return -1


def make_payload(seed: int, n: int) -> bytes:
    # Deterministic per-connection bytes, cheap to build.
    return bytes(((seed + i) & 0xFF) for i in range(n))


async def one_exchange(host: str, port: int, seed: int, n: int) -> bool:
    try:
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(host, port), timeout=15)
    except (OSError, asyncio.TimeoutError):
        return False
    try:
        payload = make_payload(seed, n)
        writer.write(payload)
        await writer.drain()
        got = b""
        while len(got) < n:
            chunk = await asyncio.wait_for(reader.read(65536), timeout=30)
            if not chunk:
                break
            got += chunk
        return got == payload
    except (OSError, asyncio.TimeoutError):
        return False
    finally:
        try:
            writer.close()
            await writer.wait_closed()
        except (OSError, BrokenPipeError):
            pass


async def run_all(host: str, port: int, conns: int, payload: int):
    sem = asyncio.Semaphore(conns)  # all may proceed; bounds loop pressure only

    async def wrapped(i: int) -> bool:
        async with sem:
            return await one_exchange(host, port, i, payload)

    t0 = time.time()
    results = await asyncio.gather(*(wrapped(i) for i in range(conns)))
    return results, time.time() - t0


def main() -> int:
    ap = argparse.ArgumentParser(description="1000+ concurrent conn load test")
    ap.add_argument("--conns", type=int, default=1200)
    ap.add_argument("--payload", type=int, default=4096)
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--proxy-port", type=int, default=8081)
    ap.add_argument("--backend-port", type=int, default=9001)
    ap.add_argument("--proxy", default=None)
    ap.add_argument("--no-spawn", action="store_true")
    args = ap.parse_args()

    raise_fds()
    backend = None
    lb = None
    try:
        if args.proxy and not args.no_spawn:
            args.no_spawn = True
        if not args.no_spawn:
            lb_bin = os.environ.get("LB_BIN", LB_BIN)
            if not os.path.exists(lb_bin):
                print(f"build {lb_bin} first", file=sys.stderr)
                return 1
            backend = subprocess.Popen(
                [sys.executable, ECHO_SERVER, str(args.backend_port), "--raw"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            lb = subprocess.Popen(
                [lb_bin, "--listen", str(args.proxy_port),
                 "--backends", f"127.0.0.1:{args.backend_port}",
                 "--workers", str(args.workers)],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if not wait_port("127.0.0.1", args.backend_port):
                print("FAIL: backend did not start", flush=True)
                return 1
            if not wait_port("127.0.0.1", args.proxy_port):
                print("FAIL: lb did not start", flush=True)
                return 1
            host, port = "127.0.0.1", args.proxy_port
            # Let probe connections fully close before measuring fds.
            time.sleep(1.0)
            fds_before = fd_count(lb.pid)
        else:
            hp = (args.proxy or f"127.0.0.1:{args.proxy_port}").rsplit(":", 1)
            host, port = hp[0], int(hp[1])
            fds_before = -1

        print(f"{args.conns} concurrent conns x {args.payload}B via {host}:{port} ...",
              flush=True)
        results, dt = asyncio.run(run_all(host, port, args.conns, args.payload))
        ok = sum(1 for r in results if r)
        fail = len(results) - ok
        total_bytes = args.conns * args.payload
        print(f"ok={ok} fail={fail} time={dt:.2f}s "
              f"xput={2 * total_bytes / max(dt, 1e-6) / 1e6:.1f} MB/s (both dirs)",
              flush=True)

        rc = 0
        if lb is not None:
            time.sleep(2.0)  # let TIME_WAIT-ish teardown settle
            fds_after = fd_count(lb.pid)
            print(f"lb fds: before={fds_before} after={fds_after}", flush=True)
            if fds_before >= 0 and fds_after != fds_before:
                print(f"FAIL: fd leak? delta={fds_after - fds_before}", flush=True)
                rc = 1
        if fail:
            print(f"FAIL: {fail} exchanges failed", flush=True)
            rc = 1
        if rc == 0:
            print("PASS", flush=True)
        else:
            print("FAIL", flush=True)
        return rc
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
