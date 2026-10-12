# Shared Saturn SH-2 SCI link-cable module (experimental)

GPL-3.0-only. Extracted from this project's YMIR YLS1 transport, retaining its
wire format. `SciState` is C++11-compatible; the socket transport requires C++20.
No emulator executable, BIOS, game media, XBAND service, card or personal data.

Responsibilities: independent master/slave byte lanes, bounded queues,
worker-owned socket I/O, 24-byte YLS1 records, optional 4096-cycle synchronization
and virtual latency. Host adapters supply monotonic cycles, clock mode,
register/DMA/IRQ wiring, lifecycle and shutdown cancellation. Synchronized
emulation explicitly waits for peer progress.

The Mednafen adapter additionally offers a Windows-local shared-memory backend
(`LocalSciCable`, C++20). It uses separate Saturn session tokens and independent
SH-2 lanes, not the PS1 protocol. TCP/YLS1 remains the default and wire-compatible;
shared memory requires synchronization and permits a quantum of
64/128/256/512/1024/2048/4096 cycles (default 4096). Both processes must select
the same backend, channel and quantum. There is no shared/TCP interoperability.

The 2026-10-12 development changes avoid locking empty/not-yet-due TCP receives
and cache Mednafen's remaining execution budget between synchronization boundaries.
SCI baud rates, frame timing, DMA and IRQ behavior are unchanged. Smaller shared
quanta reduce the additional virtual delay but increase barrier frequency and can
expose host instruction/DMA overshoot; they are opt-in pending real-game tests.

The existing YMIR patch remains unchanged. Wire compatibility is not a claim
of cross-emulator gameplay compatibility. See the [serial specification](../../docs/BATTLE-CABLE-SERIAL-SPEC.md)
and [Mednafen kit](../../integration/mednafen/LINK-CABLE.md).

```powershell
cmake -S components/link-cable -B C:/build/link-cable
cmake --build C:/build/link-cable --config Release
ctest --test-dir C:/build/link-cable -C Release --output-on-failure
```

Windows tests cover SCI flags/timing/buffering/DMA requests/IRQ selection,
4000 ordered bytes across four directional lanes, synchronization/latency,
reset and Mednafen C ABI lifecycle. Optional `-DSATURN_YMIR_SOURCE=C:/src/integrated-ymir`
adds an original-YMIR transport comparison. These are not real-game tests.

Shared-memory tests exercise all seven quanta, 28000 ordered bytes across four
directional lanes, delivery deadlines, independent clock origins, clock/revision
changes, overflow, cancellation, replacement sessions and abrupt peer-process
exit. `link-performance-test` is an optional two-process microbenchmark; it is
not a CTest or a measurement of game FPS. See the
[optimization verification](../../integration/mednafen/LINK-OPTIMIZATION-VERIFICATION.md).

Limits: no physical waveforms, parity-error injection, exact external clock
or guaranteed 7-bit framing. Pausing one synchronized instance stalls its peer.
Both peers need matching synchronization/video/CPU clock modes. Reset/state
load invalidates external traffic; Mednafen currently requires restarting both.
Shared memory uses bounded owner-thread spin/yield waiting with a 30-second
progress timeout; CPU usage can increase while waiting. It is limited to two
processes in the same Windows logon session and does not support LAN peers.
