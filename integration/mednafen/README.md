# Mednafen 1.32.1 XBAND integration kit (development)

Windows x64向けソース取り込みキットです。Mednafen本体・EXE・BIOS・ゲームDISC・個人データは含みません。公開済みv0.4.0は変更しません。本版はv0.5.0の公開開発版です。共通・ホスト固有の仕様は[統合仕様](../../docs/EMULATOR-INTEGRATION-SPEC.md)を参照してください。

## 共通部分とホスト境界

導入から起動・カード・ポイント確認までの詳細は[Mednafen組み込み・セットアップマニュアル](../../docs/MEDNAFEN-INTEGRATION-MANUAL.md)を参照してください。

`components/xband/include/xband/`のモデム、AT、UART、通信、カード制御とWindows画像保存を共用します。Ymirの保存ヘッダーは互換エイリアスです。`components/xband/adapters/mednafen/`はバス幅、SCU IRQ、時計、保存、所有スレッドの結線を担当し、C++20ランタイムをC ABIでC++11ホストへ接続します。VF/XOSの待機・正常帰還観測はMednafen向け互換処理で、Ymirへ無条件に移植しません。ゲームRAMには書き込みません。

## 適用とビルド

1. [公式Mednafen 1.32.1ソース](https://mednafen.github.io/releases/files/mednafen-1.32.1.tar.xz)を別ディレクトリへ展開します。アーカイブSHA-256: `de7eb94ab66212ae7758376524368a8ab208234b33796625ca630547dbc83832`。
2. PowerShellで `./integration/mednafen/apply-integration.ps1 -SourceDirectory C:/src/mednafen`。変更対象の元ファイルのハッシュと適用前後のパッチを検査します。別バージョン・改変済みソースは拒否し、reset・復元上書きはしません。
3. MSYS2 **UCRT64**のGCC、make、SDL2、zlib、FLAC、libiconv、gettext、Gitを用意します。nlohmann/json **3.12.0**のincludeを別途指定します。サーバーCMakeが取得する`build/_deps/json-src/include`も使用可です。
4. UCRT64 bashで `bash integration/mednafen/build-mednafen.sh /c/src/mednafen /c/build/mednafen-xband /c/deps/json/include 4`。出力はビルド先の`src/mednafen.exe`です。ホストへMSVC製ライブラリをリンクしないでください。

検証環境ではEXEの隣へ`SDL2.dll`、`libFLAC.dll`、`libogg-0.dll`、`libiconv-2.dll`、`libintl-8.dll`、`libwinpthread-1.dll`、`zlib1.dll`を配置して起動しました。実際の依存名は使用したMSYS2版のビルドに従います。各依存物の配布ライセンスを守ってください。GUIラッパー・RetroArchは不要です。Windowsの長いパス制限を避け、ソース・キット・ビルド先は短いパスへ置いてください。

## 起動と初期値

サーバーはルートの`start-server.ps1`、標準ポート58240～58245です。新規設定はメール送信1通1度数、通常対戦3、途中リセット各1を有効にします。新規ゲーム設定は勝者+1／敗者0です。現行ローカル方針で史実の料金・ポイントではありません。保存済み設定・カード残高・台帳は上書きしません。

左右は**別プロフィールと別の登録電話番号**で起動します。

```powershell
./integration/mednafen/start-mednafen.ps1 -Executable C:/build/mednafen-xband/src/mednafen.exe -Disc C:/games/vf.cue -JapaneseBios C:/bios/saturn-jp.bin -ProfileDirectory C:/profiles/left -Subscriber 0355555555 -Side 0
./integration/mednafen/start-mednafen.ps1 -Executable C:/build/mednafen-xband/src/mednafen.exe -Disc C:/games/vf.cue -JapaneseBios C:/bios/saturn-jp.bin -ProfileDirectory C:/profiles/right -Subscriber 0366666666 -Side 1
```

新規カードは挿入ON・100度数、左右異なる合成IDで作成します。既存カード・選択サイドカーは維持し自動補充しません。サーバーUIの50／100／任意0～32767とOKで交換します。通信中も交換可能で、挿入中なら1フレーム以上OFFを経てON、未挿入ならOFFを維持します。カード交換で回線は強制切断しません。

XBAND宛て番号は **0120-717360**（記号なし:0120717360）。端末自身の登録電話番号とは別です。カート設定名`xband-probe`は互換性のため維持しています。

## 自動試験と未実施項目

SH-2 SCI対戦ケーブルの[任意追加キット](LINK-CABLE.md)を用意しました。共通YLS1部品を利用し、基礎XBAND差分の後に別の追加差分を適用します。通常起動OFF・XBANDカート併用不可です。Daytona USA Circuit Editionの動作確認完了と、ゲボッカーズの安定動作がユーザーから報告されました。公開済みv0.4.0や既存XBAND試験環境を置き換えません。

```powershell
cmake -S components/xband/adapters/mednafen -B C:/build/xband-adapter -DXBAND_JSON_INCLUDE_DIR=C:/deps/json/include
cmake --build C:/build/xband-adapter --config Release
ctest --test-dir C:/build/xband-adapter -C Release --output-on-failure
```

v0.5.0はサーバーソースも同梱し、ZIPと完全リポジトリの双方でバス・カード・TCP/AT・回線・カード制御の3試験が有効です。個人メール履歴は同梱しません。

ユーザー操作で対戦成立、正常終了・切断、再対戦、メール本文を確認済みです。2026-10-11に通常各3度数（100→97）、勝者+1／敗者0、再接続で二重計上なしを確認し、メール送信課金・受信無料の試験完了も報告されました。切断だけから勝敗を推測しません。その後、通信ログ機能の確認完了もユーザーから報告されました。

保存状態ロードでは外部カード・通信を巻き戻しません。実カード認証、全タイトル、別PC通信、完全な物理モデム再現は未検証です。
