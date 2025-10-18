/*
 * [責務] CoreS3（M5Unified） ESPNOW受信 → Roller485(0x42,0x43,0x41)制御
 *        + A押下で「5秒間」メロディ＆外付けWS2812レインボー開始（非ブロッキング）
 *        + B押下で上記を即時強制停止（非ブロッキング）
 *
 * [公開API]
 *   setup(), loop()
 *   on_recv()                               : ESPNOW受信（V1/V2）
 *   map_i16_to_speed(), apply_speed()       : 速度マップ＆適用
 *   fx_start_5s(), fx_stop()                : 5秒演出の開始/停止
 *   melody_tick(now), led_anim_tick(now)    : 非ブロッキング更新
 *
 * [送信プロトコル]
 *   V2: {seq,lx,ly,rx,ry,buttons,vbat_mV} 推奨（V1は暫定割当）
 *
 * [配線]
 *   - Roller485: GROVE(A) I2C (SDA=2, SCL=1, 400kHz)
 *   - WS2812(NeoPixel): WS2812_PIN(既定5)をDINへ。5V/GNDはCoreS3と共通。必要に応じてレベルシフタ。
 */

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "unit_rolleri2c.hpp"   // あなたのI2Cドライバ（src/に配置）

// ==== WS2812（NeoPixel）設定 ====
#include <Adafruit_NeoPixel.h>
#ifndef WS2812_PIN
#define WS2812_PIN        5     // ★外付けWS2812のDINへ（必要なら変更）
#endif
#ifndef WS2812_COUNT
#define WS2812_COUNT      4    // LED個数
#endif
#ifndef WS2812_BRIGHT
#define WS2812_BRIGHT     255    // 0-255
#endif
Adafruit_NeoPixel strip(WS2812_COUNT, WS2812_PIN, NEO_GRB + NEO_KHZ800);

// ==== Wi-Fi / ESPNOW ====
#ifndef WIFI_CHANNEL
#define WIFI_CHANNEL 1
#endif

// ==== Roller I2C アドレス ====
#define ROLLER_LEFT_ADDR   0x42   // 左Y
#define ROLLER_RIGHT_ADDR  0x43   // 右Y
#define ROLLER_BTN_ADDR    0x41   // L/R押しで一定速

// ==== CoreS3 GROVE(A) I2C ====
#define I2C_SDA_PIN  2
#define I2C_SCL_PIN  1
#define I2C_FREQ     400000

// ==== スケール・デッドゾーン ====
#define MAX_SPEED_ABS     40000
#define DEADZONE_I16      1200
#define BTN_FIXED_SPEED   10000

// ==== スピーカー ====
#define SPK_VOLUME        200   // 0-255程度

// ==== 受信メッセージ ====
typedef struct __attribute__((packed)) {
  uint32_t seq; int16_t x; int16_t y; uint16_t buttons; uint16_t vbat_mV;
} JoyMsgV1;

typedef struct __attribute__((packed)) {
  uint32_t seq;
  int16_t  lx; int16_t ly;
  int16_t  rx; int16_t ry;
  uint16_t buttons; uint16_t vbat_mV;
} JoyMsgV2;

// L/R は「スティック押し」を使う（side A/Bは演出開始/停止）
#define USE_SIDE_AB_AS_LR   0
#define USE_STICK_LR_AS_LR  1

// A/Bボタンのビット位置（送信側準拠）
#define ABIT_A  0   // A=開始(5秒)
#define ABIT_B  1   // B=強制停止

// ==== 受信バッファ ====
static volatile bool g_has_new=false;
static uint32_t g_last_rx_ms=0, g_seq=0;
static int16_t g_ly=0, g_ry=0;
static uint16_t g_buttons=0, g_prev_buttons=0;
static bool g_is_v1=false;

// ==== Roller485 ====
UnitRollerI2C Roller_L, Roller_R, Roller_B;
bool UnitRollerI2C::initialized = false;

// ==== ユーティリティ ====
static inline int32_t map_i16_to_speed(int16_t v){
  int32_t a=v, absv=(a<0?-a:a);
  if (absv < DEADZONE_I16) return 0;
  int32_t num = (absv - DEADZONE_I16) * MAX_SPEED_ABS;
  int32_t den = (32767 - DEADZONE_I16);
  int32_t out = num / den;
  return (a<0)? -out : out;
}
static inline void apply_speed(UnitRollerI2C &dev, int32_t spd){
  if (spd >  MAX_SPEED_ABS) spd =  MAX_SPEED_ABS;
  if (spd < -MAX_SPEED_ABS) spd = -MAX_SPEED_ABS;
  dev.setSpeed(spd);
}

// ==== ESPNOW 受信コールバック ====
static void on_recv(const uint8_t*, const uint8_t* data, int len){
  if (len==(int)sizeof(JoyMsgV2)){
    const JoyMsgV2* m = (const JoyMsgV2*)data;
    g_is_v1 = false; g_seq=m->seq;
    g_ly=m->ly; g_ry=m->ry;
    g_buttons=m->buttons; g_has_new=true; g_last_rx_ms=millis();
  } else if (len==(int)sizeof(JoyMsgV1)){
    const JoyMsgV1* m = (const JoyMsgV1*)data;
    g_is_v1 = true; g_seq=m->seq;
    g_ly=m->y; g_ry=m->x;  // 暫定割当（V2へ移行推奨）
    g_buttons=m->buttons; g_has_new=true; g_last_rx_ms=millis();
  }
}

// ===========================
// 5秒メロディ（非ブロッキング）
// ===========================
// 1ステップ=250ms（鳴動220ms+休符30ms）→ 20ステップで約5秒
static const uint16_t NOTE_C4=262, NOTE_D4=294, NOTE_E4=330, NOTE_F4=349, NOTE_G4=392,
                      NOTE_A4=440, NOTE_B4=494, NOTE_C5=523;
static const uint8_t  MELODY_LEN = 8;
static const uint16_t melody_motif[MELODY_LEN] = {
  NOTE_C4, NOTE_E4, NOTE_G4, NOTE_C5, NOTE_B4, NOTE_A4, NOTE_G4, NOTE_D4
};
struct MelodySM {
  bool active=false;
  uint8_t idx=0;            // motif index [0..7]
  uint32_t next_ms=0;       // 次のノート開始時刻
  uint32_t deadline_ms=0;   // 演奏終了の時刻（開始+5000ms）
} g_mel;

static void melody_tick(uint32_t now){
  if (!g_mel.active) return;
  if (now >= g_mel.deadline_ms){
    M5.Speaker.stop();
    g_mel.active=false;
    return;
  }
  if (now < g_mel.next_ms) return;

  uint16_t f = melody_motif[g_mel.idx];
  M5.Speaker.tone(f, 220);             // 非ブロッキングで220ms鳴らす
  g_mel.idx = (g_mel.idx+1) % MELODY_LEN;
  g_mel.next_ms = now + 250;           // 休符込みで250msステップ
}

// ===========================
// WS2812 レインボー（非ブロッキング）
// ===========================
struct LedAnim {
  bool active=false;
  uint32_t t0=0;
  uint32_t deadline_ms=0;   // 開始+5000ms
} g_led;

static uint32_t color_wheel(uint8_t pos) {
  if(pos < 85) {
    return strip.Color(pos * 3, 255 - pos * 3, 0);
  } else if(pos < 170) {
    pos -= 85;
    return strip.Color(255 - pos * 3, 0, pos * 3);
  } else {
    pos -= 170;
    return strip.Color(0, pos * 3, 255 - pos * 3);
  }
}
static void led_clear(){
  for (int i=0;i<WS2812_COUNT;i++) strip.setPixelColor(i, 0);
  strip.show();
}
static void led_anim_tick(uint32_t now){
  if (!g_led.active) return;
  if (now >= g_led.deadline_ms){
    led_clear();
    g_led.active=false;
    return;
  }
  // レインボー・ランナー
  uint8_t base = (now / 8) & 0xFF;
  for (int i=0;i<WS2812_COUNT;i++){
    uint8_t hue = base + i*6;
    strip.setPixelColor(i, color_wheel(hue));
  }
  strip.show();
}

// ===========================
// 5秒演出の開始/停止 API
// ===========================
static void fx_start_5s(){
  uint32_t now = millis();
  // Melody
  g_mel.active=true; g_mel.idx=0; g_mel.next_ms=now; g_mel.deadline_ms=now+5000;
  M5.Speaker.setVolume(SPK_VOLUME);
  // LED
  g_led.active=true; g_led.t0=now; g_led.deadline_ms=now+5000;
  // UI
  M5.Display.fillRect(6, 66, M5.Display.width()-12, 14, TFT_BLACK);
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.drawString("A: Start FX (5s)", 6, 66);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
}

static void fx_stop(){
  // 即時停止（B押下時）
  M5.Speaker.stop();
  g_mel.active=false;
  led_clear();
  g_led.active=false;
  // UI
  M5.Display.fillRect(6, 66, M5.Display.width()-12, 14, TFT_BLACK);
  M5.Display.setTextColor(TFT_MAGENTA, TFT_BLACK);
  M5.Display.drawString("B: Force Stop FX", 6, 66);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
}

// ===========================
// Arduino 標準
// ===========================
void setup(){
  auto cfg = M5.config(); M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setBrightness(200);
  Serial.begin(115200);

  // WiFi / ESPNOW
  WiFi.mode(WIFI_STA); WiFi.setSleep(false); esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init()!=ESP_OK) { Serial.println("esp_now_init fail"); while(1) delay(500); }
  esp_now_register_recv_cb(on_recv);

  // Roller 初期化
  Roller_L.begin(ROLLER_LEFT_ADDR,  I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ);
  Roller_R.begin(ROLLER_RIGHT_ADDR, I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ);
  Roller_B.begin(ROLLER_BTN_ADDR,   I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ);
  Roller_L.setMode(1); Roller_L.setOutput(1); Roller_L.setSpeed(0);
  Roller_R.setMode(1); Roller_R.setOutput(1); Roller_R.setSpeed(0);
  Roller_B.setMode(1); Roller_B.setOutput(1); Roller_B.setSpeed(0);
  Roller_L.setSpeedMaxCurrent(60000);
  Roller_R.setSpeedMaxCurrent(60000);
  Roller_B.setSpeedMaxCurrent(60000);

  // スピーカー
  M5.Speaker.begin();
  M5.Speaker.setVolume(SPK_VOLUME);

  // NeoPixel
  strip.begin();
  strip.setBrightness(WS2812_BRIGHT);
  strip.show(); // クリア

  // UI
  M5.Display.clear();
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.drawString("CoreS3 ESPNOW -> Roller", 6, 6);
  M5.Display.drawString("LY->0x42  RY->0x43  L/R(push)->0x41", 6, 20);
  M5.Display.drawString("A: 5s Melody+LED / B: Force Stop", 6, 34);
}

void loop(){
  static uint32_t last_act=0, last_ui=0; uint32_t now=millis();

  if (g_has_new){
    g_has_new=false; last_act=now;

    // 左/右ジョイスティック（Y）を速度へ
    int32_t spd_L = map_i16_to_speed(g_ly*-1);
    int32_t spd_R = map_i16_to_speed(g_ry);
    apply_speed(Roller_L, spd_L);
    apply_speed(Roller_R, spd_R);

    // スティック押し L/R → 0x41
#if USE_STICK_LR_AS_LR
    bool L_on = (g_buttons & (1u<<2));
    bool R_on = (g_buttons & (1u<<3));
#elif USE_SIDE_AB_AS_LR
    bool L_on = (g_buttons & (1u<<0));
    bool R_on = (g_buttons & (1u<<1));
#else
    bool L_on=false, R_on=false;
#endif
    int32_t spd_B = L_on && !R_on ? +BTN_FIXED_SPEED :
                    (!L_on && R_on ? -BTN_FIXED_SPEED : 0);
    apply_speed(Roller_B, spd_B);

    // === A:開始 / B:停止（立ち上がりエッジ） ===
    uint16_t newly = (g_buttons) & (~g_prev_buttons);
    if (newly & (1u<<ABIT_A)) fx_start_5s();
    if (newly & (1u<<ABIT_B)) fx_stop();
    g_prev_buttons = g_buttons;
  }

  // 受信切れフェイルセーフ（0.5s停止）
  if (now - last_act > 500){
    apply_speed(Roller_L, 0);
    apply_speed(Roller_R, 0);
    apply_speed(Roller_B, 0);
  }

  // ノンブロッキング更新（演出）
  melody_tick(now);
  led_anim_tick(now);

  // 軽いUI
  if (now - last_ui > 200){
    last_ui=now; static bool hb=false; hb=!hb;
    M5.Display.fillCircle(M5.Display.width()-8, 10, 4, hb?TFT_GREEN:TFT_DARKGREY);
    char ln[64];
    sprintf(ln,"seq:%lu  btn:0x%X%s",
      (unsigned long)g_seq, (unsigned)g_buttons, g_is_v1?"  (V1 RX)":"");
    M5.Display.fillRect(6, 36, M5.Display.width()-12, 14, TFT_BLACK);
    M5.Display.drawString(ln, 6, 36);
    if (g_is_v1){
      M5.Display.fillRect(6, 52, M5.Display.width()-12, 14, TFT_BLACK);
      M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
      M5.Display.drawString("Update TX to V2(lx,ly,rx,ry) for full split", 6, 52);
      M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    } else {
      M5.Display.fillRect(6, 52, M5.Display.width()-12, 14, TFT_BLACK);
    }
  }

  delay(1);
}
