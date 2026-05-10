# samples/hello

Minimum Zephyr application for the `nucleo_h753zi` board (STM32H753ZI
/ ARM Cortex-M7). It prints

```
Hello, fsw-core / nucleo_h753zi
boot ok; idling
```

to the UART console and then idles. The sample exists to prove the
fsw-core cross-compile toolchain end-to-end — ARM GCC, Zephyr SDK,
the west manifest, the BSP — under both local builds and CI.

The hello string is also the contract the (future, slice fsw-3) Renode
UART smoke test will assert on; see `sample.yaml`.

## Build

The workspace bootstrap is documented in the repo-root
[`README.md`](../../README.md#embedded-build-zephyr--slice-fsw-2).
Once the workspace is initialised:

```shell
# From the workspace root (the parent of fsw-core):
west build -b nucleo_h753zi fsw-core/samples/hello --pristine=always
```

The ELF lands at `build/zephyr/zephyr.elf`.
