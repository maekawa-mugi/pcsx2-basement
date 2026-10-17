# PCSX2 RAW agent console (prototype)

The opt-in `-agent-console <fresh-run-directory>` mode captures EE SIO bytes
before newline handling, buffering for display, or Shift-JIS conversion. It
also captures IOP stdout MMIO and intercepted IOP HLE output as separate
channels. The HLE Kprintf channel is formatted by the existing HLE code; it is
not a hardware UART stream. EE/IOP GUI logging toggles do not control capture.

The prototype supports cold boot, RAW reads, EE SIO input, register snapshots,
bounded event execution, recording/replay verification, and shutdown. Savestate
loading is explicitly rejected during an agent session.

## Setup and build

Build `pcsx2-qt` using the normal supported Clang build. Qt Network is now
required alongside Qt Core, Gui and Widgets. CMake and Windows Visual Studio
project integration include it and the Windows DLL copy step.

Use an explicit, already configured data directory. **The existing `-datapath`
option appends `PCSX2` to its argument.** Consequently `--data-dir work/data`
requires `work/data/PCSX2/inis/PCSX2.ini`, with the BIOS in the configured BIOS
directory. Configure Full TLB and the CPU implementation in that INI. The CLI
does not silently change the CPU/TLB choices. The deterministic mode below
copies the boot assets into the run directory and explicitly pins its profile.

For testing, use an isolated data directory. Disable `Logging/EnableEEConsole`
and `Logging/EnableIOPConsole` to verify that capture is independent of them.
An incomplete setup fails instead of opening the setup wizard in agent mode.

## CLI

Examples below run from the repository root. Use a different run directory for
each process; paths in the run metadata are absolute. Python 3.9+ is sufficient.

```powershell
python tools/pcsx2ctl.py --run build/linux-run start `
  --exe bin/pcsx2-qtx64-avx2-clang.exe `
  --data-dir build/agent-console-data `
  --elf C:/path/to/linux.elf -- -unlimited

python tools/pcsx2ctl.py --run build/linux-run status
python tools/pcsx2ctl.py --run build/linux-run read --from 0
python tools/pcsx2ctl.py --run build/linux-run send --text "uname -a" --newline lf --request-id uname-1
```

`send` reports the `output_offset` at which the CPU accepted the input. Use
that offset for a subsequent wait so that boot messages cannot satisfy it:

```powershell
python tools/pcsx2ctl.py --run build/linux-run expect --from 6146 --text "mips GNU/Linux" --timeout 15
python tools/pcsx2ctl.py --run build/linux-run read --from 6146 --out build/response.bin
python tools/pcsx2ctl.py --run build/linux-run stop
```

The numerical offset above is illustrative; use the actual `send` response.
An echoed command can itself match an expected string. Select a response
which is absent from the submitted command, or have the guest construct a
fresh response marker from separate arguments. An arbitrary prompt match is
not proof that a command succeeded.

`read --raw` writes only the original bytes to stdout and metadata to stderr.
For byte-exact files, `--out` avoids any shell-dependent pipeline conversion.
The default JSON view decodes UTF-8 for display, escaping invalid bytes; the
archive and RAW transport remain unchanged. ANSI sequences are retained.

Additional options:

- `read --channel iop.stdout` or `--channel iop.hle` inspects the other routes.
- `read --max-bytes 65536` bounds a response. Reads never consume the archive.
- `send --file input.bin` or `send --hex 030a` sends arbitrary bytes.
- Newlines are explicit: `--newline none` (default), `lf`, `crlf`, or `cr`.
- `input-status --request-id ID` queries an uncertain send without resending it.
- `--epoch N` on read/send/expect/input-status pins an observed VM generation.
- `expect` requires `--from` and supports `--hex` for binary patterns.

Each operation opens a fresh local connection. The emulator survives CLI
completion; no persistent interactive terminal, terminal echo emulation, or
screen scraping is required. Environments that restrict localhost networking
must allow the CLI's local connection. No external service is involved.

## Register dumps and deterministic execution

```powershell
python tools/pcsx2ctl.py --run build/record start `
  --exe bin/pcsx2-qtx64-avx2-clang.exe `
  --data-dir build/agent-console-data --elf C:/path/to/linux.elf --deterministic

python tools/pcsx2ctl.py --run build/record run-events --until 8000000 --request-id boot
python tools/pcsx2ctl.py --run build/record registers --request-id inspect --out build/registers.json
python tools/pcsx2ctl.py --run build/record send --text "uname -a" --newline lf --request-id uname
python tools/pcsx2ctl.py --run build/record run-events --until 9000000 --request-id response
python tools/pcsx2ctl.py --run build/record read --from 0
python tools/pcsx2ctl.py --run build/record stop
python tools/pcsx2ctl.py --run build/replay replay --source build/record
```

The event counts are examples for the tested Linux ELF; other kernels can need
different budgets. `--until` is an **absolute EE event-test index**, not an EE
instruction count, cycle count, or host time. The VM starts paused at event 0,
runs exactly to the requested boundary, synchronizes VU/GS, captures a checkpoint,
and remains paused. Host polling and elapsed wall time do not advance this clock.
Input is accepted only while paused and no action is in progress. Its `at_event`
records the boundary where the bytes entered the emulated RX FIFO. Wait for
acceptance before advancing. Pending input and overlapping actions are rejected.

`registers` also works outside the deterministic profile. It pauses the VM,
captures after leaving CPU execution and synchronizing VU/GS, and leaves it paused.
The JSON includes EE GPR/HI/LO, CP0, FPU/FCR, VU0/VU1, IOP GPR/CP0/CP2,
all 48 TLB entries, GS private registers, EE performance counters, and scheduler
samples. Register values are fixed-width hexadecimal strings, including 128-bit
values, rather than floating-point JSON numbers. Companion binary files contain
EE/IOP MMIO **shadows** and pending SIO RX bytes. These are copied without calling
MMIO read handlers; some device registers live outside those shadows. This is an
architectural/debug snapshot, not a restorable or exhaustive internal device state.

The opt-in profile `isolated-elf-v1-same-build` fixes:

- Cold ELF boot without disc media, with copied ELF, BIOS, optional BIOS ROM
  companions, and source INI. SHA-256 records these inputs plus the executable
  and its sibling DLLs. Each run has its own writable settings and device data.
- A frozen core configuration, single-threaded VUs, synchronous Null GS,
  Null audio with host pacing disabled, and standard EE cycle rate/skip.
- RTC at 2020-01-01 00:00:00 UTC and fresh BIOS-default NVRAM, independent of
  host time, timezone, or previously saved NVRAM.
- Disabled network, HDD/swap, host filesystem, USB peripherals, controllers,
  memory cards, patches/cheats/game fixes, achievements, PINE, and input recording.
  Host keyboard/controller events cannot inject unrecorded guest input.
- Paused-boundary SIO inputs with IDs and RAW payloads, and finite event budgets.
  Reset invalidates the deterministic run; loading savestates is rejected.

Use the CLI launcher for this isolation; the lower-level emulator flag alone
does not stage or fingerprint boot files. A black rendering window named `guest`
is expected with Null GS and the staged `guest.elf`; use the RAW console to
observe progress. A paused VM needs `run-events` to produce a command response.

Replay cold-boots again, checks the build/input fingerprints, injects the
recorded RAW inputs at the same event boundaries, and compares every recorded
checkpoint. A checkpoint hashes the dumped registers, EE/IOP RAM, scratchpad,
VU memories, MMIO shadows, RX FIFO, complete serialized core settings, and all
three RAW output streams. Run-directory paths are normalized for the settings
hash. Missing/incomplete histories, changed inputs/builds, and mismatches fail
explicitly; no automatic retry or source edit follows a divergence. The result
is saved in `replay-verification.json`, including the first mismatching boundary.

This provides tested reproducibility for this isolated ELF profile on the same
build/host architecture. It does **not** establish universal determinism across
PCSX2 versions, compilers, host architectures, graphics backends, or external
devices. Checkpoint agreement also cannot prove equality of internal device
state omitted from the snapshot. Enabling those features requires extending
the profile, input recording, and validation first.

## Meaning of results

`start` success means the VM is running or paused, not that Linux has booted.
`send` success means the bytes entered the RX FIFO, not that a shell executed
them. `rx_consumed` counts actual guest reads and is cumulative across epochs.
`guest_console_verified` remains false: the transport does not infer guest
health from a prompt or a string match. Record application-level verification
in the calling test harness.

The CLI exits with:

| Code | Meaning |
| --- | --- |
| 0 | The requested operation completed |
| 2 | Client, configuration, or transport error |
| 3 | Wait expired or VM stopped before the expected response; guest outcome unknown |
| 4 | Observation fault; guest outcome cannot be judged |
| 5 | Startup failed |
| 6 | Input/API request rejected |

Never turn an absent response into an automatic kernel/emulator defect.
Inspect startup errors, all available channels, produced/saved byte counts,
CPU sample changes, and input acceptance/consumption first. Do not continue a
code-editing loop after an observation fault. `stop` remains available when
observation is faulty. A startup timeout retains the process for diagnosis.

Input IDs are retained for the entire session (maximum 4096 requests). The
same ID and identical payload return the previous result without replaying
the input; conflicting reuse fails. If a send loses its connection, query its
ID. `unknown_request` is inconclusive about a request still in transit: do not
automatically submit it again. The CLI prints the ID before sending.

## Artifacts and protocol

- `run.json`: protocol version, PID, loopback endpoint, per-run token, arguments.
- `serial.ee-sio.bin`: original EE SIO TX bytes, including incomplete lines.
- `serial.ee-sio.rx.bin`: submitted input bytes, once per input ID.
- `serial.iop-stdout.bin`, `serial.iop-hle.bin`: separate IOP output routes.
- `events.jsonl`: transport readiness, epochs, observed input results, shutdown.
- `inputs-manifest.json`, `inputs/`: deterministic profile, input/build hashes
  and copied boot assets. BIOS assets stay local and must not be committed.
- `snapshot-*.json`, `snapshot-*.ini`, `snapshot-*.bin`: register/checkpoint
  documents, full effective core settings, and RAW MMIO/RX copies.
- `replay-verification.json`: matched checkpoints or a replay failure.
- `status.json`: last status sample; **not a live-process liveness guarantee**.
- `effective-config.json`: initial CPU/TLB choices and arguments only. Complete
  core settings are in the snapshot INI files; freezing requires the opt-in profile.
- `launcher.log`: subprocess output, including failures before the API starts.
- `result.json`: final saved ranges, observation state and shutdown reason;
  the guest outcome stays `unknown`.

After shutdown, read the archive files directly. A reset advances the epoch,
cancels queued inputs and records each channel's `epoch_start`. Old cursor
ranges cannot satisfy a read in the new epoch. Archives remain append-only.

Protocol 1 uses one request per TCP connection: an ASCII JSON line containing
`command`, `run_id`, `token`, `payload_size`, and command fields, followed by
exactly `payload_size` binary bytes. The response has the same JSON-header and
binary-body framing. Integer counters and offsets are decimal strings. Send
payloads are limited to 1024 bytes, reads to 65536 bytes. No base64, encoding
conversion, line buffering, or C-string treatment is applied to the body.

The server binds explicitly to 127.0.0.1 on an automatically assigned port.
Treat `run.json` as a local credential; do not publish its token. CPU hooks
only enqueue/copy bounded data; socket and file operations run on the host
thread. Each capture buffer is limited to 4 MiB and each output archive to
256 MiB. Overflow or I/O failure latches an observation fault. A blocked host
I/O operation is caught by the client's transport timeout; a CPU that never
returns to an event test cannot accept new input or stop normally.

## Tests

```text
python -m unittest discover -s tests -p test_pcsx2ctl.py -v
```

Core CTest coverage is in `tests/ctest/core/agent_console_tests.cpp`. Integration
testing must additionally boot a real Linux ELF and check a response after its
input's reported offset. Serial device emulation remains the existing PCSX2
implementation; interrupt/FIFO fidelity is not expanded by this prototype.

For a running Linux with a BusyBox-style `~ # ` prompt, the repeatable smoke
test below checks `uname -a`, a fresh echo-resistant response marker, NUL/FF
bytes without a newline, request deduplication, conflicting IDs, and agreement
between the saved archive, binary transport and CLI RAW stdout:

```text
python tests/agent_console_linux_smoke.py --run build/linux-run --timeout 90
```

It saves `smoke-result.json` and `smoke-response.bin` in the run directory.

The deterministic integration test starts and stops its own two processes,
verifies paused clock stability, exact budgets, register/MMIO/RX dumps, a real
Linux `uname` response, NUL/FF bytes, mutation rejection and checkpoint replay:

```text
python tests/agent_console_determinism_smoke.py --run build/record --replay-run build/replay --exe bin/pcsx2-qtx64-avx2-clang.exe --elf C:/path/to/linux.elf --data-dir build/agent-console-data
```

It writes `determinism-smoke-result.json` after successful replay. Both run
directories must be fresh. Python unit tests also cover changed-build rejection,
boot-asset isolation, incomplete trace rejection and uncertain action timeouts.
