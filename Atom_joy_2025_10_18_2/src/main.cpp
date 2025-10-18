/*
 * AtomS3 + AtomJoyStick (I2C: Wire1 SDA=38 SCL=39 addr=0x59)
 * - 2つの円に左/右スティックを別々表示（重なり防止・縁残像対策）
 * - 入力: 3回読み→中央値→0..4095サチュレート→アンチラップ
 * - 12bit→±32768 対称マッピング / Yのみ測定時に符号反転
 * - 起動時にオフセット自動測定（反転後で平均→オフセット）
 * - ★生値が端なら最終出力を ±32767 に強制（エンドストップ強制）
 * - ESPNOWは V2 形式 {seq,lx,ly,rx,ry,buttons,vbat_mV} をブロードキャスト送信
 */

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>

#ifndef WIFI_CHANNEL
#define WIFI_CHANNEL 1
#endif

// ===== I2C (AtomS3 内部 PortA は Wire1) =====
#define I2C_SDA_PIN 38
#define I2C_SCL_PIN 39
#define I2C_FREQ    400000

// ===== AtomJoyStick I2C マップ =====
#define JOY_ADDR                 0x59
#define LEFT_STICK_X_ADDRESS     0x00
#define LEFT_STICK_Y_ADDRESS     0x02
#define RIGHT_STICK_X_ADDRESS    0x20
#define RIGHT_STICK_Y_ADDRESS    0x22
#define LEFT_STICK_BUTTON_ADDR   0x70
#define RIGHT_STICK_BUTTON_ADDR  0x71
#define LEFT_BUTTON_ADDR         0x72
#define RIGHT_BUTTON_ADDR        0x73

// ===== アンチラップ閾値（端ジャンプ抑制：必要に応じて調整） =====
#define AW_PREV_HIGH_NEAR 3850
#define AW_NEW_LOW_POP     120
#define AW_PREV_LOW_NEAR   250
#define AW_NEW_HIGH_POP   3975

// ===== エンドストップ判定（生12bitの端判定しきい値） =====
#define RAW_LOW_END        10    // これ以下なら“下端”
#define RAW_HIGH_END     4085    // これ以上なら“上端”

static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static const int TX_HZ = 50;

// ====== V2 メッセージ ======
typedef struct __attribute__((packed)) {
  uint32_t seq;
  int16_t  lx;
  int16_t  ly;
  int16_t  rx;
  int16_t  ry;
  uint16_t buttons;   // bit0=A, bit1=B, bit2=L(左スティ押), bit3=R(右スティ押)
  uint16_t vbat_mV;   // AtomS3は0固定でもOK
} JoyMsg;

static bool i2c_ready = false;
static volatile uint32_t send_ok=0, send_fail=0;
static uint32_t seq=0, last_ui_ms=0; static bool hb=false;

static M5GFX &gfx = M5.Display;
static LGFX_Sprite sp(&gfx);

// ───────────── レイアウト（重ならない保証） ─────────────
static int cxL, cxR, cy, r;
static int btnSize, btnGap, btnY;
static int topMargin = 16, bottomMargin = 28, circleGap = 10;

// ───────────── キャリブレーション（反転後のオフセット） ─────────────
static int16_t off_lx = 0, off_ly = 0, off_rx = 0, off_ry = 0;

// 前回の生値（アンチラップ用、12bit 0..4095）
static uint16_t prev_lx = 2048, prev_ly = 2048, prev_rx = 2048, prev_ry = 2048;

// ───────────── I2Cユーティリティ ─────────────
static bool i2c_write_reg(uint8_t addr, uint8_t reg) {
  Wire1.beginTransmission(addr);
  Wire1.write(reg);
  return (Wire1.endTransmission(false) == 0);
}
static bool i2c_read_bytes(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len) {
  if (!i2c_write_reg(addr, reg)) return false;
  uint8_t n = Wire1.requestFrom(addr, len);
  if (n != len) return false;
  for (uint8_t i=0;i<len;i++) buf[i] = Wire1.read();
  return true;
}
static bool read_word_le(uint8_t reg, uint16_t &val) {
  uint8_t b[2];
  if (!i2c_read_bytes(JOY_ADDR, reg, b, 2)) return false;
  val = (uint16_t)(b[1]<<8 | b[0]); // little-endian（0..4095想定）
  return true;
}

// 0..4095 に強制サチュレート
static inline uint16_t sat12(uint32_t v){ return (v>4095)?4095:(uint16_t)v; }

// 3回読み→サチュレート→中央値、さらに前回値からの不自然ラップを潰す
static bool read_axis12_sanitized(uint8_t reg, uint16_t &out, uint16_t &prev) {
  uint16_t a, b, c;
  if (!read_word_le(reg, a)) return false;
  (void)read_word_le(reg, b);
  (void)read_word_le(reg, c);
  a=sat12(a); b=sat12(b); c=sat12(c);

  // 中央値
  uint16_t lo = min(a, b), hi = max(a, b);
  uint16_t med = (c < lo) ? lo : (c > hi ? hi : c);
  out = med;

  // アンチラップ（端→反対端への瞬間ジャンプを無効化）
  if (prev > AW_PREV_HIGH_NEAR && out < AW_NEW_LOW_POP)  out = 4095;
  if (prev < AW_PREV_LOW_NEAR  && out > AW_NEW_HIGH_POP) out = 0;

  prev = out;
  return true;
}

static bool read_byte(uint8_t reg, uint8_t &val) {
  if (!i2c_write_reg(JOY_ADDR, reg)) return false;
  if (Wire1.requestFrom((uint8_t)JOY_ADDR, (uint8_t)1) != 1) return false;
  val = Wire1.read();
  return true;
}

// 12bit(0..4095) → int16(-32768..32767) 対称マッピング
static inline int16_t map12_to_i16_symmetric(uint16_t v) {
  if (v <= 0)     return -32768;
  if (v >= 4095)  return  32767;
  if (v >= 2048) {
    int32_t num = (int32_t)(v - 2048) * 32767;  // 正側: /2047
    return (int16_t)(num / 2047);
  } else {
    int32_t num = (int32_t)(v - 2048) * 32768;  // 負側: /2048
    return (int16_t)(num / 2048);
  }
}
static inline int16_t clamp_i16(int32_t x){
  if (x < -32768) return -32768;
  if (x >  32767) return  32767;
  return (int16_t)x;
}

// ───────────── ESPNOW ─────────────
static void on_sent(const uint8_t*, esp_now_send_status_t s){
  if (s==ESP_NOW_SEND_SUCCESS) ++send_ok; else ++send_fail;
}

// ───────────── 画面描画 ─────────────
static void draw_guides(int cx, int cy, int rr) {
  sp.drawCircle(cx, cy, rr, TFT_DARKGREY);
  sp.drawCircle(cx, cy, rr/2, TFT_DARKGREY);
  sp.drawLine(cx-rr, cy, cx+rr, cy, TFT_DARKGREY);
  sp.drawLine(cx, cy-rr, cx, cy+rr, TFT_DARKGREY);
}
static void draw_stick_dot(int cx, int cy, int rr, int16_t xi, int16_t yi, uint32_t color) {
  sp.fillCircle(cx, cy, rr+1, TFT_BLACK);   // 円内クリア（縁残り対策）
  draw_guides(cx, cy, rr);
  float dx = xi / 32767.0f, dy = yi / 32767.0f;
  int px = cx + (int)(dx * (rr - 5));
  int py = cy - (int)(dy * (rr - 5));
  float vx = px - cx, vy = py - cy, len = sqrtf(vx*vx + vy*vy);
  if (len > rr - 5){ float s = (rr - 5) / (len + 1e-6f); px = cx + (int)(vx*s); py = cy + (int)(vy*s); }
  sp.fillCircle(px, py, 5, color);
}
static void draw_button_box(int x, int y, int size, const char* label, bool on) {
  sp.fillRect(x+1, y+1, size-2, size-2, on ? TFT_GREEN : TFT_BLACK);
  sp.drawRect(x, y, size, size, TFT_DARKGREY);
  sp.setTextColor(TFT_WHITE, TFT_BLACK);
  sp.setFont(&fonts::Font0);
  sp.drawString(label, x + size/2 - 3, y + size/2 - 4);
}
static void compute_layout() {
  sp.fillScreen(TFT_BLACK);
  sp.setFont(&fonts::Font0);
  sp.setTextColor(TFT_WHITE, TFT_BLACK);
  sp.drawString("AtomS3 Dual Joystick TX (V2)", 4, 2);

  const int usableW = gfx.width();
  const int usableH = gfx.height() - topMargin - bottomMargin;
  int r_h = (usableW - circleGap) / 4;
  int r_v = (usableH) / 2;
  r = min(r_h, r_v); if (r<10) r=10; if (r>40) r=40;

  cy  = topMargin + usableH / 2;
  cxL = usableW/2 - (circleGap/2 + r);
  cxR = usableW/2 + (circleGap/2 + r);

  draw_guides(cxL, cy, r); draw_guides(cxR, cy, r);

  btnGap = 6; btnSize = 20;
  int totalW = btnSize*4 + btnGap*3;
  int startX = (gfx.width() - totalW)/2;
  btnY = gfx.height() - btnSize - 4;
  for (int i=0;i<4;i++){ int x = startX + i*(btnSize+btnGap); draw_button_box(x, btnY, btnSize, (i==0)?"A":(i==1)?"B":(i==2)?"L":"R", false); }

  sp.fillCircle(sp.width()-10, 10, 4, hb ? TFT_GREEN : TFT_DARKGREY);
  sp.pushSprite(0,0);
}

// ───────────── オフセット自動測定 ─────────────
static void calibrate_offsets() {
  sp.fillScreen(TFT_BLACK);
  sp.setTextColor(TFT_WHITE, TFT_BLACK);
  sp.setFont(&fonts::Font0);
  sp.drawString("Calibrating...", 20, gfx.height()/2 - 6);
  sp.pushSprite(0,0);

  const int N = 40;
  int32_t sum_lx=0, sum_ly=0, sum_rx=0, sum_ry=0;
  int cnt_l=0, cnt_r=0;

  for (int i=0; i<N; ++i) {
    uint16_t lx=0, ly=0, rx=0, ry=0;
    bool okL = read_axis12_sanitized(LEFT_STICK_X_ADDRESS, lx, prev_lx)
            && read_axis12_sanitized(LEFT_STICK_Y_ADDRESS, ly, prev_ly);
    bool okR = read_axis12_sanitized(RIGHT_STICK_X_ADDRESS, rx, prev_rx)
            && read_axis12_sanitized(RIGHT_STICK_Y_ADDRESS, ry, prev_ry);

    if (okL) { int16_t x =  map12_to_i16_symmetric(lx);
               int16_t y = -map12_to_i16_symmetric(ly); sum_lx += x; sum_ly += y; ++cnt_l; }
    if (okR) { int16_t x =  map12_to_i16_symmetric(rx);
               int16_t y = -map12_to_i16_symmetric(ry); sum_rx += x; sum_ry += y; ++cnt_r; }
    delay(8);
  }
  if (cnt_l > 0) { off_lx = (int16_t)(sum_lx / cnt_l); off_ly = (int16_t)(sum_ly / cnt_l); }
  if (cnt_r > 0) { off_rx = (int16_t)(sum_rx / cnt_r); off_ry = (int16_t)(sum_ry / cnt_r); }

  compute_layout();
}

// ───────────── 動的UI更新 ─────────────
static void draw_dynamic_ui(int16_t lx_i16, int16_t ly_i16, int16_t rx_i16, int16_t ry_i16, uint16_t buttons) {
  draw_stick_dot(cxL, cy, r, lx_i16, ly_i16, TFT_GREEN);
  draw_stick_dot(cxR, cy, r, rx_i16, ry_i16, TFT_CYAN);

  int totalW = btnSize*4 + btnGap*3;
  int startX = (gfx.width() - totalW)/2;
  auto P = [&](int idx, bool on){ int x = startX + idx*(btnSize+btnGap); draw_button_box(x, btnY, btnSize, (idx==0)?"A":(idx==1)?"B":(idx==2)?"L":"R", on); };
  P(0, buttons & (1u<<0)); P(1, buttons & (1u<<1)); P(2, buttons & (1u<<2)); P(3, buttons & (1u<<3));

  sp.fillCircle(sp.width()-10, 10, 4, hb ? TFT_GREEN : TFT_DARKGREY);
  sp.pushSprite(0,0);
}

// ───────────── セットアップ ─────────────
void setup(){
  auto cfg = M5.config(); M5.begin(cfg);
  M5.Display.setBrightness(200); M5.Display.setRotation(0);
  Serial.begin(115200); delay(80);
  sp.setColorDepth(16); sp.createSprite(gfx.width(), gfx.height());

  Wire1.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ);
  delay(5);
  uint8_t tmp;
  i2c_ready = read_byte(LEFT_STICK_BUTTON_ADDR, tmp) || read_byte(RIGHT_STICK_BUTTON_ADDR, tmp);
  if (!i2c_ready) {
    sp.fillScreen(TFT_BLACK); sp.setTextColor(TFT_RED, TFT_BLACK);
    sp.drawString("I2C NG: fix and reset.", 6, gfx.height()/2 - 6);
    sp.pushSprite(0,0);
    while(1){ delay(300); }
  }

  WiFi.mode(WIFI_STA); WiFi.setSleep(false);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init()!=ESP_OK) { while(1) delay(1000); }
  esp_now_register_send_cb(on_sent);
  esp_now_peer_info_t p{}; memcpy(p.peer_addr, BCAST, 6);
  p.channel = WIFI_CHANNEL; p.encrypt = false; p.ifidx = WIFI_IF_STA;
  if (esp_now_add_peer(&p)!=ESP_OK){ while(1) delay(1000); }

  compute_layout();
  calibrate_offsets();
}

// ───────────── メインループ ─────────────
void loop(){
  static uint32_t last_tx=0; const uint32_t period = 1000/TX_HZ;
  uint32_t now = millis();

  // ハートビート
  if (now - last_ui_ms > 200) { last_ui_ms = now; hb = !hb;
    sp.fillCircle(sp.width()-10, 10, 4, hb ? TFT_GREEN : TFT_DARKGREY);
    sp.pushSprite(0,0);
  }
  if (now - last_tx < period) { delay(1); return; }
  last_tx = now;

  // 実測（中央値＋サチュレート＋アンチラップ）
  uint16_t lx=0, ly=0, rx=0, ry=0;
  bool okL = read_axis12_sanitized(LEFT_STICK_X_ADDRESS, lx, prev_lx)
          && read_axis12_sanitized(LEFT_STICK_Y_ADDRESS, ly, prev_ly);
  bool okR = read_axis12_sanitized(RIGHT_STICK_X_ADDRESS, rx, prev_rx)
          && read_axis12_sanitized(RIGHT_STICK_Y_ADDRESS, ry, prev_ry);
  if (!okL && !okR) { Wire1.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ); return; }

  // 12bit→i16（対称）、★Yのみ符号反転★
  int16_t lx_raw = okL ?  map12_to_i16_symmetric(lx)  : 0;
  int16_t ly_raw = okL ? -map12_to_i16_symmetric(ly)  : 0;
  int16_t rx_raw = okR ?  map12_to_i16_symmetric(rx)  : 0;
  int16_t ry_raw = okR ? -map12_to_i16_symmetric(ry)  : 0;

  // オフセット補正＋クランプ
  int16_t lx_i16 = clamp_i16((int32_t)lx_raw - off_lx);
  int16_t ly_i16 = clamp_i16((int32_t)ly_raw - off_ly);
  int16_t rx_i16 = clamp_i16((int32_t)rx_raw - off_rx);
  int16_t ry_i16 = clamp_i16((int32_t)ry_raw - off_ry);

  // ★End-stop force: 生12bitが端なら最終値を ±32767 に固定（Yは反転方向）★
  if (okL) {
    if (lx <= RAW_LOW_END)  lx_i16 = -32768;
    else if (lx >= RAW_HIGH_END) lx_i16 =  32767;
    if (ly <= RAW_LOW_END)  ly_i16 =  32767;
    else if (ly >= RAW_HIGH_END) ly_i16 = -32767;
  }
  if (okR) {
    if (rx <= RAW_LOW_END)  rx_i16 = -32768;
    else if (rx >= RAW_HIGH_END) rx_i16 =  32767;
    if (ry <= RAW_LOW_END)  ry_i16 =  32767;
    else if (ry >= RAW_HIGH_END) ry_i16 = -32767;
  }

  // ボタン（A/B/スティ押しL/R）
  uint8_t lb=0, rb=0, lsb=0, rsb=0;
  (void)read_byte(LEFT_BUTTON_ADDR,  lb);
  (void)read_byte(RIGHT_BUTTON_ADDR, rb);
  (void)read_byte(LEFT_STICK_BUTTON_ADDR,  lsb);
  (void)read_byte(RIGHT_STICK_BUTTON_ADDR, rsb);
  uint16_t btn = 0;
  if (lb)  btn |= 1u<<0;  // A
  if (rb)  btn |= 1u<<1;  // B
  if (lsb) btn |= 1u<<2;  // L(左スティ押)
  if (rsb) btn |= 1u<<3;  // R(右スティ押)

  // === V2 メッセージで “全軸” を送信 ===
  JoyMsg m{};
  m.seq = seq++;
  m.lx = lx_i16;  m.ly = ly_i16;
  m.rx = rx_i16;  m.ry = ry_i16;
  m.buttons = btn;
  m.vbat_mV = 0;  // 必要に応じて取得

  esp_now_send(BCAST, (uint8_t*)&m, sizeof(m));

  // 表示（数値は出さず、動きが分かるビジュアルのみ）
  draw_dynamic_ui(lx_i16, ly_i16, rx_i16, ry_i16, btn);
}
