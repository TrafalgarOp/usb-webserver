#!/usr/bin/env python3
"""USB NCM binary throughput benchmark client.

Requests the firmware's temporary benchmark endpoint and measures the
application-level transfer rate of the existing USB NCM + lwIP transport.

The payload is a 32-bit little-endian counter: the word at body offset 4*w is
w, low byte first. This detects missing, duplicated, reordered and corrupted
bytes in one comparison.

Standard library only.
"""

import argparse
import array
import socket
import struct
import sys
import time

HOST = "192.168.7.1"
PORT = 8080
SIZES = (4096, 16384, 32768, 65536, 131072)


def expected_payload(size):
    """The exact body the firmware must produce for `size` bytes."""
    if size % 4 != 0:
        raise ValueError("size must be a multiple of 4")
    words = array.array("I", range(size // 4))
    if array.array("I").itemsize != 4:
        raise RuntimeError("array('I') is not 4 bytes on this host")
    if sys.byteorder != "little":
        words.byteswap()
    return words.tobytes()


def open_conn(host, port, timeout=10.0):
    s = socket.create_connection((host, port), timeout=timeout)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    except OSError:
        pass
    return s


def parse_headers(buf):
    head, sep, rest = buf.partition(b"\r\n\r\n")
    if not sep:
        raise RuntimeError("no header terminator")
    lines = head.split(b"\r\n")
    status = int(lines[0].split(b" ")[1])
    headers = {}
    for line in lines[1:]:
        if b":" in line:
            k, v = line.split(b":", 1)
            headers[k.strip().lower()] = v.strip()
    return status, headers, rest


def read_exact(sock, n, initial=b""):
    """Read exactly n bytes, returning (body, first_byte_ns, last_byte_ns)."""
    body = bytearray(initial)
    first_ns = None
    if body:
        first_ns = time.monotonic_ns()
    while len(body) < n:
        chunk = sock.recv(min(65536, n - len(body)))
        if not chunk:
            break
        if first_ns is None:
            first_ns = time.monotonic_ns()
        body += chunk
    return bytes(body), first_ns, time.monotonic_ns()


def fetch(host, port, path, timeout=15.0, read_body=True):
    """Issue one GET and return a result dict.

    `body_ns` times only the payload (first body byte -> last body byte);
    `total_ns` includes request send and header round trip.
    """
    sock = open_conn(host, port, timeout)
    req = ("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n"
           % (path, host)).encode()
    t0 = time.monotonic_ns()
    sock.sendall(req)

    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    t_headers = time.monotonic_ns()
    status, headers, rest = parse_headers(buf)
    clen = int(headers.get(b"content-length", b"0"))

    if not read_body:
        sock.close()
        return {"status": status, "headers": headers, "length": clen,
                "body": rest, "body_ns": None, "total_ns": t_headers - t0}

    # part of the body may already be in `rest`
    take = rest[:clen]
    body, first_ns, last_ns = read_exact(sock, clen, initial=take)
    sock.close()
    if first_ns is None:
        first_ns = t_headers
    return {
        "status": status,
        "headers": headers,
        "length": clen,
        "body": body,
        "body_ns": last_ns - first_ns,
        "total_ns": last_ns - t0,
    }


def verify(body, size):
    exp = expected_payload(size)
    if len(body) != size:
        return False, "length %d != %d" % (len(body), size)
    if body == exp:
        return True, ""
    for i in range(size):
        if body[i] != exp[i]:
            word = i // 4
            return False, "first mismatch at byte %d (word %d): got 0x%02x want 0x%02x" % (
                i, word, body[i], exp[i])
    return False, "unreachable"


def fmt_bytes_per_s(nbytes, ns):
    if ns <= 0:
        return 0.0, 0.0
    bps = nbytes / (ns / 1e9)
    return bps / 1e3, bps / 1e6


def bench_once(host, port, size):
    path = "/api/v1/benchmark/stream?bytes=%d" % size
    r = fetch(host, port, path)
    if r["status"] != 200:
        raise RuntimeError("status %d for %s" % (r["status"], path))
    ok, why = verify(r["body"], size)
    if not ok:
        raise RuntimeError("payload mismatch: %s" % why)
    return r


def run_sweep(host, port, sizes, repeats):
    print("payload verification + throughput (body-only and end-to-end)")
    print("%8s  %6s  %10s  %10s  %10s  %10s  %s" % (
        "size", "rep", "body_ms", "body_MB/s", "e2e_ms", "e2e_MB/s", "ok"))
    results = {}
    for size in sizes:
        body_rates = []
        e2e_rates = []
        for rep in range(repeats):
            r = bench_once(host, port, size)
            b_kbs, b_mbs = fmt_bytes_per_s(size, r["body_ns"])
            e_kbs, e_mbs = fmt_bytes_per_s(size, r["total_ns"])
            body_rates.append(b_mbs)
            e2e_rates.append(e_mbs)
            print("%8d  %6d  %10.3f  %10.3f  %10.3f  %10.3f  yes" % (
                size, rep + 1,
                r["body_ns"] / 1e6, b_mbs,
                r["total_ns"] / 1e6, e_mbs))
        body_rates.sort()
        e2e_rates.sort()
        results[size] = {
            "min": body_rates[0],
            "median": body_rates[len(body_rates) // 2],
            "max": body_rates[-1],
            "e2e_median": e2e_rates[len(e2e_rates) // 2],
        }
    print()
    print("%8s  %12s  %12s  %12s  %12s" % (
        "size", "min_MB/s", "med_MB/s", "max_MB/s", "e2e_med_MB/s"))
    for size in sizes:
        r = results[size]
        print("%8d  %12.3f  %12.3f  %12.3f  %12.3f" % (
            size, r["min"], r["median"], r["max"], r["e2e_median"]))
    return results


def test_concurrent_during_transfer(host, port, size=131072):
    """Hold a stream open (headers only) and prove the second slot still
    serves normal API traffic."""
    print("\nconcurrent normal requests during a transfer:")
    path = "/api/v1/benchmark/stream?bytes=%d" % size
    s = open_conn(host, port)
    s.sendall(("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n"
               % (path, host)).encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    status, headers, _ = parse_headers(buf)
    print("  stream opened: status=%d content-length=%s" % (
        status, headers.get(b"content-length", b"?").decode()))
    for probe in ("/api/v1/system", "/api/v1/gpio"):
        r = fetch(host, port, probe, timeout=5.0)
        print("  during transfer GET %-16s -> %d" % (probe, r["status"]))
    # drain and confirm the payload is intact
    clen = int(headers.get(b"content-length", b"0"))
    body, _, _ = read_exact(s, clen, initial=buf.partition(b"\r\n\r\n")[2])
    ok, why = verify(body, size)
    print("  stream payload after probes: %s" % ("ok" if ok else why))
    s.close()
    return ok


def test_busy(host, port, size=131072):
    """A second concurrent benchmark must be refused, and the gate must free
    when the first connections closes."""
    print("\nconcurrent benchmark gate:")
    path = "/api/v1/benchmark/stream?bytes=%d" % size
    s = open_conn(host, port)
    s.sendall(("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n"
               % (path, host)).encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    r = fetch(host, port, "/api/v1/benchmark/stream?bytes=4096", timeout=5.0)
    print("  second stream while busy -> %d %s" % (
        r["status"], r["body"][:80].decode(errors="replace")))
    s.close()
    time.sleep(0.3)
    r2 = bench_once(host, port, 4096)
    print("  stream after first closed  -> %d (%d bytes ok)" % (
        r2["status"], len(r2["body"])))
    return r["status"] == 409 and r2["status"] == 200


def test_abort_restart(host, port, size=131072):
    """Abort a stream mid-transfer (RST), then immediately start another."""
    print("\nabort mid-transfer then restart:")
    path = "/api/v1/benchmark/stream?bytes=%d" % size
    s = open_conn(host, port)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.sendall(("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n"
               % (path, host)).encode())
    buf = s.recv(2048)  # read a little, then reset
    print("  aborted after %d bytes" % len(buf))
    s.close()
    time.sleep(0.3)
    r = bench_once(host, port, 4096)
    print("  restart -> %d (%d bytes ok)" % (r["status"], len(r["body"])))
    return r["status"] == 200


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default=HOST)
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--repeats", type=int, default=5)
    ap.add_argument("--sizes", type=int, nargs="*", default=list(SIZES))
    ap.add_argument("--extra", action="store_true",
                    help="also run concurrency, busy-gate and abort tests")
    args = ap.parse_args()

    run_sweep(args.host, args.port, args.sizes, args.repeats)
    if args.extra:
        test_concurrent_during_transfer(args.host, args.port)
        test_busy(args.host, args.port)
        test_abort_restart(args.host, args.port)


if __name__ == "__main__":
    main()
