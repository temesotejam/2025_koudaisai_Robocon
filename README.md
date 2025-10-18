# README（Atom JoyStick ↔ CoreS3 / ESP-NOW 基礎一式）

## 概要
- **送信側（TX）**：M5Stack **AtomS3 + Atom JoyStick** からジョイスティック／ボタン状態を取得し、**ESP-NOW** で送信  
- **受信側（RX）**：M5Stack **CoreS3** が ESP-NOW を受信して **Roller485**（I2C: `0x42, 0x43, 0x41`）を制御  
- **混信対策**：起動時に **パッシブスキャン**でチャネルを自動選択し、**ユニキャスト化 & 切替ハンドシェイク**でリンク安定化  
- **UI・演出**：非ブロッキングで WS2812（RGB LED）演出／音楽再生  
  - A（開始側）が **約 5 秒**メロディー再生  
  - B（停止側）の指令で **強制停止** 可能  
- **パケット互換**：新形式 **V2**（`{seq,lx,ly,rx,ry,buttons,vbat}`）を基本。旧形式 **V1**（`{seq,x,y,buttons,vbat}`）も受信側で警告しつつ暫定マッピング（`ly←y, ry←x`）

---

## プロジェクト
```
/Atom_joy_2025_10_18/                 # 送信側（AtomS3 + Atom JoyStick）
/Atom_joy_2025_10_18_ESPNOW_R_3/      # 受信側（CoreS3 + Roller485）
```

---

## 用意するもの

### ハードウェア
- M5Stack **AtomS3** 本体 ＋ **Atom JoyStick** モジュール（送信側）
- M5Stack **CoreS3** 本体（受信側）
- **Roller485**（I2C）×1（または 0x42/0x43/0x41 の 3 台構成）
- USB-C ケーブル ×2（書き込み用）
- 必要に応じて外部電源、I2C 配線（SDA/SCL, GND 共通）

### ソフトウェア
- **Visual Studio Code** + **PlatformIO**
- 主要ライブラリ
  - **M5Unified**
  - **ESP-NOW**（`esp_now.h`）
  - （外付け RGB 使用時）**FastLED** または **Adafruit_NeoPixel**

---

## このプログラムで使っている主な要素
- **ESP-NOW**：初期化／ペア登録（`esp_now_init / esp_now_add_peer`）／送受信コールバック
- **パッシブスキャン**：周辺 AP を受動検出 → 混雑度の低いチャネルを自動選択
- **ユニキャスト化**：起動直後はブロードキャスト、ハンドシェイク後に互いの MAC でユニキャスト固定
- **切替ハンドシェイク**：`SWITCH_REQ / SWITCH_ACK` により **同時チャネル切替**
- **非ブロッキング演出**：FreeRTOS タスク／タイマで LED／メロディーを並行実行
- **パケット互換**：既定 **V2**、旧 **V1** を受信側で暫定マッピング

---

## 配線（要点）
- **I2C（CoreS3 ⇄ Roller485）**：SDA, SCL, GND を接続（CoreS3 標準 I2C ピン）
- **電源**：各デバイス仕様に従って安定供給
- **LED**：内蔵 RGB を使用（外付け時はピン定義に合わせて接続）

---

## ビルド & 書き込み
1. VS Code で各フォルダ（送信側／受信側）を個別に開く  
2. `platformio.ini` を確認（ボード・ライブラリ）  
3. USB 接続 → **PlatformIO: Upload**  
4. シリアルモニタでログ確認（スキャン結果／ハンドシェイク／受信警告など）

---

## 事前設定
- **MAC アドレス**：受信側（CoreS3）起動ログに表示される自機 MAC を、送信側の `esp_now_add_peer` に設定（必要に応じて相互設定）
- **初期チャネル**：`CHANNEL_AUTO`（スキャンで決定）または固定値
- **Roller485 アドレス**：`0x42, 0x43, 0x41`
- **音楽制御**：A 側で開始、B 側停止指令で即停止

---

## 操作手順（ダイジェスト）
1. **受信側（CoreS3）** を起動  
   - パッシブスキャン → 混雑の少ないチャネルを仮決定  
   - ブロードキャスト待受（ハンドシェイク待機）
2. **送信側（AtomS3）** を起動  
   - パッシブスキャン → 仮チャネル決定 → `SWITCH_REQ` 送出  
   - 受信側の `SWITCH_ACK` 受信後に **同時切替** → ユニキャスト固定
3. **送受信**  
   - 送信側：V2 パケットで状態送信（60Hz など）  
   - 受信側：V2 正常処理／V1 は警告＋暫定マッピング  
   - 受信側：Roller485 へ速度指令（I2C）

---

## 送信側（TX）フロー（概略）
```
Boot
 └─ パッシブスキャン → 最適チャネル選択
     └─ 一時ブロードキャスト
         └─ SWITCH_REQ 送出 → ACK 受信
             └─ 同時チャネル切替 → ユニキャスト固定
                 └─ 送信ループ（例: 60Hz）
                     ├─ ジョイスティック/ボタン読み取り
                     ├─ V2 パケット生成
                     ├─ ESP-NOW 送信
                     └─ 非ブロッキング LED/音タスク
```

## 受信側（RX）フロー（概略）
```
Boot
 └─ パッシブスキャン → 最適チャネル選択
     └─ ブロードキャスト待受
         └─ SWITCH_REQ 受信 → SWITCH_ACK 応答
             └─ 同時チャネル切替 → ユニキャスト固定
                 └─ 受信ループ
                     ├─ V2 受信 → 正常処理
                     ├─ V1 受信 → 警告 + 暫定マッピング
                     ├─ Roller485 指令(I2C:0x42/0x43/0x41)
                     └─ 非ブロッキング LED/音タスク
```

---

## パケット仕様

**V2（推奨）**
```c
typedef struct __attribute__((packed)) {
    uint16_t seq;      // 送信カウンタ
    int16_t  lx, ly;   // 左スティック
    int16_t  rx, ry;   // 右スティック
    uint8_t  buttons;  // ビットフラグ（L/R 押下含む）
    float    vbat;     // 送信機バッテリ電圧
} PacketV2;
```

**V1（旧形式）**
```c
typedef struct __attribute__((packed)) {
    uint16_t seq;
    int16_t  x, y;     // 旧：単一スティック表現
    uint8_t  buttons;
    float    vbat;
} PacketV1;
// 受信側暫定マッピング: ly ← y,  ry ← x
```

---

## オートチャネル選択 & 切替ハンドシェイク
- **起動時スキャン（両機）**：周辺 AP をパッシブ観測 → `候補チャネル / 混雑度（AP数・平均RSSI）` で最小を選択  
- **初期リンク**：ブロードキャストで相互検出  
- **切替手順**  
  1. TX → `SWITCH_REQ(ch)` を送出  
  2. RX → `SWITCH_ACK(ch)` を返信  
  3. **同時切替**（`wifi_set_channel(ch)`）  
  4. `esp_now_add_peer(PEER_MAC, ch)` でユニキャスト固定  
- **切替中のリスク**：同時切替により最小化（瞬断は再送で吸収）

---

## 非ブロッキング実装（概要）
- **FreeRTOS タスク／タイマ** による並列化  
  - `task_link`（ESP-NOW 送受信）  
  - `task_ui`（LED）  
  - `task_audio`（メロディ）  
  - `task_main`（入力取得／制御計算）  
- **メロディ**：開始／停止をキュー or フラグで制御し、処理をブロックしない  
- **LED**：周期タイマで更新

---

## トラブルシューティング
- **リンクしない**：MAC 設定（`esp_now_add_peer`）を確認／受信側→送信側の順で起動／固定チャネル試行  
- **旧パケット警告**：送信側が V1。可能なら V2 に更新  
- **音が止まらない**：B 側停止指令の経路設定を確認（TX 経由 or 直接 RX）  
- **WS2812 無反応**：ピン定義／電源／GND 共通を確認（外付け時）

---

## 既知の制限
- ESP-NOW は同一チャネル前提。Wi-Fi インフラ併用時は干渉の可能性  
- チャネル切替時は短時間の瞬断あり  
- 強混雑環境では現地スキャン＋固定運用が有効

---

## Roller485 制御マッピング（例）
- 左スティック **Y（ly）** → **I2C: 0x42** 速度指令  
- 右スティック **Y（ry）** → **I2C: 0x43** 速度指令  
- スティック押下 **L/R** → **I2C: 0x41** を一定速度で **正／逆転**  
  - 両押し・OFF は停止（0 指令）

---

## PlatformIO 設定例

**送信側（AtomS3）** – `/Atom_joy_2025_10_18/platformio.ini`
```ini
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
```

**受信側（CoreS3）** – `/Atom_joy_2025_10_18_ESPNOW_R_3/platformio.ini`
```ini
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
```

---

## ライセンス
- 本 README は 2 つのプログラム（送信側／受信側）を対象とした説明用文書です。  
- 利用ライブラリのライセンス（M5Unified, ESP-NOW など）に従ってください。
