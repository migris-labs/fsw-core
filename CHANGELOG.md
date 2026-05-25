# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Slice fsw-17: schedule, hkstore and mode persistence.** Three more
  subsystems join the datapool on flash, completing the persistence
  story slice fsw-16 opened. `lib/schedule/`, `lib/hkstore/` and
  `lib/mode/` each gain a pure `serialize` / `deserialize` pair and a
  `generation` counter bumped on every persisted mutation
  (`insert`/`delete`/`reset`/`set_enabled`/`pop_due` for the schedule;
  `create`/`delete`/`set_enabled` for the hkstore — explicitly not
  `_due`, which only stamps the un-persisted `last_emit_sec`;
  `_request` on a successful transition for the mode manager), and
  `lib/nvstore/` claims three new record types: `SCHEDULE = 2`,
  `HKSTORE = 3`, `MODE = 4`. The on-flash image header, encoding
  rules and `MIGRIS_NVSTORE_FORMAT_VERSION` are unchanged — adding a
  record type is non-breaking by the format's own rule — so old
  firmware silently ignores the new records and new firmware degrades
  to first-boot defaults on a record it doesn't find.
  `MIGRIS_NVSTORE_PAYLOAD_MAX` bumps from 512 to 1536 B to fit a
  worst-case full schedule (16 max-size TCs ≈ 1123 B). Per-record
  byte layouts:
  - `SCHEDULE`: `count(2 BE) + enabled(1) + { release_time(4 BE),
    tc_len(2 BE), tc(tc_len) } * count`. Variable-length per entry so
    the image stays proportional to actual scheduled volume.
  - `HKSTORE`: `count(2 BE) + { sid(2 BE), interval_sec(4 BE),
    enabled(1), param_count(1), param_ids(2 BE) * N } * count`. Only
    in-use slots are written, packed back-to-back; `last_emit_sec` and
    `in_use` are NOT serialised. Restored structures re-arm with
    `last_emit_sec = 0` — otherwise the `(now - last_emit_sec) >=
    interval_sec` arithmetic would underflow as `uint32_t` against a
    fresh FSW clock and the structure would fire on every tick until
    the clock caught up.
  - `MODE`: `current(1)`. The mode set, allowed-target bitmasks and
    optional event sink are code-defined every `init` and not
    persisted; an unknown restored ID silently falls back to the
    init-time current mode (firmware that shipped a new mode table
    boots cleanly). Restore does NOT emit a `MODE_CHANGED` event
    (boot restore is not a runtime transition) and does NOT bump
    `generation`.
  The `tc_uart` sample factors the per-subsystem auto-save into a
  small `nv_autosave_tick` helper (one helper, four callers; the four
  `last_saved_gen_*` snapshots stay inline next to the call site).
  Boot-restore order is `datapool → schedule → hkstore → mode`; each
  step silently skips when its record is absent so a first boot and
  a partial-rollback boot both behave like first-boot for the missing
  records. **The boot-time `BOOT → NOMINAL` demo transition is now
  guarded on the post-restore current still being BOOT** — a
  spacecraft persisted in SAFE (e.g. after the FDIR recovery slice
  fsw-14 lands it there) outlives a reboot rather than being silently
  undone. Operational notes:
  - **Schedule entry-ID collisions across reboot.** A pre-fsw-17
    reboot wiped the schedule; post-fsw-17 a restored entry can
    collide with a freshly-inserted one (existing `ERR_DUPLICATE`).
    Correct behaviour — ground sequence counts should be monotonic —
    but worth flagging: ground tooling that re-creates structures on
    every pass may need to swap to "create iff not in `[3,11]` report".
  - **Hkstore SID collisions.** Same correct-behaviour-but-now-visible
    story as schedule.
  Host coverage: `tests/{schedule,hkstore,mode}_persistence_test.cpp`
  cover round-trip / truncation / over-capacity / SID-floor /
  duplicate-SID / unknown-mode-ID / generation-mutation-vs-restore.
  Renode closed-loop: `tests/renode/test_tc_uart_hk.py` gains
  `test_persistence_save_writes_all_four_records` — mutates schedule,
  hkstore and mode, reads the on-flash bytes at `0x080C0000`, and
  asserts all four record types (1/2/3/4) land in the image with
  `format_version == 1` and `seq >= 2`. Same Renode 1.16.1
  `machine Reset` constraint as fsw-16 — warm-reboot closed-loop is
  not feasible (CPU stays debug-halted), so on-flash save is proved
  here and load is proved host-side. The `docs/nv-image-format.md`
  spec gains three new record-type rows and three body subsections.
- **Slice fsw-16: non-volatile (flash-backed) parameter storage.**
  Operator-tuned parameters now survive a reboot — the framework's
  first persistence capability, the most-deferred dependency in the
  codebase (`lib/datapool/`, `lib/schedule/`, `lib/pktstore/`,
  `lib/fdir/` and `lib/hkstore/` all carried explicit "until a
  non-volatile-storage subsystem exists" comments). A new freestanding
  C subtree `lib/nvstore/` (with its own C-friendly `.clang-tidy`)
  carries the on-flash image format: two sectors of the board's
  `storage_partition` are used as an **A/B ping-pong** pair, each
  holding one image (magic + `format_version` + monotonic `seq` +
  payload-length header, then a sequence of `{type, len, bytes}`
  records, then a CRC-16-CCITT-FALSE over header+payload, padded to
  the backend's write-block alignment). `save` erases the older sector
  and writes the new image with `seq + 1`; `load` validates both
  sectors and picks the valid copy with the higher `seq`. A power loss
  mid-`save` leaves the previous copy intact in the other sector — the
  standard power-safe NVM pattern, and it falls out of the
  `nucleo_h753zi` storage partition's 2-sector geometry for free. The
  flash I/O lives behind a seam (`migris_nv_backend_t`, modelled on the
  fsw-8 event-sink seam) — the Zephyr sample supplies a concrete
  backend over `flash_area_*` in `samples/tc_uart/src/nv_flash_backend.c`,
  and the host unit tests supply a RAM-backed backend, so the image
  format, A/B logic, CRC and `format_version` rejection are fully
  host-tested. `lib/datapool/` gains pure `migris_datapool_serialize`
  /`_deserialize` (reusing the existing big-endian value codec —
  `migris_dp_value_encode`/`_decode`) and a `generation` counter
  bumped on every successful `migris_datapool_set`; the `tc_uart`
  sample restores the datapool from the persisted image at boot and
  auto-saves whenever the generation advances (one coalesced flash
  write per batch of sets). The Renode closed-loop in
  `tests/renode/test_tc_uart_hk.py` is the proof: tune a parameter via
  PUS-20[3] → `machine Reset` the emulated MCU → PUS-20[1] reports the
  tuned value, not the Kconfig default. **No new PUS service, no wire
  change** — persistence rides on the existing PUS-20 set/report. The
  on-flash image format is pinned in [`docs/nv-image-format.md`](docs/nv-image-format.md).
  Scoped decisions:
  - **NVS rejected; raw `flash_area_*` used.** Zephyr's NVS subsystem
    encodes a sector size as `uint16_t`, which overflows the STM32H7's
    128 KB sector (Zephyr issues #36590 / #93295). The raw flash-map
    API handles 128 KB sectors natively and gives us the explicit
    control the A/B image format needs. *Trigger to revisit*: NVS
    fixes the `uint16_t` overflow upstream AND a mission's
    write-amplification budget requires per-record wear-levelling.
  - **A/B redundancy at sector granularity, not record granularity.**
    The whole image is rewritten on every save — flash thrash, in
    principle. In practice operator parameter sets are operationally
    infrequent (an operator tunes a parameter occasionally; STM32H7
    flash endurance is ~10k cycles), so a once-per-set 128 KB erase
    over the spacecraft's lifetime stays well inside the budget. A
    log-structured / journalled layout earns its complexity only when
    the write rate makes A/B unviable.
  - **Auto-save on a successful PUS-20[3] set, not an explicit commit
    command.** A separate "save to NVM" wire surface would have to
    live in a vendor PUS service, which the pinned platform rule
    places downstream (`cry4-fsw`), not in `fsw-core`. Auto-save needs
    no new wire surface, the trigger (PUS-20[3]) is already a
    deliberate operator action, and the closed-loop test stays clean
    (set → reset → report). *Trigger to revisit*: a mission needs
    decoupled RAM-copy / NVM-copy semantics — that adds the vendor
    service downstream rather than re-shaping fsw-core.
  - **One consumer (datapool) in this slice; schedule / hkstore /
    mode persistence deferred to small follow-on slices.** The
    `lib/nvstore/` infrastructure is the hard part; once it exists,
    each additional consumer is a `serialize` / `deserialize` pair +
    a new record type. Matching the fsw-4 = PUS infra + one service
    rhythm gets a working, reviewable increment landed before the
    less-load-bearing consumers pile on. *Trigger to revisit*: an
    operational gap (the schedule re-loads from scratch on every
    reboot is a meaningfully worse experience than the parameters
    re-loading — fsw-17 candidate).
  - **`pktstore` (mass memory) and `fdir` counters/latch deferred.**
    The packet store's access pattern (every TM is captured) and its
    typical content (re-derivable from the live stream) are different
    enough from a config store that bundling them is wrong. The FDIR
    confirmation latch surviving a reboot is also subtly wrong —
    arguably the reboot IS part of recovery, and a latched-across-
    reboot fault risks safing the spacecraft forever. Both warrant
    their own slice with explicit semantics.
  - **`format_version` rejection, unknown-record skip.** An on-flash
    image with a version we do not understand is rejected (load
    returns `NO_VALID_IMAGE` and the datapool falls back to defaults);
    an unknown *record type* inside a recognised image is silently
    ignored. So a layout-breaking change is fail-safe, while adding a
    new record type later is non-breaking — old firmware ignores it,
    new firmware emits both.
- **Slice fsw-15: PUS-3 housekeeping structure management.** Ground can
  now define its own housekeeping structures, not just poll the one
  hard-coded framework structure fsw-7 shipped. A *dynamic* structure
  is a Structure ID, a list of on-board datapool parameters it samples,
  a reporting interval and an enabled flag; the FSW emits a
  datapool-backed PUS-3[25] report for it whose source data is the
  Structure ID followed by each parameter's MIB-decoded value. This
  closes the structure-management deferral fsw-7 recorded "until a
  parameter datapool exists" — fsw-9 landed that datapool. A new
  freestanding C subtree `lib/hkstore/` (with its own C-friendly
  `.clang-tidy`) carries the bounded, RAM-only structure store, modelled
  on `lib/schedule/`: `migris_hkstore_create` / `_delete` /
  `_set_enabled` / `_find` / `_due`, the application supplying capacity
  and per-structure parameter count as compile-time constants. The
  PUS-3 codec gains four structure-management subtypes — [3,1] create,
  [3,2] delete, [3,5] enable, [3,6] disable — behind a new
  `migris_pus3_execute` entry point, and a `migris_pus3_build_dynamic_hk_report`
  encoder; two new error codes `MIGRIS_PUS3_ERR_UNKNOWN_PARAM` and
  `MIGRIS_PUS3_ERR_EXEC_FAILED`. The TC router gains a nullable
  `migris_hkstore_t*` in its context and routes service 3 by subtype: a
  [3,27] poll of SID `0x0001` still serves the frozen framework
  structure, a [3,27] poll of any other SID serves a dynamic one, and
  [3,1]/[3,2]/[3,5]/[3,6] reach `migris_pus3_execute`. The `tc_uart`
  sample wires the structure store, registers two read-only
  firmware-identity datapool parameters for a ground structure to
  sample, and drains one due structure per main-loop iteration. New
  `tests/hkstore_test.cpp`; `tests/pus3_test.cpp` and
  `tests/tc_router_test.cpp` gain the structure-management and
  dynamic-report suites; `tests/renode/test_tc_uart_hk.py` gains the
  create → enable → observe → disable → delete closed loop. The new
  subtypes and the dynamic report layout are added to
  [`docs/wire/pus-3.md`](docs/wire/pus-3.md) — **purely additive and
  non-breaking**: the frozen FRAMEWORK_DIAG report
  (`migris_pus3_build_hk_report`) is byte-for-byte unchanged. Scoped
  decisions:
  - **`lib/hkstore/` does not depend on `lib/datapool/`.** The store
    holds parameter IDs opaquely and never resolves them; a structure
    naming a parameter the datapool does not define is caught at
    *emission* time (the dynamic encoder fails the whole report,
    `MIGRIS_PUS3_ERR_UNKNOWN_PARAM`), not at create time. Keeping the
    two decoupled means a structure can be created independently of the
    parameters it references. *Trigger*: a mission that needs
    create-time parameter-ID validation gets it as an explicit option.
  - **Dynamic Structure IDs must be `0x0100` or above.** The
    `0x0001..0x00FF` block stays reserved for fsw-core framework
    structures; FRAMEWORK_DIAG (`0x0001`) remains a frozen, hard-coded
    layout in the PUS-3 codec, never an entry in the structure store —
    so it can never be deleted or shadowed. `migris_hkstore_create`
    rejects a framework-range SID.
  - **A created structure starts disabled.** Flight-safe, like the
    fsw-10 schedule: a freshly defined structure does not add to the
    downlink until ground enables it with a [3,5]. An interval of 0
    means poll-only (never emitted periodically).
  - **Structure-management failures are `FC_EXEC_FAILURE`, not
    `FC_UNKNOWN_SUBTYPE`.** The [3,27] poll keeps its fsw-7 contract
    (an unknown SID is `FC_UNKNOWN_SUBTYPE` — the SID space is the
    addressable unit there); a [3,1]/[3,2]/[3,5]/[3,6] is a *known*
    subtype whose *execution* failed, so it follows the
    PUS-11/15/20 routed-service model.
  - **No Kconfig demo gate.** Structure management is TC-driven and a
    created structure starts disabled, so the verification-stream build
    emits no unsolicited structure telemetry — unlike the fsw-12/13/14
    spontaneous boot demos, no `CONFIG_FSW_*_DEMO` is needed and
    `test_tc_uart.py` is unaffected.
  - **Deferred** (recorded here and in `docs/wire/pus-3.md`): the
    diagnostic-structure subtypes [3,3]/[3,4] and the diagnostic report
    [3,26] (a separate report stream — no driving use case yet);
    structure-definition reporting [3,9]/[3,10] (ground reading a
    structure's parameter list — no in-platform consumer until the
    MCS); super-commutated parameter groups [3,7]/[3,8]; in-place
    modification of a structure; non-volatile persistence of the
    structure store across reset (RAM-only, like every other framework
    store — *trigger*: the first non-volatile-storage slice).
- **Slice fsw-14: FDIR isolation and recovery.** Completes the
  fault-management story fsw-8 began — FDIR now *acts* on a fault, not
  only reports it. Each anomaly accumulates a saturating occurrence
  count; when it crosses an application-supplied **threshold** the
  fault is *confirmed* — a single transient never recovers (debounce) —
  and FDIR autonomously commands a transition to a safe mode through
  the fsw-13 mode manager and emits a high-severity PUS-5
  **`FDIR_RECOVERY`** event (event ID `0x0005`; aux = anomaly type,
  commanded safe-mode ID, and the occurrence count at confirmation).
  `lib/fdir/` gains the isolation/recovery state in
  `migris_fdir_ctx_t` (per-anomaly occurrence counters, thresholds, a
  confirmation latch, a nullable `migris_mode_manager_t*` and the
  safe-mode ID), `migris_fdir_arm_recovery` (the application supplies
  the per-anomaly thresholds, like the datapool parameter set or the
  mode set), and `migris_fdir_set_enabled` / `migris_fdir_is_enabled`
  (suppress recovery for commissioning). Confirmation hooks **both**
  anomaly entry points — the typed `migris_fdir_report_anomaly` and
  the generic event sink — so a `TC_REJECTED` counts toward
  confirmation whether the RX-overflow detector or the TC router
  produced it. The `tc_uart` sample, under a new
  `CONFIG_FSW_FDIR_RECOVERY_DEMO` Kconfig, arms recovery to SAFE after
  `CONFIG_FSW_FDIR_TC_REJECTED_THRESHOLD` rejected telecommands; the
  Renode housekeeping build turns it on for a genuine closed-loop —
  send malformed TCs, watch the spacecraft safe itself. New
  isolation/recovery suite in `tests/fdir_test.cpp`;
  `tests/renode/test_tc_uart_hk.py` gains the recovery round-trip. The
  event ID is added to [`docs/wire/pus-5.md`](docs/wire/pus-5.md) — a
  non-breaking addition within the reserved framework block. Scoped
  decisions:
  - **FDIR depends on the mode manager directly — no recovery-action
    seam.** A confirmed fault commanding a safe mode *is* FDIR's job;
    the mode manager is a generic primitive FDIR consumes, not a
    policy needing decoupling (contrast the event-sink seam, which
    keeps the *generic* TC router free of fault policy). A
    recovery-action vtable would be a one-implementation abstraction —
    speculative. *Trigger*: a second recovery action (a subsystem
    power-cycle, a reconfiguration) earns the seam.
  - **Plain occurrence count, not a sliding time window.** A lifetime
    count crossing a threshold is the minimal correct debounce and a
    genuine fault signal. A time-windowed count is a tightening for a
    mission whose transient-anomaly rate would false-trigger a
    lifetime count. *Trigger*: such a mission profile.
  - **Per-anomaly threshold, application-supplied.** The anomaly
    registry owns *classification* (severity, event ID); *policy* (the
    threshold) is mission tuning, supplied at
    `migris_fdir_arm_recovery`, never hard-coded in framework code. A
    threshold of 0 means the anomaly never confirms (detection-only —
    the fsw-8 behaviour).
  - **Enable/disable is a C API flag, not a datapool parameter.**
    `lib/fdir/` must not depend on `lib/datapool/`; whether to expose
    FDIR-enable to ground as a PUS-20 parameter is a sample/mission
    choice, wired sample-side with no `lib/fdir/` change.
  - **Recovery latches once per anomaly per boot.** A confirmed fault
    that already safed the vehicle does not re-command SAFE on every
    further occurrence; detection (the PUS-5 anomaly report)
    continues. *Trigger*: a mission needing re-armable recovery —
    which needs persistence first.
  - **Persistence across reset deferred.** The occurrence counters and
    the confirmation latch are RAM-only and reset on reboot, like
    every other framework store. *Trigger*: the first
    non-volatile-storage slice.
- **Slice fsw-13: operating-mode manager.** The framework's first
  operating-state abstraction — a generic mode / state-machine
  primitive. A spacecraft runs in one of a small set of operating
  modes (boot, safe, nominal, payload-active) with only certain
  transitions between them legal; the mode manager holds the current
  mode, checks a transition request against a table of allowed
  transitions, and announces a successful change. A new freestanding C
  subtree `lib/mode/` (with its own C-friendly `.clang-tidy`) carries
  it. Like the datapool and the schedule it is **mission-agnostic** —
  fsw-core hard-codes no modes; the application supplies the mode set
  and the allowed transitions at init. A mode is a numbered 1-byte ID
  (names live in the ground MIB); each declared mode carries a bitmask
  of the modes it may transition to, an O(1) "is this transition
  allowed" test. A successful transition is announced through the
  fsw-8 event-sink seam as a new framework PUS-5 event
  **`MODE_CHANGED`** (event ID `0x0004`, severity info, aux = previous
  then new mode ID) — no new wire packet type, no codec change. The
  `tc_uart` sample, under a new `CONFIG_FSW_MODE_DEMO` Kconfig, defines
  a three-mode set (boot / nominal / safe) and performs one
  BOOT → NOMINAL transition at boot; the Renode housekeeping build
  turns the demo on so the closed-loop has an observable transition.
  New host suite `tests/mode_test.cpp`;
  `tests/renode/test_tc_uart_hk.py` gains a `MODE_CHANGED` round-trip.
  The event ID is added to [`docs/wire/pus-5.md`](docs/wire/pus-5.md)
  — a non-breaking addition within the reserved framework block.
  Scoped decisions:
  - **No mode-commanding PUS service.** A ground mode-change
    telecommand is a vendor PUS-128-range service; per the pinned
    "PUS-128+ vendor assignments live in cry4 / cry4-fsw, not
    fsw-core" decision it belongs downstream. fsw-13 ships the generic
    primitive and its C API only — no PUS service, no TC-router change
    (exactly fsw-12's telemetry-only PUS-13 decision). *Trigger*:
    cry4-fsw defining its mode-commanding service, which will hold a
    `migris_mode_manager_t*` and map `MIGRIS_MODE_ERR_FORBIDDEN` to a
    PUS-1[8] completion failure.
  - **No FDIR-driven autonomous transition.** fsw-8 deferred FDIR
    Isolation/Recovery — "recovery actions, mode transitions …
    presuppose a mode manager" — and named this slice as the trigger.
    fsw-13 supplies the mechanism (`migris_mode_request`) FDIR will
    call; wiring FDIR to autonomously demand a safe mode on a
    confirmed fault needs occurrence counters and debounce that do not
    exist yet. *Trigger*: the first FDIR slice adding occurrence
    thresholds and a defined recovery action.
  - **A rejected transition emits no event.** A forbidden transition
    returns `MIGRIS_MODE_ERR_FORBIDDEN` only; no anomaly is raised —
    the sole caller this slice (the sample demo) controls its own
    requests, and a rejected ground-commanded transition is properly
    the downstream service's concern. *Trigger*: the first caller that
    can request a forbidden transition from an unverified path.
  - **The current mode is not in housekeeping.** The PUS-3[25]
    housekeeping source data is a frozen 27-byte block; adding a
    current-mode field is a breaking wire change. The `MODE_CHANGED`
    event already gives ground every transition. *Trigger*: a PUS-3
    structure able to carry the mode, or a mission requirement for a
    polled current-mode field.
  - **No mode-gated behaviour.** fsw-13 is a pure state-machine
    primitive — the sample does not make schedule release or
    housekeeping cadence mode-dependent. A consumer that reacts to the
    mode is the future FDIR recovery path or a mission payload.
    *Trigger*: the first subsystem with a defined mode-dependent
    behaviour.
  - **Mode set bounded at 32 IDs.** The allowed-target bitmask is a
    `uint32_t`, so a mode ID must be below 32; the mode-set capacity
    is the separate compile-time `MIGRIS_MODE_CAPACITY`. *Trigger*: a
    mission needing more than 32 distinct modes.
- **Slice fsw-12: PUS-13 large data transfer (downlink).** The
  framework's first mechanism for downlinking a unit of data larger
  than a single CCSDS Space Packet — a schedule detail report, a
  window of stored telemetry, a payload product. A new stateless C
  codec `lib/pus/pus13.{h,c}` builds one [13,1] first / [13,2]
  intermediate / [13,3] last downlink **part** packet per call; a new
  freestanding C subtree `lib/largedata/` (with its own C-friendly
  `.clang-tidy`) carries the **large-data downlink session** that
  drives it — a cursor over a borrowed, caller-owned data unit that
  slices the unit into `MIGRIS_PUS13_PART_SIZE`-byte chunks (64 by
  default) and emits one part per call, so the application's main loop
  drips a transfer out one packet per iteration, the same shape the
  PUS-15 retrieval drain uses. Every part carries a 6-byte **part
  header** — transaction identifier, 0-based part number, total part
  count — which is the authoritative reassembly key. The `tc_uart`
  sample, under a new `CONFIG_FSW_LARGEDATA_DEMO` Kconfig, starts one
  transfer of a synthetic byte-ramp unit at boot and drains one part
  per iteration through `transmit_tm` (parts are live TM, tapped into
  the packet store like any other telemetry); the Renode housekeeping
  build turns the demo on so the reassembly closed-loop has an
  observable transfer. New host suites `tests/pus13_test.cpp` and
  `tests/largedata_test.cpp`; `tests/tc_router_test.cpp` extended;
  `tests/renode/test_tc_uart_hk.py` gains a part-reassembly
  closed-loop. Wire format pinned in
  [`docs/wire/pus-13.md`](docs/wire/pus-13.md). Scoped decisions:
  - **Pragmatic downlink subset.** Only the downlink direction, and
    within it only [13,1] / [13,2] / [13,3]. The uplink direction is
    excluded — no on-board feature ingests a data unit larger than one
    Space Packet (the largest inbound TC is a 192-byte PUS-11[4]
    insert); the [13,16] downlink-abort report is excluded — nothing
    in this slice can interrupt a transfer in progress; concurrent
    transactions are excluded — one session at a time. *Triggers to
    revisit*, respectively: the first consumer of a large uplinked
    unit; the first slice where a transfer can be interrupted (a
    closing pass window); a second concurrent producer of large data.
  - **Telemetry-only — the TC router is untouched.** PUS-13 has no
    inbound subtype, so service 13 is added neither to the accept-stage
    whitelist nor to the dispatch table — a service-13 TC is rejected
    `UNKNOWN_SERVICE` (a new `tc_router_test.cpp` test pins this).
    Wiring a `largedata` pointer into the router context would be
    speculative — nothing in the router consumes it — so the PUS-13
    message counters live inside the session itself, exactly as the
    `tc_uart` sample already keeps a local PUS-3 context for its
    spontaneous periodic reports.
  - **CCSDS sequence flags stay UNSEGMENTED.** PUS-13 segmentation is
    a service-layer concern carried by the part header; each part is a
    complete, standalone Space Packet. The framework's single-APID
    interleaving (housekeeping, events, verification and parts share
    one TM sequence-count space) means CCSDS-level segment reassembly
    would be incorrect — reassembly is by the part header alone.
  - **PUS-15 retrieval stays a verbatim replay.** fsw-11's [15,9]
    by-time retrieval still re-emits stored packets verbatim; rewiring
    it to chunk a window through a PUS-13 session is a breaking change
    to `docs/wire/pus-15.md` and is kept a separate, deliberate slice.
    fsw-12 ships PUS-13 as an independent primitive. *Trigger*: a
    retrieval that must be downlinked as one reassemblable unit rather
    than a packet replay.
  - **The demo is Kconfig-gated.** PUS-13 has no inbound telecommand,
    so a closed-loop test cannot trigger a transfer the way it triggers
    PUS-11 or PUS-15; the sample runs one demo transfer at boot behind
    `CONFIG_FSW_LARGEDATA_DEMO` (default off). The verification-stream
    ELF leaves it off — `test_tc_uart.py` reads a fixed byte count per
    stimulus and must not see unsolicited telemetry; the housekeeping
    ELF turns it on. `test_tc_uart_hk.py`'s first-housekeeping-report
    test accordingly drops its exact-sequence-count assertion (the
    boot-time demo now precedes the first report) — an intended slice
    consequence, like the fsw-6 boot-event rebase, not a regression.
- **Slice fsw-11: on-board packet store + PUS-15 storage and
  retrieval.** The framework's first mass-memory primitive — the
  produce-on-orbit / downlink-next-pass model a spacecraft needs
  because it makes telemetry continuously but has a ground contact
  only during a pass. A new freestanding C subtree `lib/pktstore/`
  (with its own C-friendly `.clang-tidy`) holds a bounded, static,
  **circular** packet store: every telemetry packet kept verbatim with
  the FSW-clock second it was stored, oldest first, and when full a new
  packet overwrites the **oldest** so the most recent telemetry is
  always retained. **PUS-15** (`lib/pus/pus15.{h,c}`) is the
  ground-facing face: a pragmatic core subset over one predefined
  packet store — enable [15,1] / disable [15,2] storage, start a
  by-time-window retrieval [15,9], delete content up to a time [15,11],
  and the store report [15,12] → [15,13]. PUS-15 is wired into the
  router as the **fifth** routable service (accept gate + the fsw-9
  dispatch table; a `migris_pus15_ctx_t` and a nullable
  `migris_pktstore_t* store` in the router context, NULL = no store = a
  routed PUS-15 TC fails its completion stage). The `tc_uart` sample
  taps **every** live TM packet it emits into the store (a new
  `transmit_tm` helper, splitting each burst into CCSDS packets) and,
  each main-loop iteration, drains at most one packet of an armed
  retrieval — re-emitting it verbatim. New host suites
  `tests/pktstore_test.cpp` and `tests/pus15_test.cpp`;
  `tests/tc_router_test.cpp` extended; `tests/renode/test_tc_uart_hk.py`
  gains a store-report round-trip, a downlink-replays-stored-TM
  closed-loop, and a disable/re-enable state round-trip. Wire format
  pinned in [`docs/wire/pus-15.md`](docs/wire/pus-15.md). Scoped
  decisions:
  - **RAM-backed and volatile — *not* non-volatile.** This is the
    "on-board storage slice" the fsw-9 and fsw-10 entries named as
    where parameter / schedule persistence "arrives", but PUS-15
    storage is a between-passes **RAM** buffer: the store is empty
    after every reboot. Non-volatile mass memory that survives a reset
    is a separate future capability — it needs a flash storage
    subsystem — and stays deferred. The `datapool.h` and `schedule.h`
    header comments that pointed at PUS-15 for persistence are
    corrected to point at that future non-volatile-storage capability.
    *Trigger to revisit*: the first slice that needs telemetry,
    parameters, or a schedule to survive a power cycle.
  - **One predefined packet store.** Dynamic packet-store creation /
    deletion / configuration, storage-selection management (which
    packets a store captures), and the packet-store catalogue are
    **deferred** — one predefined store covers the model. *Trigger to
    revisit*: a second concurrent store with distinct retention.
  - **A downlink arms; the main loop drains.** [15,9] does not itself
    emit telemetry — it arms an at-most-one by-time retrieval the
    application drains one packet per iteration, re-emitting each
    stored packet with its original headers (a replay of history). No
    separate execution engine — the same "re-dispatched, not specially
    executed" shape fsw-10 used for the schedule.
  - **The store freezes during a retrieval.** While a retrieval is in
    progress, storage and [15,11] delete are suspended / rejected so
    the buffer cannot shift under the retrieval cursor.
  - **Summary report, not content.** [15,13] is a fixed 11-byte
    store summary (enabled, count, oldest / newest time). Chunked
    bulk downlink of the stored packets themselves is "large data"
    and pairs with **PUS-13** (the next slice).
- **Slice fsw-10: on-board schedule + PUS-11 time-based scheduling.**
  The framework's first scheduling primitive — time-tagged
  telecommands that execute autonomously, with no continuous ground
  link. A new freestanding C subtree `lib/schedule/` (with its own
  C-friendly `.clang-tidy`) holds a bounded, static store of
  activities — each an absolute CUC release time plus a telecommand
  kept verbatim. **PUS-11** (`lib/pus/pus11.{h,c}`) is the
  ground-facing face: a pragmatic core subset — enable [11,1] /
  disable [11,2] the schedule, reset [11,3], insert [11,4], delete by
  request identifier [11,5], and the summary report
  [11,11] → [11,12]. A scheduled activity is identified by its 4-byte
  **request identifier** — the first four bytes of its telecommand,
  reusing the existing PUS-1 request-ID concept. Insert and delete are
  **all-or-nothing**; the summary report is a query (an unscheduled
  identifier is omitted, not an error). The `tc_uart` sample carries
  the schedule and, each main-loop iteration, releases the one due
  activity with the earliest release time — re-dispatching its stored
  TC through the router exactly as if it had just arrived. PUS-11 is
  wired into the router as the **fourth** routable service (accept
  gate + the fsw-9 dispatch table; a `migris_pus11_ctx_t` and a
  nullable `migris_schedule_t* schedule` in the router context,
  NULL = no schedule = a routed PUS-11 TC fails its completion stage).
  New host suites `tests/schedule_test.cpp` and `tests/pus11_test.cpp`;
  `tests/tc_router_test.cpp` extended; `tests/renode/test_tc_uart_hk.py`
  gains a scheduled-release closed-loop and a summary-report
  round-trip. Wire format pinned in
  [`docs/wire/pus-11.md`](docs/wire/pus-11.md). Scoped decisions:
  - **Summary report, not detail.** The ECSS detail report
    (11,9/11,10) echoes each activity's telecommand verbatim — an
    unbounded packet. The summary report (11,11/11,12 — release time +
    request identifier per activity) is bounded and fits the fixed
    router buffer. The detail report is "large data" and pairs with
    PUS-13 (the next slice); it, time-shift (11,7/8/15),
    sub-schedules, groups, and filter-based selection are deferred.
  - **The schedule starts disabled.** A freshly booted FSW does not
    autonomously fire a stale schedule until ground sends [11,1] —
    flight-safe by default.
  - **RAM-only / volatile.** The schedule is empty after every reboot;
    non-volatile persistence is deferred to the on-board storage slice
    (PUS-15). *Trigger to revisit*: PUS-15.
  - **A scheduled activity is re-dispatched, not specially executed.**
    At release the stored TC goes through the normal
    `migris_tc_router_dispatch` path — full accept-stage validation
    and its own PUS-1 verification — so the release path needs no
    separate execution engine, and a released TC that is itself
    malformed is rejected exactly as a fresh one would be.
- **Slice fsw-9: parameter datapool + PUS-20 on-board parameter
  management.** The framework's first on-board parameter store and the
  service that reports and sets it. A new freestanding C subtree
  `lib/datapool/` (with its own C-friendly `.clang-tidy`) carries a
  typed, fixed-capacity parameter store and the framework's first
  tagged-union **variant type** `migris_dp_value_t` — the unsigned and
  signed 8/16/32-bit integers and 32-bit IEEE-754 float — plus a
  big-endian value codec. **PUS-20** (`lib/pus/pus20.{h,c}`) is the
  ground-facing face: the complete three-message standard service —
  TC[20,1] report parameter values, TM[20,2] parameter value report,
  TC[20,3] set parameter values. A [20,3] set is **all-or-nothing**:
  every (ID, value) pair is decoded and validated (defined,
  read-write, value bytes present) before any datapool write, so a
  failure leaves the pool untouched and the router maps it to a
  PUS-1[8] completion failure. PUS-20 is wired into the TC router as
  the third routable service — added to the accept gate, with a
  `migris_pus20_ctx_t` and a nullable `migris_datapool_t* datapool` in
  the router context (held by pointer like `sink`; NULL — the
  zero-init default — means no datapool, so a routed PUS-20 TC fails
  its completion stage and prior callers are unaffected). The
  `tc_uart` sample instantiates a datapool holding the PUS-3
  housekeeping period as a read-write parameter (framework ID
  `0x0001`, seeded from Kconfig) and reads it live every main-loop
  iteration — so a PUS-20[3] set reconfigures the housekeeping cadence
  with no rebuild, and a period of 0 disables it. New host suites
  `tests/datapool_test.cpp` and `tests/pus20_test.cpp`;
  `tests/tc_router_test.cpp` extended (the PUS-20 dispatch, the
  null-datapool and unknown-ID completion failures, the worst-case
  burst proving the buffer bump); `tests/renode/test_tc_uart.py` gains
  the report round-trip and the unknown-ID failure, and
  `test_tc_uart_hk.py` the live cadence retune. Wire format pinned in
  [`docs/wire/pus-20.md`](docs/wire/pus-20.md). Scoped decisions:
  - **Dispatch table, earned now.** PUS-20 is the third routable
    inbound service (after PUS-17 and PUS-3), so the trigger fsw-7
    pinned — "a function-pointer dispatch table is the next
    abstraction, earned at a third independent service" — fires: the
    router's service-type `switch` becomes a `{service_type →
    handler}` table of uniform-signature handlers. The FDIR anomaly
    registry's own `switch` is a *different* dispatch and stays a
    switch — its trigger (a third anomaly) has not fired.
  - **RAM-only / volatile.** Parameters reset to their initial values
    on every reboot; non-volatile persistence across reset is
    **deferred** to the on-board storage slice (PUS-15). *Trigger to
    revisit*: the first slice needing a parameter to survive a reset.
  - **Numbered parameters only.** A 2-byte ID is the addressable unit;
    human-readable names live in the ground MIB, never on-board. The
    ID range `0x0001`–`0x00FF` is reserved for fsw-core framework
    parameters, `0x0100`+ for mission FSW (`cry4-fsw`) — mirroring the
    PUS-3 SID and PUS-5 event-ID block splits. fsw-core hard-codes no
    parameters; the application supplies the set at init.
  - **Standard scalar type set.** Unsigned and signed 8/16/32-bit
    integers and `f32`; no arrays, strings, 64-bit or double-precision
    types. The `migris_dp_type_t` enum is **append-only** — a new type
    takes the next free code and never renumbers an existing one — so
    widening the set later is a non-breaking wire change.
  - **Out of scope:** full PUS-3 ↔ datapool integration (housekeeping
    structures selecting datapool parameter lists — the
    structure-management subtypes PUS-3 deferred in fsw-7); PUS-20
    parameter-definition reporting and vendor-extension subtypes;
    non-volatile persistence.
- **Slice fsw-8: FDIR primitives + router-side anomaly reporting +
  the bounded event FIFO.** The framework's first fault-detection and
  -reporting layer. A new freestanding C bounded **event FIFO**
  (`lib/fdir/event_fifo.{h,c}` — drop-newest, single-context,
  non-atomic) and **FDIR** module (`lib/fdir/fdir.{h,c}` — anomaly
  registry, `report_anomaly`, the event-sink factory, and a
  `drain`→PUS-5 encoder) sit behind a generic two-field **event-sink
  vtable** (`lib/pus/include/migris/fsw/event_sink.h`) so the generic
  TC router reports a rejected TC without depending on FDIR. The
  router now reports every acceptance-stage rejection (length / CRC /
  PUS-version / unknown-service) as a spontaneous **PUS-5[2]
  `TC_REJECTED`** (event ID `0x0002`, aux = failure code + service
  type + subtype) — *independent of the TC's ack flags*, so a no-ack
  rejected TC stays PUS-1-silent yet still raises the anomaly. The
  `tc_uart` sample also turns a UART RX-ring overflow into a
  **PUS-5[3] `RX_OVERFLOW`** (event ID `0x0003`, aux = u32 bytes-
  dropped delta): the ISR is unchanged (it only bumps its `volatile`
  counter); the main loop observes the delta and is the sole FIFO
  producer, so the FIFO stays single-context and non-atomic. The loop
  drains one event per iteration into a spontaneous PUS-5 report,
  *after* a TC's PUS-1 verification, keeping the shared per-APID CCSDS
  sequence strictly monotonic across boot, verification/service
  bursts, periodic housekeeping and anomalies. The PUS-5 context is
  hoisted into `migris_tc_router_ctx_t` (alongside pus1/17/3), wired
  via a nullable `sink` field (NULL = zero-init default = no FDIR, so
  every prior caller and test is unaffected). New host suites
  `tests/event_fifo_test.cpp` and `tests/fdir_test.cpp`;
  `tests/tc_router_test.cpp` extended (mock sink: rejection→one LOW
  anomaly with correct aux, accepted→none, the no-ack-still-raises
  invariant, partially-init-sink safety, the [27]-poll de-zero
  guard); `tests/renode/test_tc_uart.py` gains the ack and no-ack
  rejection→anomaly round-trips and `test_tc_uart_hk.py` the polled
  de-zero proof. Wire format pinned in
  [`docs/wire/pus-5.md`](docs/wire/pus-5.md) (event IDs + the
  spontaneous/ungated rationale), cross-referenced from
  [`docs/wire/pus-1.md`](docs/wire/pus-1.md). Scoped decisions:
  - **Event-sink seam, earned now.** A 2-field vtable is the minimum
    that severs `lib/pus` → FDIR; it is not a speculative
    generalisation (cf. the fsw-7 "switch, not a registration table"
    rule) — it is earned by two independent producers this slice (the
    router and the RX-overflow detector) and is the exact trigger
    `pus5.h` named for the FIFO abstraction.
  - **Detection + reporting only.** Isolation/Recovery — occurrence
    counters with thresholds, debounce/confirmation, recovery actions,
    mode transitions, FDIR enable/disable (PUS-5 control subtypes
    5/6), persistence across reset — is **deferred**: it presupposes
    a mode manager and a recoverable-subsystem consumer. *Trigger to
    revisit*: the first slice introducing a mode manager or a
    subsystem with a defined recovery action.
  - **FIFO overflow is drop-newest** (the causal head — the first
    faults — is preserved); the internal `dropped` count is a C-API
    health field, **not on the wire** this slice (the PUS-3[25] block
    is frozen; the per-severity PUS-5 message counter already lets
    ground detect a gap). *Trigger*: a consumer that needs the drop
    count downlinked.
  - **Acceptance-stage rejections only.** Exec-stage completion
    failures already get PUS-1[8]; a distinct anomaly class for them
    is deferred until a driving need appears.
- **Slice fsw-7: PUS-3 housekeeping & diagnostic telemetry.** New
  freestanding C housekeeping-report encoder (`lib/pus/pus3.{h,c}`),
  used by **both** a spontaneous periodic emitter and a TC[3,27]
  one-shot poll, for one predefined framework structure
  (`SID 0x0001 FRAMEWORK_DIAG`) carrying a frozen 27-byte parameter
  block: uptime, the shared TM sequence count it consumed, the
  per-service PUS message counters, and three new TC-router counters —
  **TC accepted**, **TC rejected**, and **UART RX-ring overflow drops**
  (previously silently dropped in the RX ISR, now counted). The TC
  router is generalised from a hardcoded-PUS-17-only accept+dispatch to
  a service-type `switch` (PUS-17 + PUS-3); the `tc_uart` sample emits
  the periodic report from its main loop on a coarse FSW-clock
  elapsed-time check (Kconfig `CONFIG_FSW_PUS3_HK_PERIOD_SEC`),
  threading the router's shared per-APID sequence count so it stays
  strictly monotonic across the boot event, every verification /
  service burst, and each periodic report. New host suite
  `tests/pus3_test.cpp`; `tests/tc_router_test.cpp` extended for the
  generalised accept and TC[3,27] routing/counters; a new
  short-period Renode build drives `tests/renode/test_tc_uart_hk.py`
  (periodic appearance, FSW-clock cadence, one-shot-poll round-trip,
  RX-drop wiring). Wire format pinned in
  [`docs/wire/pus-3.md`](docs/wire/pus-3.md). Scoped decisions:
  - Structure-management subtypes (`[1]/[2]/[3]/[4]` create/delete) and
    periodic-generation control (`[5]/[6]` enable/disable) are
    **deferred**: all presuppose a parameter datapool the framework
    does not have yet. Only one predefined structure; dynamic
    creation lands with the datapool.
  - Structure IDs `0x0001`–`0x00FF` are reserved for fsw-core
    *framework* structures; `0x0100`+ is mission-owned (scheme pinned
    when `cry4-fsw` bootstraps), mirroring the PUS-5 event-ID split and
    the pinned "PUS-128+ vendor assignments live downstream" decision.
  - **`switch`, not a registration table.** A service-type switch over
    two services is correct and minimal; a function-pointer dispatch
    table is the *next* abstraction, earned at a third independent
    service.
  - **PUS-5 counters are zero on the [27]-polled path.** The router
    does not own the PUS-5 context; hoisting it in is the deferred
    "FDIR raises events from inside the router" abstraction. The
    spontaneous report (emitted by the context owner) carries live
    values. Asymmetry pinned in `docs/wire/pus-3.md`.
  - **No event/FIFO for the RX-drop counter.** A single-writer (ISR) /
    single-reader (loop) `volatile uint32_t` snapshot is the minimal
    correct mechanism; the bounded event FIFO stays deferred (same
    rationale as fsw-6).
- **Slice fsw-6: PUS-5 event reporting.** New freestanding C
  event-report encoder (`lib/pus/pus5.{h,c}`) for the four severity
  subtypes — informative [1], low [2], medium [3], high [4] anomaly —
  carrying a 2-byte big-endian event-definition ID plus optional
  auxiliary data (≤ 32 bytes). This is the framework's first
  *asynchronous* TM service: a report is emitted spontaneously at the
  point a condition is detected, not as a side effect of an inbound
  TC. The `tc_uart` sample emits one PUS-5[1] `FSW_BOOT` informative
  event on reset — the first TM it produces — threading the router's
  shared per-APID CCSDS sequence count so the boot event consumes
  count 0 and the per-APID sequence stays strictly monotonic across it
  and every subsequent verification / service packet. New host suite
  `tests/pus5_test.cpp`; `tests/renode/test_tc_uart.py` asserts the
  boot event end-to-end and its four pre-existing tests are rebased
  past the leading boot packet (+1 shared sequence count) — an
  intended consequence of the sample now emitting boot TM, not a
  regression. Wire format pinned in
  [`docs/wire/pus-5.md`](docs/wire/pus-5.md). Scoped decisions:
  - The control subtypes [5]/[6] (enable/disable event generation),
    [7] and [8] are **deliberately excluded** — TC-driven event
    reconfiguration overlaps PUS-20 (onboard parameter management,
    P1) and has no driving use case yet.
  - Event-definition IDs `0x0001`–`0x00FF` are reserved for fsw-core
    *framework* events; `0x0100`+ is mission-owned (scheme pinned
    when `cry4-fsw` bootstraps), mirroring the pinned "PUS-128+
    vendor assignments live downstream" decision.
  - **No event queue.** PUS-5 stays a pure stateless encoder (the
    proven pus1/pus17 shape). A freestanding bounded event FIFO is
    the explicit *next* abstraction, earned when a producer that does
    not own a TM output buffer first exists (an FDIR monitor, PUS-3
    housekeeping, or the ISR-context UART RX-ring overflow event).
  - **Router-side anomaly emission on TC rejection is deferred** to
    the FDIR slice (its first real consumer); the TC router is
    untouched, so all fsw-5 host suites stay green unchanged and the
    pinned `docs/wire/pus-1.md` rule-3 "no-ack ⇒ silence" invariant
    is preserved.
- **Slice fsw-5: PUS-1 TC verification (acceptance + completion).**
  New freestanding C TC reception / acceptance / routing layer
  (`lib/pus/tc_router.{h,c}`) — the framework's first on-board
  dispatcher — plus a PUS-1 verification-report encoder
  (`lib/pus/pus1.{h,c}`). A received TC is validated (CCSDS primary,
  length, CRC, PUS-C version, routable service) and, gated by its
  ack-flag bits, the FSW emits PUS-1[1]/[2] acceptance and
  PUS-1[7]/[8] completion reports around the routed service response,
  all back-to-back on the existing UART. Reports carry the verified
  TC's 4-byte request ID (and a 1-byte failure code on failures).
  PUS-1 start ([3]/[4]) and progress ([5]/[6]) are deferred until a
  long-running command exists to exercise them (workspace
  `CLAUDE.md`). New host unit suites `tests/pus1_test.cpp` and
  `tests/tc_router_test.cpp`; `tests/renode/test_tc_uart.py` drives
  the full PUS-1 + PUS-17 round-trip on the emulated `nucleo_h753zi`.
  Wire format pinned in [`docs/wire/pus-1.md`](docs/wire/pus-1.md).

### Changed (breaking)
- **PUS-3[25] PUS-5 counter sub-block on the TC[3,27]-polled path
  (fsw-8).** Wire bytes 28..31 changed *value semantic* on a *polled*
  housekeeping report from a pinned constant `00 00 00 00` to the live
  PUS-5 message counters (identical to the spontaneous report). The
  byte layout is unchanged. By the versioning rule in
  [`docs/wire/pus-3.md`](docs/wire/pus-3.md) a value-semantic change
  to an emitted field is breaking; it is recorded here as such. It is
  the *planned, pre-blessed* resolution of the asymmetry that document
  deliberately pinned in fsw-7 ("it disappears when the router gains a
  PUS-5 producer"), not accidental drift. The project is pre-1.0
  (SemVer 0.y.z) so no major bump is required, but any ground decoder
  that special-cased "polled HK ⇒ bytes 28..31 are zero" must update.

### Changed
- **`tc_uart` sample `TC_BUF_SIZE` raised 96 → 192** (fsw-10). The
  largest baseline TC is now a PUS-11[4] insert carrying scheduled
  telecommands. Sample only — no wire or library-API impact;
  `MIGRIS_TC_ROUTER_MAX_TM` is unchanged (the PUS-11[12] summary
  report's worst case fits the existing 128-byte buffer).
- **`MIGRIS_TC_ROUTER_MAX_TM` raised 96 → 128** (fsw-9). The
  worst-case single-TC burst is now `PUS-1[1] (22) + PUS-20[2] (67,
  the maximum eight parameters) + PUS-1[7] (22) = 111`. This is a
  C-API / caller-buffer-size change only — **the bytes on the wire are
  unchanged**. The `tc_uart` sample picks it up automatically
  (`out[MIGRIS_TC_ROUTER_MAX_TM]`).
- **`tc_uart` sample buffers grew for PUS-20** (fsw-9). `TC_BUF_SIZE`
  64 → 96 (the largest baseline TC is now a PUS-20[3] set of eight
  parameters, 62 bytes) and `CONFIG_MAIN_STACK_SIZE` 1024 → 2048 (the
  datapool `main()`-local plus the PUS-20 dispatch call chain). Sample
  only — no wire or library-API impact.
- **`test_tc_uart.py` corrupted-TC expectation updated (fsw-8).** A
  rejected TC now additionally yields a trailing PUS-5[2]
  `TC_REJECTED` anomaly (and the no-ack case yields one with no
  PUS-1). The renamed test asserts the new packet set; this is an
  intended slice consequence — the closure of the fsw-6 "router-side
  anomaly emission deferred" item — exactly analogous to the fsw-6
  boot-event rebase, not a regression.
- **`MIGRIS_TC_ROUTER_MAX_TM` raised 64 → 96** (fsw-7). The worst-case
  single-TC burst is now `PUS-1[1] (22) + PUS-3[25] (47) + PUS-1[7]
  (22) = 91`. This is a C-API / caller-buffer-size change only — **the
  bytes on the wire are unchanged**. The `tc_uart` sample picks it up
  automatically (`out[MIGRIS_TC_ROUTER_MAX_TM]`).
- **Renode tc_uart is now built twice** (fsw-7). Renode fast-forwards
  idle virtual time, so a fixed short housekeeping period would race
  `test_tc_uart.py`'s fixed-offset reads non-deterministically. The
  verification-stream ELF pins the period to "never within a test"
  (`test_tc_uart.py` is unchanged and still deterministic); a dedicated
  short-period ELF drives the new `test_tc_uart_hk.py`. The
  `zephyr-build` / `renode-smoke` matrices and `conftest.py` gain the
  `tc-hk` entry / `tc_hk_running` fixture / `FSW_CORE_TC_HK_ELF` env
  var.
- **TM sequence count is now shared per-APID.** It moved out of the
  per-service context into the new TC router context, so PUS-1 and
  PUS-17 packets emitted for one TC share a single, strictly
  monotonic CCSDS sequence space (CCSDS 133.0-B-2: one count per APID
  per direction). `migris_pus17_handle_are_you_alive` is replaced by
  `migris_pus17_execute` (generic TC validation moved to the router).
  **The bytes on the wire are unchanged** — this is a C-API reshape,
  not a wire-breaking change.
- **`samples/pus17_uart` renamed to `samples/tc_uart`** (it is now a
  generic TC-reception + verification demonstrator, not PUS-17-only).
  The `zephyr-build` / `renode-smoke` CI matrix entries, the Renode
  `.resc` script, the `tc_running` fixture, and the
  `FSW_CORE_TC_ELF` override env var are renamed to match.

- **Slice fsw-4: PUS-17 connection test over UART.** New freestanding
  C codec under `lib/pus/` (CCSDS Space Packet primary header
  pack/unpack, CRC-16-CCITT-FALSE, PUS-C TC/TM secondary headers,
  PUS-17 service handler), compiled into both `migris::fsw-core`
  (host) and the new `samples/pus17_uart` Zephyr application
  (Cortex-M7) so the wire-format bytes are covered by ASan / UBSan /
  clang-tidy on every PR. New `tests/renode/test_pus17_uart.py`
  drives a round-trip on the emulated `nucleo_h753zi`: ground sends
  a PUS-17[1] TC on USART3, FSW replies with a PUS-17[2] TM. New
  CI job `renode-smoke · pus17` parallels the existing
  `renode-smoke · hello`. Wire format is authoritatively pinned in
  [`docs/wire/pus-17.md`](docs/wire/pus-17.md).
- **Slice fsw-3: Renode-driven UART smoke test on `nucleo_h753zi`.**
  New `tests/renode/` pytest suite boots the fsw-2 hello-world ELF on
  Renode 1.16.1 against the bundled `nucleo_h753zi` platform and
  asserts the hello-world contract strings on USART3. CI gains the
  `renode-smoke-hello` job, depending on `zephyr-build` and consuming
  its ELF artefact via the v4 download-artifact layout.
- **Slice fsw-2: Zephyr west workspace + `nucleo_h753zi` hello-world.**
  Repository is now a west T2 manifest repo (`west.yml`) pinned to
  Zephyr v3.7 LTS. New `samples/hello/` Zephyr application builds for
  `nucleo_h753zi` (STM32H753ZI / ARM Cortex-M7) under ARM GCC
  13.2.Rel1 and Zephyr SDK 0.16.8, prints
  `Hello, fsw-core / nucleo_h753zi` on the UART console.
- New `zephyr-build` CI job builds the ELF and uploads
  `zephyr.elf` / `zephyr.bin` / `zephyr.map` as artefacts.
- `samples/` directory added to the clang-format scope (now covers
  `.c` files in addition to the existing C++ extensions).
- Initial repository scaffold: modern CMake (≥ 3.25) build, Conan 2 recipe,
  CMake presets for `debug` / `release` / `asan` / `tsan` / `tidy` / `docs`.
- GoogleTest integration with `gtest_discover_tests` and a sanitizer build
  matrix (AddressSanitizer + UndefinedBehaviorSanitizer + ThreadSanitizer).
- Static analysis via clang-tidy (comprehensive check set, warnings treated
  as errors) and clang-format (custom LLVM-derived style; identical to
  hw-catalog).
- Pre-commit configuration: trailing-whitespace, EOF, large-file guard,
  clang-format, cmake-format/cmake-lint, Conventional Commits.
- GitHub Actions CI: build + test matrix on Ubuntu and macOS for Debug,
  Release, and ASan+UBSan; clang-format check; clang-tidy job.
- Apache License 2.0, SECURITY policy, CONTRIBUTING guide, CODEOWNERS,
  issue and PR templates.
- Public API placeholder: `migris::fsw::version_major/minor/patch` and
  `migris::fsw::version_string()`.

[Unreleased]: https://github.com/migris-labs/fsw-core/commits/main
