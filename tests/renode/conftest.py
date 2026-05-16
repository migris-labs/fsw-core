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
PUS17_RESC_PATH = HERE / "scripts" / "pus17_nucleo_h753zi.resc"


def _find_elf(env_var: str, artefact_subdir: str, west_build_dir: str) -> Path | None:
    """Locate a cross-compiled sample ELF for the Renode smoke suite.

    Resolution order (same shape used for every fsw-core sample):
      1. ``<env_var>`` (absolute path). If set, the path must exist —
         raises ``FileNotFoundError`` otherwise, since an explicit
         setting with a typo should fail loudly.
      2. CI download-artifact layout (upload-artifact@v4 strips the
         shared path prefix from multi-file uploads when all paths
         share one, so the ELF lands flat at the artefact root):
         ``<repo>/artifacts/<artefact_subdir>/zephyr.elf``.
      3. Local west build under the workspace root (parent of the
         fsw-core dir): ``<ws>/build/<west_build_dir>/zephyr/zephyr.elf``.
      4. Same with the default west build dir name:
         ``<ws>/build/zephyr/zephyr.elf``.
    """
    explicit = os.environ.get(env_var)
    if explicit:
        p = Path(explicit)
        if not p.exists():
            raise FileNotFoundError(f"{env_var} points at missing file: {p}")
        return p

    candidates = [
        REPO_ROOT / f"artifacts/{artefact_subdir}/zephyr.elf",
        REPO_ROOT.parent / f"build/{west_build_dir}/zephyr/zephyr.elf",
        REPO_ROOT.parent / "build/zephyr/zephyr.elf",
    ]
    for c in candidates:
        if c.exists():
            return c
    return None


_RENODE_BIN = find_renode_binary()
_HELLO_ELF = _find_elf("FSW_CORE_HELLO_ELF", "hello", "zephyr-hello")
_PUS17_ELF = _find_elf("FSW_CORE_PUS17_ELF", "pus17", "zephyr-pus17")


def _boot(
    elf: Path, resc: Path
) -> Iterator[tuple[RenodeMonitor, UartCapture]]:
    """Shared boot path: spawn Renode, load the .resc with the given
    ELF and a fresh UART port, attach the UART capture *before* start
    so neither boot bytes nor early replies are dropped, then yield
    ``(monitor, uart)`` for the test body."""
    uart_port = _pick_free_port()
    with RenodeMonitor(_RENODE_BIN) as mon:
        # Wider timeout on platform/ELF load: cold-loading the H7
        # platform description + ELF takes longer than a free-running
        # monitor command.
        mon.cmd(f"$elf = @{elf}")
        mon.cmd(f"$uart_port = {uart_port}")
        mon.cmd(f"i @{resc}", timeout=20.0)

        with UartCapture("127.0.0.1", uart_port) as uart:
            mon.cmd("start")
            yield mon, uart


@pytest.fixture
def hello_running() -> Iterator[tuple[RenodeMonitor, UartCapture]]:
    """Boot the hello-world ELF in Renode with a TCP UART terminal
    attached. Yields ``(monitor, uart_capture)`` for the test body."""
    assert _RENODE_BIN is not None  # pytestmark guarantees this
    assert _HELLO_ELF is not None  # pytestmark guarantees this
    yield from _boot(_HELLO_ELF, RESC_PATH)


@pytest.fixture
def pus17_running() -> Iterator[tuple[RenodeMonitor, UartCapture]]:
    """Boot the PUS-17 UART sample ELF in Renode with a TCP UART
    terminal attached. Yields ``(monitor, uart_capture)``."""
    assert _RENODE_BIN is not None
    assert _PUS17_ELF is not None
    yield from _boot(_PUS17_ELF, PUS17_RESC_PATH)
