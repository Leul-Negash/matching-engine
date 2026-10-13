#!/usr/bin/env python3
"""End-to-end test of ob_server over real TCP sockets.

Covers order entry, fills routed to both sides, market data, cancel-on-
disconnect, protocol errors, crash recovery from the journal (kill -9 and
restart), session limits and graceful shutdown.

usage: smoke_test.py <build-dir>
"""

import os
import signal
import socket
import subprocess
import sys
import tempfile
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "build/release"
SERVER = os.path.join(BUILD, "ob_server")
failures = 0


def check(cond, what):
    global failures
    print(("PASS  " if cond else "FAIL  ") + what)
    if not cond:
        failures += 1


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Client:
    def __init__(self, port):
        deadline = time.time() + 5
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=2)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.05)
        self.buf = b""

    def send(self, *lines):
        self.sock.sendall("".join(l + "\n" for l in lines).encode())

    def expect(self, *wanted, timeout=2.0):
        """Reads lines until every wanted line has been seen (any order). Returns all lines read."""
        pending = list(wanted)
        seen = []
        deadline = time.time() + timeout
        while pending and time.time() < deadline:
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                text = line.decode()
                seen.append(text)
                if text in pending:
                    pending.remove(text)
            if not pending:
                break
            self.sock.settimeout(max(0.01, deadline - time.time()))
            try:
                data = self.sock.recv(65536)
            except (socket.timeout, ConnectionResetError):
                break
            if not data:
                break
            self.buf += data
        if pending:
            print("    missing:", pending)
            print("    got:    ", seen)
        return not pending, seen

    def close(self):
        self.sock.close()


servers = []  # every process started; killed on exit whatever happens


def start(port, journal, *extra):
    log = open(journal + ".log", "a")
    proc = subprocess.Popen(
        [SERVER, "--symbol", "AAPL", "--symbol", "MSFT:5", "--port", str(port), "--journal", journal, *extra],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=log,
    )
    servers.append(proc)
    Client(port).close()  # wait until it accepts connections
    return proc


def main():
    tmp = tempfile.mkdtemp(prefix="ob_smoke_")
    journal = os.path.join(tmp, "ob.journal")
    port = free_port()
    proc = start(port, journal)

    a, b = Client(port), Client(port)
    a.send("NEW a1 AAPL SELL 100 101", "NEW a2 AAPL SELL 50 102")
    check(a.expect("ACK a1 1", "ACK a2 2")[0], "resting orders acknowledged")

    b.send("SUB AAPL", "NEW b1 AAPL BUY 30 101")
    ok, _ = b.expect("SUBSCRIBED AAPL", "ACK b1 3", "FILL b1 101 30 0", "TRADE AAPL 101 30 BUY",
                     "BBO AAPL - 0 101 70")
    check(ok, "aggressor filled, trade and quote published")
    check(a.expect("FILL a1 101 30 70")[0], "resting order owner receives its fill")

    b.send("NEW b2 AAPL BUY 1 abc", "NEW b3 NOPE BUY 1 1", "CANCEL never", "PING")
    check(b.expect("ERROR invalid price", "REJECT b3 UNKNOWN_SYMBOL", "REJECT never UNKNOWN_ORDER", "PONG")[0],
          "malformed and invalid requests rejected")

    b.send("NEW b4 MSFT BUY 10 102")  # rejected by the engine, but still consumes order id 4
    check(b.expect("REJECT b4 INVALID_PRICE")[0], "per-instrument tick size enforced")

    # Cancel on disconnect: d1 must vanish when its session drops.
    d = Client(port)
    d.send("NEW d1 AAPL BUY 10 90")
    check(d.expect("ACK d1 5")[0], "order from short-lived session acknowledged")
    d.close()
    time.sleep(0.2)
    b.send("BOOK AAPL")
    ok, seen = b.expect("BOOKEND AAPL")
    check(ok and not any("BID 90" in l for l in seen), "cancel-on-disconnect removed the order")

    # Crash: kill -9 with a's orders resting, then restart from the journal.
    proc.send_signal(signal.SIGKILL)
    proc.wait()
    a.close()
    b.close()
    proc = start(port, journal)
    c = Client(port)
    c.send("BOOK AAPL")
    check(c.expect("LEVEL AAPL ASK 101 70 1", "LEVEL AAPL ASK 102 50 1", "BOOKEND AAPL")[0],
          "book recovered from journal after kill -9")
    c.send("NEW c1 AAPL BUY 70 101")
    check(c.expect("ACK c1 6", "FILL c1 101 70 0")[0], "order ids continue after recovery and recovered orders trade")

    # Session limit.
    proc.send_signal(signal.SIGTERM)
    check(proc.wait(timeout=5) == 0, "graceful shutdown on SIGTERM exits 0")
    c.close()
    proc = start(port, journal, "--max-sessions", "1")
    # start()'s readiness probe may still occupy the only slot for a moment.
    deadline = time.time() + 5
    while True:
        first = Client(port)
        first.send("PING")
        if first.expect("PONG", timeout=1)[0] or time.time() > deadline:
            break
        first.close()
        time.sleep(0.05)
    second = Client(port)
    check(second.expect("ERROR server full")[0], "connections over --max-sessions are refused")
    first.send("BOOK AAPL")
    check(first.expect("LEVEL AAPL ASK 102 50 1", "BOOKEND AAPL")[0], "state survives a clean restart too")
    first.close()
    second.close()
    proc.send_signal(signal.SIGINT)
    check(proc.wait(timeout=5) == 0, "graceful shutdown on SIGINT exits 0")

    with open(journal + ".log") as f:
        log = f.read()
    check("replayed" in log and "stopped:" in log, "server logs recovery and shutdown")
    check("Sanitizer" not in log and "runtime error" not in log, "no sanitizer reports from the server")

    print(f"\nsmoke test: {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    code = 1
    try:
        code = main()
    finally:
        for p in servers:
            if p.poll() is None:
                p.kill()
                p.wait()
    sys.exit(code)
