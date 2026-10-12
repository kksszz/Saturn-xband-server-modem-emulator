# Mednafen Saturn communication optimization: v0.5.1

This records the optimization work and v0.5.1 verification. Existing
game profiles and executables have not been replaced. The PS1 link-cable work
informed the empty-receive fast path, execution-budget cache and local IPC idea;
Saturn keeps its own master/slave SH-2 lanes, clock tags and session identity.

## SCI changes and tests

- TCP/YLS1 wire format and quantum 4096 remain unchanged. Empty/not-yet-due
  receives avoid a mutex; actual deque consumption still validates under lock.
- The Mednafen C ABI caches the remaining budget inside a boundary, checking
  cancellation, clock, revision, monotonic cycles, state and session epoch.
- The optional Windows shared-memory backend supports seven quanta, two
  processes, independent origins and bounded queues. It is not PS1 IPC, a LAN
  protocol or a substitute for the XBAND server.
- MSVC Release: four suites passed, and all four passed ten consecutive runs.
  A subsequent C ABI test additionally passed disconnect/reconnect between
  budget calls, verifying that a fresh session does not reuse a cached origin.
- Shared contract: 28000 ordered bytes across four directional lanes and seven
  quanta; delivery deadlines; third-instance and quantum mismatch rejection;
  clock/revision/rewind/overshoot; overflow; cancellation; endpoint replacement;
  abrupt termination of an isolated child process without a 30-second wait.
- UCRT64 GCC 16.2.0: the shared contract passed. Separate MSVC/MinGW processes
  exchanged 512 bytes on each lane in both directions using shared Q=128, and
  also using TCP Q=4096. Original/current TCP binaries passed the same ordered
  byte exchange. These do not establish cross-emulator game compatibility.
- Launcher parse and invalid TCP quantum/shared synchronization checks passed
  before profile creation. Defaults remain TCP, synchronization ON and Q=4096.

## Measurement method

`link-performance-test` runs two processes for 16777216 emulated cycles with
64-cycle owner polling and both SCI receive lanes. It requires no BIOS, DISC,
game input or saved profile. `SATURN_LINK_BENCH_BYTES=1` enables a separate
512-byte-per-lane ordering test. The baseline runtime/transport is from commit
`3792579bfe57d1e0839c6dd16d822fab9730b2c8`, built with the same benchmark harness
and MSVC Release configuration as the candidate.

Wall time measures this workload, not FPS, input-to-screen latency, real cable
timing or overall emulator CPU usage. Shared spin/yield waiting can increase CPU
use. Smaller quanta increase barrier frequency while reducing additional virtual
delivery delay; Q=128 adds 128 rather than 4096 cycles (1/32), without changing
SCI baud rates, DMA or IRQ timing.

The official 1.32.1 host with the existing XBAND/SCI patches built successfully
using UCRT64 GCC 16.2.0, C++11 host and C++20 adapters. Existing upstream and
deprecation warnings remain. Native-core diagnostics printed
`LINK_CORE_SELFTEST_PASS` with TCP and shared launch settings: mapped registers,
TX timing, native IRQ vectors, byte DMA and SCI reset. These deliberately end
loading before gameplay; they do not exercise a paired shared game session.
Help smoke loaded the runtime DLLs. Each used a fresh isolated profile; the
Mednafen help/diagnostic paths intentionally exited -1.

Final microbenchmark: three paired runs per mode after the full build ended;
median of the six endpoint wall times below. Dedicated original/current TCP
and shared byte-order cases also passed.

| Mode | Quantum | Median seconds | Reduction from baseline |
| --- | --- | --- | --- |
| Baseline TCP | 4096 | 0.280405 | — |
| Optimized TCP | 4096 | 0.225079 | 19.7% |
| Shared | 4096 | 0.085690 | 69.4% |
| Shared | 128 | 0.116506 | 58.5% |

An earlier run during compilation had a shared Q=4096 outlier of 1.14 seconds;
wall scheduling is noisy, and this small sample is not a statistical guarantee.
Raw final reports are kept locally in
`saturn-link-optimization-20261012/measurements-20261012-225213`; cross-compiler
reports are in `measurements-20261012-224634`. No private paths or game files are
needed to reproduce the transport benchmark. Local unstripped build EXE SHA-256:
`0737712fb4cb274c3839e8d24d381067bba7222b3f30c62cbafa956bf61cd523`.
The trial copies have debug symbols stripped to reduce each EXE from 81.2 to
17.2 MiB; their help smoke passed. Trial EXE SHA-256:
`3289e7807824db507aa4d3213883db5d36d619ac1a9e9ce1956cba8fef2a9b81`.
Reproducible diagnostic EXE/DLL copies (256.6 MiB) were removed after tests,
retaining their logs and fresh profiles; `verify-mednafen.ps1` recreates them.

At this initial benchmark stage, shared gameplay and clock transitions were
unverified and clock-tag changes invalidated the session. The initialization
timebase fix and subsequent gameplay reports below supersede those two points.
Repeat matches and complete pause/reset coverage remain unverified.

## XBAND follow-up implementation

XBAND uses independent modem clocks and the server's `async-v1` relay, not SCI
lockstep. PS1's shared transport and low quantum cannot simply replace it.

| Candidate | Local evidence | Safe boundary for a follow-up |
| --- | --- | --- |
| Active-call exchange scheduling | The old `pollControl` waited 16ms after each complete control response | Implemented 1ms cooldown while peer-connected, 16ms while idle; queued call notices bypass cooldown but never an in-flight operation |
| Sub-frame network pumping | The old cart events only advanced guest UART clocks; network poll used driver frame/wait loops | Implemented `xband_command_live_poll` from cart events only, paced by 4096-cycle prechecks and at most once per high-resolution wall millisecond; existing driver pump retained |
| UART event scheduling | The current cart event always requests timestamp+256 while peer-connected; `PeerUartTiming` and `AsyncModemReceive` already expose next-event queries | Schedule actual guest TX/RX/timeout deadlines while retaining retry behavior for full FIFOs; validate IRQ and first-byte timing |
| Relay/wire overhead | Each exchange carries JSON byte arrays and staged transmit/advance/response phases | Measure before changing the wire; maintain server monitoring, hangup/generation checks and backpressure |

The server already waits on socket readiness (at most 1ms timeout), and its
socket setup already enables TCP_NODELAY. Its UI refresh at 500ms is not the
network service cadence. There is no evidence here that these should be blindly
reduced or removed. The modem's 18750-cycle byte period, 75000-cycle receive
timeout, independent-clock policy, game-to-XOS disconnect detection, mail,
points and card settlement have not been changed. The 256-cycle cart event
period also remains unchanged; event-deadline optimization and wire redesign
are deferred. The server and YMIR code are unchanged.

### Verification

- The command runtime uses `std::chrono::steady_clock` milliseconds for **all**
  client creation/steps/deadlines, including the driver pump. Mixing it with
  `GetTickCount64` would reverse the clock domain; that is not done. The latter's
  coarse ticks initially prevented a 1ms cadence from taking effect.
- MSVC adapter tests: bus, card and real TCP/AT/control passed 3/3, including
  five consecutive runs of each. Tests cover the scheduler, same-cycle pacing,
  16 binary byte round trips using sub-frame-only pumps between frame calls,
  full control sequencing, carrier close, reset, generation renewal, management
  card swaps and existing UART IRQ/timeout behavior. No private state is used.
- UCRT64 host rebuilt and linked successfully. `cart.o` has a relocation to the
  new `xband_command_live_poll`, verifying that the host event hook was compiled,
  not merely that the new runtime symbol exists.
- The same real-TCP command fixture also passed when built with UCRT64 GCC;
  its median RTT was 67.9995ms. The stripped combined trial passed help smoke,
  and the local `start-xband-trial.ps1` parsed successfully. It is prepared to
  use the existing Saturn test profiles/card files and isolated server on
  ports 58280..58285, without replacing stable emulator executables. It has
  not been launched into gameplay. Unrelated PS1 processes are not stopped.
- A baseline command runtime from commit `3792579bfe57d1e0839c6dd16d822fab9730b2c8`
  uses the same updated fixture, with sub-frame pumping compiled out. Both test
  processes request a 1ms Windows timer period only during the fixture and
  release it on exit; production does not change Windows timer settings.
- Each fixture has two real loopback control endpoints and independent guest
  clocks advanced by 5000/7000 cycles per owner iteration. Driver polling is
  approximately every 16ms; the new cart-style pump runs between those calls.
  It exchanges 16 one-byte echoes, checking ordering, value and carrier, using
  steady-clock microseconds for the wall measurement. This is not game FPS or
  input-to-screen latency, and the fixture's server clock differs from the
  production server configuration.

| Paired run | Old median RTT, ms | New median RTT, ms |
| --- | --- | --- |
| 1 | 311.991 | 67.994 |
| 2 | 311.997 | 68.0075 |
| 3 | 314.004 | 67.9875 |

Raw local report: `saturn-link-optimization-20261012/xband-measurements-20261012-230659.json`.
These are diagnostic results, not confirmation of a complete game match, mail
delivery or live billing after the change. Real-game testing is still required.

The user subsequently tested the combined candidate with two VF Remix
(Japan, SegaNet) instances in the XBAND configuration and reported that
Mednafen had no problems. This is user-reported operation confirmation, not a
measured input-latency result or a separate confirmation of mail/settlement.
The launcher's old `-sound 0` trial setting explained the missing audio. At the
user's request, subsequent XBAND trial and integration launches specify
`-sound 1`; the running instances were not restarted or modified.

## Daytona synchronized initialization follow-up

The user reported black screens with synchronized TCP on both the candidate
and the previously tested executable, with sound enabled and disabled. The
same trial booted with synchronization OFF. Logs showed a normal boot-time CPU
clock change tearing down the synchronized session and discarding its queued
traffic. This isolates a synchronized-initialization issue; it is not proof
that the budget-cache optimization alone caused it.

The revised host keeps native SCI timing in SH-2 cycles but converts **all**
transport timestamps/budgets to accumulated master-clock/65 ticks. CPU divider
61/65 changes therefore preserve the transport timebase and do not require
resetting queues. The new clock tag is `0x100 | (PAL << 1)`, which rejects a
legacy CPU-cycle peer rather than mixing time units. Video-rate mismatches and
explicit reset/state-load invalidation remain enforced. The server/XBAND clock
is unchanged. This changes the SCI host patch, not just the C ABI library.

Clock tests cover equivalent physical durations at different CPU speeds,
61/65 transitions, fractional carry across 65 frame rebases and unchanged SCI
baud timing. A paired TCP test preserves initialization bytes across the
normalized transition and still rejects a changed video tag. The four common
suites passed. The revised patch applied successfully to nine selected files
freshly extracted from the SHA-256-verified official archive after the verified
XBAND base patch; reverse checks and second-call idempotency passed.
The revised host built successfully and was deployed to the isolated trial
executables with SHA-256
`c606fdbdc74ef9536d36e9bcfe127bb6f9321d1803ab184b4dbfb5d776b43a14`.
Both Daytona instances were launched with TCP synchronization ON and sound ON;
initial logs reported Connected to peer without the earlier timeline error.
The user subsequently confirmed progression into Linkbattle course selection
with synchronization ON and reported that processing remained heavy. This
confirms the initialization recovery, not a complete race or acceptable speed.
After the trial windows were closed, a shared-memory Q=4096 comparison was
launched with synchronization and sound ON, keeping the same virtual delay and
changing only the transport. Shared spin/yield can increase waiting CPU usage;
CPU load alone does not establish a gameplay-speed improvement.

The user reported a slight improvement with shared Q=4096, but a heavy opening
sequence remained. The next host revision removes fixed-clock conversion from
each interpreter instruction: the fast raw SH-2 clock gates the poll, and the
normalized transport clock is sampled only inside a poll slice. Because a
master/65 tick spans at least one SH-2 cycle for divisors 61/65, bounding raw
execution by the remaining transport budget is conservative. The quantum,
delivery deadline, SCI frame timing and clock identity remain unchanged.
Unit simulation crosses 61/65 changes in both directions, verifies the grant
is not crossed, and limits conversion to fewer than 400 samples per 20001
raw-cycle checks. Four common suites passed. Actual opening-speed improvement
from this host change is not yet measured.
The host rebuilt successfully, the updated patch passed verified base apply
checks, and the revised shared Q=4096/synchronization ON/sound ON trial was
launched. Trial SHA-256:
`4c32dc59671c30e648e15601818c590eb29ffc9ca1b7b58176a3bb5b66611869`.
An early two-second thread sample showed two busy threads per process, but no
same-scene baseline was collected; this is not a quantified gameplay gain.
Combined unstripped build SHA-256:
`998e4bbb488f50f31c21a6c4d0b589e90cea7b96754826957448c64eb9e419c6`;
stripped trial SHA-256:
`33271253e0ef7ed90a85484257c00cd4bd1571ada3ce97de45623f19c37394c2`.

## User gameplay verification — 2026-10-13

The final gameplay executable has SHA-256
`4c32dc59671c30e648e15601818c590eb29ffc9ca1b7b58176a3bb5b66611869`.
The earlier hashes above identify intermediate builds, not this final build.
Two Windows processes on the same PC used shared SCI, synchronization ON,
quantum 4096 and sound ON. The user confirmed:

| Title / DISC variant | Reported result |
| --- | --- |
| Doom (Japan) | Link gameplay working |
| Gebockers (Japan) | Link gameplay working and light processing load; both games require OPTION / LINK ON |
| Gungriffon II (Japan) | Link gameplay working |
| Hyper Reverthion (Japan) (2M) | Link gameplay working |
| Steeldom (Japan) (2M) | Link gameplay working; some slowdown reported |
| Daytona USA - Circuit Edition (Japan) | Audio buffer explicitly set to 64ms on both sides; improved performance and stable completion of a race |

Daytona's audio change is configuration only (`sound.buffer_time 64`), not a
CD-DA decoder, host source or transport change. Logs confirmed WASAPI shared
mode, 48kHz and 3072-frame / 64ms buffers. The original automatic setting used
1536-frame / 32ms buffers. The tradeoff is extra audio latency. This does not
prove CD-DA decoding caused the slowdown, nor establish 64ms as optimal for
every game or device. Steeldom's CUE contains AUDIO tracks 2 through 95, but
its slowdown has not been isolated to audio output.

Gebockers initially lacked LINK BATTLE because the game option was not enabled;
the Q=128 and old TCP/asynchronous comparisons did not establish a transport
regression. With both game options ON, the original shared Q=4096 configuration
worked. Sega Rally Plus was excluded after the user corrected cable support.
These observations are user reports, not measurements of input-to-screen
latency or exhaustive reset, replay, LAN or cross-emulator coverage.
