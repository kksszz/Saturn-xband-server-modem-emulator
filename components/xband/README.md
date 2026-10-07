# XBAND reusable components: first extraction

Development foundation, not a complete modem or LAN server. No Ymir, GUI, OS,
network or third-party headers are required for the default core build.
The optional JSON adapter requires nlohmann_json 3.12 or newer (3.x).
This is C++20 source reuse, not a stable C/DLL ABI.

`include/xband/mail_profile_names.hpp` is a bounded, emulator-independent
observed-name registry keyed by endpoint, profile epoch and local player index.
It rejects incomplete/conflicting batch associations; it does not authenticate,
retain, deliver or clear mail itself. The diagnostic server additionally has an
explicit fresh-COW player-0/3 experiment. An additional opt-in mailbox experiment
uses `include/xband/mail_profile_mailbox.hpp` to select recipients by endpoint,
controlled run scope and current player. Ordinary admission is unchanged; neither
experiment authenticates, acknowledges delivery or clears outboxes.
See `MAIL-PROFILE-NAMES.md` and `MAIL-PROFILE-MAILBOX.md`.

The separately opt-in profile inventory experiment tracks offered custody IDs
with `include/xband/mail_profile_offers.hpp`. Only IDs previously offered to the
same controlled endpoint/epoch/player may suppress a later response. It does
not dequeue, acknowledge delivery or deduplicate new outbox resubmissions.
Wire token encoding stays in the diagnostic adapter. See `MAIL-PROFILE-INVENTORY.md`.

A DIFFERENT, default-off mixed-profile clear experiment requires a fresh owned
snapshot file and successful whole-request commit before emitting the observed
all-outbox control. It is restricted to exactly the measured player0/3 mixed
request, not individual clear or production delivery acknowledgement. The
ordinary profile/inventory experiments still never clear. Persistence and ROM
control remain separate diagnostic adapters; Ymir core is unchanged. See
`MAIL-PROFILE-BATCH-CLEAR.md`.

A separate default-off submission-clear experiment extends commit-before-clear
to observed single player0 or player3 requests. It does not expand the old mixed
setting; both switches are mutually exclusive. Unknown/same-player/reversed
batches, missing persistence and lost scope cannot authorize clear. See
`MAIL-PROFILE-SUBMISSION-CLEAR.md` for scope and limitations.

## Current migration: original PB3 and emulator boundary (2026-09-27)

Real-game migration now takes priority over additional synthetic UART cases.
The separated service/peer relay reached the PB3 two-player field with exact
opposite-direction byte counts. See
`../../outputs/ymir-xband-investigation/pb3-process-migration-20260927.md`.

`include/xband/device_bus.hpp` is an emulator-independent read/write boundary.
The optional `adapters/ymir/bus_binding.hpp` is the thin Ymir-specific mapping.
It preserves the prior CS0/CS1 and shared CS2 fallback and bus-wait behavior;
the current endpoint still delegates to the established diagnostic modem.
Independent-process PB3 gameplay has now passed a bounded 4923-frame replay;
see `../../outputs/ymir-xband-investigation/ymir-modem-bus-migration-20260927.md`.
Normal Ymir UI integration, separate-PC LAN validation and hot-unplug remain pending.

`include/xband/modem_register_bank.hpp` now contains the shared UART register/FIFO
and flash behavior plus board address dispatch, with no emulator/OS dependencies.
The original-game diagnostic uses this implementation, not a copied second UART.
It is the observed subset, not a complete hardware model. `at_command.hpp` now
extracts observed command framing/classification/profile decoding, while
`login_handshake.hpp` provides a server-side byte-driven login wrapper around PPP.
The opt-in PB3 call replay passed without guest login PC/RAM hooks. AT call
lifecycle integration remains; the whole modem is not yet a standalone core.
`modem_service_buffer.hpp` now owns bounded UART/service queues and dial/carrier
state. The opt-in real-PB3 host no longer opens a local diagnostic TCP bridge;
remote open acknowledgement precedes guest CONNECT. A 49-frame peer slice
passed after original login/hangup/dial/answer. Ordinary frontend integration
and fully asynchronous modem lifecycle remain pending.
See `../../outputs/ymir-xband-investigation/modem-core-extraction-20260927.md`.

`adapters/ymir/board_attachment.hpp` now supplies the shared board/UART dispatch
used in the game host. `include/xband/windows_modem_service.hpp` owns one
nonblocking Windows service connection and response staging. Its step/open/
transfer/drain/close/stop methods do not contain a wait loop. The diagnostic
driver still waits outside that component; normal SDL App wiring is not done.
See `adapters/ymir/README.md` for lifecycle, pause, reset and shutdown contracts.
No performance improvement is claimed; the prior batch-reuse comparison flag
is retired because staging now belongs to the service owner. Timing buckets in
new diagnostic runs are not directly comparable: step now includes batch work.

Latest extraction: `modem_command_session.hpp` owns the observed service-mode
AT lifecycle and uses `modem_escape.hpp` for the existing frame-based +++ policy.
The real PB3 REMOTE_LOGIN path now uses this implementation. Generation tickets
protect CONNECT after reset; peer routing is external and only intercepted while
idle without carrier. It is not the complete peer-call modem or normal App.

## Previous integration: live guest UART update (2026-09-27)

`xband_remote_cpu_login_test --change-uart` changes divisor/LCR using guest SH-2
instructions after the first CONNECT byte, with byte 2 already held by the pacer.
Assertions verify the held byte retains its old deadline and later bytes use
the new period. All 70 bytes match guest RAM: 7203 quanta, 68 batches.
Default and --slow-8e2 regression runs also pass; six related unit tests pass.
Only the diagnostic test/docs changed. This is not real-game or hardware-clock
validation; deployed game executables and modem/server defaults are unchanged.

## Previous integration: UART profile comparison (2026-09-27)

The CPU login fixture now uses the locally observed PB3 initialization values
DLL=8/DLM=0/LCR=0x13 by default (8N1 under the diagnostic register model).
`--slow-8e2` selects divisor16/LCR=0x1f and exercises a longer character period.
Both use an explicit TEST oscillator, not a verified Saturn hardware clock.
Default passed: 6800 quanta/65 batches; slow passed: 7203 quanta/68 batches.
Both deliver all 70 bytes correctly to guest RAM and check minimum RX intervals.
Six related unit tests passed (0.13 seconds). Full 134-suite result below is from
the preceding slice; only the standalone fixture and documentation changed here.
No production EXE, default route or card balance changed.

## Previous integration: register-derived diagnostic UART timing (2026-09-27)

uart16550CharacterNanoseconds computes a rounded-up character interval from an
explicit input clock, DLL/DLM and LCR (data/parity/1, 1.5 or 2 stop bits).
Unknown clock, zero divisor and unsupported break return no interval. This is an
explicit 16550-style diagnostic profile, not identification of Saturn hardware.
GuestBytePacer period updates preserve an already-held byte's deadline.
The autonomous SH-2 fixture now programs DLL=8, DLM=0, LCR=3 itself; an explicit
test oscillator of 1843200 Hz gives 694445 ns. No production clock is assumed.
All 134 component tests passed (60.08 seconds). CPU login passed twice: 6800
quanta, 65 batches, 70 spaced RX bytes, exact guest RAM. Actual game behavior,
hardware clock and SCU fidelity remain unverified; deployed EXEs are unchanged.

## Previous integration: guest-time RX pacing (2026-09-27)

GuestBytePacer retains one byte until an explicit guest-tick deadline, preserves
backpressure and does not accumulate idle-time/burst credit. Reset drops pending
data; reversed time, overflow or uncertain sink effects fail closed.
The autonomous SH-2 login test now routes CONNECT and service replies through a
fixed 694445 ns diagnostic interval: 6800 quanta, 65 batches, 70 spaced RX bytes,
exact guest RAM. This assumes 10 bits/byte at 14400 bit/s for testing, not measured
hardware behavior or UART-divisor-derived timing. Delivery remains quantum-bound.
No deployed game EXE was changed.
Release build and all 131 component tests passed (60.02 seconds); paced CPU
login passed twice with the same counts.

## Previous integration: autonomous synthetic SH-2 login (2026-09-27)

xband_remote_cpu_login_test installs one original SH-2 RAM program and runs AT,
CONNECT, login prompts, synthetic credentials and LCP/PPP without host PC/register
rewrites between stages. The guest waits for known reply lengths and saves all
bytes to RAM; host assertions compare their values. Initial pass: 6555 quanta,
63 TCP batches, exact RAM contents. Network waits hold real guest cycles.
Empty advances are sent on diagnostic frame changes only; TX flushes at current
quantum. This optimization is a fixture policy, not validated game scheduling.
No BIOS/disc, authentic authentication, baud/SCU conformance or original-game
success is claimed. Existing production routes and deployed EXEs are unchanged.

## Previous integration: combined AT/UART/login/PPP fixture (2026-09-27)

The standalone xband_remote_login_test now routes UART-written AT dial commands
through ModemCallControl, real loopback TCP and DiagnosticLoginEndpoint. All
CONNECT/prompts/PPP replies are read through UART registers. Partial-username
hangup and staged-PPP-reply hangup both permit fresh-ID, zero-origin redial with
no stale reply. This uses synthetic credentials, direct host register calls and
an external hangup control, NOT a tested +++ escape or original game login.
Default server, launchers and game binaries remain unchanged.

## Previous integration: VF diagnostic login extraction (2026-09-27)

DiagnosticLoginEndpoint extracts the existing VF fixture's CR/HELO, 60-guest-frame
delay, login/username and Password sequence before the limited PPP endpoint.
It is explicitly NOT historical authentication. The eight-byte password-like line
is counted, not stored or compared; tests use synthetic content. Printable-byte
restrictions are conservative fixture policy, not verified service behavior.
Three tests cover real TCP/batch traversal, fragmented login/coalesced PPP, invalid
input and reset. No launcher or default server routing changed. Other titles,
real-game login and accurate receive cadence remain pending.
Release build and all 128 component tests passed (60.20 seconds).

## Previous integration: limited AT call lifecycle (2026-09-27)

ModemCallControl maps normalized AT dial/hangup commands to protocol open/close.
Configured subscriber and destination service number are distinct; only the
configured destination is routed. CONNECT/DCD waits for open_ok. Pending dial or
transfer cancellation ends the connection without a late CONNECT; normal close
allows redial with a fresh call ID. Responses are bounded to 256 bytes.
The UART/TCP/PPP test now enters via an actual UART-written dial command, but
still explicitly bypasses login. ATZ profile reset, escape guards and authentic
baud/credential behavior are not implemented by this limited component.
Release build and all 125 component tests passed (59.70 seconds); updated
standalone UART/TCP/PPP test passed separately.

## Previous integration: instruction-driven UART/TCP/PPP test (2026-09-27)

The standalone xband_remote_cpu_uart_test runs original SH-2 RAM instructions
through Ymir's real scheduler and mapped diagnostic UART. CPU-written LCP bytes
travel through loopback TCP; staged replies are read by guest polling code into
RAM. A prefilled 256-byte UART does not deadlock the resumed CPU. Checks cover
unchanged cycles/reads during network wait, exact RAM order/count and real reset
invalidation. No BIOS/disc, AT/login, SCU delivery or baud-accurate behavior is
claimed. Delivery at 4096-cycle boundaries is a diagnostic policy, not a completed
game integration. See EMULATOR-ADAPTER.md for build and scope.

## Previous integration: bounded response staging (2026-09-27)

StagedServiceBatch separates remote replies from the guest UART FIFO. The default
64 KiB ring retains ordered replies across batches, exposes them only after batch
completion, and permits guest resumption even when the UART rejects more bytes.
Overflow fails closed/disconnects rather than waiting forever for a paused guest.
The updated UART/TCP/PPP test passed with a full UART during network completion;
two new component tests cover ring wrap/backlog, overflow and cancellation.
This is a resume-permission boundary, not automatic CPU scheduling or baud timing.
Production game integration and instruction-driven delivery remain pending.
Release build and all 123 component tests passed (58.92 seconds); the standalone
UART/TCP/PPP test passed separately.

## Previous integration: diagnostic UART/TCP boundary (2026-09-27)

An opt-in callback boundary in the existing diagnostic Probe routes data-mode TX,
carrier status and hangup to a remote service. Bounded RX admission preserves
backpressure and receive notification. The standalone xband_remote_uart_test
passed actual loopback TCP/PPP with direct UART register calls, DLAB isolation,
full RX queue/resume, LSR/IIR and hangup clearing. Existing Probe self-test also
passed after its old end02 fixture was updated to satisfy the current validator's
card-block and trailing-command markers; runtime validation was not weakened.
No deployed game executable was replaced. AT/login, SH-2 MMIO instruction execution,
SCU delivery and combining the guest gate with full-RX handling remain pending.
See EMULATOR-ADAPTER.md: holding the guest while waiting for it to drain RX can
deadlock and must not be used as a production integration strategy.

## Previous integration: client-to-PPP TCP parity (2026-09-27)

The new client_ppp tests connect TcpClient and RemoteServiceBatch to TcpHost and
OwnedDiagnosticEndpoint over real loopback sockets. LCP/IPCP negotiation, escaped
frames split into single-byte batches, retry boundaries and close/reopen after an
incomplete escape match the existing LocalPPPProbe byte for byte. Output sink
backpressure and three-byte work budgets are exercised without duplicate delivery.
The fixture explicitly uses synthetic 60 frames/second and nanosecond guest ticks;
it does not establish the production Saturn frame mapping. These are limited PPP
service tests, not modem UART integration or game/LAN-PC validation.
Release build and all 121 CTest cases passed (57.94 seconds).

## Previous integration: real Ymir scheduler/TCP smoke test (2026-09-27)

GuestExecutionGate converts exact rational scheduler cycle frequency to explicit
nanosecond ticks without accumulated frame rounding. It holds execution until a
batch completes, then grants 4096 cycles; timeline/frequency changes fail closed.
The isolated xband_remote_core_test target links the local Ymir core, runs original
SH-2 test code without BIOS/disc, and exercises eight actual TCP batch boundaries.
Repeated RunFrame while waiting leaves guest cycles fixed; real Reset is detected.
That core test passed, as did all 118 component CTest cases (54.98 seconds).
No existing cable callback or deployed game route was changed. Modem UART, real
PPP clock mapping, dynamic video-clock changes and real games remain unverified.
See EMULATOR-ADAPTER.md for the exact scope, build target and integration contract.

## Previous integration: asynchronous service batch boundary (2026-09-27)

RemoteServiceBatch bridges the legacy synchronous pumpService calling pattern to
ClientControl: snapshots at most 64 KiB of input plus an explicit guest tick,
resumes bounded sends without duplication, advances once, and drains replies into
a backpressure-aware sink. Consumed output credit is acknowledged in batches.
Completion is reported only after advance_ok and draining guest output. Cancel
ends the whole connection, never silently replays uncertain remote effects.
The ClientControl reference must outlive the batch; discard the batch without
calling resume/cancel after a TcpClient failure invalidates that reference.
Two loopback tests cover a 5000-byte batch with source mutation/sink blocking,
exactly-once service effects, and cancellation after partial admission.
Ymir clock acquisition and scheduler/UART wiring remain pending. The existing
RunFrame return type and cable budget callback were inspected, not modified.

## Previous integration: nonblocking Windows TCP client (2026-09-27)

windows_tcp_client.hpp adds an owner-thread TcpClient owning ClientControl and
Winsock resources. Zero-wait connect readiness, bounded partial IO and retained
frame tails; no sleeps, worker threads, DNS, auto reconnect or guest progression.
Default destination policy is 127.0.0.1; private LAN IPv4 needs explicit opt-in.
EOF/protocol failure/deadlines/time reversal dispose the socket and client state.
Five new tests cover actual loopback binary roundtrip/heartbeat/EOF, wrong key,
hello timeout, reversed clock and destination policy. Release build and all 112
CTest cases passed (54.19 seconds). LAN sockets and real games remain untested.
EMULATOR-ADAPTER.md documents lifecycle and pointer/thread ownership. Ymir clock,
UART and scheduler integration still remain; deployed game routes are unchanged.

## Previous integration: client data and guest-tick protocol (2026-09-27)

ClientControl now supports transmit/advance and receive/consume APIs with per-call
send/receive windows. Data is staged until an explicit guest tick; only one advance
may be outstanding. Responses require matching call/request/tick. Receive credit
grows on consumption, not just receipt; ACK queue pressure retains the latest
credit for a later poll. Close completion/disconnect clears all transfer state.
Five additional cases cover binary parity, capacity/reuse, reset, timeout and
invalid transfer messages. The real loopback client/host test now checks binary
echo as well. All 107 CTest cases passed in Release (45.93 seconds).
This is protocol integration, not Ymir runtime wiring: real guest clock mapping,
UART delivery scheduling and production client socket integration remain pending.

## Previous integration: emulator-side control foundation (2026-09-27)

ClientControl is a portable owner-thread control channel: hello, open, snapshot,
server ping response and close, with bounded queues, strict correlations/session
checks and host-time deadlines. No sockets, Ymir dependencies or card writes.
Six in-memory cases plus an actual loopback TCP control/status roundtrip passed;
all 102 CTest cases passed in Release. See EMULATOR-ADAPTER.md for the caller API.
This is not yet wired into Ymir: guest data/advance, real clock mapping and the
production socket client remain missing. Existing working game routes are intact.

## Previous integration: opt-in diagnostic native status view (2026-09-27)

The diagnostic runner accepts --status-stdout to emit HOST_STATUS records every
500 ms and on normal stop. Default output remains unchanged. This synchronous
diagnostic channel is not a production asynchronous UI transport; a blocked
stdout consumer can stall it. Exports may contain phone/card numbers: opt in only.
outputs/xband-card-manager/modem_monitor.py --host-status selects the new Tk view;
legacy monitor behavior is unchanged. It reads bounded UTF-8 logs, distinguishes
missing data from an absent/empty card, and labels stale/future-dated records as
last observations. No card DB access or new network listener is added.
See HOST-STATUS.md for the schema and development invocation. Distribution EXE
repackaging, visual pixel QA, Ymir/game integration and multi-PC validation remain.

## Previous integration: owned host status export (2026-09-27)

TcpHost::status() exports an owned JSON copy of configured endpoints, verified
connection/call state and optional peer snapshots. Unverified sockets are counted
separately; disconnects leave an offline configured row with no stale snapshot.
No auth keys or connection session tokens are exported. Call it on the host owner
thread and pass the copy to consumers; it is not a network or UI API endpoint.
See HOST-STATUS.md for fields, lifetime, privacy and observation-time limitations.
Existing monitor logs/EXEs are unchanged. Actual loopback tests cover export,
copy independence, call changes, EOF and stop. All 95 CTest cases passed.

## Previous integration: heartbeat and peer snapshots (2026-09-27)

ServiceConnection queues one ping after 2 seconds without valid inbound traffic.
Its matching pong is required within 5 seconds of queue admission, not socket
delivery. Other messages do not extend that deadline. A full output queue does
not arm a ping; the existing 10-second idle timeout still applies. Unsolicited
or incorrectly correlated pong messages close the connection.

Validated snapshot messages are retained as untrusted, display-only peer state.
Call/subscriber/state must match the current session phase. Call changes and
disconnect clear the snapshot. ServiceConnection and ServerSession expose a
borrowed snapshot view, invalidated by later mutation; host aggregation and UI
display are not yet implemented. Snapshots never update the card ledger or drive
game/service processing. Peer error messages conservatively close the connection.

Four in-memory tests and a real loopback TCP test cover this slice. Deadlines in
these tests use synthetic host timestamps. All 94 CTest cases passed in Release.

## Previous integration: owned diagnostic endpoint and bounded runner (2026-09-27)

The bounded host now has diagnostic PPP integration tests and a development-only
`xband_diagnostic_server.exe`. OwnedDiagnosticEndpoint owns a LocalPPPProbe and
its borrowed adapter together, avoiding parser lifetime sharing between peers.
The runner loads strict config and allows only 127.0.0.1 with LAN opt-in disabled.
Required run duration is 1..60000 ms; optional --ephemeral-loopback picks a port,
otherwise the configured port is used. It stops at the duration or Ctrl+C, closes
all sockets, and registers no service/autostart. No UI/deployed EXE is replaced.

This is NOT a game server: hello clock_hz must be 1000 and the diagnostic mapping
is 1000 ticks per frame. It has only the historical limited PPP implementation,
not complete XBAND services. Tests compare LCP/IPCP reply bytes through TcpHost
against the direct parser and verify incomplete-frame reset/reopen/retry. CLI
smoke tests cover actual startup/automatic stop and invalid duration rejection;
Ctrl+C handling has not been automated. See SERVER-CONFIG.md for invocation.

The target is opt-in through Windows + JSON + diagnostic path + BUILD_TESTING.
Remaining work: host/UI status integration, real emulator clock negotiation,
Ymir/UI integration, multi-peer game behavior and separate-PC LAN verification.

## Previous implementation: bounded Windows TCP host (2026-09-27)

`windows_tcp_host.hpp` / optional `xband::windows_tcp` accepts ServerConfig and
owns Winsock, an exclusive nonblocking listener, endpoint registry and sessions.
Default production construction uses the configured port. Explicit ephemeral-test
mode overrides only the port and requires 127.0.0.1; configuration files cannot
select this mode. Before binding, nondefault-loopback addresses must appear in
GetAdaptersAddresses on an operational interface's IPv4 unicast list, in addition
to the existing private-address/opt-in policy. No automatic firewall changes.

Host step(now) is owner-thread only, requires monotonic host milliseconds, and
contains no sleep/background thread. Per step: accept at most four sockets;
retain at most 32 peers; per peer receive at most 4096 bytes when its previous
buffer is drained, attempt at most four message feeds, and send at most 4096 bytes.
Protocol/service queues retain their existing bounds. Factories and service methods
must remain nonblocking/non-reentrant. Excess sockets are closed without creating
services. EOF/fatal peer errors release that peer; host-level failures or clock
reversal stop the whole host. stop/destruction releases peers, listener and Winsock.

OS-random session and call tokens are issued by the adapter. The call candidate
stays unchanged while open is blocked, then rotates after a successful open.
Tests cover configuration-derived loopback hello/echo/close/reopen/EOF, exclusive
bind and port release, 32-peer admission plus handshake expiry, and unsafe bind
rejection. No LAN bind, multi-PC game or UI integration has been tested. The config
checker remains validation-only; no new production server CLI is deployed yet.

## Previous implementation: configuration validation (2026-09-27)

`server_config.hpp` parses/loads at most 64KiB of strict configuration JSON, with
duplicate/unknown keys rejected and fixed secret-free diagnostic messages. It
requires version 1, port 1..65535 and 1..32 valid unique endpoint/key identities.
Bind defaults to 127.0.0.1 and LAN opt-in to false. Non-loopback RFC1918 IPv4
literals require explicit allow_lan=true; wildcard/public/hostname/IPv6 forms are
rejected. This is syntax/policy validation, NOT local-interface/subnet validation.
The future socket adapter must verify an actual local unicast interface before bind.

The JSON-enabled build also produces `xband_config_check`, a validation-only CLI.
It reads a path, reports validity/identity count/LAN opt-in, and opens NO socket or
UI. It does not modify settings, launch games or install a service. Windows MSVC
uses wide command-line paths. See SERVER-CONFIG.md for fields and invocation.
The sample fixture uses an intentionally fake shared key and an example port;
neither is installed as operational configuration. Tests cover parsing, address
policy, registry construction, bounded/error streams and valid/missing files.

## Previous integration: random identifiers and authenticated diagnostic TCP (2026-09-27)

`windows_token.hpp` supplies 128-bit tokens from Windows BCryptGenRandom, encoded
as 32 lowercase hex characters. Link the optional `xband::windows_token` target;
portable protocol targets do not acquire Windows headers/libraries. OS failure
throws with no deterministic fallback. The failure branch has not been fault-injected.
256 generated samples passed format/no-duplicate checks; this is a smoke test, not
proof of randomness or guaranteed global uniqueness. No persistent collision ledger
is implemented. IDs do not encrypt or authenticate plaintext LAN traffic.

Authenticated TCP tests now share `tests/authenticated_fixture.hpp`. Each connection
gets an OS-generated session and each requested open a fresh candidate call token
held stable during its exchange. The client learns the session from hello_ok; the
pipelined ping stress fixture deliberately knows the server's candidate in advance.
These are test host policies, not production client/configuration integration.

The existing four diagnostic PPP TCP cases now run through the full ServerSession
hello/key/ID/service path rather than bypassing hello. LCP/IPCP, retry, malformed
FCS and fragmented escape/reset/reopen retain direct-parser byte/state parity.
The reset case also checks a new call identifier. An additional actual TCP case
reconnects, sends the old session ID and verifies rejection plus service disposal.

Release /W4 /WX build and all 75 CTest cases passed. Remaining work includes a
production socket/configuration adapter, clock mapping from actual Ymir settings,
status/heartbeat handlers and multi-PC/game verification. Stable EXEs, settings,
images and card data remain unchanged; no LAN listener or firewall changes.

## Previous integration: handshake through TCP to service (2026-09-27)

`ServerSession` now owns lifecycle, handshake/reservation and the subsequent
ServiceConnection. A supplied non-reentrant factory is invoked only after hello_ok
is fully sent; it receives the declared guest clock rate. The host supplies the
actual FrameClock mapping separately. Factory/dispatch failures close the session
and release the reservation, as do EOF/error via disconnect. Socket ownership
remains external; registry must outlive every ServerSession.

The socket host must retain unconsumed input, stop reading when its bounded input
buffer is occupied, poll with monotonic host milliseconds, send only output(), and
report the actual count through sent(). During partial hello_ok, input can make
zero progress; retain it without spinning until output progresses. A stable fresh
call token is required when feed/poll can process an open, including blocked retry.
Never retain output spans across mutations. On stopped(), close the OS socket.

Five new actual loopback TCP cases cover configured hello followed by open/data/
advance/close, wrong key rejection without service creation, duplicate endpoint
rejection followed by reuse after disconnect, pipelined hello/ping ordering, and
service-factory failure releasing the reservation. They use real steady-clock
milliseconds and fragmented nonblocking writes, not preapproved identity setup.

Session/call tokens are STILL fixed test fixtures. OS-random generation, session
uniqueness policy, production socket adapter, configuration loading, application
status/heartbeat handling and full PPP through this combined path are pending.
These tests use echo; earlier diagnostic PPP socket tests remain separate.
No deployed executable/settings, game images, card data or firewall were changed.

## Previous implementation: configured identity handshake (2026-09-27)

`server_handshake.hpp` adds a platform-neutral, post-TCP-accept hello gate and
an immutable registry of 1..32 configured endpoint/key pairs. It validates the
strict framed hello, requires first request ID 1, compares the configured 64-hex
key and reserves the endpoint before building hello_ok. Unknown identity, wrong
key, duplicate active identity, malformed/incorrect first message or expiry fail
closed without a wire error response. Local failure enums contain no input/key.
These are plaintext configured-peer checks, not protection against LAN attackers.

Integration contract:

- Begin the lifecycle once at TCP acceptance; create a fresh ServerHandshake.
  Registry and lifecycle must outlive it. All calls stay on one owner thread.
- Supply a fresh OS-generated session token externally. Current tests use a fixed
  fixture; OS randomness and session uniqueness tracking are not implemented here.
- Feed only one hello and retain any unconsumed TCP tail. Send the exposed reply,
  reporting only bytes actually sent. No service construction/dispatch until the
  entire hello_ok has been sent and state becomes accepted. Poll with host monotonic
  time before IO; the original five-second handshake deadline includes reply drain.
- The post-authentication inbox/outbox start at ID 2. Transfer accepted name,
  session and declared clockHz to the host; derive FrameClock using the selected
  emulator's actual clock/video configuration, not the test fixture's ratio.
- Keep ServerHandshake alive for the whole service connection to retain the
  endpoint reservation. On EOF/error/timeout, destroy/disconnect service then
  handshake and close the OS socket. Loser/old-generation cleanup cannot release
  another connection's reservation or close a newer lifecycle generation.

Five new in-memory tests cover fragmented hello/partial reply, successful handoff,
bad key/unknown endpoint/first ID/version/type, duplicate reservation and release,
deadline during reply, retained TCP tail and duplicate hello after handoff.
The hello gate is not yet integrated into the real socket test harness: existing
TCP/PPP cases still use approved-identity fixtures. Production socket adapter,
token generator, configuration loading and application handlers remain pending.

## Previous validation: diagnostic PPP over actual TCP (2026-09-27)

With WIN32, XBAND_WITH_JSON and XBAND_DIAGNOSTIC_SOURCE_DIR enabled, the
diagnostic TCP target runs LocalServiceEndpoint/LocalPPPProbe through
ServiceConnection over the same ephemeral loopback socket fixture as the echo
tests. Socket setup and pumping now live in tests/loopback_fixture.hpp; this is
test infrastructure, not a production transport adapter.

Four new cases compare reply bytes and selected parser state against direct
feed/tick execution: LCP open/termination; IPCP address negotiation; 180-frame
retry boundaries and five-attempt stop; invalid FCS, incomplete escape, close/open
reset and an escape split across separate data/advance batches. Output offsets,
receipt ACK offsets, completion correlation/tick and shared response IDs are
checked. Test tick/frame ratio remains 1000:1, not a Saturn clock assumption.

Release /W4 /WX build and all 63 CTest cases passed. Authentication still uses a
preapproved fixture; these tests prove transport parity with the existing limited
diagnostic parser, NOT full PPP implementation or hardware/game compatibility.
No Ymir/game launch, multi-PC LAN test, deployed EXE/configuration change or
firewall modification. The next integration stage is the initial handshake and
configured-identity dispatcher, which must precede use as a real LAN server.

## Previous integration: service ownership and TCP loopback (2026-09-27)

`ServiceConnection` in the optional JSON adapter owns a ServiceEndpoint, framed
inbox/output queue, control handler and per-call transfer. It is a post-authentication
host, not a complete listening server. The externally owned lifecycle must outlive
it. Identity verification, endpoint reservation, hello/hello_ok, fresh random tokens,
socket lifetime and heartbeat scheduling remain external responsibilities.

- Feed received bytes and retain any unconsumed tail. Poll regularly with host
  monotonic milliseconds even when no bytes arrive; guest ticks belong only in
  advance messages. The loopback fixture uses synthetic host timestamps.
- On open, reset service and create fresh transfer state before pumping output.
  On successful close, atomically cancel staged input/unfinished advance and reset
  service. Responses already queued remain ordered before close_ok; no old data
  is re-admitted to a subsequent call. All methods/callbacks are non-reentrant and
  restricted to one owner thread. A blocked close retains state until admission.
- Send only the exposed output span, then report the actual count sent. Would-block
  consumes nothing. The socket adapter must enforce generation ownership and poll
  deadlines before IO. EOF/error calls disconnect and closes the socket externally.
- Disconnect/timeout/fatal failure destroys service state and discards pending
  output. An old generation's destructor cannot close a newer connection.
- This focused host handles open/close/ping/data/data_ack/advance only. Snapshot,
  pong and error notifications currently fail closed rather than pretending to be
  supported; application-level handlers are still required before production use.

Windows JSON-enabled builds include five new tests with actual Winsock TCP on
`127.0.0.1:0` (ephemeral port), no TLS. The listener closes after accept and every
socket closes on completion or exception. No LAN exposure/firewall changes.
Roundtrip and cancellation tests deliberately split writes into at most 17 bytes.
They verify reply ordering, echo payload, close/reopen with fresh offsets/clock,
staged-input cancellation, EOF cleanup, idle expiry and stale-generation cleanup.
Authentication is an approved fixture, tokens are constants, service is an echo;
this does NOT test actual keys, diagnostic PPP over TCP, Ymir or games.

Release `/W4 /WX`: all 59 configured CTest cases passed (including diagnostic
memory-path parity). Earlier sections below describe historical slices; their
statements that no socket has been opened predate this loopback-only milestone.

## Standalone build (from this directory)

```text
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Consumers use add_subdirectory and link `xband::components`.
Public headers reside under include/xband.

For the optional adapter, configure `-DXBAND_WITH_JSON=ON` and provide an installed
`nlohmann_json` CMake package through CMAKE_PREFIX_PATH or nlohmann_json_DIR.
Link `xband::wire_json` to use wire_json.hpp. Nothing is downloaded or vendored.
The local test used an existing vcpkg installation of version 3.12.0; no Ymir
path is hardcoded in the project. The core remains independently buildable.

## Strict JSON control-message adapter

parseWire rejects empty/oversized payloads, BOM, invalid UTF-8/JSON, duplicate
decoded keys (including escaped aliases), container nesting beyond eight levels,
unknown envelope/body fields and wrong numeric/string types. Parsing is pure;
success does not authenticate, authorize or dispatch a message. On failure no
partially validated DOM is returned. Allocation failures propagate to the host.
Never log the returned hello DOM: it includes the supplied authentication key.

Supported schemas: hello, hello_ok, ping, pong, open, open_ok, close, close_ok,
advance, advance_ok, data_ack, data, snapshot and error. Response messages require
reply_to; requests/notifications require null. Errors allow either correlation or
null. Session equality, response correlation, ACK
semantics and tick progression remain the state-machine/dispatcher responsibility.
Client name and error-message lengths are currently bounded in UTF-8 bytes,
not Unicode characters.

Data validation requires canonical standard padded Base64 matching the stated
1..4096 byte length. base64.hpp also provides bounded decoding into caller-owned
storage; failed decoding leaves storage and written count unchanged. It rejects
nonzero padding bits, whitespace and the URL-safe alphabet. No JSON dependency
is required to use the decoder alone. Input/output spans must not overlap.

Snapshot validation is passive: no balance mutation or card I/O. Absent cards
must have absent read_state and null number/balance/nominal fields; inserted cards
cannot be absent. Known remaining units cannot exceed known nominal units (no
recharge). No-call counters must be zero. Reading status does not consume data.

Errors before session issuance may use an empty session only with connection
scope and null call. Call-scoped errors require a session and call token. This
explicit pre-session exception supports authentication failure reporting; the
future dispatcher must still reject unauthorized traffic and must not trust it.
Unknown error codes and unknown message types fail closed.

This is not yet a usable complete wire protocol. No token generation,
credential comparison or timeout scheduler is implemented here.
With JSON enabled the suite contains 18 CTest cases; the JSON case covers both
valid control/data/status/error messages and invalid syntax/schema/encoding
boundaries, including Base64 decoding and card consistency checks.

## Implemented

- service_endpoint.hpp: existing owner-thread service API extracted to namespace
  xband. Diagnostic runners use a compatibility bridge, not a duplicate copy.
  Legacy emulated-frame timing and exception behavior are deliberately preserved.
- frame_codec.hpp: incremental 4-byte big-endian framing, 1..65,536 byte payloads,
  no dynamic allocation, partial-input accounting and explicit output capacity.
- Eight independent test cases with literal wire bytes, split input and failure paths.
- protocol_state.hpp: canonical decimal uint64 and endpoint/session token validators,
  a post-authentication inbound session/ID guard, and a 64 KiB receiving ring.
  Four additional test cases cover field boundaries, session rejection, ordered
  ring wrap, guest consumption credit, capacity and sequence errors (12 total).

## Typed protocol state contract

These new types are not a JSON parser or a complete protocol dispatcher. Validate
UTF-8, duplicate keys, all fields/types, Base64 and message/call identity before
using them. The guard does not generate tokens or authenticate clients.

- SessionGuard.bind is called only after authentication and takes the next inbound
  ID explicitly, because handshake messages also consume directional IDs.
  IDs never wrap. On rejection close the connection; a subsequent accept fails
  until a new authenticated binding. reset invalidates the old binding.
- ReceiveWindow is one call/direction, accepts decoded chunks of 1..4096 bytes,
  and copies them before nextOffset can be acknowledged. Incoming data must start
  exactly at nextOffset. Duplicate and gap data are errors, not retransmissions.
- limit is an absolute exclusive credit boundary, initially 65536. It increases
  only on consume, not on receipt. peek returns only the contiguous ring segment;
  consume that segment and peek again for wrapped data. Borrowed views must not
  survive mutation. All methods are owner-thread only.
- Any window rejection ends the call. Counters retained after failure are only
  diagnostic; do not send an ACK. reset discards the buffer for the next call.
  An overflow also ends the call, never wraps counters. The capacity/sequence
  paths are tested; lifetime uint64 credit overflow has not been runtime-tested.

## Sender and client service-call state

- send_window.hpp rejects acknowledgements of unsent bytes, decreasing offsets or
  limits, and credit larger than acknowledged bytes plus the 64 KiB window.
  Identical ACKs are valid. Full credit is ordinary retryable backpressure;
  malformed ACKs are sticky failures. No retransmission is performed.
- Commit bytes only after reserving space for the complete message in a bounded
  ordered transport queue. Commit is accounting, not actual socket delivery.
  The transport integration must make reservation/commit/enqueue atomic on the
  owner thread and discard all queued messages on disconnect.
- service_call.hpp tracks the client service call through idle, opening, service,
  closing and error. open_ok/close_ok must match the pending request ID; data and
  ACKs must match the current call token. Hangup, abort and disconnect discard
  the receive ring and send accounting. Closing blocks further guest data.
- ServiceCall assumes authentication/session and full message validation before
  dispatch. Token uniqueness remains the server's responsibility. Timers/server
  errors invoke abort externally. Peer dial/answer is NOT this service lifecycle.

No existing modem route uses these types yet. Authentication handshake, server
call handling, JSON schema, Base64, timers and transport remain separate work.
The standalone suite now has 17 cases, including a 300,000-byte ordered transfer
through the bounded ring with unequal producer/consumer chunk sizes (no sockets).

## Adapter contract

1. One decoder per connection, called only on its owner thread.
2. Advance the input cursor by Result.consumed, not the socket read size.
3. On ready, validate/copy peek() before consume(). If downstream is full, retain
   the frame. After consumption feed any remaining coalesced input.
4. On invalid_length close the connection; never scan ahead to resynchronize.
5. On disconnect discard partial frames AND unsent output; do not replay fragments.
6. Encoder input/output must not overlap. Track partial socket writes with a cursor.

peek() is an ephemeral borrowed view, not an external ABI pointer. The decoder
does NOT validate UTF-8, JSON, wire v2 schema, credentials, session, or Base64.
Those layers must validate data before it is dispatched to any service.

## Remaining milestones

### Bounded transport output queue

frame_queue.hpp supplies an owner-thread framed FIFO with 131,080 wire-byte
capacity (two maximum frames) and 128 message slots. This wire-storage limit is
separate from the 65,536-byte decoded per-call flow-control window. A complete
frame is admitted or enqueue returns full without changing the queue. Empty or
oversized payloads return invalid. No allocation, I/O or implicit drop occurs.

peek exposes a contiguous borrowed segment. After a successful socket send,
consume only its actual positive byte count; on would-block consume nothing.
Message slots remain occupied until the last byte of each frame is consumed.
reset discards unsent fragments on disconnect. The host must generation-check
late asynchronous completions before consuming any new connection's queue.
Input to enqueue must not alias queue storage; views cannot survive mutation.

The queue is payload-opaque: callers must validate JSON, respect sender credit
and serialize send-accounting/admission on one owner thread. message_pipeline.hpp
now connects data admission to SendWindow; no socket is connected. Control-message reservation and latest-only
snapshot coalescing belong above this FIFO and remain unimplemented.

Four queue tests cover partial sends, both capacity limits, reset and a 400-frame
stream (1,638,400 payload bytes) through FrameDecoder with repeated ring wrapping.
All 26 CTest cases pass with JSON enabled; this is not a LAN transport test.

### Next integration stages

message_pipeline.hpp (optional JSON target) combines framing/schema validation in
MessageInbox and atomic data admission/accounting in enqueueData. The inbox holds
one validated DOM until explicit consume; feed reports only bytes it consumed so
the caller retains any coalesced tail. A full downstream consumer must leave the
message pending. peek is passive and borrowed. Invalid messages latch failure;
reset clears fragments/DOM for a fresh connection. Allocation exceptions leave
the inbox failed and must close the host connection.

enqueueData validates schema, current call and next offset, stages a SendWindow
copy, then enqueues the entire frame before committing accounting. Queue/credit
backpressure does not advance offsets. The dispatcher must still check outbound
endpoint/session/ID, advance ID only on successful admission, and never repeat
accounting after partial socket writes. Wrong-session prevention is not provided
by this helper. Consumers must not bypass it with raw queue writes for data.

Four integration cases now cover all two-part input splits, retained validated
messages, bad-schema/length failure, reset, queue/credit admission and one-byte
delivery through Base64 decoding, ReceiveWindow and ACK accounting. All 30 CTest
cases pass with JSON enabled. Tests use memory buffers, not TCP or a game.
The complete message dispatcher/control scheduler remains unimplemented.

server_inbox.hpp now gates post-hello server input using ConnectionLifecycle's
existing session/endpoint/sequence guard (no second guard or duplicate received
call). It classifies accepted messages into service, data, acknowledgement, clock,
snapshot, ping, pong and error routes; it does not execute their handlers.
Server-only replies and another hello are rejected. Handshake is external and
must be completed before constructing/feeding the gate.

The gate holds one classified message under downstream backpressure. Observing it
does not re-advance IDs or refresh liveness. Consume only after a handler accepts;
handlers must have no partial side effects when reporting backpressure. Call ID,
tick, request correlation and service state still require handler checks before
game effects. Envelope acceptance refreshes liveness, not guest time.
The referenced lifecycle must outlive the inbox. Reconnection requires a fresh
inbox: old-generation callbacks cannot close the new lifecycle, and old pending
messages are hidden. Poll via feed with an empty span for idle deadline checks.
Closing policy state still requires host transport/queue/resource cleanup.
Four routing test cases bring the JSON-enabled CTest suite to 34 passing cases.
These are memory-only tests, not proof of LAN or game interoperability.

service_control.hpp now handles routed ping/open/close requests. It produces
schema-validated pong/open_ok/close_ok or BUSY/STALE_CALL replies with correlated
request IDs. Successful open/close invokes ServiceEndpoint.reset exactly once.
The host supplies a fresh unique call token, and the controller stores the
subscriber string without stripping leading zeros. Token generation/uniqueness
is not implemented by this class.

Queue admission occurs before reset and ID/state commit, on the same owner thread
with no socket pumping or reentrant callbacks. Full queues leave the request,
call, subscriber, reply ID and service untouched. If reset or serialization throws,
the controller clears queued output and closes the lifecycle; the host must then
dispose/reset the service and transport before reuse. Never emit a queued success
before process returns handled. Poll the lifecycle/inbox with current host time
before processing/retrying; process itself does not read a clock.

The controller now uses SessionOutbox's shared outbound IDs starting at 2 after
hello_ok. Data/advance/ACK/status input routes return other_handler and stay pending.
No game data is pumped and no legacy emulated-frame/tick conversion is guessed.
References must share one connection generation and outlive the controller; discard
the controller on disconnect. Tests use a reset-counting/throwing test service,
not the diagnostic PPP implementation. All 37 CTest cases pass with JSON enabled.

### Shared output sequencer

session_outbox.hpp binds endpoint/session and a connection generation, builds
envelopes and assigns a shared directional ID to control and data producers.
Control admissions are schema-validated; data admissions also use the existing
atomic SendWindow/FrameQueue transaction. IDs advance only after whole-message
admission, never on queue-full, credit-full, invalid input or partial socket writes.
The maximum uint64 ID is usable once; subsequent sends return inactive, never
wrap. The host must then end the connection. Old-generation writers are inactive.

Exactly one SessionOutbox must own all post-hello output for a connection. Do not
enqueue raw frames or use low-level enqueueData alongside it. Queue/lifecycle
references must outlive the outbox and belong to that connection. Endpoint/session
must come from the host's approved handshake, not an incoming untrusted body.
Poll host deadlines before admitting output. Constructor first_id is for explicit
sequence handoff/testing; never reset it within an active stream. discard clears
pending wire bytes and disables the writer, but host call/transport cleanup is
still required. Stale queued data must be discarded before reusing a transport.

The control path explicitly rejects data so callers cannot bypass credit checks.
This is sequencing, not direction/call authorization: producers must still follow
their role and use the active call. Shared output does not implement peer relay,
guest-clock adaptation, control-priority scheduling or TCP.
Three new tests cover mixed control/data replies, rejection rollback, stale
writers and ID exhaustion. All 40 CTest cases pass with JSON enabled (memory-only).

### Explicit virtual-time service pump

timed_service.hpp adds FrameClock and TimedService without JSON/OS dependencies.
FrameClock converts call-relative guest ticks to legacy unsigned frame numbers
using floor(tick * numerator / denominator). The host supplies the exact rational
frames-per-tick ratio; no default 60 Hz or millisecond interpretation is assumed.
The reduced numerator is bounded to 1,000,000 and denominator to 1,000,000,000,000;
conversion checks the legacy unsigned range and never wraps. Ratio/frequency
changes require a fresh call. The correct game-specific ratio remains an emulator
adapter responsibility and has not been verified against diagnostic PPP yet.

TimedService stages received bytes in ReceiveWindow without touching the service.
beginAdvance fixes the request tick/frame; resume delivers staged input first,
calls the service timer exactly once, then drains output through an accepting
sink. Input/output backpressure retains the unaccepted byte and current phase.
Each resume has a bounded work budget. It never reads host time or sleeps.
Same-tick new advances are allowed; backward time and conversion/service/sink
failures end the pump. Construction assumes a fresh service; no implicit reset.

The sink returns true only after safe downstream admission and false only with
no side effects. Host emits advance_ok only after done AND successful reply queue
admission; do not begin the same advance again if its acknowledgement is blocked.
While an advance is pending, new data is not admitted. After any fatal error,
discard this pump, associated service state and queued output; no automatic replay.
Counter access is diagnostic only after failure. Service reset and full-call
teardown remain host responsibilities.

Four tests cover rational clock boundaries, input-before-timer ordering,
backpressure without replay, work budgets, equal/backward ticks and timer failure.
All 44 CTest cases pass with JSON enabled. This pump is not yet connected to
ServerInbox/SessionOutbox or the real PPP parser. Wire ACK/advance handlers, bounded
output chunk assembly, actual TCP and game timing regression remain to be integrated.

### Service data/advance integration (supersedes the wiring limitation above)

service_transfer.hpp now joins ServerInbox, TimedService and the shared
SessionOutbox for one active call. Host constructs it after open and destroys it
before close/reset/disconnect; referenced objects must share the same generation.
process handles data/ACK/advance, and drive performs bounded service work.
Poll connection deadlines before either method. Call tokens are checked before
effects. Malformed call/offset/credit or service failures discard queued output
and close the connection conservatively; host must dispose the remaining service.

Input data is staged before data_ack. A blocked ACK retains the already-admitted
request without accepting bytes again. advance is consumed once and its request
ID retained until advance_ok is queued. The client must wait for advance_ok before
new data/advance; pipelining these is rejected. Incoming data_ack must still be
processed during an outstanding advance, otherwise output-credit recovery stalls.

Output is assembled in a 4096-byte buffer, canonical-Base64 encoded, and admitted
with send credit and shared IDs. Small credit grants permit partial chunks. Bytes
removed from the service are owned by this bounded staging buffer until admission;
do not recreate the transfer object on a blocked result. Data replies and updated
input-consumption ACK precede advance_ok in the FIFO. Blocking completion never
reruns the service timer. These handlers do not implement automatic call ownership
integration with ServiceControl or actual socket pumping yet.

Six tests cover end-to-end echo/order, input ACK retry, 70,000-byte output stopping
at the 65,536-byte credit boundary and resuming via a one-byte grant, stale call,
blocked completion retry, and all Base64 chunk lengths 1..4096. All 50 CTest cases
pass. Service fixtures, not the real PPP parser or a game, were exercised. Next is
diagnostic service parity and host lifecycle integration before TCP loopback.

### Optional existing diagnostic-parser parity

Set XBAND_DIAGNOSTIC_SOURCE_DIR to the existing live-capture-tests directory
(containing local_service_endpoint.hpp) with XBAND_WITH_JSON and BUILD_TESTING
enabled to build xband_diagnostic_parity_tests. This opt-in test target alone
includes diagnostic headers; no diagnostic or Ymir code is added to the portable
library target, and no files are copied or downloaded. External headers are
treated as system includes; new test code is built with warnings as errors.

The parity harness compares direct LocalPPPProbe feed/tick with the same parser
behind ServiceControl, ServerInbox, ServiceTransfer and SessionOutbox. Both wire
directions are fragmented into single bytes. It checks reply-byte equality,
selected parser state, sequential output IDs and advance correlation. Four cases:
LCP open/terminate/option rejection; 180-frame retry and five-attempt stop;
all split positions in an escaped LCP frame plus bad FCS; IPCP negotiation.
Some runs use a one-operation pump budget to exercise resumability.

With this optional target the suite passes 54 cases on MSVC Release. The original
service_endpoint_test and peer_line_api_test also passed separately. The fixture
uses exactly 1000 synthetic guest ticks per legacy frame; it does not establish
the clock ratio for a real game. This verifies extraction/transport-layer parity,
not PPP standards conformance, full XBAND authentication/service behavior,
guest TCP application payloads, socket communication or game compatibility.

1. Strict wire v2 schema/JSON, handshake/server call lifecycle and timers;
   integrate the typed state machines only after full message validation.
2. Plain TCP and endpoint shared-key checking; loopback tests before LAN exposure.
3. Dial/answer/peer relay contracts and separate service implementation.
4. Modem/UART/card extraction with emulator, clock, IRQ and storage adapters.
5. Second headless adapter, Ymir integration, then separate-PC game regression.

The diagnostic PPP implementation remains outside this package. Do not label
ServiceEndpoint as non-throwing or tick-based. Future process/ABI adapters must
catch exceptions; changing timing or result semantics requires parity tests.

## Connection lifecycle and host deadlines

connection_lifecycle.hpp is a server-side plaintext TCP policy component, not a
credential checker. It accepts trusted results from future host adapters and
transitions through hello wait, verification, active and closed. TCP acceptance
enters hello wait directly; no TLS API, certificate or TLS downgrade path exists.
The host must validate the first hello (including id=1), compare credentials,
reserve the endpoint, securely generate a fresh session and queue hello_ok.
The component binds the inbound guard starting at ID 2 only after approval.
No key is stored in this component. Calling approval with true is not itself
authentication and must never be exposed to an untrusted wire command.

Timing policy uses caller-supplied monotonic host milliseconds, never guest ticks:

- Five seconds total from TCP acceptance through hello/shared-key checking.
  Moving between handshake phases does not extend that deadline. This is an
  explicit conservative implementation policy; transport integration must verify it.
- Five seconds per outstanding open/close/advance request. One request slot;
  unrelated traffic cannot prolong it. Expected response type/body must be checked
  by the dispatcher before requestCompleted. Wrong correlation closes the connection.
- Ten seconds without a fully validated inbound message closes the connection.
  Outgoing traffic does not refresh liveness. Ping becomes due every two seconds;
  pingDue is passive and pingQueued acknowledges admission to the output queue.
- At the exact deadline the operation expires. Clock reversal closes the connection.
  Differences, rather than summed deadlines, avoid integer overflow near uint64 max.
- begin returns a monotonically increasing generation. All asynchronous results and
  timer calls must carry it. Stale callbacks do not touch the new connection.
  Generation exhaustion prohibits reuse. This component is owner-thread only.

Closing here only clears this component. The future host must close the transport,
cancel work, discard queued frames/call buffers, and release its endpoint reservation.
No timers are scheduled, pings sent or sockets closed by this header itself.
There is still no listener, credential comparison or game test.
With JSON enabled all 22 CTest cases pass, including four lifecycle cases using
synthetic timestamps (no sleeps). The authentication case tests policy transitions,
not cryptographic verification.

## Approved transport change: trusted LAN, no TLS

The user approved plaintext TCP on 2026-09-26. This supersedes the TLS/pinned
certificate requirement in the earlier LAN PDF v0.3; the PDF remains a historical
design, not the current transport requirement. See the transport-policy amendment
in outputs/ymir-xband-investigation/plain-tcp-lan-policy.md.
Wire v2 length framing and message bodies are unchanged, including auth_key.
The key provides a simple configured-peer check only: it is visible to anyone
who can observe traffic, and does not prevent interception, tampering or replay
by a network attacker. Generation/sequence checks protect application consistency,
not transport integrity. Never reuse an account password or other secret as this key.
Use fictional phone/card identifiers. No Internet exposure or port forwarding.
Future listener defaults to loopback; LAN binding requires an explicit local
interface choice, not wildcard binding. Do not automatically alter firewall rules.
No socket has been opened or firewall setting changed by this policy change.

## Provenance

ServiceEndpoint originated in this workspace's live-capture-tests header; framing
and standalone tests are newly authored for this project. No ROM, BIOS, card
ledger, key or real-user data is included. Review source licensing and dependencies
before distribution; no licensing permission is inferred by this extraction.
