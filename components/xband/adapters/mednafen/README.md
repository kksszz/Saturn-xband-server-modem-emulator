# Mednafen host adapter

Instructions: [Mednafen integration kit](../../../../integration/mednafen/README.md).

`requested_bus_bridge.hpp` preserves original requested width before native bus splitting. Unowned/debug accesses retain host fallback. `xband_probe.inc` binds clocks, SCU EXT0+12 IRQ, frame callbacks, flash NVRAM and owner-thread polling. C ABI headers isolate the C++20 runtime from the C++11 host. This is source integration, not a stable DLL ABI.

Keep the borrowed card alive until the command runtime is destroyed or unbound. Network/filesystem work runs outside mapped accesses. VF/XOS wait/return observation is trace-backed compatibility behavior, not exact physical hardware emulation.

The 2026-10-12 development candidate adds bounded active-call network pumping
from cart events (owner thread only): guest-cycle precheck every 4096 cycles,
at most one pump per monotonic high-resolution millisecond. Active control
response cooldown is 1ms; idle polling remains 16ms and queued call notices
may bypass that cooldown only after any in-flight request completes. Driver
polling while paused remains in place. UART guest timing, async-v1 sequencing,
backpressure, mail and settlement policies are unchanged. Rebuild the host with
the updated `xband_probe.inc` and C ABI; no new base patch is required.
