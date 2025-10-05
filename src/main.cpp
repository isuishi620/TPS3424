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

//=== LEDパターン指定 ===
constexpr uint8_t  STARTUP_BLINK_COUNT   = 3;
constexpr uint32_t STARTUP_BLINK_ON_MS   = 120;  // 「100–150ms」の中庸
constexpr uint32_t STARTUP_BLINK_OFF_MS  = 120;  // 同上
constexpr uint32_t POWERDOWN_BLINK_ON_MS = 500;

//=== 起動時のKILL抑止 ===
constexpr uint32_t STARTUP_INHIBIT_MAX_MS = 1000; // 念のための上限

//==================== 共有変数 ====================
// ISR→main 連携用（必要最小限）
volatile bool     g_intPending = false;
volatile uint32_t g_resetHighSinceMs = 0;  // RESETがHになった瞬間の刻印（0なら直前までL）
volatile bool     g_doKillFlag = false;    // ISRで判定済み「KILLすべきか」
volatile bool     g_startupInhibit = false;

// ループ側状態
bool     g_lastReset = false;
bool     g_killActive = false;
uint32_t g_killAssertAt = 0;
uint32_t g_startupInhibitAtMs  = 0;

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

// LED点滅（ブロッキング、終了時は消灯）
inline void blinkNTimes(uint8_t n, uint32_t onMs, uint32_t offMs) {
  for (uint8_t i = 0; i < n; ++i) {
    setLed(true);
    delay(onMs);
    setLed(false);
    if (i + 1 < n) delay(offMs);
  }
}

// 起動シーケンス：短点灯×3回 & KILL抑止ON
inline void startPowerOnSequence() {
  g_startupInhibit = true;
  g_startupInhibitAtMs = millis();
  blinkNTimes(STARTUP_BLINK_COUNT, STARTUP_BLINK_ON_MS, STARTUP_BLINK_OFF_MS);
  // 通常動作ではLED消灯を維持
  setLed(false);
}

// 電源OFFシーケンス：やや長い点灯×1回
inline void powerOffIndication() {
  setLed(true);
  delay(POWERDOWN_BLINK_ON_MS);
  setLed(false);
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

  // 起動直後の点滅中はKILL抑止
  bool killAllowed = !g_startupInhibit;

  g_doKillFlag = highLongEnough && killAllowed;
  g_intPending = true;
}

//==================== セットアップ ====================
void setup() {
  pinMode(PIN_RESET, INPUT);          // TPS3424EVMのpush-pull出力を受ける
  pinMode(PIN_INT,   INPUT_PULLUP);   // OD想定でプルアップ
  pinMode(LED_BUILTIN, OUTPUT);
  setLed(false);
  killIdle();                         // 起動時は確実にKILL=H

  g_lastReset = (digitalRead(PIN_RESET) == HIGH);
  g_resetHighSinceMs = g_lastReset ? millis() : 0;

  // 起動時にすでにRESET=Hなら、毎回起動点滅を実行（A案）
  if (g_lastReset) {
    startPowerOnSequence();
  }

  attachInterrupt(digitalPinToInterrupt(PIN_INT), onIntFalling, FALLING);
}

//==================== ループ ====================
void loop() {
  // 1) RESET監視（立下り/立上りでのみLEDアクション）
  bool nowReset = (digitalRead(PIN_RESET) == HIGH);
  if (nowReset != g_lastReset) {
    if (nowReset) {
      // 立上り: 電源ONインジケータ（毎回実行）
      g_resetHighSinceMs = millis();
      startPowerOnSequence();
    } else {
      // 立下り: 電源OFFインジケータ
      g_resetHighSinceMs = 0;
      powerOffIndication();
    }
    g_lastReset = nowReset;
  }

  // 1.5) 起動シーケンスのKILL抑止解除条件
  if (g_startupInhibit) {
    uint32_t elapsed = millis() - g_startupInhibitAtMs;
    // RESETが一度Lになったら解除、もしくは最大時間経過で解除
    if (!nowReset || (elapsed >= STARTUP_INHIBIT_MAX_MS)) {
      g_startupInhibit = false;
    }
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
