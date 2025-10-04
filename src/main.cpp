#include <Arduino.h>

//==================== ピン設定 ====================
constexpr int PIN_RESET = 1;   // TPS3424EVM -> MCU (Active HIGH, push-pull)
constexpr int PIN_INT   = 3;   // TPS3424EVM -> MCU (Active LOW, open-drain)
constexpr int PIN_KILL  = 5;   // MCU -> TPS3424EVM (Active LOW)

// 内蔵LED（アクティブLOW）
#ifndef LED_BUILTIN
  #define LED_BUILTIN 21       // 必要に応じて実機のLEDピンに変更
#endif
constexpr bool LED_ACTIVE_HIGH = false; // ★内蔵LEDがアクティブLOW

//==================== 時間パラメータ ====================
// INT直後のKILL無視窓を超えるための最小保持
constexpr uint32_t KILL_MIN_HOLD_MS  = 800;   // 例: 800ms（環境で調整可）
// 念のためのタイムアウト（RESETが下がらなくても解放する上限）
constexpr uint32_t KILL_TIMEOUT_MS   = 3000;  // 3s
// INT入力のチャタリング抑制
constexpr uint32_t INT_DEBOUNCE_MS   = 30;
// 起動直後の誤動作抑制（ログ表示のみの意味）
constexpr uint32_t STARTUP_GUARD_MS  = 300;
// 「RESETがHになってからこの時間以上継続している時だけKILLする」
constexpr uint32_t RESET_HIGH_MIN_MS_BEFORE_INT = 80; // 50〜150msで調整

//==================== 共有変数 ====================
volatile bool     g_intPending = false;
volatile uint32_t g_intStampMs = 0;
// RESETがHになった“瞬間”の時刻（Hの継続時間判定用）
volatile uint32_t g_resetHighSinceMs = 0;
// ISRで判定済みの「KILLすべきか」
volatile bool     g_doKillFlag = false;

bool     g_lastReset = false;
bool     g_killActive = false;
uint32_t g_killAssertAt = 0;
uint32_t g_lastReportMs = 0;

//==================== ユーティリティ ====================
inline void setLed(bool on) {
  digitalWrite(LED_BUILTIN, (LED_ACTIVE_HIGH ? (on ? HIGH : LOW) : (on ? LOW : HIGH)));
}

// KILLアイドル: 内部プルアップで確実にH（Hi-Z + Pull-up）
inline void killIdle() {
  pinMode(PIN_KILL, INPUT_PULLUP);
}

// KILLアサート: 強制L
inline void killAssert() {
  pinMode(PIN_KILL, OUTPUT);
  digitalWrite(PIN_KILL, LOW);
}

//==================== 割り込み（INT立下り） ====================
void IRAM_ATTR onIntFalling() {
  uint32_t now = millis();
  static uint32_t last = 0;
  if (now - last < INT_DEBOUNCE_MS) return; // デバウンス
  last = now;

  // RESETがHになってからの経過時間を確認
  uint32_t since = g_resetHighSinceMs; // 0なら直前までL
  bool highLongEnough = (since != 0) && (now - since >= RESET_HIGH_MIN_MS_BEFORE_INT);

  g_doKillFlag = highLongEnough;  // 直前にHが十分続いていた場合のみKILL
  g_intStampMs = now;
  g_intPending = true;
}

//==================== セットアップ ====================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println(F("=== TPS3424EVM + XIAO ESP32S3 Demo (RESET-H hold guard + KILL hold) ==="));
  Serial.println(F("Pins: RESET=GPIO1(in), INT=GPIO3(in,pullup,falling-IRQ), KILL=GPIO5(active LOW)"));
  Serial.println(F("Rule: If RESET has been HIGH >= RESET_HIGH_MIN_MS just before INT -> assert KILL and hold until RESET LOW."));

  pinMode(PIN_RESET, INPUT);          // push-pull出力を受ける
  pinMode(PIN_INT,   INPUT_PULLUP);   // OD想定でプルアップ
  pinMode(LED_BUILTIN, OUTPUT);
  killIdle();                         // 起動時は確実にKILL=H

  g_lastReset = (digitalRead(PIN_RESET) == HIGH);
  setLed(g_lastReset);
  if (g_lastReset) {
    g_resetHighSinceMs = millis();
  } else {
    g_resetHighSinceMs = 0;
  }

  attachInterrupt(digitalPinToInterrupt(PIN_INT), onIntFalling, FALLING);

  Serial.print(F("Startup: RESET="));
  Serial.print(g_lastReset ? F("HIGH") : F("LOW"));
  Serial.print(F(", INT="));
  Serial.println(digitalRead(PIN_INT) == LOW ? F("LOW(active)") : F("HIGH(idle)"));
}

//==================== ループ ====================
void loop() {
  // 1) RESET監視→LED反映＆H継続タイムスタンプ更新
  bool nowReset = (digitalRead(PIN_RESET) == HIGH);
  if (nowReset != g_lastReset) {
    g_lastReset = nowReset;
    setLed(g_lastReset);

    if (g_lastReset) {
      g_resetHighSinceMs = millis(); // Hになった瞬間に刻印
    } else {
      g_resetHighSinceMs = 0;        // Lに戻ったらリセット
    }

    Serial.print(F("[RESET change] RESET="));
    Serial.println(g_lastReset ? F("HIGH") : F("LOW"));
  }

  // 2) INTイベント処理（ISR外で重い処理）
  if (g_intPending) {
    noInterrupts();
    bool doKill = g_doKillFlag;
    uint32_t intAt = g_intStampMs;
    uint32_t since = g_resetHighSinceMs; // 参考表示用にコピー
    g_intPending = false;
    interrupts();

    int32_t highFor = (since == 0) ? -1 : (int32_t)intAt - (int32_t)since;

    Serial.print(F("[INT] t=")); Serial.print(intAt);
    Serial.print(F("ms, RESET high for "));
    Serial.print(highFor);
    Serial.print(F(" ms -> "));
    Serial.println(doKill ? F("KILL") : F("ignore"));

    if (doKill && !g_killActive) {
      Serial.print(F(" -> KILL=LOW (hold until RESET LOW & "));
      Serial.print(KILL_MIN_HOLD_MS);
      Serial.print(F(" ms, or "));
      Serial.print(KILL_TIMEOUT_MS);
      Serial.println(F(" ms timeout)"));
      killAssert();
      g_killActive   = true;
      g_killAssertAt = millis();
    } else if (!doKill) {
      Serial.println(F(" -> KILL ignored (RESET was not HIGH long enough before INT)"));
    }
  }

  // 3) KILL保持ロジック
  if (g_killActive) {
    uint32_t elapsed   = millis() - g_killAssertAt;
    bool     resetLow  = (digitalRead(PIN_RESET) == LOW);
    bool     minHoldOk = (elapsed >= KILL_MIN_HOLD_MS);

    if ((resetLow && minHoldOk) || (elapsed >= KILL_TIMEOUT_MS)) {
      killIdle();  // 入力プルアップへ戻してHに解放
      Serial.print(F("[KILL] released at t="));
      Serial.print(millis());
      Serial.print(F("ms (elapsed="));
      Serial.print(elapsed);
      Serial.print(F("ms, RESET="));
      Serial.print(digitalRead(PIN_RESET) ? F("H") : F("L"));
      Serial.println(F(")"));
      if (!resetLow) {
        Serial.println(F("WARN: Timeout reached but RESET stayed HIGH (check wiring/J9/IC option)."));
      }
      g_killActive = false;
    }
  }

  // 4) 1秒ごとに状態表示
  if (millis() - g_lastReportMs >= 1000) {
    g_lastReportMs = millis();
    Serial.print(F("STAT t=")); Serial.print(g_lastReportMs); Serial.print(F("ms | "));
    Serial.print(F("RESET=")); Serial.print(digitalRead(PIN_RESET) ? F("H") : F("L"));
    Serial.print(F(" INT="));   Serial.print(digitalRead(PIN_INT)   ? F("H") : F("L"));
    // KILLはモードで読み値が変わる: INPUT_PULLUP中はH、OUTPUT(LOW)中はL
    Serial.print(F(" KILL="));  Serial.println(digitalRead(PIN_KILL) ? F("H") : F("L"));
  }

  delay(1);
}
