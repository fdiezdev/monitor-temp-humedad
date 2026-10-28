#pragma once
#include <stdint.h>

// Politica independiente del hardware; aritmetica segura al desbordar millis().
class RecoveryPolicy {
 public:
  enum class Action { None, Reconnect, ResetRadio, Restart };

  void begin(uint32_t now) {
    lastHealthy = lastAttempt = lastRadioReset = now;
  }

  Action update(uint32_t now, bool configured, bool healthy,
                uint32_t retryMs, uint32_t radioMs, uint32_t restartMs) {
    if (!configured || healthy) {
      begin(now);
      return Action::None;
    }
    // Solo una conexion con IP reinicia el plazo de recuperacion.
    if (uint32_t(now - lastHealthy) >= restartMs) return Action::Restart;
    if (uint32_t(now - lastRadioReset) >= radioMs) {
      lastRadioReset = lastAttempt = now;
      return Action::ResetRadio;
    }
    if (uint32_t(now - lastAttempt) >= retryMs) {
      lastAttempt = now;
      return Action::Reconnect;
    }
    return Action::None;
  }

 private:
  uint32_t lastHealthy = 0;
  uint32_t lastAttempt = 0;
  uint32_t lastRadioReset = 0;
};
