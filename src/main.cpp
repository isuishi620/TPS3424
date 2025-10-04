#include <Arduino.h>
//==================== ピン設定 ====================
constexpr int PIN_RESET = 1;   // TPS3424EVM -> MCU (Active HIGH, push-pull)
constexpr int PIN_INT   = 2;   // TPS3424EVM -> MCU (Active LOW, open-drain)
constexpr int PIN_KILL  = 3;   // MCU -> TPS3424EVM (Active LOW)

// 内蔵LED（ボードによりHIGH/LOW極性が異なるので定数で吸収）
#ifndef LED_BUILTIN
  #define LED_BUILTIN 21
#endif
constexpr bool LED_ACTIVE_HIGH = false; // 内蔵LEDがアクティブLOWなら false

//==================== 時間パラメータ ====================
// INT直後の「KILL無視窓」を超えるための最低保持
constexpr uint32_t KILL_MIN_HOLD_MS  = 10;
// 念のための上限（RESETがLOWにならなくても解放）
constexpr uint32_t KILL_TIMEOUT_MS   = 1000;
// INT入力のチャタリング抑制
constexpr uint32_t INT_DEBOUNCE_MS   = 10;
// 「RESETがHになってからこの時間以上継続している時だけKILLする」
constexpr uint32_t RESET_HIGH_MIN_MS_BEFORE_INT = 10; // 環境で50〜150ms程度を調整

//==================== 共有変数 ====================
// ISR→main 連携用（必要最小限）
volatile bool     g_intPending = false;
volatile uint32_t g_resetHighSinceMs = 0;  // RESETがHになった瞬間の刻印（0なら直前までL）
volatile bool     g_doKillFlag = false;    // ISRで判定済み「KILLすべきか」

// ループ側状態
bool     g_lastReset = false;
bool     g_killActive = false;
uint32_t g_killAssertAt = 0;

//==================== ユーティリティ ====================
inline void setLed(bool on) {
  digitalWrite(
    LED_BUILTIN,
    (LED_ACTIVE_HIGH ? (on ? HIGH : LOW) : (on ? LOW : HIGH))
  );
}

// KILLアイドル: Hi-Z + 内部プルアップでH維持
inline void killIdle() {
  pinMode(PIN_KILL, INPUT_PULLUP);
}

// KILLアサート: 強制L
inline void killAssert() {
  pinMode(PIN_KILL, OUTPUT);
  digitalWrite(PIN_KILL, LOW);
}

//==================== 割り込み（INT立下り） ====================
// INT直前に RESET=H が十分続いていたかで KILL可否を即決
void IRAM_ATTR onIntFalling() {
  uint32_t now = millis();
  static uint32_t last = 0;
  if (now - last < INT_DEBOUNCE_MS) return; // デバウンス
  last = now;

  uint32_t since = g_resetHighSinceMs; // 0なら直前までL
  bool highLongEnough = (since != 0) && (now - since >= RESET_HIGH_MIN_MS_BEFORE_INT);

  g_doKillFlag = highLongEnough;
  g_intPending = true;
}

//==================== セットアップ ====================
void setup() {
  pinMode(PIN_RESET, INPUT);          // TPS3424EVMのpush-pull出力を受ける
  pinMode(PIN_INT,   INPUT_PULLUP);   // OD想定でプルアップ
  pinMode(LED_BUILTIN, OUTPUT);
  killIdle();                         // 起動時は確実にKILL=H

  g_lastReset = (digitalRead(PIN_RESET) == HIGH);
  setLed(g_lastReset);
  g_resetHighSinceMs = g_lastReset ? millis() : 0;

  attachInterrupt(digitalPinToInterrupt(PIN_INT), onIntFalling, FALLING);
}

//==================== ループ ====================
void loop() {
  // 1) RESET監視 → LED反映＆H継続タイムスタンプ更新
  bool nowReset = (digitalRead(PIN_RESET) == HIGH);
  if (nowReset != g_lastReset) {
    g_lastReset = nowReset;
    setLed(g_lastReset);
    g_resetHighSinceMs = g_lastReset ? millis() : 0;
  }

  // 2) INTイベント処理（ISR外で軽量に）
  if (g_intPending) {
    noInterrupts();
    bool doKill = g_doKillFlag;
    g_intPending = false;
    interrupts();

    if (doKill && !g_killActive) {
      killAssert();
      g_killActive   = true;
      g_killAssertAt = millis();
    }
  }

  // 3) KILL保持＆解放ロジック
  if (g_killActive) {
    uint32_t elapsed   = millis() - g_killAssertAt;
    bool     resetLow  = (digitalRead(PIN_RESET) == LOW);
    bool     minHoldOk = (elapsed >= KILL_MIN_HOLD_MS);

    if ((resetLow && minHoldOk) || (elapsed >= KILL_TIMEOUT_MS)) {
      killIdle();
      g_killActive = false;
    }
  }

  // 4) 最小ディレイ（WDTケア＆CPU占有防止）
  delay(1);
}
