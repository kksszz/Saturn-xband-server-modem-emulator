# Shared Saturn SH-2 SCI link-cable module (experimental)

GPL-3.0-only. Extracted from this project's YMIR YLS1 transport, retaining its
wire format. `SciState` is C++11-compatible; the socket transport requires C++20.
No emulator executable, BIOS, game media, XBAND service, card or personal data.

Responsibilities: independent master/slave byte lanes, bounded queues,
worker-owned socket I/O, 24-byte YLS1 records, optional 4096-cycle synchronization
and virtual latency. Host adapters supply monotonic cycles, clock mode,
register/DMA/IRQ wiring, lifecycle and shutdown cancellation. Synchronized
emulation explicitly waits for peer progress.

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

Limits: no physical waveforms, parity-error injection, exact external clock
or guaranteed 7-bit framing. Pausing one synchronized instance stalls its peer.
Both peers need matching synchronization/video/CPU clock modes. Reset/state
load invalidates external traffic; Mednafen currently requires restarting both.
