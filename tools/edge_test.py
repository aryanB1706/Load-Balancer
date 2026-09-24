#!/usr/bin/env python3
"""Edge-case tests for the TCP load balancer.

1. backend-dies-mid-connection: start a 20MB echo transfer, kill the backend
   halfway, expect the client to see EOF (truncated, not a hang) and the lb
   to stay alive for the next request.
2. client-abort-mid-transfer: send 1MB of 10MB then RST (SO_LINGER=0);
   lb must clean up (fd count back to baseline) and keep serving.
3. slow-reader: push 2MB through, read 1KB per 5ms; exercises b2c
   backpressure; bytes must still verify.
4. fd-leak: open-fd count before == after all cases.

Usage: python3 tools/edge_test.py
Exit 0 when all cases PASS.
"""
import hashlib
import os
import socket
import struct
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LB_BIN = os.environ.get("LB_BIN", os.path.join(ROOT, "build", "lb"))
ECHO_SERVER = os.path.join(ROOT, "tools", "echo_server.py")

PROXY_PORT = 8089
BACKEND_PORT = 9009


def wait_port(host, port, timeout=10.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with socket.create_connection((host, port), timeout=1):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def fd_count(pid):
    try:
        return len(os.listdir(f"/proc/{pid}/fd"))
    except OSError:
        return -1


def start_backend(port):
    return subprocess.Popen([sys.executable, ECHO_SERVER, str(port), "--raw"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def quick_echo(payload: bytes, timeout=10) -> bytes:
    s = socket.socket()
    s.settimeout(timeout)
    try:
        s.connect(("127.0.0.1", PROXY_PORT))
        s.sendall(payload)
        s.shutdown(socket.SHUT_WR)
        out = b""
        while True:
            d = s.recv(65536)
            if not d:
                break
            out += d
        return out
    finally:
        s.close()


results = []


def check(name, cond, detail=""):
    print(f"[{'PASS' if cond else 'FAIL'}] {name} {detail}", flush=True)
    results.append(cond)


def case_backend_dies(lb_proc, backend_proc):
    total = 20 * 1024 * 1024
    block = os.urandom(1 << 20)
    got = bytearray()
    err = []

    def xfer():
        s = socket.socket()
        s.settimeout(30)
        try:
            s.connect(("127.0.0.1", PROXY_PORT))
            # Concurrent send/recv so backpressure can't deadlock the client.
            def sender():
                try:
                    sent = 0
                    while sent < total:
                        n = min(len(block), total - sent)
                        s.sendall(block[:n])
                        sent += n
                        if sent >= total // 2 and backend_proc.poll() is None:
                            backend_proc.kill()  # die mid-connection
                    try:
                        s.shutdown(socket.SHUT_WR)
                    except OSError:
                        pass
                except OSError as e:
                    err.append(f"send:{e}")

            t = threading.Thread(target=sender, daemon=True)
            t.start()
            try:
                while True:
                    d = s.recv(65536)
                    if not d:
                        break
                    got.extend(d)
            except (OSError, socket.timeout) as e:
                err.append(f"recv:{e}")
            t.join(timeout=30)
        except (OSError, socket.timeout) as e:
            err.append(f"conn:{e}")
        finally:
            s.close()

    t0 = time.time()
    xfer()
    dt = time.time() - t0
    # Backend died: client must NOT hang, must see EOF with partial data.
    check("backend-dies: client saw EOF, no hang", dt < 25, f"({dt:.1f}s, got={len(got)})")
    check("backend-dies: partial data only", 0 < len(got) < total, f"(got={len(got)})")
    # lb must still be alive for the next (restarted) backend.
    nb = start_backend(BACKEND_PORT)
    time.sleep(2.5)  # let healthchecker mark UP (fail_after/healthy_after=1 here)
    try:
        echo = quick_echo(b"alive-after-death")
        check("backend-dies: lb alive afterwards", echo == b"alive-after-death")
    finally:
        nb.terminate()
        try:
            nb.wait(timeout=5)
        except subprocess.TimeoutExpired:
            nb.kill()
    return nb


def case_client_abort(lb_proc):
    s = socket.socket()
    s.settimeout(10)
    try:
        s.connect(("127.0.0.1", PROXY_PORT))
        s.sendall(os.urandom(1 << 20))
        # Abrupt RST: no FIN, no more reads.
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        s.close()
    except OSError:
        try:
            s.close()
        except OSError:
            pass
    time.sleep(2.0)  # let epoll observe the RST and tear down
    echo = quick_echo(b"after-abort")
    check("client-abort: lb serves next conn", echo == b"after-abort")


def case_slow_reader():
    total = 2 * 1024 * 1024
    seed = os.urandom(1 << 20)
    h_send = hashlib.sha256()
    sent = 0
    while sent < total:
        h_send.update(seed)
        sent += len(seed)
    h_recv = hashlib.sha256()
    s = socket.socket()
    s.settimeout(60)
    try:
        s.connect(("127.0.0.1", PROXY_PORT))
        # Send it all first (backend echoes into lb buffers + kernel).
        sent = 0
        while sent < total:
            n = min(len(seed), total - sent)
            s.sendall(seed[:n])
            sent += n
        s.shutdown(socket.SHUT_WR)
        # Now read painfully slowly -> b2c backpressure on the lb side.
        recvd = 0
        while recvd < total:
            d = s.recv(1024)
            if not d:
                break
            h_recv.update(d)
            recvd += len(d)
            time.sleep(0.005)
        check("slow-reader: bytes verify under backpressure",
              recvd == total and h_recv.hexdigest() == h_send.hexdigest(),
              f"(recvd={recvd})")
    except (OSError, socket.timeout) as e:
        check("slow-reader: bytes verify under backpressure", False, f"({e})")
    finally:
        s.close()


def main():
    if not os.path.exists(LB_BIN):
        print(f"build {LB_BIN} first", file=sys.stderr)
        return 1
    backend = start_backend(BACKEND_PORT)
    lb = subprocess.Popen(
        [LB_BIN, "--listen", str(PROXY_PORT),
         "--backends", f"127.0.0.1:{BACKEND_PORT}",
         "--workers", "4", "--health-interval", "1",
         "--fail-after", "1", "--healthy-after", "1"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rc = 1
    try:
        if not wait_port("127.0.0.1", BACKEND_PORT) or not wait_port("127.0.0.1", PROXY_PORT):
            print("FAIL: processes did not start", flush=True)
            return 1
        time.sleep(1.0)
        fds_before = fd_count(lb.pid)
        case_backend_dies(lb, backend)
        backend = start_backend(BACKEND_PORT)  # fresh backend for the rest
        time.sleep(2.5)
        case_client_abort(lb)
        case_slow_reader()
        time.sleep(2.0)
        fds_after = fd_count(lb.pid)
        check("fd-leak: before == after", fds_before == fds_after,
              f"({fds_before} -> {fds_after})")
        rc = 0 if all(results) else 1
        print("PASS" if rc == 0 else "FAIL", flush=True)
        return rc
    finally:
        for p in (lb, backend):
            if p is not None and p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill()


if __name__ == "__main__":
    sys.exit(main())
