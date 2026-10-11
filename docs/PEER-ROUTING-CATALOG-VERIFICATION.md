# 対戦先指定・Subscriber・標準ゲーム定義の検証

2026-10-12。現行ソースを検証した記録です。

## 変更範囲

- YMIRのSubscriberは数字1～20桁（ハイフン除去）を許可。空欄なら端末別の1／2を使用し、旧固定番号の必須制約を解除。
- MednafenのSubscriberも外側wireと同じ20桁上限で検査。ゲーム内の本人番号とは別の接続識別子。
- 両アダプター・診断用接続は対戦回線制御にtarget=peer-0／peer-1とgenerationを送信。発信役・相手・世代・参加状態を検査。
- 旧クライアントの固定試験番号によるnumber発信経路は削除。targetと現在のgenerationだけで発信する。保存済み電話番号・メール・履歴を改変しない。
- ROMへのコマンド1Dは取得済みの相手登録番号を使用する。発信側で未取得・不正の場合は応答を保留し、固定番号や以前の相手番号を返さない。着信待機の応答には相手の発信番号は不要。ROMの数字列と内部endpointは別。
- 第3／第6スロットの標準表示名をPuzzle Bobble 3／Sega Rally Championship Plusへ確定。旧設定の完全一致のID仮名だけをメモリー上で移行し、独自表示名・配点・ランク条件・累積台帳・設定ファイルを保持。
- 全10タイトルのDISC情報と旧設定の扱いを[サーバー仕様書16.1](SERVER-MODEM-SPEC.md#161-標準ゲーム定義と追加)へ記載。

## DISC情報の読み取り

手元の日本版DISCの第1データトラック（MODE1/2352）からISO9660のファイル一覧を読み取り、各ファイル先頭80バイトを確認した。下表の各ファイルは288バイト。LBAは確認に使用したイメージの値であり、別版・別イメージの固定オフセットを保証しない。先頭BE32がゲームID、第2BE32はオフセット4、格納タイトルはオフセット8からNULまでのASCII。第2BE32の意味はこの値だけから断定しない。

| slot | ファイル | LBA | ID（先頭BE32） | 第2BE32 | DISC格納タイトル |
| --- | --- | --- | --- | --- | --- |
| 1 | `/DEMOINFO/GAMEINFO.BIN;1` | 1123 | `00010003` | `00000001` | `VIRTUA FIGHTER REMIX` |
| 2 | `/GAMEINFO/GAMEINFO.BIN;1` | 65 | `00018003` | `00000001` | `PUYO PUYO SUN` |
| 3 | `/PB3INFO/PB3INFO.BIN;1` | 1141 | `00018002` | `00000000` | `PUZZLE BOBBLE3 FOR SEGANET` |
| 4 | `/DECAINFO/GAMEINFO.BIN;1` | 1133 | `00010007` | `00000001` | `DECATHLETE XBAND` |
| 5 | `/GAMEINFO/GAMEINFO.BIN;1` | 1128 | `00010006` | `00000001` | `DAYTONA USA CIRCUIT EDITION` |
| 6 | `/GAMEINFO/GAMEINFO.BIN;1` | 1128 | `00010004` | `00000001` | `SEGA RALLY CHAMPIONSHIPPLUS` |
| 7 | `/XBAND/GAMEINFO.BIN;1` | 1120 | `00018001` | `00000000` | `SATURN BOMBERMAN` |
| 8 | `/GAMEINFO/GAMEINFO.BIN;1` | 1129 | `00010005` | `00000001` | `VIRTUAL-ON` |
| 9 | `/WWE98INF/GAMEINFO.BIN;1` | 1146 | `00010008` | `00000001` | `SEGA WORLD WIDE SOCCER 98` |
| 10 | `/TUSKINFO/TUSKINFO.BIN;1` | 60 | `00018004` | `00000001` | `TUSK GAME` |

標準表示名は別途管理する。VFの商標表示、Puzzle Bobble 3の短縮名、Sega Rallyの空白、Shadows of the Tuskの製品名を、上記の生の格納文字列と混同しない。DISC・抽出バイナリー・個人データはリポジトリへ追加していない。

## 自動検証

Windows x64、Visual Studio 2022、Release構成。

| 対象 | 結果 | 範囲 |
| --- | --- | --- |
| サーバー／共通部品 | 148/148 | XBAND_WITH_JSON=ON。サーバー・JSON・TCP・旧版互換・ポイント・度数・メール |
| YMIR統合 | 21/21 | 標準14件＋通信回帰7件。今回の追加整理ではモデム／電話線試験2ターゲットを現行共通ヘッダーで再ビルド |
| Mednafenアダプター | 3/3 | バス・サターンメディアカード・コマンド／対戦制御ランタイム |

追加・拡張した試験：

- `server.peer_endpoint_routes`：両方向のtarget発信、任意の登録番号、自己・不明endpoint、発信役違い、世代欠落／未来、相手未参加、numberのみ（世代あり／なし）・target/number併記の拒否。古い世代はpoll扱いで状態を変えない。
- `server.match_card_admission`：既存カード回帰に加え、両端末の相手番号更新・ハイフン除去、未取得／不正時の発信応答保留、番号取得後の再開、古い相手番号を再利用しないこと、相手番号未取得でも着信待機できることを確認。
- `xband.local_phone`：空欄の端末別Subscriber、任意番号、明示番号、文字不正・上限超過・不正side。
- `migration.frontend_telephone_line_test`：固定番号ではないSubscriberで実loopback接続・回線OFF/ON・サターンメディアカード制御。
- `mednafen.command_runtime_contract`：任意のゲスト発信番号でpeer制御を往復し、Subscriberの20桁受理・21桁拒否を確認。固定試験番号への依存はない。
- `server.vf_title_display`：第3／第6の標準名、旧仮名の移行、設定JSONを読み込みだけで書き換えないこと、独自表示名を保持することを確認。
- 更新YMIR統合差分を、適用済み検証ソースに対して `git apply --reverse --check` で検査。

今回の整理後の実ゲーム手動対戦・全タイトル対戦・別PC間試験は未実施。Mednafen本体の再ビルドは今回行っていない。既存の個人プロファイル・サターンメディアカード・台帳は変更しない。
