# Mednafen 1.32.1 SCI対戦ケーブル追加モジュール（実験版）

2026-10-12更新。XBANDカートリッジとは別経路です。SH-2内蔵SCIを、同一PC上の
左右2プロセス間で直接TCPまたは任意選択の共有メモリーで接続します。サーバー・電話番号・メディアカード・ポイントは
使用しません。試験タイトルはユーザー指定の **Daytona USA Circuit Edition（日本版）**。
ユーザーからDaytona USA Circuit Editionの動作確認完了とゲボッカーズでの安定動作が報告されました。個別の完走・再対戦・リセット項目をすべて確認したという意味ではありません。

## 取り込み・ビルド

1. [基礎キット](README.md)の手順で、別途展開した公式1.32.1へ`apply-integration.ps1`を適用します。
2. `./integration/mednafen/apply-link-cable.ps1 -SourceDirectory C:/src/mednafen`を実行します。
   基礎差分適用後の5ファイルのSHA-256、適用前後、再適用を検査します。
3. UCRT64の`build-mednafen.sh`を**別の新しいビルド先**で実行します。追加差分があれば
   共通TCPソース・C ABIをリンクし、`XBAND_MEDNAFEN_LINK_CABLE=1`を定義します。
   MSVCライブラリは流用しません。

通常起動ではケーブルOFF。XBAND用`start-mednafen.ps1`も明示的にOFFにします。
専用起動は`ss.cart auto`を使い、XBANDモデムカートとの同時使用を拒否します。
既存のXBANDカード・設定・メール・台帳は変更しません。

## 左右の起動

DLL・日本BIOS・正しいCUEと全トラックを用意し、左右でプロフィールを共用しません。

```powershell
./integration/mednafen/start-link-cable.ps1 -Executable C:/build/cable/src/mednafen.exe -Disc C:/games/daytona-ce.cue -JapaneseBios C:/bios/saturn-jp.bin -ProfileDirectory C:/profiles/daytona-left
./integration/mednafen/start-link-cable.ps1 -Executable C:/build/cable/src/mednafen.exe -Disc C:/games/daytona-ce.cue -JapaneseBios C:/bios/saturn-jp.bin -ProfileDirectory C:/profiles/daytona-right
```

両側とも標準ポート32458、同期ON。先に起動した側がローカル待受、後から起動した側が
接続します。変更時は両側へ同じ`-Port`を指定します。`-Synchronize 0`は非同期比較用で、
両側を揃えてください。本ランチャーは同一PC用で、外部ホスト指定・専用GUIは未追加です。

### 同一PC用の低遅延方式（開発版）

既定は互換性を維持した`-Backend tcp -Quantum 4096`です。共有メモリーは両側の
起動引数に`-Backend shared -Quantum 128`を追加して試せます。共有方式では
`-Port`はTCPポートではなくペアを区別するチャンネル番号です。同じWindowsログオン
セッション内で、両側のチャンネル・方式・Quantumを一致させてください。

`-Quantum`は64／128／256／512／1024／2048／4096サイクルから選択できます。
共有方式も既定は4096で、同期ONのみ対応します。TCP方式のQuantumは4096固定です。
送信サイクルにQuantumを加えた時点から相手のSCIへ渡すため、128では追加の仮想遅延が
4096の1/32になります。SCIのボーレートやフレーム時間を速める設定ではありません。

小さなQuantumは同期待機の頻度・CPU負荷や、命令／DMAの実行超過に影響します。
ゲームで不安定になる場合はQuantumを大きくするか、既定のTCPへ戻してください。
初期化修正版は同期用時計をマスタークロック÷65の基準tickへ換算します。
通常のCPU速度切替で回線とキューを破棄しません。SCIのボーレート・フレーム時間は
引き続きSH-2サイクルを使い、映像方式の不一致・リセット／状態ロードは別に検査します。
両側とも更新した本体差分が必要で、旧取込済み本体はクリーンな基礎差分適用後へ
新しいSCI差分を適用して再ビルドします。共有方式／4096／同期ONでの実ゲーム確認結果は最適化検証記録にまとめています。
共有方式は進行待ちが30秒を超えるとエラーになります。片側の終了・リセット・時計の
不一致でもセッションを無効化し、左右とも再起動が必要です。TCP側の待機仕様は変えていません。
測定条件と確認範囲は[最適化検証記録](LINK-OPTIMIZATION-VERIFICATION.md)を参照してください。
この追加と固定通信時計の初期化修正はv0.5.1 ZIPに収録します。従来のSCI差分を適用済みの場合は、基準ソースから新しい差分で取り込み直してください。

音声出力の引っ掛かりがある場合は、両プロフィールで `sound.buffer_time 64` を比較できます。Daytona CEで改善・レース終了をユーザー確認しました。設定値のみの調整で、音声遅延は増えます。

ログの`[Link cable] Connected to peer`を確認後、ゲーム内の対戦ケーブル用モードへ
進みます。TCP接続確認はゲーム内対戦成立とは別です。通信が変化すると2秒間隔で
TX/RX/delivered/pendingの累計を表示します。サーバーUIの通信ログはこの直接SCI経路を表示しません。

片側の一時停止・非アクティブ時停止・負荷は同期相手を待たせます。映像・CPUクロックを一致させます。
終了はウィンドウを閉じる操作で可能です。ハードリセット／状態ロード後は現在**左右とも再起動**が必要です。
SMPCのソフトリセットボタンNMIによる切断は未結線なので、こちらも左右を閉じて再起動してください。
ケーブルをまたぐセーブステート再開は保証しません。

## ホスト側の追加箇所

| 部分 | 結線 |
| --- | --- |
| `sh7095.h/.inc` | 両SH-2の8ビットSCI、送受信タイミング、DRCR、バイトDMA、IPRB/VCRA/VCRB、状態保存 |
| `ss.h/.cpp` | 共通状態、64サイクルのpoll目安と選択方式の同期予算、単調時計、映像／CPUクロック、初期化・終了・リセット・状態ロード |
| `drivers/main.cpp` | 所有ゲームスレッドの状態poll、終了時の同期待機キャンセル |
| `cable_runtime.*` | C++11ホスト／C++20共通TCP・共有メモリーのC ABI、環境設定、ローカル自動ペア、予算キャッシュ、診断表示 |

物理波形・外部クロック・全DMAモードの完全再現ではありません。CPU命令・DMAの
まとまった実行によるスライス超過やゲーム固有の相性は実ゲーム試験が必要です。
共通化済みなのは追加側のTCP部品・SCI状態で、既存YMIR差分は変更せず維持しています。

## 診断試験

共通自動試験は[README](../../components/link-cable/README.md)。本体結線試験は、
新しい診断専用プロフィールでケーブル環境に`XBAND_MEDNAFEN_CABLE_CORE_TEST=1`を
追加して起動します。BIOS/DISC初期化後、ゲーム実行前にレジスター・タイミング・
ネイティブ割り込みベクター・バイトDMA・SCIリセットを試験し、
`LINK_CORE_SELFTEST_PASS`／`FAIL`を出してロードを意図的に終了します。
成功もロードエラー扱いなので終了コードだけで判定しません。通常起動では変数を設定しません。
確認済み・未確認項目は[検証記録](VERIFICATION.md)に記載します。
