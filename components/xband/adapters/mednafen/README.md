# Mednafen host adapter

Instructions: [Mednafen integration kit](../../../../integration/mednafen/README.md).

`requested_bus_bridge.hpp` preserves original requested width before native bus splitting. Unowned/debug accesses retain host fallback. `xband_probe.inc` binds clocks, SCU EXT0+12 IRQ, frame callbacks, flash NVRAM and owner-thread polling. C ABI headers isolate the C++20 runtime from the C++11 host. This is source integration, not a stable DLL ABI.

Keep the borrowed card alive until the command runtime is destroyed or unbound. Network/filesystem work runs outside mapped accesses. VF/XOS wait/return observation is trace-backed compatibility behavior, not exact physical hardware emulation.
