# Mednafen 1.32.1 SCI対戦ケーブル追加モジュール（実験版）

2026-10-11。XBANDカートリッジとは別経路です。SH-2内蔵SCIを、同一PC上の
左右2プロセス間で直接TCP接続します。サーバー・電話番号・メディアカード・ポイントは
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
| `ss.h/.cpp` | 共通状態、64サイクルのpoll目安と4096サイクル同期境界、単調時計、映像／CPUクロック、初期化・終了・リセット・状態ロード |
| `drivers/main.cpp` | 所有ゲームスレッドの状態poll、終了時の同期待機キャンセル |
| `cable_runtime.*` | C++11ホスト／C++20共通TCPのC ABI、環境設定、ローカル自動ペア、診断表示 |

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
