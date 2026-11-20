#!/usr/bin/env python3
"""Burst acquisition client.

Triggers a timer+DMA ADC capture on the firmware, polls status until the
capture completes, then fetches the raw samples over the USB NCM + lwIP
transport and reports timing and throughput.

The samples are little-endian uint16 (12-bit codes, right-aligned 0..4095).
Capture time is set by the ADC/timer; transfer time is set by USB, and the two
are independent (burst capture, not real-time streaming).

Standard library only.
"""

import argparse
import array
import json
import socket
import statistics
import struct
import sys
import time

HOST = "192.168.7.1"
PORT = 8080
DEFAULT_RATE = 1_000_000
DEFAULT_SAMPLES = 24576
SAMPLE_BYTES = 2


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
    first_ns = time.monotonic_ns() if body else None
    while len(body) < n:
        chunk = sock.recv(min(65536, n - len(body)))
        if not chunk:
            break
        if first_ns is None:
            first_ns = time.monotonic_ns()
        body += chunk
    return bytes(body), first_ns, time.monotonic_ns()


def request(host, port, method, path, body=None, ctype=None,
            timeout=15.0, read_body=True):
    """Issue one HTTP request and return a result dict."""
    sock = open_conn(host, port, timeout)
    lines = ["%s %s HTTP/1.1" % (method, path), "Host: %s" % host,
             "Connection: close"]
    payload = b""
    if body is not None:
        payload = body.encode() if isinstance(body, str) else body
        lines.append("Content-Type: %s" % (ctype or "application/json"))
        lines.append("Content-Length: %d" % len(payload))
    req = ("\r\n".join(lines) + "\r\n\r\n").encode() + payload

    t0 = time.monotonic_ns()
    sock.sendall(req)

    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    status, headers, rest = parse_headers(buf)
    clen = int(headers.get(b"content-length", b"0"))

    if not read_body or clen == 0:
        sock.close()
        body_bytes = rest[:clen] if read_body else b""
        return {"status": status, "headers": headers, "body": body_bytes,
                "body_ns": None, "total_ns": time.monotonic_ns() - t0}

    body_bytes, first_ns, last_ns = read_exact(sock, clen, initial=rest[:clen])
    sock.close()
    if first_ns is None:
        first_ns = time.monotonic_ns()
    return {"status": status, "headers": headers, "body": body_bytes,
            "body_ns": last_ns - first_ns, "total_ns": last_ns - t0}


def json_body(result):
    try:
        return json.loads(result["body"].decode())
    except (ValueError, UnicodeDecodeError):
        return None


def post_capture(host, port, rate, samples):
    body = json.dumps({"rate_hz": rate, "samples": samples})
    return request(host, port, "POST", "/api/v1/acquisition/capture", body=body)


def get_status(host, port):
    return request(host, port, "GET", "/api/v1/acquisition/status")


def reset(host, port):
    return request(host, port, "DELETE", "/api/v1/acquisition", read_body=False)


def wait_complete(host, port, timeout=5.0):
    """Poll status until the capture leaves 'capturing'."""
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        r = get_status(host, port)
        doc = json_body(r)
        if doc is None:
            raise RuntimeError("status %d: %s" % (r["status"], r["body"][:120]))
        data = doc.get("data", {})
        last = data
        state = data.get("state")
        if state != "capturing":
            return state, data
        time.sleep(0.005)
    return (last or {}).get("state", "timeout"), last


def fetch_data(host, port):
    return request(host, port, "GET", "/api/v1/acquisition/data")


def fmt_rate(nbytes, ns):
    if not ns:
        return 0.0, 0.0
    bps = nbytes / (ns / 1e9)
    return bps / 1e3, bps / 1e6


def analyze_samples(body, rate):
    """Return (n, min, max, mean, span_ms) from little-endian uint16 samples."""
    n = len(body) // 2
    codes = array.array("H")
    codes.frombytes(body[: n * 2])
    if sys.byteorder != "little":
        codes.byteswap()
    if n == 0:
        return 0, 0, 0, 0.0, 0.0
    return (n, min(codes), max(codes), statistics.fmean(codes),
            n / rate * 1e3)


def capture_once(host, port, rate, samples, save=None):
    reset(host, port)

    r = post_capture(host, port, rate, samples)
    doc = json_body(r)
    if r["status"] != 202 or doc is None:
        raise RuntimeError("capture rejected: %d %s" % (
            r["status"], r["body"][:160]))

    state, data = wait_complete(host, port)
    if state != "complete":
        raise RuntimeError("capture did not complete: state=%s status=%s" % (
            state, data))

    r = fetch_data(host, port)
    if r["status"] != 200:
        raise RuntimeError("data fetch failed: %d %s" % (
            r["status"], r["body"][:160]))
    body = r["body"]
    if len(body) != samples * SAMPLE_BYTES:
        raise RuntimeError("length %d != %d" % (
            len(body), samples * SAMPLE_BYTES))

    if save:
        with open(save, "wb") as f:
            f.write(body)

    n, lo, hi, mean, span_ms = analyze_samples(body, rate)
    t_kbs, t_mbs = fmt_rate(len(body), r["body_ns"])
    cfg_us = data.get("elapsed_us", 0)
    return {
        "status": data,
        "n": n, "min": lo, "max": hi, "mean": mean,
        "capture_ms": cfg_us / 1e3,
        "expected_ms": samples / rate * 1e3,
        "span_ms": span_ms,
        "transfer_ms": (r["body_ns"] or 0) / 1e6,
        "transfer_MBps": t_mbs,
        "overrun": data.get("overrun"),
        "dma_errors": data.get("dma_errors"),
    }


def probe_concurrent(host, port, rate, samples):
    """Prove normal API traffic still answers while the sample buffer streams."""
    print("\nnormal traffic during a data transfer:")
    post_capture(host, port, rate, samples)
    wait_complete(host, port)

    sock = open_conn(host, port)
    sock.sendall(b"GET /api/v1/acquisition/data HTTP/1.1\r\n"
                 b"Host: " + host.encode() + b"\r\nConnection: close\r\n\r\n")
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    status, headers, _ = parse_headers(buf)
    clen = int(headers.get(b"content-length", b"0"))
    print("  data stream opened: status=%d content-length=%d" % (status, clen))

    for probe in ("/api/v1/system", "/api/v1/gpio", "/api/v1/acquisition/status"):
        pr = request(host, port, "GET", probe, timeout=5.0)
        print("  during transfer GET %-28s -> %d" % (probe, pr["status"]))

    body, _, _ = read_exact(sock, clen, initial=buf.partition(b"\r\n\r\n")[2])
    sock.close()
    ok = len(body) == clen
    print("  stream payload after probes: %s (%d/%d bytes)" % (
        "ok" if ok else "short", len(body), clen))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default=HOST)
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--rate", type=int, default=DEFAULT_RATE,
                    help="sample rate in Hz (default 1000000)")
    ap.add_argument("--samples", type=int, default=DEFAULT_SAMPLES,
                    help="samples per capture (default 24576 = 48 KB)")
    ap.add_argument("--repeats", type=int, default=1)
    ap.add_argument("--save", metavar="FILE",
                    help="write the last capture's raw samples to FILE")
    ap.add_argument("--concurrent", action="store_true",
                    help="also probe normal traffic during a data transfer")
    args = ap.parse_args()

    print("burst acquisition: rate=%d Hz samples=%d (%d bytes)" % (
        args.rate, args.samples, args.samples * SAMPLE_BYTES))
    print("%4s  %10s  %10s  %10s  %10s  %10s  %6s  %5s" % (
        "rep", "capt_ms", "exp_ms", "xfer_ms", "xfer_MB/s",
        "min", "max", "ovr"))

    for rep in range(args.repeats):
        r = capture_once(args.host, args.port, args.rate, args.samples,
                         save=args.save)
        print("%4d  %10.3f  %10.3f  %10.3f  %10.3f  %10d  %10d  %6s  %5s" % (
            rep + 1, r["capture_ms"], r["expected_ms"], r["transfer_ms"],
            r["transfer_MBps"], r["min"], r["max"],
            "yes" if r["overrun"] else "no", r["dma_errors"]))
        if rep == args.repeats - 1:
            print("  samples=%d mean=%.1f span=%.3f ms state=%s" % (
                r["n"], r["mean"], r["span_ms"], r["status"].get("state")))

    if args.concurrent:
        probe_concurrent(args.host, args.port, args.rate, args.samples)


if __name__ == "__main__":
    main()
