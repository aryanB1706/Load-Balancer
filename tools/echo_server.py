#!/usr/bin/env python3
"""Simple TCP echo server for load-balancer testing.

Usage:
    python3 tools/echo_server.py 9001
    python3 tools/echo_server.py --port 9001 --prefix backend-9001
"""
import argparse
import socket
import threading


def handle(conn: socket.socket, addr, prefix: str) -> None:
    try:
        with conn:
            while True:
                data = conn.recv(4096)
                if not data:
                    break
                conn.sendall((prefix + ": ").encode() + data if prefix else data)
    except (ConnectionResetError, BrokenPipeError):
        pass


def main() -> None:
    ap = argparse.ArgumentParser(description="TCP echo server")
    ap.add_argument("port", nargs="?", type=int, help="port to listen on")
    ap.add_argument("--port", dest="port_opt", type=int, default=None)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--prefix", default=None, help="prefix echoed replies with backend id")
    ap.add_argument("--raw", action="store_true", help="echo bytes verbatim (no prefix)")
    args = ap.parse_args()

    port = args.port_opt if args.port_opt is not None else args.port
    if port is None:
        ap.error("port is required: echo_server.py 9001")
    if not 1 <= port <= 65535:
        ap.error("port must be 1..65535")

    if args.raw:
        prefix = ""
    elif args.prefix is not None:
        prefix = args.prefix
    else:
        prefix = f"backend-{port}"
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, port))
    srv.listen(128)
    print(f"echo server on {args.host}:{port} prefix={prefix!r}", flush=True)
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=handle, args=(conn, addr, prefix), daemon=True).start()


if __name__ == "__main__":
    main()
