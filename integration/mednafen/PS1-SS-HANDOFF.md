# Mednafen PS1 / Saturn full-build handoff — 2026-10-13

The PS1 development chat owns assembly of the combined emulator distribution.
This repository retains the reusable Saturn XBAND / SCI integration kit.

## Inputs and integration order

- Baseline: official Mednafen 1.32.1; archive identity and file hashes are in
  `baseline-files.json` and `link-cable-baseline-files.json`.
- Apply `mednafen-1.32.1-xband.patch`, then the current
  `mednafen-1.32.1-link-cable.patch` using the supplied checked helpers.
- Compile `components/xband/adapters/mednafen` and `components/link-cable` as
  described in `build-mednafen.sh`. Include the new header-only timebase,
  shared SCI transport and peer poll scheduler.
- Merge PS1 changes into the resulting host deliberately. Both integrations
  may change `src/drivers/main.cpp`; inspect lifecycle, frame polling, reset,
  exit and settings hooks. Do not blindly apply two host patches or remove
  their validation merely to force a build.
- The supplied Saturn-only build script uses `--disable-psx`; the combined
  build must enable both PSX and SS and link both adapter runtimes. Its strict
  patch checks describe the Saturn-only source, so a merged build needs its
  own verified source manifest and build script.

## Saturn behavior to preserve

- SCI guest register / baud / DMA / IRQ timing remains in SH-2 cycles.
  Transport time uses accumulated master-clock/65 ticks; tag
  `0x100 | (PAL << 1)` survives normal CPU divider 61/65 changes. Explicit
  reset/load or incompatible peer clocks invalidate sessions. Older raw-clock
  peers must not silently join this new native transport.
- Windows shared SCI uses `Local\\SaturnSCI_v1_<channel>` and its own SSC1
  layout. Do not share it with PS1 IPC. Default TCP remains available;
  same-PC gameplay tests used shared / sync ON / quantum 4096.
- XBAND cart event polling uses one steady-clock domain, paced sub-frame pumps
  and prompt active-peer control polls. Retain UART timings, hangup/generation
  validation, card, point, mail and settlement behavior.
- Cable and XBAND modem are mutually exclusive within a Saturn instance.
  Start sound ON. `sound.buffer_time 64` helped Daytona but is a configurable
  host setting, not a mandatory transport setting.

## Verification and licensing boundary

See `LINK-OPTIMIZATION-VERIFICATION.md` for automated tests, final gameplay
binary identity, title variants and the user reports. Run PS1 and SS regression
tests again against the final merged binary; the Saturn-only test binary is
not evidence that the combined binary has passed.

Our components are GPL-3.0-only. Retain Mednafen's GPL-2.0-or-later notices and
all upstream/third-party notices; the combined distribution is GPLv3. A full
binary distribution must provide the matching complete corresponding source,
including modified host source, both runtimes, build scripts and configuration
needed to regenerate that binary. Patches and a link to an unmodified upstream
archive alone are not the planned complete-source deliverable. Audit all
bundled DLLs and their licenses/source obligations separately; do not assume
the emulator's license replaces their licenses. Include GPLv3 text, prominent
modification/date notices and no-warranty information. Do not include ROM,
BIOS, personal saves, cards, mail, ledgers or captured communication logs.
