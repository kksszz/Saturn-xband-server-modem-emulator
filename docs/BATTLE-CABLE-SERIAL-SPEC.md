# Saturn対戦ケーブルのシリアル通信エミュレート仕様（YMIR／Mednafen）

仕様書 第2版　2026年10月11日（配布更新注記：2026年10月12日）

本書はv0.5.0のYMIR／Mednafen両方の対戦ケーブル機能を対象とする。SCI・YLS1・実行同期の共通仕様と、ホストごとの結線・設定・停止／復帰方法を区別して記載する。TCP部品はYMIR実装から`components/link-cable`へ抽出した。既存YMIR差分は維持し、Mednafenは共通部品と専用アダプターを利用する。

標準の試験構成は同一PC上で同じエミュレーターの独立2プロセスを直接TCP接続する。YMIRにはLANホスト／クライアント設定もあるが、Mednafenの追加ランチャーは同一PC用である。別PC間のLAN接続は将来の検討事項で、異種エミュレーター間の実ゲーム対戦も未検証である。両者のYLS1形式が一致することだけを、実ゲーム互換性の確認とはしない。

SH-2のシリアル通信インターフェイス（SCI）のレジスタと送受信をエミュレートし、相手プロセスへTCPでバイトを運ぶ。電気的な端子波形や実ケーブルをそのままTCPへ変換する仕様ではない。

| ホスト | 基準と組み込み方法 |
| --- | --- |
| YMIR | 上流コミット `9a237ea6642912ae0833f691809aa47dc27f908d`に`ymir-9a237ea-serial-xband.patch`を適用。SCI／TCP／UIを本体へ追加する |
| Mednafen | 公式1.32.1に基礎XBAND差分を適用後、`mednafen-1.32.1-link-cable.patch`を任意追加。共通SCI／TCP部品をC ABI経由で結線する |

YMIRの移植元は開発用コミット `ef53c22c29883d1916f5b543a67888795b7a0c24`と当時の通信改修である。両エミュレーター本体・EXE、BIOS、ゲームDISCは配布しない。取り込み手順は[YMIRガイド](YMIR-INTEGRATION-MANUAL.md)、[Mednafenガイド](MEDNAFEN-INTEGRATION-MANUAL.md)、[Mednafen対戦ケーブル手順](../integration/mednafen/LINK-CABLE.md)を参照する。

TCP上のYLS1形式、マスター/スレーブの独立経路、4096サイクルの仮想遅延と同期は、今回の実装仕様である。当時のXBANDサーバーや対戦ケーブルの公式プロトコルとして扱わない。

## 1 XBANDモデムとの違い

| 項目 | 対戦ケーブル | XBANDモデム |
| --- | --- | --- |
| ゲスト側 | SH-2内蔵SCI | カートリッジMMIOとモデムUART |
| ホスト側 | エミュレーター間の直接TCP接続 | XBAND互換サーバーへの接続 |
| 主な処理 | SCIのバイト交換と任意の実行同期 | AT、サーバー要求、待機、対戦回線、メール |
| 通信形式 | YLS1固定長レコード、旧単一路は生バイト | モデム部品の外側フレーム/JSONとゲームDISC(ROM)側要求 |
| 標準ポート | 32458 | ベース58240からの複数ポート |
| 保存データ | CPUのSCI状態。ネットワーク接続は保存しない | 別仕様の128KiB保存領域とサーバーDB |

対戦ケーブルにはXBANDのサーバー・電話番号・メディアカード・マッチング・メール・ポイント処理を使わない。XBANDモデム機能とは同時使用できない。

YMIRではモデム接続操作が対戦ケーブルを切断し、両者が接続状態になった場合はアプリ制御側でモデムをリセットする。Mednafenではケーブル有効時にXBANDモデム用カートリッジが選択されていると初期化を拒否する。専用ランチャーは`-ss.cart auto`を指定し、XBAND用ランチャーはケーブルをOFFにする。

## 2 構成とCPUの経路

両ホストとも`SetDualSCI(true)`を設定し、マスターSH-2とスレーブSH-2の送受信を独立した経路へ結線する。YMIRはCPUのコールバック、Mednafenは`CableSCIPoll()`からC ABIの送受信関数を呼ぶ。

```text
端末A マスターSCI → マスター用レコード → 端末B マスター受信キュー → マスターSCI
端末A スレーブSCI → スレーブ用レコード → 端末B スレーブ受信キュー → スレーブSCI
逆方向も同じ経路で送受信する
```

TCPソケットは1本で、レコード種別によりCPUの経路を分ける。受信キューはCPU別なので、停止中または受信無効のCPUが相手CPU用のデータを消費しない。ゲーム名によるSCIの切り替えは行わない。この2経路はソフトウェア上の互換機構であり、実機ケーブルに独立した2本の通信路があることを示さない。

`LinkCableService`は接続・受信用ワーカーと送信用スレッドを持つ。ゲストの送信はキューへ登録し、受信はキューから取り出す。ゲストのレジスタ操作からソケットI/Oを直接実行しない。ただし同期ON時には、エミュレーション側が相手の進行を待つ。

| ホスト | SCI状態と実行ループへの結線 |
| --- | --- |
| YMIR | 各SH-2のSCI状態・コールバック、Saturnの実行予算コールバックを使用する |
| Mednafen | 各SH7095に共通`SciState`の`CableSCI`を保持。レジスタ・DMAアクセス時と通常実行ループで両CPUをpollし、実行ループの目安は64サイクルと同期予算の小さい方とする |

MednafenホストはC++11、TCPランタイムはC++20でビルドし、`cable_runtime.h/.cpp`のC ABIを境界にする。追加差分は`XBAND_MEDNAFEN_LINK_CABLE=1`で有効になり、通常起動時のケーブル使用は別途OFFが既定である。

## 3 SCIレジスタと状態フラグ

SH-2の内部レジスタアドレスを使う。両CPUにそれぞれ独立した状態がある。以下は主な8ビットアクセスの仕様。

| アドレス | レジスタ | リセット値 | 現行実装の役割 |
| --- | --- | --- | --- |
| 0xFFFFFE00 | SMR | 0x00 | 同期/非同期、文字長、パリティ、ストップビット、クロック倍率の設定保持 |
| 0xFFFFFE01 | BRR | 0xFF | 1ビットの所要サイクルの計算 |
| 0xFFFFFE02 | SCR | 0x00 | 送受信許可と割り込み許可 |
| 0xFFFFFE03 | TDR | 0xFF | 送信データ保持 |
| 0xFFFFFE04 | SSR | 0x84 | 送受信状態とエラーフラグ |
| 0xFFFFFE05 | RDR | 0x00 | 受信データ。通常書き込みは無視 |

SSRの主なビットはTDRE=0x80（TDR空）、RDRF=0x40（受信データあり）、ORER=0x20（オーバーラン）、TEND=0x04（送信完了）。SCRはTIE=0x80、RIE=0x40、TE=0x20、RE=0x10、TEIE=0x04を使う。

通常のSSR読み出しは、その時点で観測したフラグを記録する（YMIRの`observedStatus`、共通`SciState`の`observed`）。通常のSSR書き込みは、観測済みのフラグに0を書いた場合にだけ対象フラグをクリアする。クリア対象は`SSR & observedFlags & ~value & 0xF8`。TDREのクリア時にTEが有効なら送信を開始する。TENDは通常のSSR書き込みではクリア対象にならない。

RDRをCPUが読んだだけではRDRFを自動クリアしない。CPUによるSSR操作とDMAの受信完了処理を区別する。YMIRのデバッガpeekはSSRの観測記録を変更せず、pokeは専用の状態変更経路で、通常の送信開始条件として扱わない。共通`SciState::read(reg, peek)`にも非観測読み出しはあるが、Mednafenの追加差分は通常の8ビットOnChipアクセスを結線しており、YMIRのデバッガUI／poke経路まで移植するものではない。

## 4 送信と受信のタイミング

### ゲスト送信

TDRと送信シフトレジスタを分離する。送信開始時にTDRの値をシフトレジスタへ移し、完了サイクルを登録する。TDRはその時点で空として扱われ、次の1バイトを保持できる。完了時に外部送信コールバックへ1バイトを渡す。TDRに次のデータがあれば連続送信し、なければTENDを立てる。

SCRのTEを無効にすると送信中の処理を停止する。TEが無効の間にTDRを用意してTDREをクリアした場合、TE再有効化時に未送信データの開始処理を行う。

### フレームの所要サイクル

YMIRの`TransmitCycles()`と共通`SciState::frameCycles()`は次の式を使う。これはSCIのシリアルフレーム時間であり、TCPパケット長ではない。

```text
divider  = 1 << (2 × (SMR & 3))              値は1、4、16、64
dataBits = SMR.CHRが1なら7、0なら8
同期SCI:
  bitCycles = 8 × (BRR + 1) × divider
  bits      = dataBits
非同期SCI:
  bitCycles = 32 × (BRR + 1) × divider
  bits      = 1 + dataBits + parityBits + stopBits
frameCycles = bitCycles × bits
```

非同期では開始ビット1、パリティ有効時1、ストップビット1または2を時間へ含める。例えばSMR=0、BRR=0なら、8N1の1バイトは320サイクル。これらの設定は所要時間に反映するが、TCPでビット波形やパリティビットを送るわけではない。7ビット設定時の値マスク、パリティ異常注入、外部クロック端子の完全な再現は保証しない。

### ゲスト受信

REが有効でRDRFが空、かつ`rxReadyCycle`に達したときに、受信コールバックから1バイトを取得する。受け付けるとRDRとRDRFを更新し、次の取得可能サイクルを現在サイクル＋`TransmitCycles()`にする。通常のキュー経由ではRDRFが満杯の間は次のデータを取り出さない。

YMIRの`SCIReceiveByte()`または共通`SciState::receive()`から満杯のRDRへ直接投入するとORERを立て、そのバイトは受け付けない。RE無効中は、取得可能な受信キューのバイトを排出する。REを有効にする境界でも古い受信を排出する。1回の排出ループは最大65536バイト。同期モードでは、まだ仮想到着時刻に達していないバイトをこの時点で取り出せないことに注意する。

これはTCPの待ちデータを後からゲストへ供給するバッファ方式であり、実機RxDが無効中に受けた波形の損失・オーバーランを完全に再現する方式ではない。

## 5 DMAと割り込み

DMAのモジュール要求RXIはREとRDRF、TXIはTEとTDREを条件にする。8ビットRXI転送で送信元が0xFFFFFE05のとき、転送後にRDRFと観測状態をクリアする。8ビットTXI転送で送信先が0xFFFFFE03のとき、SCIの送信開始処理を呼ぶ。大きなバッファをTCPへ一括で直接渡す経路ではない。

| 割り込み | 成立条件 |
| --- | --- |
| ERI | RIEが有効、SSRの0x38に該当するエラーあり |
| RXI | RIEが有効、RDRFが1 |
| TXI | TIEが有効、TDREが1 |
| TEI | TEIEが有効、TENDが1 |

両ホストの既存INTCが優先度とベクタを扱う。MednafenはIPRBのSCI優先度、VCRAのERI／RXI、VCRBのTXI／TEIへ結線し、DRCR=1をRXI、2をTXIとして要求を判定する。追加の直接SCI転送はバイトDMAのRDR／TDRアクセスが対象で、DMAレジスタ・転送数・アドレス更新などは本体側を使用する。全DMAモードや物理タイミングを完全再現した仕様ではない。

## 6 TCP接続と設定

IPv4/TCPを使い、接続後にTCP_NODELAYを設定する。TLS、認証、暗号化、公開ネットワーク用のアクセス制御は実装していない。

### YMIR

Serial Port設定UIのモードは次のとおり。

| UIのモード | 動作 |
| --- | --- |
| Same PC automatic | 127.0.0.1で待ち受けを試す。bind/listenに失敗すると同じポートへ接続する |
| LAN host listen | INADDR_ANYで1接続を待ち受ける |
| LAN client connect | 指定ホストをIPv4で解決し、指定ポートへ接続する |

同一PCの自動接続モードだけは、相手が離れた後も有効状態を維持し、200ms待って新規セッションを試す。明示的なhost/clientは1回のセッションで、自動再接続ループにしない。同じポートで複数ペアを同時に扱う仕組みではない。

現在のdual-SCIアプリの標準ポートは32458。旧単一路同期プレビューの自動接続には32457を使用する分岐が残るが、現行アプリはdual-SCIで起動する。ポートは1～65535に制限される。`--link-local`はローカル自動接続を開始する。

`general.LinkSynchronization`は0=同期OFF、1=同期ON。既定値は0。設定は保存されるが、変更の適用には両YMIRの再起動が必要。同じ設定を両側で使用する。

### Mednafen

専用GUIは設けず、`integration/mednafen/start-link-cable.ps1`で左右を起動する。通常起動はケーブルOFF、専用起動はケーブルON・ローカル自動接続・同期ONが既定。XBANDサーバーの起動は不要である。

| ランチャー引数 | 既定値／制約 | ランタイムへ渡す設定 |
| --- | --- | --- |
| `-Port` | 32458、1024～65535 | `XBAND_MEDNAFEN_CABLE_PORT` |
| `-Synchronize` | 1、0または1 | `XBAND_MEDNAFEN_CABLE_SYNC` |
| `-ProfileDirectory` | 必須。左右で別フォルダー | `MEDNAFEN_HOME` |
| 専用起動で自動設定 | ケーブルON | `XBAND_MEDNAFEN_CABLE=1` |

ほかに`-Executable`、`-Disc`、`-JapaneseBios`を指定する。先に起動した側が待ち受け、後から起動した側が同じポートへ接続する。パラメーター変更時は両側を閉じ、同じポート・同期設定で再起動する。専用ランチャーはホストアドレスやLANモードを公開していない。共通TCP部品のAPIにLAN機能があることを、Mednafenランチャーの対応機能とはしない。

両ホストとも、ここでの「実行同期」はSMRで指定するSCI自体の同期通信モードとは別である。設定例とビルド手順は各組み込みガイドを参照する。

## 7 YLS1レコード形式

現行dual-SCIは同期ON/OFFのどちらも24バイト固定長レコードを使う。TCPはバイトストリームなので、recv単位をレコード境界としない。端数を保持し、24バイト揃った時点で解析する。

| オフセット | 長さ | 内容 |
| --- | --- | --- |
| 0 | 4 | magic `59 4C 53 31`、ASCIIのYLS1 |
| 4 | 4 | type、u32 BE |
| 8 | 8 | cycle、u64 BE |
| 16 | 8 | value、u64 BE |

内部では3つのu64 BE、`0x594C533100000000 | type`、cycle、valueとして構築する。JSON、改行、可変長文字列、チェックサムは付けない。TCPの順序保証を使う。

| type | 名前 | cycle | value |
| --- | --- | --- | --- |
| 0 | HELLO | 4096 | 経路と実行同期のモード |
| 1 | BYTE | 同期ONなら相対送信サイクル、OFFなら0 | 0～255のデータ。dual-SCIではスレーブ用 |
| 2 | PROGRESS | 到達した同期境界 | clockTag、u32の範囲 |
| 3 | MASTER_BYTE | BYTEと同じ | マスター用データ。dual-SCIだけで受理 |

dual-SCIのHELLO valueは2=同期OFF、3=同期ON。旧単一路の同期機能では0=スレーブ選択、1=マスター選択。HELLOは各側が送信し、相手のmagic、type、cycle、modeが一致してからConnectedになる。初期HELLOの期限はホスト実時間で3秒。

dual-SCI同期ONのHELLOは次の24バイト。

```text
59 4C 53 31 00 00 00 00
00 00 00 00 00 00 10 00
00 00 00 00 00 00 00 03
```

非同期dual-SCIでマスターが0x41を送る例は、type=3、cycle=0、value=0x41。

```text
59 4C 53 31 00 00 00 03
00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 41
```

HELLO/PROGRESSやヘッダーのバイトをゲストRDRへ渡さない。BYTE/MASTER_BYTEから抽出したデータ1バイトだけを対象CPUへ渡す。8ビットの任意値を運ぶので、文字列としてのUTF-8/EUC-JP変換を通信経路に入れない。

旧分岐として、dual-SCIも実行同期も無効の場合に限りヘッダーなしの生バイトTCPを扱う。現行標準モードと自動交渉する形式ではなく、混在は非互換である。

## 8 実行同期と仮想遅延

同期ONは、ホストの実時間ではなくエミュレーションのサイクル数で両側の進行を制御する。量子Qは4096。接続後の最初の実行予算呼び出しで各側のoriginを記録し、送信cycleを現在サイクル－originにする。両プロセスの累積サイクル数が同じ値である必要はない。

```text
相対0 → 4096まで実行 → PROGRESS 4096を送信 → 相手の4096到達を待つ
      → 8192まで実行 → PROGRESS 8192を送信 → 相手の8192到達を待つ
```

受信データは`localRelativeCycle >= remoteByteCycle + 4096`になってからゲストへ供給可能になる。1量子の遅延は実装上の仮想ケーブル遅延であり、物理ケーブルの遅延測定値ではない。SCIのフレーム所要時間とは別に扱う。

境界待ちは条件変数を1ms単位で確認し、ホストが渡すキャンセル判定で実行予算0を返す。YMIRはUIイベント等を処理するために実行をいったん中断できる。Mednafenはゲームスレッド終了を検出し、同期待機をキャンセルして終了へ進む。データがないからゲストへ架空のバイトを返すことはしない。

PROGRESSのclockTagが両側で一致しなければエラーにする。YMIRは`(VideoStandard << 1) | ClockSpeed`を使用し、SH-2クロック倍率が`Ratio::One()`でない場合も同期を中止する。Mednafenは`(CablePAL << 1) | (cur_clock_div == 61)`を使用する。両側の映像方式・CPUクロック条件を一致させ、ホスト別の値の作り方と実ゲーム互換性を区別する。

時間軸revisionの変化、originより前への巻き戻し、clockTagの変更、同期境界から1量子以上の実行超過もエラー条件。YMIRはSaturnのResetと成功したLoadStateでrevisionを更新する。MednafenはC ABIランタイムのrevisionを使用し、ハードリセット／状態ロード時に明示的に切断する。同期ONの接続を、復元したCPU状態へそのまま持ち越さない。Mednafenの64サイクルpollは実行目安であり、命令やDMAのまとまった実行による境界超過は試験項目として残る。

HELLO後に相手のPROGRESSが止まった場合、別の固定秒数による同期待ちタイムアウトは設けていない。TCPが切れず相手が一時停止している間は、進行待ちが続く。停止した相手が必ず自動でエラーになる仕様ではない。

## 9 キュー容量と異常処理

| 対象 | 上限と処理 |
| --- | --- |
| dual-SCI受信 | 2CPUのキュー合計65536データバイト。超過はエラー |
| YLS1送信 | 65536レコード。制御レコードも含む。24バイト×件数でデータバイト上限とは別 |
| YLS1送受信バッチ | 最大64レコード、1536バイト単位で処理 |
| 旧生バイト分岐 | 送受信それぞれ65536バイト。バッチ最大256バイト |

不正magic、HELLO不一致、未知のtype、255を超えるデータ、同期OFFで0以外のcycleはエラーにする。同期ONではCPU経路別に送信cycleの逆行を禁止し、peerProgressより前、またはpeerProgress＋2Qを超えるデータも拒否する。PROGRESSは直前からQずつ増えることを要求する。

新しいYLS1セッション開始時に、送信レコード、両CPUの受信キュー、相手の進行・HELLO状態を消去し、世代番号を更新する。切断前のデータを再送する永続キューはない。Disconnectは停止通知とワーカーのjoinを行う。再接続時に旧受信を持ち越さない。

サービスのStateはDisconnected、Listening、Connecting、Connected、Error。YMIRはUI状態・アプリ通知へ表示する。Mednafenはゲームスレッドのpollから`[Link cable]`付きでOffline、Listening、Connecting、Connected to peer、Errorを診断出力する。いずれもゲームDISC(ROM)内のXBAND切断メッセージとは別である。

## 10 保存状態とデバッグ

両ホストの保存対象は各CPUのSMR、BRR、SCR、TDR、SSR、RDR、観測済みフラグ、送信中の値と完了サイクル、送信busy、受信可能サイクル。YMIRは既存のSH-2保存構造へ、MednafenはSH7095のStateActionへ追加する。TCPソケット、ワーカースレッド、サービスのネットワークキューは保存状態へシリアライズしない。

| 操作 | YMIR | Mednafen |
| --- | --- | --- |
| 通常のTCP切断 | AutoLocalは有効状態を保ち新規接続を試す。明示host／clientは1セッション | ローカル自動接続中は新規接続を試す |
| リセット／状態ロード | 同期ONでは時間軸不一致を検出してセッションを無効化。AutoLocalは停止フラグがなければ新規接続を試せる | ハードリセット／状態ロードは明示切断。復帰には左右プロセスを再起動する |
| SMPCソフトリセットボタンNMI | YMIR固有のホスト処理に従う | ケーブル切断は未結線。左右を閉じて再起動する |
| 同期中の一時停止 | 相手の進行も待機する | 相手の進行も待機する。非アクティブ時停止にも注意 |

YMIRの同期OFFにはrevisionで検出する同じ保護がない。Mednafenは同期ON／OFFを問わず明示的なリセット処理で外部交通を切断する。両者とも通信中のセーブステートから対戦を継続する仕様ではなく、左右を揃えて新しいセッションを開始する。

YMIRでは同期接続中のデバッガ単一ステップで同期を中止する。通常ビルドは`YMIR_LINK_DIAGNOSTICS=0`で、詳細トレースCSVや同期待ち時間サンプルを生成しない。診断専用ビルドではレジスタ、バイト、フレーム、DMA、待機時間の記録を任意で有効にできる。

Mednafenは状態変化時の診断と、通信量に変化がある場合の約2秒間隔のTX／RX／delivered／pending表示を使用する。`XBAND_MEDNAFEN_CABLE_CORE_TEST=1`は本体結線診断用で、ゲーム実行前に試験してロードを意図的に終了する。通常起動では指定しない。サーバーUIの通信ログ閲覧機能は、この直接SCI経路のログを表示しない。

`transmitted`は送信登録側、`sent`はソケット送出側、`received`はホスト受信キュー側、`delivered`はキューから取り出した側のカウンタ。deliveredには受信無効時の排出も含み得るため、ゲームが読み取ったバイト数・対戦成功・メール受信確認と同一視しない。

## 11 検証範囲と移植時の確認

YMIRの2026年10月7日のWindows x64検証では以下の通信試験が通過した。v0.5.0では最新外部部品を参照した本体Releaseビルドと標準14件＋通信回帰7件が成功した。試験プログラムの「raw」は同期OFFを表す箇所があり、dual-SCI試験のraw 4000 bytesもTCP上ではYLS1レコードを使う。

| 試験 | 確認した内容 |
| --- | --- |
| dual_sci_test | 両方向/両CPU計4000バイト、別々の取り出し、再接続での旧データ消去、同期ONでの両経路、モード不一致拒否、通常ビルドの診断ファイル抑止 |
| sync_core_test | 新版Saturnコア2台での単一路SCI交換、ホスト側に50msの停止を入れた同期 |
| dual_sci_core_test | 新版コア2台でdual-SCIを使い、無効CPUが他CPUの受信を奪わないこと |

Mednafen／共通追加部品では次を確認している。

| 試験 | 確認した内容 |
| --- | --- |
| 共通SCI状態 | フラグ、送信時間、バッファ、DMA要求、割り込み選択 |
| 共通TCP／同期 | 両方向・両CPU計4000バイト、同期・仮想遅延、切断・再接続 |
| Mednafen C ABI | 起動・停止・リセットのライフサイクル |
| 旧YMIR通信形式との比較 | 共通部品と旧YMIRサービスのYLS1形式の互換性。実ゲーム対戦試験ではない |
| Mednafen本体結線診断 | SCIレジスタ、送受信時間、ネイティブ割り込みベクター、バイトDMA、SCIリセット |

共通部品のMSVC Release試験は4/4、展開ZIPの独立試験は3/3が成功した。Mednafen本体のUCRT64ビルドと結線診断、および利用者による実ゲーム動作確認は[Mednafen検証記録](../integration/mednafen/VERIFICATION.md)を参照する。各ゲームの全リセット・状態復元・再対戦条件の検証完了を意味しない。配布検査全体は[リリース検証記録](RELEASE-VERIFICATION.md)、YMIR側の記録は[YMIR検証記録](../integration/ymir/VERIFICATION.md)に分けて保存する。

実機ケーブルの波形、端子配線、外部クロック、全タイトル、全SCIエラー、非Windows環境、旧保存状態との互換性はこの試験だけでは保証しない。サーバー/モデムの再接続試験結果も対戦ケーブル自体の全機能保証には流用しない。

移植先では、両CPUのコールバック、SCIのDMA/割り込み、Saturnの実行予算とrevision、UIの停止・再起動条件、保存状態フィールドを一緒に確認する。ソケットサービスだけの移植では同期動作は成立しない。

## 12 実装の参照先

### 配布キット内の共通部品・Mednafenアダプター

以下は本リポジトリ内のパス。

| 内容 | パス |
| --- | --- |
| SCI状態、フレーム時間、DMA／IRQ条件 | [components/link-cable/sci_state.hpp](../components/link-cable/sci_state.hpp) |
| TCPサービスAPI・接続・キュー | [link_cable_service.hpp](../components/link-cable/link_cable_service.hpp)、[link_cable_service.cpp](../components/link-cable/link_cable_service.cpp) |
| YLS1レコードと実行同期 | [link_cable_sync.inc](../components/link-cable/link_cable_sync.inc) |
| Mednafen C ABI・環境設定・診断 | [cable_runtime.h](../components/xband/adapters/mednafen/cable_runtime.h)、[cable_runtime.cpp](../components/xband/adapters/mednafen/cable_runtime.cpp) |
| Mednafen本体結線診断 | [cable_core_test.inc](../components/xband/adapters/mednafen/cable_core_test.inc) |
| Mednafen追加差分・適用・起動 | [追加差分](../integration/mednafen/mednafen-1.32.1-link-cable.patch)、[適用スクリプト](../integration/mednafen/apply-link-cable.ps1)、[起動スクリプト](../integration/mednafen/start-link-cable.ps1) |

### 組み込み後のYMIR本体

以下は通信改修を適用したYMIRリポジトリ内の相対パス。上流の未改修ソースや本配布リポジトリに全ファイルが存在するという意味ではない。

| 内容 | パス |
| --- | --- |
| サービスAPI、上限値 | apps/ymir-sdl3/src/app/services/link_cable_service.hpp |
| 接続とTCP送受信 | apps/ymir-sdl3/src/app/services/link_cable_service.cpp |
| YLS1と同期 | apps/ymir-sdl3/src/app/services/link_cable_sync.inc |
| CPU登録、倍率条件、モデムとの排他 | apps/ymir-sdl3/src/app/app.cpp |
| UIとポート | apps/ymir-sdl3/src/app/ui/views/settings/serial_port_settings_view.cpp と .hpp |
| 同期設定の保存 | apps/ymir-sdl3/src/app/settings.cpp と .hpp |
| SCI状態と時間計算 | libs/ymir-core/include/ymir/hw/sh2/sh2_sci.hpp |
| CPU送受信、DMA、割り込み | libs/ymir-core/src/ymir/hw/sh2/sh2.cpp |
| 実行予算と時間軸 | libs/ymir-core/src/ymir/sys/saturn.cpp |
| 保存状態 | libs/ymir-core/include/ymir/savestate/savestate_sh2.hpp と apps/ymir-sdl3/src/serdes/cereal_savestate.hpp |

### 組み込み後のMednafen本体

以下は基礎差分とSCI追加差分を適用した公式1.32.1内の相対パス。

| 内容 | パス |
| --- | --- |
| 両SH-2のSCI、DMA、IRQ、保存状態 | `src/ss/sh7095.h`、`src/ss/sh7095.inc` |
| 実行ループ・単調時計・clockTag・リセット・状態ロード | `src/ss/ss.h`、`src/ss/ss.cpp` |
| 状態pollとゲーム終了のキャンセル判定 | `src/drivers/main.cpp` |

XBANDモデムとサーバーの仕様は[サーバーとモデムの仕様書](SERVER-MODEM-SPEC.md)、共通／ホストの責務は[エミュレーター統合仕様](EMULATOR-INTEGRATION-SPEC.md)を参照する。2026-10-12のv0.5.0差し替えZIP・対応タグにも本仕様を収録する。今回の差し替えでは対戦ケーブルの動作コードは変更しない。
