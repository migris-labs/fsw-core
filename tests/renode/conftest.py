# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Migris Labs

"""Shared pytest fixtures for the fsw-core Renode smoke suite.

Provides:

  * ``_RENODE_BIN`` / ``_HELLO_ELF`` — module-level globals used by
    test modules' ``pytestmark`` skipifs so the suite degrades to a
    clean skip when run on a machine that has neither Renode nor the
    cross-compiled hello-world ELF.
  * ``hello_running`` fixture — spawns Renode, runs the parametrised
    ``.resc``, attaches a ``UartCapture`` to USART3, starts the CPU,
    yields ``(monitor, uart)``.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Iterator

import pytest

from _renode_driver import (
    RenodeMonitor,
    UartCapture,
    _pick_free_port,
    find_renode_binary,
)


HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
RESC_PATH = HERE / "scripts" / "hello_nucleo_h753zi.resc"


def _find_hello_elf() -> Path | None:
    """Locate the cross-compiled hello-world ELF.

    Resolution order:
      1. ``FSW_CORE_HELLO_ELF`` env var (absolute path). If set, the
         path must exist — raises ``FileNotFoundError`` otherwise.
      2. CI download-artifact layout (path-preserving v4):
         ``<repo>/artifacts/hello/build/zephyr-hello/zephyr/zephyr.elf``.
      3. Local west build under the workspace root (parent of the
         fsw-core dir): ``<ws>/build/zephyr-hello/zephyr/zephyr.elf``.
      4. Same with the default west build dir name:
         ``<ws>/build/zephyr/zephyr.elf``.
    """
    explicit = os.environ.get("FSW_CORE_HELLO_ELF")
    if explicit:
        p = Path(explicit)
        if not p.exists():
            raise FileNotFoundError(
                f"FSW_CORE_HELLO_ELF points at missing file: {p}"
            )
        return p

    candidates = [
        REPO_ROOT / "artifacts/hello/build/zephyr-hello/zephyr/zephyr.elf",
        REPO_ROOT.parent / "build/zephyr-hello/zephyr/zephyr.elf",
        REPO_ROOT.parent / "build/zephyr/zephyr.elf",
    ]
    for c in candidates:
        if c.exists():
            return c
    return None


_RENODE_BIN = find_renode_binary()
_HELLO_ELF = _find_hello_elf()


@pytest.fixture
def hello_running() -> Iterator[tuple[RenodeMonitor, UartCapture]]:
    """Boot the hello-world ELF in Renode with a TCP UART terminal
    attached. Yields ``(monitor, uart_capture)`` for the test body.

    The UART socket is attached *before* ``start`` so the boot bytes
    are not dropped. Each test gets a fresh Renode process — isolation
    is worth the small wall-clock cost for a smoke test."""
    assert _RENODE_BIN is not None  # pytestmark guarantees this
    assert _HELLO_ELF is not None  # pytestmark guarantees this

    uart_port = _pick_free_port()

    with RenodeMonitor(_RENODE_BIN) as mon:
        # Wider timeout on platform/ELF load: cold-loading the H7
        # platform description + ELF takes longer than a free-running
        # monitor command.
        mon.cmd(f"$elf = @{_HELLO_ELF}")
        mon.cmd(f"$uart_port = {uart_port}")
        mon.cmd(f"i @{RESC_PATH}", timeout=20.0)

        with UartCapture("127.0.0.1", uart_port) as uart:
            mon.cmd("start")
            yield mon, uart
