README（Atom JoyStick ↔ CoreS3 / ESPNOW 基礎一式）
概要

送信側（TX）：M5Stack AtomS3 + Atom JoyStick からジョイスティック／ボタン状態を取得し、ESPNOWで送信

受信側（RX）：M5Stack CoreS3 が ESPNOW を受信して Roller485（I2C: 0x42, 0x43, 0x41）を制御

混信対策：起動時に パッシブスキャンでチャネルを自動選択し、ユニキャスト化 & 切替ハンドシェイクでリンク安定化

UI・演出：

TX/RXとも 非ブロッキングで WS2812（RGB LED）の演出

音楽：A（開始側）が 約5秒のメロディーを再生、B（停止側）からの指令で 強制停止可能

パケット互換：新形式 V2（{seq,lx,ly,rx,ry,buttons,vbat}）を基本。旧形式 V1（{seq,x,y,buttons,vbat}）も受信側で警告しつつ暫定マッピング対応（ly←y, ry←x）

リポジトリ（想定）
/Atom_joy_2025_10_18/                 # 送信側（AtomS3 + Atom JoyStick）
/Atom_joy_2025_10_18_ESPNOW_R_3/      # 受信側（CoreS3 + Roller485）


実際の構成に合わせて読み替えてください（PlatformIO の board/env など）。

用意するもの
ハードウェア

M5Stack AtomS3 本体 ＋ Atom JoyStick モジュール（送信側）

M5Stack CoreS3 本体（受信側）

Roller485（I2C）×1（または 0x42/0x43/0x41 の3台構成）

USB-C ケーブル（書き込み用 ×2）

必要に応じて：外部電源、配線（I2C: SDA/SCL, GND 共通）

ソフトウェア

PlatformIO（VS Code 拡張推奨）

ボード設定（例）：

送信側：platform = espressif32@6.7.0, board = m5stack-atoms3, framework = arduino

受信側：platform = espressif32@6.7.0, board = m5stack-cores3, framework = arduino

主要ライブラリ（Arduino / PlatformIO）

M5Unified（CoreS3/AtomS3 のLCD/音/LED入出力等）

ESP-NOW（esp_now.h）

（RGB LEDを直接使う場合）FastLED もしくは Adafruit_NeoPixel（※M5Unifiedで足りる構成なら不要）

このプログラムで使っている主な要素

ESPNOW 基本API：初期化、ペア登録（esp_now_init / esp_now_add_peer）、送受信コールバック

パッシブスキャン：WIFI スタックのスキャン機能で周辺 AP を受動検出 → 混雑度が低いチャネルを選択

ユニキャスト化：起動直後はブロードキャスト、ハンドシェイク完了後に相互の MAC でユニキャストへ移行

切替ハンドシェイク：SWITCH_REQ / SWITCH_ACK を用いて 同時チャネル切替を安全に実施

非ブロッキング演出：FreeRTOS タスク／タイマで LEDアニメ と メロディー再生を並行（他処理をブロックしない）

パケット互換：V2 が基本、V1 を受けた場合は 警告しつつ暫定マッピング

配線（要点）

I2C（CoreS3 ⇄ Roller485）：SDA, SCL, GND を接続（CoreS3 の I2C ピンは M5Unified 既定に準拠）

電源：各デバイスの仕様に従い安定供給

LED：AtomS3/ CoreS3 の内蔵 RGB を使用（外付け LED を使う場合は定義ピンをコードで合わせる）

ピン番号は実装中の #define / 設定関数に合わせてください（M5Unified で隠蔽している場合は設定不要）。

ビルド & 書き込み

VS Code で各フォルダ（送信側 / 受信側）を 別々に開く

platformio.ini を各ボードに合わせて確認

USB 接続 → Upload（PlatformIO: Upload）

シリアルモニタで起動ログを確認（スキャン結果・ハンドシェイク・受信警告など）

事前設定（重要）

MAC アドレス：

受信側（CoreS3）で自機 MAC を起動ログに表示 → 送信側の PEER_MAC に設定

逆も同様（必要に応じて両方向）

初期チャネル：CHANNEL_AUTO（スキャン結果から決定）／固定値運用も可

Roller485 I2C アドレス：0x42, 0x43, 0x41（コード内の定義がハードと一致しているか確認）

音楽ON/OFF：A 側の 5 秒メロディを有効化、B 側の停止指令を受信側・送信側どちらで処理するか（設計に合わせる）

パケット形式：既定は V2。旧式 V1 送信機を混在させるなら、受信側の暫定マッピングを有効化

操作手順（ダイジェスト）

受信側（CoreS3） を起動

パッシブスキャン → 混雑の少ないチャネルを仮決定

待受（ブロードキャスト受信、ハンドシェイク待機）

送信側（AtomS3） を起動

パッシブスキャン → 仮チャネル決定 → SWITCH_REQ を送出

受信側から SWITCH_ACK を受けたら 同時に切替 → ユニキャスト固定

送信開始

V2 パケットでジョイスティック/ボタン送信（V1 は暫定マッピング）

受信側制御

ly → 0x42 の速度, ry → 0x43 の速度

スティック押下 L/R → 0x41 を正/逆転（同時/OFF は停止）

音・LED

A 側：メロディ 5 秒再生（非ブロッキング）

B 側：停止指令で 即停止

WS2812：10 秒程度のカラフル演出（非ブロッキング）

送信側（TX）フロー（概略）
[Boot]
  └─ パッシブスキャン → 最適チャネル選定
        └─ 一時ブロードキャスト開始
             └─ SWITCH_REQ 送出 → ACK 受信
                  └─ 同時チャネル切替 → ユニキャスト固定
                       └─ 送信ループ（60Hzなど）
                            ├─ ジョイスティック/ボタン読み取り
                            ├─ V2 パケット生成（V1互換は不要）
                            ├─ ESPNOW 送信
                            └─ 非ブロッキング LED/音タスク

受信側（RX）フロー（概略）
[Boot]
  └─ パッシブスキャン → 最適チャネル選定
        └─ ブロードキャスト待受
             └─ SWITCH_REQ 受信 → SWITCH_ACK 返信
                  └─ 同時チャネル切替 → ユニキャスト固定
                       └─ 受信ループ
                            ├─ V2 受信 → 正常処理
                            ├─ V1 受信 → 警告表示 + 暫定マッピング
                            ├─ Roller485 速度指令(I2C:0x42/0x43/0x41)
                            └─ 非ブロッキング LED/音タスク

パケット仕様（推奨：V2）
// V2: 推奨（基本運用）
struct PacketV2 {
    uint16_t seq;      // 送信カウンタ
    int16_t  lx, ly;   // 左スティック
    int16_t  rx, ry;   // 右スティック
    uint8_t  buttons;  // ビットフラグ（L/R押下含む）
    float    vbat;     // 送信機バッテリ電圧
} __attribute__((packed));

// V1: 旧形式（受信側で暫定互換処理）
struct PacketV1 {
    uint16_t seq;
    int16_t  x, y;     // 旧：単一スティック表現
    uint8_t  buttons;
    float    vbat;
} __attribute__((packed));
// 受信側暫定マッピング例： ly ← y, ry ← x

オートチャネル選択 & 切替ハンドシェイク

起動時スキャン（両機）：周囲 AP のビーコンを パッシブに観測し、
候補チャネル → 混雑度（AP数/平均RSSI） で評価して最小を選択

初期リンク：ブロードキャストで相互検出

切替手順：

TX → SWITCH_REQ(ch) 送出（ch は TX の最適案）

RX → SWITCH_ACK(ch) 返信

同時切替（wifi_set_channel(ch)）

esp_now_add_peer(PEER_MAC, ch) で ユニキャスト固定

切替中の混信リスク：ハンドシェイクで 両機同時に切替するため最小化

それでも瞬断（ミリ秒級）はあり得る → 送信リトライ／タイムアウト再同期を実装済み

非ブロッキング実装（概要）

FreeRTOS タスク/タイマで処理分離：

task_link（ESPNOW送受信）

task_ui（LED）

task_audio（メロディ）

task_main（入力取得/制御計算）

音：メロディは ノンブロッキング再生（開始/停止はキューやフラグで制御）

LED：WS2812 は周期タイマで更新（描画のみ、メインループを塞がない）

トラブルシューティング

つながらない：

双方の MAC 設定（esp_now_add_peer）を再確認

近距離で起動順序を守る（RX → TX）

周辺が極端に混雑する場合、固定チャネル運用も検討

旧パケット警告が出る：送信側が V1。可能なら送信側を V2 に更新

音が止まらない：B 側停止指令の経路（TX経由 or 直接RX）を実装方針と一致させる

WS2812 が光らない：ピン定義／電源／GND 共通を確認（外付け時）

既知の制限 / 注意

ESPNOWは同一チャネル前提。Wi-Fi インフラと同時利用時は干渉の可能性

切替時はごく短い瞬断あり（再送で吸収）

周辺電波状況によっては最適チャネルが変動するため、屋内イベント等では固定運用＋現地スキャン推奨

ライセンス / クレジット

本 README は 2025-10-18 版の 2 プログラム（送信側 / 受信側）を前提に作成

使用ライブラリのライセンスに従ってください（M5Unified, ESP-NOW 等）

付録：Roller485 制御マッピング（例）

左スティック Y（ly） → I2C: 0x42 の速度指令

右スティック Y（ry） → I2C: 0x43 の速度指令

スティック押下 L/R → I2C: 0x41 を一定速度で 正/逆転

両押し・OFF は停止（0 指令）

付録：PlatformIO サンプル設定（抜粋・例）

送信側（AtomS3）

[env:tx-atoms3]
platform = espressif32@6.7.0
board = m5stack-atoms3
framework = arduino
monitor_speed = 115200
build_flags =
  -DCORE_DEBUG_LEVEL=3
  -DESPNOW_CHANNEL_AUTO=1
lib_deps =
  m5stack/M5Unified


受信側（CoreS3）

[env:rx-cores3]
platform = espressif32@6.7.0
board = m5stack-cores3
framework = arduino
monitor_speed = 115200
build_flags =
  -DCORE_DEBUG_LEVEL=3
  -DESPNOW_CHANNEL_AUTO=1
lib_deps =
  m5stack/M5Unified


実装の #define やファイル構成に合わせて調整してください。
