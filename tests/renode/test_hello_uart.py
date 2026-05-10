# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Migris Labs

"""fsw-3 UART smoke test for the fsw-2 hello-world.

Boots ``samples/hello/zephyr.elf`` on the Renode-bundled
``nucleo_h753zi`` platform and asserts that the contract strings
emitted by ``main()`` via ``printk`` appear on USART3 (the board's
default ``zephyr,console``) within a bounded timeout.

The module skips at collection time if either Renode or the
cross-compiled ELF is missing — devs without the Zephyr SDK get a clean
skip rather than a hard failure.
"""

from __future__ import annotations

import pytest

from conftest import _HELLO_ELF, _RENODE_BIN, hello_running  # noqa: F401

pytestmark = [
    pytest.mark.skipif(
        _RENODE_BIN is None,
        reason="Renode not installed (set RENODE_BIN, put `renode` on PATH, "
        "or install Renode.app on macOS)",
    ),
    pytest.mark.skipif(
        _HELLO_ELF is None,
        reason="hello-world ELF not built. Run `west build -b nucleo_h753zi "
        "fsw-core/samples/hello --pristine=always` from the workspace root, "
        "or set FSW_CORE_HELLO_ELF to a prebuilt ELF.",
    ),
]


def test_hello_world_prints_on_usart3(hello_running) -> None:  # noqa: F811
    """The fsw-2 hello-world reaches ``main()`` and emits both contract
    strings on the board's ``zephyr,console`` (USART3).

    The first string proves boot reached ``main``; the second proves
    control flow continued past it into the idle loop (i.e. the
    process didn't trap or hard-fault between the two printks)."""
    mon, uart = hello_running

    try:
        uart.expect(b"Hello, fsw-core / nucleo_h753zi", timeout=30.0)
        uart.expect(b"boot ok; idling", timeout=5.0)
    except TimeoutError as e:
        pytest.fail(
            f"expected console string not seen: {e}\n"
            f"uart buffer so far:\n"
            f"{uart.buffer().decode(errors='replace')}"
        )
