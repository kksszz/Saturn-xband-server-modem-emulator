# Verification: 2026-10-11 local candidate

## Optional Mednafen link-cable add-on (dev.5 source)

- Final additive patch: fresh official-source extraction, verified XBAND base,
  five post-base file hashes, forward/reverse checks and second-call idempotency passed.
- MSVC shared SCI/transport/C ABI/original-YMIR wire suites passed 4/4. Each
  suite also passed ten consecutive iterations (40 runs). One earlier transport
  run failed under simultaneous full-build load; no root cause was established.
  Keep it as an unresolved intermittent test observation, not proven fixed.
- Official-source UCRT64 GCC 16.2.0 full build and revised incremental link
  passed. GCC upstream/deprecation warnings remain. No emulator EXE is packaged.
- Native-core diagnostic with a fresh profile and the user's Japanese Daytona CE
  DISC/BIOS printed `LINK_CORE_SELFTEST_PASS`: mapped registers, timed TX,
  native RXI/ERI/TXI/TEI vectors, native byte DMA RDR/TDR, reset.
  The test deliberately aborts loading before executing the game.
- CLI help smoke loaded the runtime DLLs; Windows Mednafen writes stdout/stderr
  into files beside the EXE, not the PowerShell redirected files. Help exits -1.
- Separate new left/right profiles and EXE directories started Daytona USA
  Circuit Edition. Both logs reported `Connected to peer`; process titles
  matched the game. Only controller/display configuration was copied from
  existing profiles, not saves, cards, journals or mail.
- During boot/title execution both endpoints changed clock timeline, dropped
  synchronization and automatically re-paired; later both reported 34 TX/RX
  bytes with 34 delivered and zero pending. Logs may append an older transport
  error to a newly connected state; the current state and fresh counters, not
  that stale suffix, determine connection status. Clock-transition continuity
  during actual gameplay remains a test item.
- User subsequently reported Daytona CE operation confirmed and Gebockers stable.
  Individual race completion/repeat, native reset/state
  workflows and cross-emulator/separate-PC gameplay are still unverified.
  XBAND profiles/binaries, existing YMIR patch and public v0.4.0 were preserved.

## Prior XBAND/logging evidence

dev.4 logging/UI revision: Release server build completed and the full repository CTest passed 60/60. The new log suite covers accepted TX versus backpressure, RX only on consume (not repeated peek), binary escaping, session boundaries, 200-row paging, separate run directories, hidden viewer filters/navigation, and capture-write failure without blocking the endpoint. The main dashboard's new button was rendered to a fixture PNG and visually inspected. The user subsequently confirmed the log feature; no active process or private state was replaced during these checks.

Documentation revision dev.3: current server/UI/host specifications and historical-scope notes were refreshed. No modem runtime, integration patch or server behavior was changed by this documentation revision. The build/gameplay evidence below belongs to the tested dev.1/dev.2 code snapshot; it is not a new combined point/credit gameplay result.

- Official Mednafen 1.32.1 archive SHA-256 matched `baseline-files.json`.
- Fresh archive extraction: apply preflight, seven baseline-file hashes, apply and reverse postflight passed. Second invocation detected the applied patch. Unsupported source was rejected without modifying the fixture.
- Reusable adapter MSVC Release build: requested-bus, card and real TCP/AT/control suites all passed (3/3). Card suite also verifies distinct new 100-unit side-1 identity and Ymir persistence type aliases.
- Existing server/shared component Release build and CTest: 59/59 passed.
- Windows ZIP manifest: 105 files verified, no personal profiles, journals, card images, BIOS, game media or emulator EXE. ZIP extracted into a short path: independent adapter Release build and both included tests passed (2/2). The complete-repository TCP regression suite is intentionally omitted from ZIP builds because its server test headers are not packaged.
- Official-source clean UCRT64 GCC 16.2.0 build through `build-mednafen.sh`: completed, including C++11 host, C++20 runtime and final link. Local EXE SHA-256: `a3bff511b0613aa15b249798130697380604c9853307a20d81a9490a854ac662`. This EXE is not packaged.
- Local CLI smoke after placing required DLLs beside EXE: Mednafen 1.32.1 loaded SDL/FLAC/iconv/zlib, printed complete `-help` output, stderr empty. Mednafen's help-only path exits -1; not a gameplay test. A separate fresh profile was used, not the user's active profiles.
- Deep Windows paths caused an MSVC file-path limit failure; moving extracted kit/build into short paths resolved it. GCC emitted upstream performance/deprecation warnings; no build errors remained.

The final dev.2 packaging revision adds this verification record and clarifies runtime dependencies; adapter code, server binary and integration patch are unchanged from the tested dev.1 snapshot. Manifest file count therefore differs from dev.1.

Original-game user tests previously confirmed match establishment, normal return/disconnect, repeat match and mail body reading. Later on 2026-10-11 the user confirmed normal match debit (100 -> 97), winner +1/loss 0, reconnect without duplicate awards/debits, and completed the mail charging/free-receive trial in the active investigation build. The log feature was subsequently confirmed by the user; this is not all-title validation. No claim of all-title, real-card, separate-PC or exact physical modem compatibility. Published v0.4.0, active emulators, server process and personal saved data were not replaced during this packaging work.
