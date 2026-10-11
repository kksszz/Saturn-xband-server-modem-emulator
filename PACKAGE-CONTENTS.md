# v0.5.0 パッケージ案内

2026-10-11。Windows x64の公開開発版（Public prerelease）です。
配布先は[GitHub v0.5.0](https://github.com/kksszz/Saturn-xband-server-modem-emulator/releases/tag/v0.5.0)です。
XBANDとシリアル対戦の両方を含むソース統合キットで、追加DLLを置くだけのプラグインではありません。

| 内容 | 場所／入口 |
| --- | --- |
| サーバーEXE・起動 | `bin/xband-server.exe`、`start-server.ps1` |
| サーバー／XBAND共通ソース | `CMakeLists.txt`、`outputs/live-capture-tests`、`components/xband` |
| シリアル対戦共通部品 | `components/link-cable`：SCI状態、YLS1 TCP、同期、試験 |
| YMIR組み込み | [ガイド](docs/YMIR-INTEGRATION-MANUAL.md)、`integration/ymir`、`components/xband/adapters/ymir` |
| Mednafen組み込み | [ガイド](docs/MEDNAFEN-INTEGRATION-MANUAL.md)、`integration/mednafen`、`components/xband/adapters/mednafen` |
| Mednafenシリアル追加 | [適用・起動・同期設定](integration/mednafen/LINK-CABLE.md) |
| 仕様 | [サーバー／モデム](docs/SERVER-MODEM-SPEC.md)、[SCI対戦](docs/BATTLE-CABLE-SERIAL-SPEC.md)、[統合境界](docs/EMULATOR-INTEGRATION-SPEC.md) |
| 検証・権利 | [検証記録](docs/RELEASE-VERIFICATION.md)、`LICENSE`、`THIRD-PARTY-NOTICES.md` |

YMIRの基準は`9a237ea6642912ae0833f691809aa47dc27f908d`、Mednafenは公式1.32.1です。
通常版をそのまま起動するだけでは通信機能は追加されません。未変更の基準ソースに差分を適用して
各ガイドに従いビルドします。既存YMIR差分は維持し、外部XBAND部品を本版へ更新します。
MednafenのSCIは基礎XBAND差分の後に任意追加し、専用スクリプトで起動します。
ケーブル起動の同期は既定ON（`-Synchronize 1`）、OFF比較は両側`-Synchronize 0`で再起動します。
XBANDモデムと直接SCIケーブルは同時に使いません。

ZIPにはサーバーを再ビルドするためのソース・CMake・回帰試験も含みます。
`outputs/live-capture-tests`は歴史的なディレクトリー名で、収録するのはソースのみです。
実際の通信ログや試験データではありません。依存ライブラリは別途用意し、サーバーの
ビルドは[README](README.md#ソースのビルド)へ進んでください。

エミュレーター本体／EXE、BIOS、ゲームDISC、個人プロフィール、カード画像、メール、
台帳、通信ログ、タイトル別Hotfixは含みません。付属スクリーンショットは文書用の動作例です。
既存環境を上書きせず、新規フォルダーで準備し、更新前に保存データ一式をバックアップします。

`manifest.json`は収録ファイルのサイズとSHA-256、`SHA256SUMS.txt`はZIPのSHA-256です。
`verify-package.ps1 -Archive <ZIP> -ExpectedVersion v0.5.0`で収録境界とハッシュを再検査できます。
