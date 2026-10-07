# 監視画面向けホスト状態API（開発中）

## 目的

`TcpHost::status()` は、サーバーが認識している接続状態と、モデムから届いた
状態通知をまとめたJSONのコピーを返します。ネットワーク公開APIではなく、
サーバーEXE内で監視画面へ情報を渡すための読み取り専用C++ APIです。
新しい待受ポート、HTTP、TLS、Windowsサービスは追加しません。

通信処理 → ServerSession → TcpHost::status() → コピーをUIへ渡す → 表示

既存の監視画面は標準では旧形式のログを読みます。開発用の`--host-status`を
指定すると、新APIの診断用出力を読むネイティブ画面へ切り替わります。
配布済みEXEやYmirのゲーム動作が今回の変更で切り替わることはありません。

## データ形式（version 1）

```json
{
  "version": 1,
  "running": true,
  "captured_at_ms": "1234",
  "pending_connections": 0,
  "endpoints": [
    {
      "endpoint": "modem1",
      "connected": true,
      "call_active": false,
      "peer_snapshot": null
    }
  ]
}
```

- `running`: サーバーの待受処理が停止していないこと。ゲームが動作中という意味ではありません。
- `captured_at_ms`: 最後のstepに渡した単調増加時刻。初期値は0。文字列化したミリ秒です。
  壁時計やモデムの通知受信時刻ではありません。これだけで通知の鮮度を判定しないでください。
- `pending_connections`: まだ接続手続きが完了していないソケット数。
- `endpoints`: 設定した名前の順に全モデムを列挙。未接続でも行は残ります。
- `connected`: hello手続きとサービス生成が完了した接続のみtrue。
  ソケットが開いただけではtrueになりません。
- `call_active`: 現在のcallが存在するか。実ゲームの対戦成立を保証しません。
- `peer_snapshot`: 最新の検証済みsnapshot本文。未取得、call切替直後、切断後はnull。
  nullは「未取得」であり、「カード未挿入」や「残高0」と解釈してはいけません。

`peer_snapshot` はwire v2のsnapshot本文をそのまま保持します。電話番号subscriber、
状態state、送受信量sent_bytes/received_bytes、cardなどは相手からの申告値です。
カードにはinserted、read_state、number、remaining_units、nominal_unitsがあります。
サーバーで測定した数値でも、カード台帳を更新する命令でもありません。
共有キーと接続sessionトークンは含めません。snapshot内のcall IDは含まれます。
電話番号やカード番号を含むため、標準で外部公開・永続ログ出力は行いません。
以下の診断オプションを指定して出力を保存する場合は、それらの情報も保存されます。

## 呼び出しと寿命

通信処理の所有スレッドで、stepの後に呼び出します。stepと同時に別スレッドから
呼んではいけません。返り値は独立したコピーなので、必要なら呼び出し側が
スレッド安全なキューでUIに渡します。UIでコピーを変更しても通信状態は変わりません。
コピーは接続が破棄されても有効ですが、過去の観測です。画面では新しいコピーに
置き換え、更新停止中のコピーを現在の接続状態として表示し続けないでください。

この呼び出しは通信、ゲスト時刻の進行、カード操作を実行しません。
メモリー確保に失敗すると例外が伝播します。UI側の取得処理で扱ってください。
状態をポーリングするだけでは、通信処理stepの代わりにはなりません。

## 検証範囲

localhostの実TCPで、未認証接続と認証後の区別、snapshotの受け渡し、コピーの独立性、
call切替時の通知消去、切断時の未接続表示、停止状態を試験します。
UI描画、別PCのLAN通信、ゲームの対戦成立はこの試験の対象外です。

## 診断用ネイティブ画面との接続

`xband_diagnostic_server` に `--status-stdout` を付けると、約500ミリ秒ごとと
正常終了時に次の1行形式を標準出力へ出します。デフォルトでは出しません。

```text
HOST_STATUS {"emitted_at_ms":1790000000000,"host":{...status()の返り値...}}
```

`emitted_at_ms`は診断EXEが出力したUnix時刻（ミリ秒、整数）です。
通信制御には使いません。画面は現在時刻から3秒以上古い、または未来なら
「更新停止／時刻不一致 — 最後の観測」と明示します。
これはホストの出力鮮度であって、peer_snapshotの更新鮮度ではありません。
別PCからログをコピーした場合は時計差に注意してください。

作業ディレクトリをcomponents/xbandとして、PowerShellでの開発用例:

```powershell
.\build\Release\xband_diagnostic_server.exe .\tests\fixtures\server-config.json 30000 --ephemeral-loopback --status-stdout > "$env:TEMP\xband-host-diagnostic.log"
```

通常のゲーム用ランチャーは接続しないでください。上記の設定は試験用です。
出力先は既存ファイルを上書きするため、必要に応じて別名にしてください。
Windows PowerShellのリダイレクトはUTF-16の場合があります。監視画面の入力は
UTF-8なので、実行時はPowerShell 7のネイティブ出力リダイレクトを使ってください。
標準出力が詰まると診断ループも停止し得ます。本番の非同期UI配信方式ではありません。

監視画面はoutputs/xband-card-managerを作業ディレクトリとして:

```powershell
python .\modem_monitor.py "$env:TEMP\xband-host-diagnostic.log" --host-status
```

このモードはカード台帳や旧モニターのカード設定ファイルを読みません。
新形式が不正・読み取り途中なら未取得に切り替え、古い接続済み情報に戻しません。
全設定モデムをスクロール表示します。検証用`--smoke-test`は短時間で画面を閉じます。
Python/Tkソースへの組み込み段階で、配布用監視EXEの再パッケージはまだです。
実診断EXEの出力読み取り、画面の生成と表示文言、旧モニターの回帰試験を実施しました。
ピクセル単位の外観確認、実ゲーム・別PC間の検証は未実施です。
