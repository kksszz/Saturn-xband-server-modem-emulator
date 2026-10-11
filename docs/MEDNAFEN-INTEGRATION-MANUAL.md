# MednafenへのXBANDモデム組み込み・セットアップマニュアル

更新日：2026-10-11。Windows x64のMednafen 1.32.1へ、本プロジェクトの共通モデム部品とMednafen専用アダプターを取り込み、独立したXBANDサーバーへ接続するための手順です。現行ソースと[検証記録](../integration/mednafen/VERIFICATION.md)に基づいて説明します。

対象はv0.5.0のソース統合開発版です。公開済みv0.4.0にはMednafenキットは含まれません。[v0.5.0の配布ZIP](https://github.com/kksszz/Saturn-xband-server-modem-emulator/releases/tag/v0.5.0)または同じタグのソースを取得し、以下のファイルが揃っていることを確認してください。

基本構成は、同一PC上のサーバー1つと、独立したMednafenの左右2プロセスです。現行アダプターの接続先IPv4は`127.0.0.1`固定です。別PC・LAN・インターネット接続、全タイトル、実カード認証、完全な物理モデム互換は確認範囲外です。RetroArchやGUIラッパーは不要です。

## 1 最初に確認すること

本キットはソース統合用であり、通常版Mednafenへ追加DLLを置くだけで使えるプラグインではありません。通常版へサーバーのアドレスを設定するだけでも接続できません。利用者が別途用意したMednafen 1.32.1ソースへ付属差分を適用し、対応版をビルドします。

キットには共通部品、Mednafenアダプター、差分、適用・ビルド・起動スクリプトを含めます。Mednafen本体・組み込み済みEXE、BIOS、ゲームDISC(ROM)、個人プロファイル・カード・履歴は配布物に含めません。既に対応版をビルド済みの場合は4章から進めます。旧版を更新する場合も、サーバーとエミュレーター側部品の双方の版を合わせてください。

既存の通常利用環境を上書きせず、ソース・ビルド・左右プロファイル・カードを分離します。コマンドが失敗した場合は次へ進まず、原因を解消してください。

## 2 組み込みに必要なもの

| 用意するもの | 用途 |
| --- | --- |
| 本プロジェクトのv0.5.0ソースまたは統合キットを含むZIP | 共通部品、ホストアダプター、差分・スクリプト |
| Mednafen 1.32.1の未改変公式ソース | エミュレーター本体 |
| PowerShell、Git、tar/xz展開環境 | 差分検査・適用、ソース展開 |
| MSYS2 UCRT64のGCC、make、SDL2、zlib、FLAC、libiconv、gettext | Windows x64本体のビルド |
| nlohmann/json 3.12.0のincludeディレクトリー | C++20共通ランタイムのビルド |
| BIOS、XBAND対応ゲームDISC(ROM) | ゲーム起動。利用者が別途用意 |
| 左右別プロファイルと登録電話番号 | 保存領域・アカウントの分離 |
| CMakeとWindows C++開発環境（任意） | 本体とは別のアダプター回帰試験 |

公式ソースアーカイブ：[mednafen-1.32.1.tar.xz](https://mednafen.github.io/releases/files/mednafen-1.32.1.tar.xz)。本キットの基準SHA-256は以下です。

```text
de7eb94ab66212ae7758376524368a8ab208234b33796625ca630547dbc83832
```

検証ではUCRT64 GCC 16.2.0で公式ソースからのクリーンビルドを確認しました。別バージョンへ同じ差分をそのまま適用できる保証はありません。Windowsの長いパス制限を避け、例のような短い保存先を使用してください。

## 3 Mednafenへの取り込み方（開発者向け）

### 3.1 部品の場所と責務

| 場所 | 内容 |
| --- | --- |
| `components/xband/include/xband/` | 共通モデムレジスター、AT、UART、転送、カード・画像保存部品 |
| `components/xband/adapters/mednafen/requested_bus_bridge.hpp` | A-bus分割前の要求幅1／2／4バイトとアクセス所有権 |
| `components/xband/adapters/mednafen/xband_probe.inc` | バス、SCU IRQ、時計、フレーム、保存領域の結線 |
| `components/xband/adapters/mednafen/command_runtime.h`、`.cpp` | C ABIとC++20通信ランタイム |
| `components/xband/adapters/mednafen/card_runtime.h`、`.cpp` | C ABIと外部カードの永続化・交換 |
| `integration/mednafen/mednafen-1.32.1-xband.patch` | Mednafen本体の7ファイルを変更する付属差分 |
| `integration/mednafen/baseline-files.json` | 基準アーカイブと変更対象ファイルのハッシュ |

サーバーの待機・マッチング・メール・結果・ポイント・度数精算・管理画面はYMIR版と共通です。ホスト固有のバス・IRQ・時計・保存・実行ループへの結線だけを専用アダプターへ分離します。Mednafen本体はC++11、ランタイムは別オブジェクトとしてC++20でビルドし、C ABIで接続します。MSVC製ライブラリをUCRT64のMednafen本体へリンクしないでください。

詳細は[統合仕様](EMULATOR-INTEGRATION-SPEC.md)と[Mednafenアダプター説明](../components/xband/adapters/mednafen/README.md)を参照してください。ファイルのコピーだけでは取り込みは完了しません。

### 3.2 基準ソースの確認と差分適用

次の例では、手元の開発キットを`C:/XBAND/kit`へ置き、取得した公式アーカイブを`C:/XBAND/mednafen-1.32.1.tar.xz`へ保存してあるものとします。パスは実際の保存先に置き換えてください。既存ソースの上へ展開しないでください。

```powershell
$xbandKit = 'C:/XBAND/kit'
$mednafenArchive = 'C:/XBAND/mednafen-1.32.1.tar.xz'
$mednafenExtract = 'C:/XBAND/src'
$mednafenSource = "$mednafenExtract/mednafen"

$expectedHash = 'de7eb94ab66212ae7758376524368a8ab208234b33796625ca630547dbc83832'
if ((Get-FileHash -LiteralPath $mednafenArchive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash) {
    throw 'Unsupported or damaged Mednafen source archive'
}
if (Test-Path -LiteralPath $mednafenExtract) {
    throw 'Use a new extraction directory; existing files are preserved'
}
New-Item -ItemType Directory -Path $mednafenExtract | Out-Null
tar -xf $mednafenArchive -C $mednafenExtract
if ($LASTEXITCODE -ne 0) { throw 'Source extraction failed' }

& "$xbandKit/integration/mednafen/apply-integration.ps1" -SourceDirectory $mednafenSource
```

公式アーカイブの展開先が`mednafen`であることを確認します。適用スクリプトは7対象ファイルのハッシュ、`git apply --check`、適用後の逆差分検査を行います。適用済みの同一差分は検出して再適用しません。改変済み・非対応ソースは拒否し、resetや復元上書きは行いません。別バージョンへ無理に適用する`--reject`や`--3way`は使用しないでください。

Windows付属のtarが`xz`フィルター不足で展開できない場合は、アーカイブのハッシュ確認後、MSYS2 UCRT64で展開します。失敗後の既存フォルダーへ重ねず、新しい空の展開先を使い、`$mednafenSource`も合わせて変更してください。展開後にPowerShellへ戻って適用スクリプトを実行します。

```bash
mkdir /c/XBAND/src-ucrt
tar -xf /c/XBAND/mednafen-1.32.1.tar.xz -C /c/XBAND/src-ucrt
```

### 3.3 UCRT64でビルドする

MSYS2の**UCRT64**シェルを開き、2章の依存関係を準備します。MINGW64/MSYS用の別ABIのライブラリを混ぜないでください。nlohmann/jsonの指定先には`nlohmann/json.hpp`が必要です。完全リポジトリでサーバーをCMake構成済みなら、`build/_deps/json-src/include`も利用できます。v0.5.0 ZIPにはサーバーのソースも含み、展開先でサーバーのCMake構成を実行すればこのincludeを取得できます。

```bash
bash /c/XBAND/kit/integration/mednafen/build-mednafen.sh \
  /c/XBAND/src/mednafen \
  /c/XBAND/build-mednafen \
  /c/XBAND/deps/json/include \
  4
```

引数は順に、差分適用済みソース、専用ビルド先、JSONのinclude、並列ジョブ数です。スクリプトは逆差分の検査、Mednafenのinclude配置、configure、C++20ランタイムのコンパイル、本体ビルド・再リンクを行います。サターン用に構成し、他システムを無効にします。このビルド先を既存の別構成と共用しないでください。

出力は`C:/XBAND/build-mednafen/src/mednafen.exe`です。これは利用者のローカルビルド成果物で、サーバーの配布ZIPへ追加しません。ゲームとサーバーはビルドスクリプトから起動されません。

### 3.4 実行時依存と最低限の起動確認

UCRT64のPATHがない環境から起動する場合は、対応するDLLをEXEの隣へ配置します。検証では次を使用しました。実際のDLL名・依存関係は使用したMSYS2のビルドに従い、配布時は各ライセンスを守ってください。

```text
SDL2.dll
libFLAC.dll
libogg-0.dll
libiconv-2.dll
libintl-8.dll
libwinpthread-1.dll
zlib1.dll
```

`mednafen.exe -help`の出力と起動ログを確認します。検証した1.32.1のhelpのみの経路は終了コード-1を返すため、終了コードだけでDLL不足と判断しないでください。help表示はゲーム・通信の動作試験ではありません。

## 4 Windows配布サーバーの起動

Mednafen対応キットを含む候補ZIPを新規フォルダーへ展開し、そのルートでPowerShellから起動します。完全リポジトリの場合は、先にサーバーをビルドしておきます。

```powershell
.\start-server.ps1
```

通信モニターが開きます。標準ベースポートは`58240`で、実際には58240～58245の6ポートを使います。別サーバーと重複させないでください。起動スクリプトはMednafenを自動起動しません。サーバーの保存先はキットルートの`runtime`です。

v0.5.0公開後の現行ソースでは、サービスendpointは左右それぞれ`service-0`／`service-1`です。更新したアダプターは更新したサーバーと組み合わせます。更新サーバーは旧`pb3-0`／`pb3-1`も同じ端末の別名として受け付けるため、旧アダプターの接続は維持します。対戦・管理endpointとポートは変更せず、公開済みv0.5.0のZIPも変更しません。[互換仕様](SERVER-MODEM-SPEC.md#サービスendpointの名称と旧版互換)を参照してください。

ポート変更例：

```powershell
.\start-server.ps1 -BasePort 58440
```

この場合は、5章の左右の起動にも`-BasePort 58440`を指定します。起動スクリプトはサーバー終了まで待機するため、Mednafen起動には別のPowerShellを使用します。

## 5 左右のMednafenを準備する

左右は同じ対応版EXE・同じ対戦タイトルを使い、プロファイルと登録電話番号を分けます。下記の番号はローカル試験用の端末アカウントで、実際の電話網へ発信しません。

```powershell
$xbandKit = 'C:/XBAND/kit'
$mednafenExe = 'C:/XBAND/build-mednafen/src/mednafen.exe'
$gameDisc = 'C:/XBAND/games/vf-remix.cue'
$japaneseBios = 'C:/XBAND/bios/saturn-jp.bin'

& "$xbandKit/integration/mednafen/start-mednafen.ps1" `
    -Executable $mednafenExe -Disc $gameDisc -JapaneseBios $japaneseBios `
    -ProfileDirectory 'C:/XBAND/profiles/left' -Subscriber 0355555555 -Side 0

& "$xbandKit/integration/mednafen/start-mednafen.ps1" `
    -Executable $mednafenExe -Disc $gameDisc -JapaneseBios $japaneseBios `
    -ProfileDirectory 'C:/XBAND/profiles/right' -Subscriber 0366666666 -Side 1
```

左右で同じ`MEDNAFEN_HOME`やカードファイルを共有しないでください。スクリプトは`-ss.cart xband-probe`と`-ss.bios_jp`を指定します。`xband-probe`という設定名は互換性のため残しています。通常版にはこのカート設定はありません。

| 設定 | 左側 | 右側 |
| --- | --- | --- |
| `Side`／モニター上の端末 | `0`／MODEM 1 | `1`／MODEM 2 |
| `Subscriber`の例 | `0355555555` | `0366666666` |
| `ProfileDirectory`の例 | `C:/XBAND/profiles/left` | `C:/XBAND/profiles/right` |
| 標準サービス／対戦／管理ポート | `58240`／`58242`／`58244` | `58241`／`58243`／`58245` |

スクリプトは子プロセスへ`MEDNAFEN_HOME`、`XBAND_MEDNAFEN_CARD`、`XBAND_MEDNAFEN_SERVICE_PORT`、`XBAND_MEDNAFEN_SUBSCRIBER`、`XBAND_MEDNAFEN_SIDE`を渡し、親PowerShellの元の値は復元します。直接EXEだけを起動しても同じサービス・カード設定にはなりません。

通常のゲーム起動、BIOS、コントローラー設定を先に確認します。左右の操作を間違えないよう、入力設定は各プロファイルで調整してください。

## 6 ゲーム内の電話設定と接続

1. ゲーム内の端末自身の電話番号を、起動時の`Subscriber`と一致させます。
2. プレイヤー情報・コードネームなどを登録します。
3. メールの接続操作でサーバーへの接続・メニュー帰還を確認します。
4. 左右で対戦開始を選び、待機・マッチング・対戦への移行を確認します。
5. 対戦終了後、通常メニューへ戻り、通話が終了して次の接続が可能か確認します。

XBAND宛ての番号は**0120-717360**（数字のみでは`0120717360`）です。端末自身の登録電話番号、内部の仮想対戦ルーティング番号、TCPポートとは別です。本人番号として設定しないでください。

サーバーUIの`Connected to XBAND Server`はサーバーへの転送接続、`Connected to peer`は対戦相手との接続を示します。ROM画面の「XBANDに接続成功！」だけでは、その後のメール・精算・対戦処理まで成功したとは判断しません。入力やメニューの詳細は各ゲームの説明書に従ってください。

## 7 仮想サターンメディアカード・ポイント・度数

### 7.1 カードの初期状態と交換

新規プロファイルでは、起動スクリプトが指定する`cards/synthetic-100.bin`を、100度数・挿入ONの合成カードとして作成します。左右は異なる合成IDを使用します。既存カードと残高は保持し、再起動や挿抜で100へ補充しません。

サーバーUIの「仮想サターンメディアカードの差し替え...」で50／100／任意の**0～32767**を選び、「OK」で新しい独立カードを適用します。既存ファイルの選択・直接補充ではありません。元カードを保持し、通信中も交換できます。挿入中なら1フレーム以上OFFを経てON、取り外し中ならOFFを維持します。交換そのものでは回線を強制切断しません。ただし精算途中の交換はカード不一致・結果不明になり得るため、課金試験では避けてください。

Mednafen版は交換先の絶対パスを、起動カードの`.selection.json`サイドカーへ保存して再起動後も選択を保持します。選択先が欠落・破損した場合は、自動補充して回復させません。YMIR版のセッション限定の交換先保持とは異なります。

外部カードの残度数はゲームのセーブステートに含めず、ロードしても巻き戻しません。これはモデムフラッシュまで状態復元から除外するという意味ではありません。

### 7.2 新規設定の既定値と適用時点

| 項目 | 現行の新規設定 |
| --- | --- |
| 度数消費 | `start-server.ps1`経由で有効 |
| 通常対戦 | 両端末各3度数 |
| 途中リセット | 両端末各1度数 |
| メール送信 | 1通1度数 |
| メール受信のみ・接続のみ | 無料 |
| 標準10ゲームの勝敗ポイント | 勝者+1／敗者0 |

保存済み設定は無効設定も含めて保持します。UIの「消費度数設定」とゲーム別配点を確認してください。サーバーEXE単体の設定なし初期値や公開済みv0.4.0の初期状態と、現行起動スクリプトの新規設定を混同しないでください。これらはローカル運用方針で、当時の公式料金・ポイントを確定したものではありません。

終了直後のゲーム画面だけでは、サーバーへの結果報告・精算が未反映の場合があります。必要に応じて次のメール／サービス接続を完了し、ゲーム内表示と履歴・台帳を確認します。通常対戦だけなら100→97、さらにメールを1通送れば追加1度数です。送信なしの接続で追加1度数になるわけではありません。切断だけから勝者を推測しません。

詳細は[サーバーUI操作マニュアル](SERVER-UI-MANUAL.md)と[サーバー／モデム仕様](SERVER-MODEM-SPEC.md)を参照してください。

## 8 検証手順と確認範囲

### 8.1 アダプター単体の自動試験

Windows C++開発環境とCMake 3.20以上がある場合、Mednafen本体とは別に次の回帰試験を実行できます。MSVCによるこの試験用ライブラリを、UCRT64本体へ流用しないでください。

```powershell
cmake -S C:/XBAND/kit/components/xband/adapters/mednafen `
    -B C:/XBAND/build-adapter `
    -DXBAND_JSON_INCLUDE_DIR=C:/XBAND/deps/json/include
cmake --build C:/XBAND/build-adapter --config Release
ctest --test-dir C:/XBAND/build-adapter -C Release --output-on-failure
```

v0.5.0 ZIPと完全リポジトリの双方で、バス・カード・共通サーバーを使うTCP/AT・回線・カード制御の3試験が対象です。旧dev.4までのZIPは2試験でした。これは本体ビルド・実ゲーム試験の代わりではありません。

### 8.2 手動試験の確認項目

- 左右の別端末・別プレイヤー・別カードが表示される。
- メール送受信・本文表示ができ、送信1通だけ課金される。
- 待機から対戦へ移行し、正常終了後に通話が切断され、再対戦できる。
- 次のサービス接続後に各3度数と勝者+1／敗者0が反映される。
- 再接続だけではポイント・度数が二重計上されない。
- カード交換のOK適用、挿抜・読取異常、再起動後の選択保持を確認する。

2026-10-11の同一PC上のVF Remix試験では、対戦成立、正常終了・切断、再対戦、メール本文、各100→97、勝者+1／敗者0、再接続で二重計上なしをユーザー操作で確認しました。メール送信課金・受信無料の試験完了も報告されています。dev.4サーバーの自動試験は60/60成功しています。その後、ユーザーから通信ログ機能の確認完了が報告されました。全タイトル・別PC・実カードの保証ではありません。

## 9 困ったとき・通信ログ・終了とバックアップ

| 症状 | 確認すること |
| --- | --- |
| `xband-probe`が使えない | 通常版・差分未適用・別のEXEを起動していないか |
| 本体のビルド失敗 | 1.32.1基準、差分検査、UCRT64、JSON include、依存関係、短いパス |
| EXEが起動しない | 対応DLL、BIOS、DISCとCUE参照先、起動ログ |
| サーバーへ接続できない | サーバー起動、BasePort、Side、6ポートの重複、起動スクリプト使用 |
| 電話番号のエラー | ゲーム内本人番号とSubscriberの一致、入力桁数。XBAND宛て番号との混同 |
| 左右に同じプレイヤーが出る | プロファイル・Subscriber・保存領域の共用 |
| カードが読めない・状態取得不可 | 対応版、管理接続、挿入・読取異常、選択先、保存失敗。補充で隠さない |
| ポイント・度数が変わらない | 対戦結果報告と次のサービス接続、保存済みの有効設定・配点、精算履歴 |
| 接続成功後にエラー・停止 | 画面と通信履歴を保全。台帳削除・カード再作成で回避しない |
| 状態ロード後に再接続できない | 古い外部通信は復元しない。新しい接続で再試行 |

HEX/Text右側の`View communication logs...`で、現在のサーバー起動分のService／Peer／Control、端末、TX／RXを絞り込めます。検索は表示中ページのみです。200記録単位のページ切替、Auto-scroll、Save page、Open log folderを利用できます。全記録は`runtime/communication-logs`へ起動ごとにJSONLで保存します。過去の未記録通信は復元できません。

これはゲスト／サービス接続境界の診断ログで、物理回線波形やエミュレーター内の全AT処理・外側TCP認証フレームまで記録するものではありません。同じバイトを複数境界で観測しても、配送確認や二重送信を直ちに意味しません。保存失敗時は不完全なログとなります。メール本文・電話番号等を含み得るため、公開前に内容を確認してください。

終了時はゲームの通信を終えて左右のMednafenを閉じ、次にサーバーの通信モニターを閉じます。バックアップは停止後に、サーバーの`runtime`全体、左右のプロファイル全体、交換先を含むカードファイルとサイドカーをまとめて保全します。外部フォルダーのカード選択先も忘れないでください。カードだけ／精算台帳だけを古い状態へ戻すと整合性を失います。

## 10 ソース取り込みの詳細境界

通常は3章の差分を使用します。以下を重ねて手動追加する必要はありません。上流更新へ移植する場合の点検箇所です。

| Mednafen変更箇所 | 必要な結線 |
| --- | --- |
| `src/ss/cart.h`、`cart.cpp` | カート種別、要求所有権・アクセス、フレーム・リセットボタン観測、初期化・終了 |
| `src/ss/scu.inc` | A-bus分割前の元の要求幅を渡す。元の物理アクセス時間は維持し、UARTを二重に読み出さない |
| `src/ss/ss.h`、`ss.cpp` | 単調なモデム時計、読み取り専用RAM観測、完了フレーム、`ss.cart`設定 |
| `src/ss/smpc.cpp` | 実際に受理された本体リセットボタンNMIを観測 |
| `src/drivers/main.cpp` | 所有ゲームスレッドから通信をpoll。映像同期待ち・一時停止中も要求処理経路を維持 |

SCU割り込みは`SCU_INT_EXT0+12`へ接続します。フラッシュはMednafenのNVRAM保存機構で`xbf`拡張子を使用します。通常カードは別の13バイト外部画像です。MMIO内でソケット応答・ファイル保存を同期的に待たず、保存や管理要求は所有スレッドのフレーム／poll経路で処理します。

リセット・状態ロードではUARTと通信ランタイムをリセットし、古い通話・要求を再開しません。状態ロードでも外部カード残数は復元しません。終了時はカード保存の成功を確認して各ランタイムを破棄します。

VF/XOSの待機・正常帰還観測はMednafen固有の互換処理です。検証付き読み取り専用RAM観測、対象通話チケットのUART帰還列、基板ID読取などを照合します。ゲームRAMを書き換えず、メニューの経過時間・無通信だけで終了と判断しません。YMIRへ無条件に移植する共通仕様ではありません。

SH-2 SCIを使う[対戦ケーブル機能](BATTLE-CABLE-SERIAL-SPEC.md)は独立経路です。Mednafenの[任意追加モジュール手順](../integration/mednafen/LINK-CABLE.md)に、基礎差分適用後の追加・別ビルド・左右起動・診断を記載しています。通常のXBAND起動ではOFFで、XBANDカートとの併用を拒否します。共通仕様とホスト固有処理を区別し、キット・基準版・ビルド設定・試験結果を併せて記録してください。
