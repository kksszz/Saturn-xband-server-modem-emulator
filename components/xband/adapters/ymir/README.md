# Ymir modem attachment

`BoardAttachment` joins `ModemRegisterBank` and Ymir's SH-2 bus. It is the
shared implementation used by the `XBAND_YMIR_BUS_BINDING` game-test targets,
not a second diagnostic-only copy of address dispatch.

## Ownership and integration contract

The normal application must own one modem per Saturn instance:

```text
Emulation-thread owner
  modem register bank + AT/session state
  BoardAttachment -> BusBinding -> saved bus handlers
  Saturn -> mainBus -> BoardAttachment
  transport queues -> independent server (outside MMIO)
```

Declare bank/session and the attachment pointer **before** Saturn in the owner,
so Saturn is destroyed first. Install after the base bus mappings are ready and
before CPU execution. All callback state must outlive Saturn as well. The test
host explicitly destroys Saturn in its destructor body before callback storage.
The attachment destructor never accesses or restores a potentially dead bus.

Required hooks are board mode, UART read and UART write. The register bank is
borrowed. Optional hooks implement card-status overlay and diagnostic observation;
none of those hooks may synchronously wait for a socket or a UI thread. The
existing diagnostic card hook is still a diagnostic implementation, not the
final nonblocking card-storage adapter.

Unowned reads/writes retain the underlying cartridge/CD behavior and bus waits.
To match the proven game implementation, a normal read first samples the saved
handler, then applies the modem overlay. Writes are forwarded exactly once only
when not handled by the modem. Debug Peek/Poke use the saved debugger handlers:
they do not consume modem receive bytes, issue AT commands or invoke observers.
They currently do **not** offer a debugger view/edit of modem registers.

`setEnabled(false)` makes this layer transparent without undoing later mappings.
Call it only on the emulation thread at a stopped boundary. It does not cancel
transport, clear call state, remove the cartridge, or make reset/save-state
operations safe. The owner must close/reset the session first. Re-enabling must
not be used to resume an old live network session after a guest rewind.

## Remaining normal-application work

`xband/modem_command_session.hpp` now provides observed AT/service state handling:
line assembly, profile requests, pending dial, generation-checked CONNECT,
data/command mode, +++ and hangup/reset. Replies and payload are admitted through
bounded caller-owned queues. Dial/peer hooks enqueue intent only; no socket work
belongs in UART access. The owner captures `ticket()` when starting an open and
passes the same ticket to `connected()` only after open_ok. Reset invalidates old
tickets. +++ leaves carrier active; ATH0 clears it and pending receive bytes.

The peer-command interceptor is called only without an active service carrier
or pending service dial. Peer ringing/answer/clock grants still live outside this
component. The 60-frame escape guard is inherited from observed test behavior,
not established real-hardware timing. Guest reset/rewind must reset the session;
this component does not discover guest clock changes or persist cartridge state.
The diagnostic host retains its explicit synthetic busy-line route and trace
hooks outside the shared component. Do not interpret those fixtures as a complete
reimplementation of historical subscriber validation.

The shared Windows transport owner is now `xband/windows_modem_service.hpp`.
`ModemServiceClient` owns TCP and response staging with the correct destruction
order; it has no Ymir headers or dependency on the diagnostic program. It uses
the existing configurable plaintext loopback/explicit-LAN address policy.

The intended emulation-loop use is:

1. Call `step(hostMilliseconds)` on every scheduling iteration, including guest
   pause. It never sleeps or loops waiting for an acknowledgement.
2. Queue `open(subscriber, now)` only in `idle`. The dialed service number is
   resolved by the modem owner, not confused with the subscriber identity.
   Supply DCD/CONNECT only after `carrier()` becomes true.
3. At a service-time boundary call `transfer(bytes, guestTick)`. It copies the
   input. Hold the guest at that boundary until `readyToRun()`, then drain the
   response into available UART space. Continue processing app events while
   waiting. Host milliseconds and guest ticks are different time domains.
4. For guest modem commands ATZ/ATH, use `requestClose(now)`. It hides carrier
   and old responses immediately. If open/transfer is pending, finish that
   already admitted operation once, discard its response and queue close.
   Keep calling step; only after idle/close_ok may the next dial open. Do not
   issue guest CONNECT for the old open. No input is resubmitted or rolled back.
   `close(now)` retains the strict completed-call contract: calling it during
   pending open/transfer terminates the connection. Emulator reset/rewind,
   disable and shutdown still call `stop()` without waiting, not requestClose.

`stop()` clears staging before destroying TCP/control state. A transport failure
also drops staging; callers must clear their own UART/call state and report the
loss. There is no automatic reconnect. The diagnostic PB3 driver still wraps
these nonblocking steps in a bounded wait; **the normal App must not copy that
wait loop**. The old `--reuse-batch` performance comparison is retired because
staging now belongs to this connection owner.

`requestClose()` returning true means a close intent was accepted, not that
close_ok arrived. `closePending()` reports this boundary; `carrier()`,
`readyToRun()`, `open()` and `transfer()` prevent the old call from being exposed
or reused while it is pending. Existing host-time request deadlines and fatal
transport/protocol checks still apply. Server-side effects already accepted
before hangup cannot be undone; this is not a mail delivery/rollback guarantee.

The opt-in regular SDL integration is now `frontend_modem.hpp/.cpp`. Configure
`Ymir_XBAND_COMPONENTS_DIR` to the absolute components/xband directory to build
it. The App owns this attachment before SharedContext/Saturn, pumps it on the
emulation thread (also while paused), and intersects scheduling with the existing
battle-cable callback. UI request/snapshot operations are mutex-protected; UART
hooks perform no socket waits. Reset/load-state invalidates the session instead
of reusing a stale call. The Serial Port panel contains session-only controls.

The SDL integration explicitly stops the modem after a successful slot-state
load, undo-load, or rewind-state load. A failed load leaves the current modem
session alone. Stopping does not restore the historical network call: reconnect
both emulator endpoints with a fresh server session before continuing XBAND.
Any UI connect request still queued at that boundary is discarded as well.
The modem state is not part of Ymir's save-state format. These hooks are guarded
by `YMIR_XBAND_FRONTEND` so a regular Ymir build remains unaffected.
The focused `frontend_modem_reset_test` verifies pending-request cancellation,
then, with `--loopback` and the current `xband_frontend_server`, checks an
actual Saturn save-state load: an invalid disc hash leaves the live connection
alone; valid loads on both Saturn instances followed by SDL reset boundaries
close both connections; and explicit requests reconnect both modem instances.
Two restore/reconnect cycles passed on 2026-09-30 with the resets staggered by
600 ms in both orders. Server snapshots show each one-sided gap and both
endpoints connected again. This is a core/owner regression, not a VF REMIX or
SDL UI test.

This remains a preview: two fixed subscriber numbers, test server identity/key,
loopback server, no general reconnect or persistent modem configuration. The
normal executable builds and its two DecAthlete endpoints join the server, but
normal-GUI game login and competition start have NOT been verified. See
`outputs/ymir-modem-frontend-preview/README.md` in the workspace for evidence and
the Windows screen-capture blocker. Do not replace the distributed cable build.

`frontend_game_replay` now exercises this exact owner with a real DecAthlete
disc, COW backup and standard controller-input fixtures, without SDL. It is not
the older diagnostic modem and does not patch guest RAM. It reaches the service
profile exchange and original peer dial/ATA, then transfers peer bytes. This
still does not establish normal-GUI operation or competition start.

When pump() returns false, the scheduling loop may call waitForActivity() once.
It registers both connections with the existing transport readiness API and
uses a single select with a requested 1 ms timeout; OS scheduling can delay its
return. Buffered/outgoing work returns immediately. It has no transaction wait
loop, no guest execution, no changed clock grants and no global timer changes.
Process UI events again on the next iteration, including disconnect/shutdown.

The focused `ymir_board_attachment_test` checks routing, debugger isolation,
bus waits, transparent disable/re-enable and destruction order. PB3 short replay
checks the real guest login/dial/answer path; it is not a full-match acceptance
test or a claim of normal SDL application integration.
# 対向UARTタイミング補足（2026-09-27）

`frontend_modem.cpp` は対向接続時に `xband/peer_uart_timing.hpp` を使います。
保持FIFO16バイトとシフトレジスターを分け、FCR送信クリアで転送中の1バイトを
消さない設計です。RX割り込みはFIFO閾値または受信タイムアウトで通知します。
時間源はゲストの経過サイクルで、UARTレジスターアクセス内にネットワークI/Oはありません。
18750/75000サイクルの値は旧カタログ検証から引き継いだ実験値であり実機仕様ではありません。
サービス側の通信は従来どおりで、対象は対向接続後のみです。
移植時はこのヘッダーとゲスト時間源を維持してください。

### 非同期ポンプと待機の契約

クライアントの `ModemServiceClient::appendWaitSockets()` とサーバーの
`TcpHost::appendWaitSockets()` がfalseを返す場合、ソケット待機へ入らず
次の有限なstepを実行してください。通信要求を保持した直後やadvanceを受理した
直後には、まだ送信キューに現れないローカル処理が残ることがあります。
サーバーでは `ServerSession::needsPoll()` がこの状態を表します。
待機可否は実時間のスケジューリングのみで、ゲストの同期幅やUARTの時計を変更しません。
この契約を無視すると、処理のたびに不要な待機が入りゲーム内時間の進行が極端に遅くなります。
