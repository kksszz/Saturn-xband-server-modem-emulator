# YMIRへのXBANDモデム組み込み・セットアップマニュアル

更新日：2026-10-09。Windows x64で、YMIRに本プロジェクトのモデム部品を取り込み、独立したXBANDサーバーへ接続するための説明です。このマニュアルでは接続先を特定するため、YMIRの名称を明記します。

まずは同一PC上での試験を対象にしています。XBANDは同一PC上のサーバーと左右2つのYMIR、対戦ケーブルは同一PC上の2つのYMIRの直接接続で確認しています。本書の基本セットアップはこの構成です。別PC間のLAN接続やインターネット越しの対戦は、現時点の動作確認範囲に含めていません。

## 1 最初に確認すること

v0.3.0のWindows配布ZIPにはサーバー実行ファイルと設定・文書に加えて、`integration/ymir`のソース差分・適用/ビルドスクリプトと、`components/xband`のモデム部品を同梱します。YMIR本体・EXE、ゲームDISC(ROM)、BIOSは含めません。以前のv0.2.0 ZIPは変更していません。

通常版YMIRへサーバーのアドレスを入力するだけでは接続できません。以下のいずれかが必要です。

- 利用者が別途取得したYMIRソースへ接続処理を移植し、利用者の環境で対応版をビルドする。

本プロジェクトからYMIR本体や組み込み済みEXEは配布しません。既に自分でビルドした対応版を持っている場合は、4章から進めてください。通常版しかない場合は、利用者が別途取得した基準ソースへ、3.4章の付属差分を適用してビルドします。通常版にはSerial Port画面もないため、差分には画面と対戦ケーブル機能も含めています。

## 2 組み込みに必要なもの

| 用意するもの | 用途 |
| --- | --- |
| 本リポジトリのソース一式 | `components/xband`のモデム・接続部品 |
| 別途取得するYMIRソース | 接続処理を組み込むエミュレーター本体 |
| Visual Studio 2022のC++開発環境、Git、CMake | Windows版のビルド |
| YMIRが要求する依存ライブラリ、Windows SDK・DXC | YMIR本体のビルド・実行 |
| BIOS、XBAND対応ゲームDISC(ROM) | 利用者が別途用意するゲーム実行環境 |
| 左右別のYMIRプロファイル | 設定、プレイヤー情報、保存領域の分離 |
| 左右別の既存13バイト仮想サターンメディアカード | 有限クレジットを使う場合に追加で必要 |

ローカルの移植検証は、YMIRのコミット`9a237ea6642912ae0833f691809aa47dc27f908d`を基準に実施しました。CMake 4.2.3、MSVC 19.44、Windows SDK 10.0.26100.0のWindows x64環境でビルドを確認しています。この基準ソースのCMake最低要件は3.28です。将来のYMIRや全構成にそのまま適用できることを保証するものではありません。

YMIR本体の取得・依存関係の準備については、別途取得するYMIRソースに同梱されたビルド説明に従ってください。既存の利用環境を上書きせず、新しいソース・ビルド・プロファイルを使います。

## 3 YMIRへの取り込み方（開発者向け）

### 3.1 部品の場所

取り込む入口は、以下の接続アダプターです。

- `components/xband/adapters/ymir/frontend_modem.hpp`
- `components/xband/adapters/ymir/frontend_modem.cpp`
- `components/xband/adapters/ymir/board_attachment.hpp`
- `components/xband/adapters/ymir/bus_binding.hpp`
- `components/xband/include/xband`の共通部品

詳細な所有権・バス接続契約は[接続アダプターの説明](../components/xband/adapters/ymir/README.md)を参照してください。ファイルをYMIRのフォルダーへコピーするだけで組み込みが完了するものではありません。

### 3.2 YMIR側へ追加する処理

| 組み込み箇所 | 必要な処理 |
| --- | --- |
| アプリケーションの所有者 | Saturnインスタンスごとに`FrontendModem`を1つ保持。Saturnより長く生存させる |
| バス初期化後・CPU実行前 | `attach()`でモデムのバス接続を取り付ける |
| プロファイル初期化 | `configureStorage()`に専用保存先を渡す。検証版ではプロファイルのPersistentState配下の`xband-modem-flash.bin`を使用 |
| エミュレーションループ | 所有スレッドから`pump()`、`budget()`、`frameCompleted()`等を呼ぶ。ネットワーク完了をMMIO内で待たない |
| シリアルポート設定画面 | `request()`で接続設定を送り、`snapshot()`で状態を表示する |
| カード設定画面 | `requestCardImage()`と`requestCardInsertion()`で既存カードの読み込み・挿抜を要求する |
| リセット・状態復元・終了 | 古い通信を再利用しないための`reset()`・`shutdown()`処理を組み込む |
| 対戦ケーブルとの排他 | XBANDモデム接続時は対戦ケーブルを切断する |

モデム状態はYMIRのセーブステートへ復元する通信状態ではありません。スレッド境界や破棄順序を含めて組み込みが必要です。

### 3.3 ビルド設定

移植済みのYMIRでは、`Ymir_XBAND_COMPONENTS_DIR`に本リポジトリの`components/xband`の絶対パスを渡します。SDL3アプリ側では、`frontend_modem.cpp`をコンパイルし、共通部品とアダプターのインクルードパス、`YMIR_XBAND_FRONTEND=1`、Windowsの`bcrypt`・`iphlpapi`ライブラリを設定します。

以下は、**3.2の組み込みと、上記CMake設定が既にあるソース専用**の設定例です。未改修の上流YMIRでは、この変数を指定するだけでは機能が追加されません。

```powershell
$ymirSource = 'C:/XBAND/YMIR-integrated'
$xbandSource = 'C:/XBAND/Saturn-xband-server-modem-emulator'
$ymirBuild = 'C:/XBAND/build-ymir'

cmake -S $ymirSource -B $ymirBuild -G 'Visual Studio 17 2022' -A x64 `
    "-DCMAKE_TOOLCHAIN_FILE=$ymirSource/vcpkg/scripts/buildsystems/vcpkg.cmake" `
    "-DYmir_XBAND_COMPONENTS_DIR=$xbandSource/components/xband"
cmake --build $ymirBuild --config Release --parallel 3
```

パスは実際の保存先に置き換え、YMIRのビルド手順が要求する追加オプション・DXCの指定も適用してください。途中でエラーになった場合は次へ進まず、原因を解消します。検証環境の依存ライブラリキャッシュのパスを、新規PCへそのまま流用しないでください。

検証構成の実行ファイルは`build/apps/ymir-sdl3/Release/ymir-sdl3.exe`に生成されました。DXCをリンクする構成では、対応する`dxcompiler.dll`・`dxil.dll`などの実行時依存ファイルも必要です。DLL不足のままEXEだけをコピーすると起動できません。

2026-10-07のローカル移植パッチは基本通信の検証用です。今回同梱する`ymir-9a237ea-serial-xband.patch`は、その差分にカード設定UIを追加し、旧テスト番号プリセットと試験用自動接続を外したものです。古いローカルパッチと混ぜず、下記の付属差分を未変更の基準ソースへ適用してください。

### 3.4 付属差分による一括取り込み

必要なものは、Git、Visual Studio 2022のC++開発環境、CMake 3.28以上、Windows SDK・DXCです。ビルド例はAVX2を使用します。DXCは`dxc.exe`と同じフォルダーに`dxcompiler.dll`・`dxil.dll`があるものを指定します。

まず現在の本リポジトリのソースを取得し、別フォルダーへYMIRの基準ソースを取得します。以下は新しい保存先の例です。各コマンドで失敗が出た場合は止めて解消してください。

```powershell
$integrationRoot = 'C:/XBAND-source-integration'
$xbandSource = "$integrationRoot/Saturn-xband-server-modem-emulator"
$ymirSource = "$integrationRoot/YMIR"
$ymirBuild = "$integrationRoot/build-ymir"

New-Item -ItemType Directory -Path $integrationRoot
git clone https://github.com/kksszz/Saturn-xband-server-modem-emulator.git $xbandSource
git clone https://github.com/ymir-emu/Ymir.git $ymirSource
git -C $ymirSource checkout 9a237ea6642912ae0833f691809aa47dc27f908d
git -C $ymirSource submodule update --init --recursive
git -C $ymirSource switch -c local-xband-integration

& "$xbandSource/integration/ymir/apply-integration.ps1" -YmirSource $ymirSource
```

適用スクリプトは基準コミットと追跡対象ファイルの未変更状態を確認し、`git apply --check`の成功後に適用します。既存の変更はリセット・上書きしません。再適用も拒否します。更新されたnightlyへ無理に適用するための`--reject`や`--3way`は使用しません。

次にvcpkgを準備してビルドします。DXCのパスはインストールしたSDKに合わせて変更してください。

```powershell
& "$ymirSource/vcpkg/bootstrap-vcpkg.bat" -disableMetrics

& "$xbandSource/integration/ymir/build-ymir.ps1" `
    -YmirSource $ymirSource `
    -BuildDirectory $ymirBuild `
    -DxcExecutable 'C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/dxc.exe'
```

初回は依存ライブラリを取得・ビルドするため、ネットワークと時間・ディスク容量が必要です。既に同構成のvcpkg依存関係を持つ開発者だけは、`-InstalledDependencies '既存のvcpkg_installedの絶対パス'`を指定して取得を省略できます。別のPC・構成のキャッシュを無条件で流用しないでください。

ビルドスクリプトは外部モデム部品へのパスを設定し、Releaseビルド、DXC実行時DLLの配置、CTestを実行します。ゲームやサーバーは起動しません。出力は`$ymirBuild/apps/ymir-sdl3/Release/ymir-sdl3.exe`です。

この版のSerial Portには、対戦ケーブル、XBANDモデム、カード設定の欄があります。旧テスト番号を入力済みにせず、電話番号を利用者が入力してから接続します。カードの自動挿入・補充は行いません。

対戦ケーブルのみを使う場合、同じPCでは左右のSerial Portで`Same PC (automatic)`、TCPポート`32458`を合わせて`Enable Battle (Taisen) Cable`を有効にします。双方で同期設定を合わせ、変更時は再起動します。XBANDサーバーは不要です。LAN設定も実装されていますが、別PC間の動作は未確認です。設定上は一方を`LAN host (listen)`、もう一方を`LAN client (connect)`にし、ホストIPv4とポートを合わせます。XBANDモデムは切断しておきます。詳しくは[対戦ケーブル仕様書](BATTLE-CABLE-SERIAL-SPEC.md)を参照してください。

XBANDを使う場合は対戦ケーブルを無効にし、4～8章へ進みます。モデムとの同時使用はできません。手動の移植箇所を確認する場合は10章を参照してください。

## 4 Windows配布サーバーの起動

1. Windows配布ZIPを新しいフォルダーへ展開します。
2. そのフォルダーでPowerShellを開き、以下を実行します。

```powershell
.\start-server.ps1
```

サーバーの通信モニターが開きます。既定の接続先は`127.0.0.1`、ベースポートは`58240`です。実際には58240～58245の6ポートを使います。同じポートを使用する別サーバーは同時に起動できません。

ポートを変更する場合の例：

```powershell
.\start-server.ps1 -BasePort 58440
```

この場合はYMIR側もベースポートを`58440`に合わせます。スクリプトはYMIRやゲームを自動起動しません。初回に作成される`runtime`へサーバーの履歴・設定・台帳を保存します。インターネットへの公開や実機の電話網への接続は行いません。

## 5 左右のYMIRを準備する

同じPCで、対応版YMIRを左右2つ起動します。左右で同じプロファイルを共有しないでください。例えば、別々の保存先を`--profile`で指定します。

```powershell
& 'C:/XBAND/YMIR/ymir-sdl3.exe' --profile 'C:/XBAND/profiles/left'
& 'C:/XBAND/YMIR/ymir-sdl3.exe' --profile 'C:/XBAND/profiles/right'
```

各プロファイルでBIOS、ゲームDISC(ROM)、コントローラーを設定します。XBAND接続を始める前に、通常のゲーム起動ができることを確認してください。左右は同じ対戦タイトルを使います。

## 6 YMIRのモデム設定

ゲームを起動する前に、両方のYMIRで`Settings → Serial Port`を開きます。対応版では`XBAND cartridge modem`の設定欄が表示されます。検証版には`(preview)`等の補足が付く場合があります。

| 画面の項目 | 左側 | 右側 |
| --- | --- | --- |
| Server IPv4 | `127.0.0.1` | `127.0.0.1` |
| Server base port | `58240` | `58240` |
| Modem endpoint | `Saturn 1` | `Saturn 2` |
| Own telephone number | 左側の仮想電話番号 | 左側とは異なる仮想電話番号 |
| Allow private LAN (plain TCP) | OFF | OFF |
| Enable Battle (Taisen) Cable | OFF | OFF |

電話番号は端末を識別する設定です。実際に電話をかけるものではありません。古い検証版に表示されるテスト番号は、利用する番号へ置き換えてください。端末選択を変更すると番号が書き換わる版もあるため、端末を選んだ後に入力内容を確認します。

`03`で始まる国内固定電話形式の番号は、地域表示で東京として扱います。番号の判定条件や曖昧な番号の扱いは[サーバー仕様書の9章](SERVER-MODEM-SPEC.md#9-対戦エリアと利用時間)を参照してください。地域表示は実際の所在地を保証するものではありません。

有限カードを使う場合は、接続ボタンを押す前に7章を実施します。カード設定後、左右それぞれ`Connect modem`を押し、通信モニターで両端末の接続状態を確認します。モデムのサーバー接続と、ゲーム内の「XBANDに接続成功！」は別の段階です。

モデム設定は検証版ではセッション限定です。YMIRを再起動したら設定を確認し、必要に応じて接続し直します。

## 7 有限クレジット用の仮想サターンメディアカード

有限カード対応版には`Virtual media card`欄があります。古い対応版にはこの欄がないため、その版では以下のUI手順は実施できません。

1. モデムが未接続の状態で、`Card image path (UTF-8)`に既存13バイトカードファイルの絶対パスを入力します。
2. `Load card image`を押します。
3. `Insert virtual card`を押し、カード状態が`inserted`になったことを確認します。
4. 左右で異なるカードファイルを使います。同じファイルを2つのYMIRで共有しないでください。
5. 6章の`Connect modem`を押します。

読み込むだけではカードは挿入されません。別のカードを選ぶ際は、検証版ではYMIRの再起動が必要です。任意の13バイトファイルを作れば有効なカードになるという意味ではありません。カードの自動生成、補充、上書きは行いません。

サーバーの度数消費は初期状態では無効です。利用する場合はサーバーUIの「消費度数設定」で明示的に有効にします。標準はメール1度数、通常対戦3度数です。途中リセットの対戦分は両側各1度数で、次のメール接続時にはメール分も加算します。料金・処理は全タイトル共通で、ゲーム別に設定できるのはポイントです。

カード内容はゲームのセーブステートと独立して保存されます。古いセーブステートを読み込んでもカードの残数は戻りません。詳しくは[サーバーUI操作マニュアル](SERVER-UI-MANUAL.md)と[サーバー仕様書の15章](SERVER-MODEM-SPEC.md#15-全タイトル共通の度数とメディアカード)を参照してください。

## 8 ゲーム内の操作

1. モデムを接続してからゲームDISC(ROM)を起動します。
2. ゲーム内で電話設定・プレイヤー登録・コードネームなどを設定します。
3. ゲーム内の自分の電話番号は、YMIRの`Own telephone number`と一致させます。
4. メールの接続操作を行い、XBANDメニューへ戻れることを確認します。
5. 左右で対戦開始を選び、マッチング・ゲームへの移行を確認します。

入力の詳細は各ゲームの説明書に従ってください。タイトルごとに料金のロジックを切り替える設定はありません。全タイトルの実プレイを保証するものではありません。

## 9 困ったときと終了・バックアップ

| 症状 | 確認すること |
| --- | --- |
| モデム設定欄がない | 通常版または組み込み未完了のYMIRです。サーバーだけでは設定欄は追加されません |
| カード設定欄がない | 古い基本通信対応版です。有限カード対応UIを組み込んだ版が必要です |
| YMIRが起動しない | BIOS設定とは別に、実行時DLL不足・ビルド構成・起動ログを確認します |
| サーバーへ接続できない | サーバー起動、ホスト、ベースポート、左右のendpoint、ポート重複を確認します |
| 同じプレイヤー情報が左右に出る | プロファイル・モデム保存領域が共有されていないか確認します |
| 接続成功表示の後に止まる | 表示だけで全処理の成功とは判断せず、通信履歴・エラーを保全します。カード補充や台帳削除はしません |
| リセット・状態復元後に接続できない | 古い通信セッションは復元されません。モデム接続状態を確認し、新しいセッションで接続し直します |

終了時はゲーム通信を終え、YMIRとサーバーを終了します。サーバーの通信モニターを閉じるとサーバーも終了します。バックアップは停止後にサーバーの`runtime`全体、左右のYMIRプロファイル、使用中のカードファイルをまとめて保全してください。カードや精算台帳だけを古い状態へ戻さないでください。

本書は既存ソースとローカル検証記録に基づく組み込み・設定説明です。新規PCでの未改修YMIRからの一括セットアップ、配布ZIPだけでの対戦、全タイトル・全環境の動作確認を完了したという意味ではありません。

## 10 ソース取り込みの詳細手順

この章では付属差分が変更する主な境界を説明します。通常の取り込みは3.4章の付属差分で実施し、この章の例を重ねて貼り付けないでください。コード例は説明用の抜粋で、完全な差分ではありません。コア機構やSerial Port・対戦ケーブルの追加は付属差分に含まれます。

### 10.1 作業フォルダーを分離して取得する

以下は新規保存先の例です。既存のYMIRプロファイルやソースの上では実行しないでください。GitHubリポジトリへのアクセス権も別途必要です。

```powershell
$integrationRoot = 'C:/XBAND-source-integration'
$ymirSource = "$integrationRoot/YMIR"
$xbandSource = "$integrationRoot/Saturn-xband-server-modem-emulator"
$ymirBuild = "$integrationRoot/build-ymir"

New-Item -ItemType Directory -Path $integrationRoot
git clone https://github.com/ymir-emu/Ymir.git $ymirSource
git -C $ymirSource checkout 9a237ea6642912ae0833f691809aa47dc27f908d
git -C $ymirSource submodule update --init --recursive
git -C $ymirSource switch -c local-xband-integration
git clone https://github.com/kksszz/Saturn-xband-server-modem-emulator.git $xbandSource
git -C $xbandSource rev-parse HEAD
```

各コマンドの終了コードを確認してから次へ進みます。最後に表示されるモデム部品側のコミットも記録してください。YMIRの基準SHAを固定するのは、追従して変化するnightlyとの混同を避けるためです。上流の更新を取り込む場合は差分とAPIを再確認します。

### 10.2 先にYMIR単体をビルドできる状態にする

YMIRソースに同梱された説明に従い、Visual Studio、CMake、vcpkg、Windows SDK・DXCを準備します。必要な場合は次を実行します。

```powershell
& "$ymirSource/vcpkg/bootstrap-vcpkg.bat" -disableMetrics
```

まずモデム未組み込みのYMIRを別のビルドフォルダーでビルドし、起動できることを確認します。依存ライブラリの取得失敗やDXC不足を、モデムの不具合と混同しないためです。検証ではMSVC向けに`DirtyBitmap`の`is_power_of_two(N)`の引数を`size_t(N)`へ明示する互換性修正も使用しました。同じ型エラーが出る場合に、該当箇所と型を確認して対応してください。通信処理を変える修正ではありません。

### 10.3 SDL3アプリのCMakeに外部部品を追加する

変更対象はYMIR側の`apps/ymir-sdl3/CMakeLists.txt`です。キャッシュ変数を宣言し、既存の`ymir-sdl3`ターゲットが作成された後に次のブロックを追加します。

```cmake
set(Ymir_XBAND_COMPONENTS_DIR "" CACHE PATH "XBAND component source directory")

# Place this block after the ymir-sdl3 target is defined.
if(WIN32 AND Ymir_XBAND_COMPONENTS_DIR)
    target_sources(ymir-sdl3 PRIVATE
        "${Ymir_XBAND_COMPONENTS_DIR}/adapters/ymir/frontend_modem.cpp")
    target_include_directories(ymir-sdl3 PRIVATE
        "${Ymir_XBAND_COMPONENTS_DIR}/include"
        "${Ymir_XBAND_COMPONENTS_DIR}/adapters/ymir")
    target_compile_definitions(ymir-sdl3 PRIVATE YMIR_XBAND_FRONTEND=1)
    target_link_libraries(ymir-sdl3 PRIVATE bcrypt iphlpapi)
endif()
```

モデム部品をYMIRのvendorへ複製する必要はありません。外部フォルダーを参照し、サーバーは引き続き独立EXEとして使用します。これは通常版YMIRが元々提供するCMakeオプションではなく、今回追加する設定です。

### 10.4 アプリ所有者・初期化・保存領域を追加する

変更対象は`apps/ymir-sdl3/src/app/app.hpp`と`app.cpp`です。`app.hpp`でヘッダーを読み込み、Appのメンバーを**`SharedContext m_context`より前**に宣言します。

```cpp
#ifdef YMIR_XBAND_FRONTEND
#include <frontend_modem.hpp>
#endif

// Inside App, immediately before SharedContext m_context:
#ifdef YMIR_XBAND_FRONTEND
::xband::ymir_adapter::FrontendModem m_modem;
#endif
```

Appのサービス登録処理に追加します。

```cpp
#ifdef YMIR_XBAND_FRONTEND
m_context.serviceLocator.Register(m_modem);
#endif
```

Saturnの基本バスマッピング完了後、エミュレーションスレッド開始前に追加します。

```cpp
#ifdef YMIR_XBAND_FRONTEND
m_modem.attach(*m_context.saturn.instance);
m_modem.configureStorage(
    m_context.profile.GetPath(ProfilePath::PersistentState)
    / "xband-modem-flash.bin");
#endif
```

実装時は保存先の例外を処理し、既存ファイルを消さずにエラーを通知します。左右で別のプロファイルを使うため、保存領域も別になります。初期化でテスト用電話番号を自動設定したり、カードを自動作成・補充・挿入する処理を追加しないでください。

### 10.5 コアと実行ループの接続を追加する

基準ソースからの移植では、`libs/ymir-core/include/ymir/sys/saturn.hpp`、`libs/ymir-core/src/ymir/sys/saturn.cpp`等への実行予算・中断機構の追加も必要です。既存の移植版には、次のAPIが用意されています。

| API・状態 | 必要な意味 |
| --- | --- |
| `SetExecutionBudgetCallback(...)` | 現在サイクルとタイムライン改訂番号を引数に、実行可能な予算を問い合わせる |
| 予算0 | ゲスト時刻を進めず、フロントエンドへ制御を返す |
| `~uint64{0}` | モデムからの予算制限なし |
| `ExecutionYielded()` | 1フレーム完了ではなく、途中で制御を返したことを示す |
| タイムライン改訂 | リセット・状態復元などを、以前の実行時刻と区別する |

単にヘッダーへ関数宣言を足すだけでは不十分です。スケジューラーが予算を実際に守り、途中フレームを完了として保存しない処理が必要です。コア側の詳細境界は[シリアル通信エミュレート仕様](BATTLE-CABLE-SERIAL-SPEC.md)も参照してください。対戦ケーブルのSCI通信とXBANDのUARTは別機構であり、対戦ケーブル全機能がXBANDに必須という意味ではありません。

モデム専用の実行予算コールバックの例は次のとおりです。対戦ケーブル用コールバックが既にある場合は上書きせず、一つのコールバック内で排他・双方の制限を処理します。

```cpp
// Requires the core execution-budget implementation described above.
m_context.saturn.instance->SetExecutionBudgetCallback(
    {this, [](uint64 cycle, uint64 revision, void *ctx) {
        auto &app = *static_cast<App *>(ctx);
        return app.m_modem.budget(cycle, revision);
    }});
```

`app.cpp`のエミュレーションループでは、UIイベント処理の後・CPU実行の前に`m_modem.pump()`を呼びます。falseなら、描画・停止イベントを処理できるようにしたまま`waitForActivity()`を呼び、ゲスト実行を進めず次のループへ戻ります。ソケットの受信完了までループ内で同期的に待つ処理は追加しません。

`RunFrame()`の後は、`ExecutionYielded()`がtrueなら部分フレームをリワインド保存せず、イベント処理へ戻します。フレームが完了した場合だけ`m_modem.frameCompleted()`を呼びます。

一時停止中もモデムの要求・通信を処理する必要があります。モデム有効中、または`hasPendingRequest()`がtrueのときは、イベントが来るまで永久に待つ`wait_dequeue_bulk()`へ入らないようにします。通信設定の変更・終了が届く経路を維持してください。

### 10.6 Serial Portへモデム・カードのUIを追加する

変更対象は次の2ファイルです。

- `apps/ymir-sdl3/src/app/ui/views/settings/serial_port_settings_view.hpp`
- `apps/ymir-sdl3/src/app/ui/views/settings/serial_port_settings_view.cpp`

UI側の設定変数の例です。電話番号は空欄にし、昔のテスト番号を既定値として埋め込まないでください。

```cpp
#ifdef YMIR_XBAND_FRONTEND
char m_modemHost[64] = "127.0.0.1";
char m_modemPhone[32] = "";
int m_modemSide = 0;
int m_modemPort = 58240;
bool m_modemLAN = false;
char m_virtualCardPath[1024] = "";
#endif
```

`Display()`ではサービスを取得し、状態表示はスナップショットで行います。

```cpp
auto &modem = m_context.serviceLocator.GetRequired<
    ::xband::ymir_adapter::FrontendModem>();
const auto state = modem.snapshot();
```

ホスト・ベースポート・端末1/2・自分の電話番号を入力する欄を作り、接続中は変更不可にします。接続ボタンでは`Config`へ入力値を詰め、`modem.request(config)`を呼びます。端末番号はSaturn 1が`side=0`、Saturn 2が`side=1`です。接続前に対戦ケーブルを切断し、空の電話番号などをUIで拒否してください。

カード欄は未接続かつ未読み込みのときだけ読み込み可能にし、UTF-8パスから`std::filesystem::u8path(...)`を作って`requestCardImage()`へ渡します。挿入ボタンは`requestCardInsertion(true)`、取り出しボタンはfalseを渡します。読み込みと挿入は別操作です。

これらの要求を送った後は、次のようにエミュレーションスレッドへイベントを送り、停止中でも要求を処理させます。

```cpp
m_context.EnqueueEvent(events::emu::RunFunction([](SharedContext &) {}));
```

`Display()`から`attach()`、`pump()`、`reset()`、`configureStorage()`を直接呼ばないでください。UIスレッドとエミュレーションスレッドを混在させるとバス状態が競合します。必要なヘッダーには`frontend_modem.hpp`と`app/events/emu_event_factory.hpp`等を追加します。

### 10.7 リセット・状態復元・終了を接続する

| 処理箇所 | 追加する処理 |
| --- | --- |
| FactoryReset / HardReset / SoftResetイベント | 所有スレッドで`m_modem.reset(...)`を呼び、古い通信を破棄してからリセットする |
| セーブステートの読み込み成功 | モデムをresetする。読み込み失敗時は現在の通信を破棄しない |
| Undo-loadの成功 | 同様にresetする |
| リワインドの状態読み込み成功 | 同様にresetする |
| Shutdownイベント | Saturnを破棄する前に`m_modem.shutdown()`を呼ぶ |

状態復元は通常スロットだけでなく、Undo・リワインドも点検します。モデムの通信状態やカード残数をYMIRの過去のセーブステートへ巻き戻す処理は組み込みません。

ゲームからのリセットボタン操作（`SetResetButton`）と、フロントエンドのHardReset等は異なります。ゲームが途中リセットを通信報告する経路を、ボタンを押した瞬間の強制切断で潰さないでください。サーバーの料金判定は全タイトル共通です。

### 10.8 ビルドと最低限の確認

3.3のCMakeコマンドで、変更済みYMIRソースをビルドします。設定ログで外部部品へのパスを確認し、`frontend_modem.cpp`がビルド対象に入り、`YMIR_XBAND_FRONTEND`がアプリへ設定されていることを確認します。

```powershell
& "$ymirBuild/apps/ymir-sdl3/Release/ymir-sdl3.exe" --help
git -C $ymirSource diff --check
git -C $ymirSource status --short
```

`--help`だけでは通信確認になりません。4～8章の操作で、最低限、Serial Portにモデム・カード欄が表示されること、左右が別endpointで接続できること、ゲーム内メール接続、対戦への移行を確認します。有限クレジットを有効にした場合は通常対戦後のメール接続で各側4度数、途中リセット後で各側2度数になることも確認します。これは全タイトルの動作保証を意味しません。

ビルドしたYMIRは利用者のローカル環境に置き、サーバーの配布ZIPには追加しません。YMIR側の変更と本リポジトリの部品を別管理し、両方のコミット・ビルド設定を記録します。
