# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Migris Labs

"""Programmatic driver for the Renode monitor.

`RenodeMonitor` spawns ``renode --port <ephemeral> --disable-xwt --plain
--hide-log`` with no startup script, connects to the resulting telnet
monitor, and exposes a tiny API for sending commands. `UartCapture` is a
companion that connects to a Renode ``CreateServerSocketTerminal`` port
and surfaces an `expect(needle, timeout)` against the running byte
buffer — the text-stream equivalent of polling a register through the
monitor.

The pair is intentionally minimal — just enough to drive the fsw-3 UART
smoke test. It is not a general-purpose Renode automation library; for
that, Antmicro's Robot Framework integration exists.

## Renode monitor protocol notes (Renode 1.16.1)

  * ``--port <P>`` exposes the monitor on a TCP socket. The first bytes
    are telnet IAC negotiation (``0xFF 0xFD ...``) which we ignore —
    they are absorbed into the banner that ends with the first prompt.
  * Prompt: ``(<machine-name>) `` once a machine is created, or
    ``(monitor) `` before. Always trailing-whitespace-terminated.
  * Renode echoes each command back terminated with ``\\n\\r`` (note
    the reversed order vs. canonical CRLF — that's Renode, not us).
  * Output lines end with ``\\r\\n\\r``.
  * LF-only command termination (``\\n``) avoids spurious double
    prompts that CRLF triggers.
"""

from __future__ import annotations

import os
import re
import shutil
import socket
import subprocess
import threading
import time
from pathlib import Path
from types import TracebackType


_PROMPT_RE = re.compile(rb"\([\w\-]+\) $")
_PROMPT_RE_TEXT = re.compile(r"\([\w\-]+\) $")


def find_renode_binary() -> Path | None:
    """Locate the Renode binary. Returns None if no Renode install is
    found at all — callers should `pytest.skip(...)` rather than fail.

    Resolution order:
      1. ``RENODE_BIN`` environment variable (absolute path). If set,
         the path must exist — raises ``FileNotFoundError`` otherwise,
         since an explicit setting with a typo should fail loudly.
      2. ``renode`` on ``$PATH``.
      3. macOS native install at
         ``~/Applications/Renode.app/Contents/MacOS/renode``.
    """
    explicit = os.environ.get("RENODE_BIN")
    if explicit:
        p = Path(explicit)
        if not p.exists():
            raise FileNotFoundError(f"RENODE_BIN points at missing file: {p}")
        return p

    on_path = shutil.which("renode")
    if on_path:
        return Path(on_path)

    mac_default = Path.home() / "Applications/Renode.app/Contents/MacOS/renode"
    if mac_default.exists():
        return mac_default

    return None


def _pick_free_port() -> int:
    """Pick an unused TCP port on localhost. Race window between close
    and the next bind is acceptable for tests."""
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class RenodeMonitor:
    """Spawned Renode process with an attached monitor connection.

    Use as a context manager so the subprocess is always cleaned up:

        with RenodeMonitor(renode_bin) as r:
            r.cmd('mach create "x"')
            r.cmd(f'machine LoadPlatformDescription @{repl}')
            r.cmd('start')
    """

    def __init__(
        self,
        renode_bin: Path,
        *,
        startup_timeout: float = 10.0,
        cmd_timeout: float = 5.0,
    ) -> None:
        self._renode_bin = renode_bin
        self._cmd_timeout = cmd_timeout
        self._port = _pick_free_port()
        self._proc = subprocess.Popen(
            [
                str(renode_bin),
                "--port",
                str(self._port),
                "--disable-xwt",
                "--plain",
                "--hide-log",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self._sock: socket.socket | None = None

        deadline = time.monotonic() + startup_timeout
        last_err: Exception | None = None
        while time.monotonic() < deadline:
            if self._proc.poll() is not None:
                raise RuntimeError(
                    f"renode exited before opening monitor port "
                    f"(rc={self._proc.returncode})"
                )
            try:
                self._sock = socket.create_connection(
                    ("127.0.0.1", self._port), timeout=2.0
                )
                break
            except OSError as e:
                last_err = e
                time.sleep(0.1)

        if self._sock is None:
            self._kill()
            raise TimeoutError(
                f"renode monitor on port {self._port} not reachable within "
                f"{startup_timeout}s: {last_err}"
            )

        # Discard the banner + initial telnet IAC bytes. The first
        # prompt marks the boundary.
        self._read_until_prompt(timeout=startup_timeout)

    # --- public API -------------------------------------------------

    def cmd(self, line: str, *, timeout: float | None = None) -> str:
        """Send a single command line, return everything the monitor
        printed before the next prompt (command echo and trailing
        prompt stripped).

        Raises ``TimeoutError`` if no prompt appears within ``timeout``
        s (defaults to the constructor's ``cmd_timeout``)."""
        if self._sock is None:
            raise RuntimeError("monitor connection already closed")
        self._sock.sendall((line + "\n").encode())
        raw = self._read_until_prompt(
            timeout=timeout if timeout is not None else self._cmd_timeout
        )
        text = raw.decode(errors="replace")
        if text.startswith(line):
            text = text[len(line):]
        text = text.lstrip("\n\r")
        text = _PROMPT_RE_TEXT.sub("", text)
        return text.rstrip("\r\n")

    def close(self) -> None:
        """Send ``quit`` and wait for the process to exit. Idempotent."""
        if self._sock is not None:
            try:
                self._sock.sendall(b"quit\n")
            except OSError:
                pass
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
        self._reap()

    # --- context-manager glue --------------------------------------

    def __enter__(self) -> "RenodeMonitor":
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        self.close()

    # --- internals --------------------------------------------------

    def _read_until_prompt(self, *, timeout: float) -> bytes:
        assert self._sock is not None
        buf = b""
        deadline = time.monotonic() + timeout
        while True:
            if time.monotonic() > deadline:
                raise TimeoutError(
                    f"renode monitor: no prompt within {timeout}s; buffer={buf!r}"
                )
            try:
                self._sock.settimeout(0.2)
                chunk = self._sock.recv(8192)
                if not chunk:
                    raise EOFError(
                        f"renode monitor closed unexpectedly; buffer={buf!r}"
                    )
                buf += chunk
                if _PROMPT_RE.search(buf):
                    return buf
            except socket.timeout:
                continue

    def _reap(self) -> None:
        if self._proc.poll() is None:
            try:
                self._proc.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                self._proc.terminate()
                try:
                    self._proc.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    self._kill()

    def _kill(self) -> None:
        if self._proc.poll() is None:
            self._proc.kill()
            try:
                self._proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                pass


class UartCapture:
    """Background TCP reader for a Renode ``CreateServerSocketTerminal``.

    Connects to ``(host, port)`` synchronously in the constructor, then
    spawns a daemon thread that ``recv()``s into an in-memory buffer
    behind a lock. ``expect(needle, timeout)`` scans the running buffer
    for a byte substring or compiled regex and returns the matched
    bytes (or raises ``TimeoutError``).

    Connect *before* the emulation starts so no boot bytes are dropped:

        uart_port = _pick_free_port()
        mon.cmd(f'$uart_port = {uart_port}')
        mon.cmd('i @<script-that-binds-the-terminal>')
        with UartCapture("127.0.0.1", uart_port) as uart:
            mon.cmd('start')
            uart.expect(b"Hello", timeout=30.0)
    """

    def __init__(
        self,
        host: str,
        port: int,
        *,
        connect_timeout: float = 5.0,
    ) -> None:
        self._sock = socket.create_connection((host, port), timeout=connect_timeout)
        # Short blocking recv() so the reader thread can observe _stop
        # promptly on shutdown.
        self._sock.settimeout(0.2)
        self._buf = bytearray()
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._closed_eof = False
        self._thread = threading.Thread(
            target=self._reader_loop,
            name=f"UartCapture(:{port})",
            daemon=True,
        )
        self._thread.start()

    # --- public API -------------------------------------------------

    def expect(
        self,
        needle: bytes | re.Pattern[bytes],
        *,
        timeout: float,
        interval: float = 0.05,
    ) -> bytes:
        """Block until the running buffer contains ``needle`` or
        ``timeout`` seconds elapse. Returns the matched bytes. On
        timeout, raises ``TimeoutError`` — callers should surface
        ``buffer()`` in the failure message for triage."""
        deadline = time.monotonic() + timeout
        while True:
            with self._lock:
                if isinstance(needle, (bytes, bytearray)):
                    idx = bytes(self._buf).find(needle)
                    if idx >= 0:
                        return bytes(needle)
                else:
                    m = needle.search(bytes(self._buf))
                    if m is not None:
                        return m.group(0)
            if self._closed_eof:
                raise TimeoutError(
                    f"uart capture: connection closed before match for {needle!r}"
                )
            if time.monotonic() >= deadline:
                raise TimeoutError(
                    f"uart capture: no match for {needle!r} within {timeout}s"
                )
            time.sleep(interval)

    def buffer(self) -> bytes:
        """Snapshot the running buffer for diagnostics."""
        with self._lock:
            return bytes(self._buf)

    def send(self, payload: bytes) -> None:
        """Write ``payload`` to the underlying Renode terminal socket.

        Renode's ``CreateServerSocketTerminal`` is bidirectional: bytes
        sent to the socket appear on the UART RX line of the connected
        peripheral, which is exactly what the fsw-4 PUS-17 round-trip
        test needs. The socket has its own short ``settimeout`` for the
        reader thread; ``sendall`` honours that for writes too, which
        is fine because small TC packets (<32 B) flush in microseconds.
        """
        self._sock.sendall(payload)

    def close(self) -> None:
        """Stop the reader thread and drop the socket. Idempotent."""
        self._stop.set()
        try:
            self._sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self._sock.close()
        except OSError:
            pass
        self._thread.join(timeout=2.0)

    # --- context-manager glue --------------------------------------

    def __enter__(self) -> "UartCapture":
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        self.close()

    # --- internals --------------------------------------------------

    def _reader_loop(self) -> None:
        while not self._stop.is_set():
            try:
                chunk = self._sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                # Socket closed under us (e.g. close() called).
                break
            if not chunk:
                # Remote (Renode) closed the terminal connection.
                self._closed_eof = True
                break
            with self._lock:
                self._buf.extend(chunk)
