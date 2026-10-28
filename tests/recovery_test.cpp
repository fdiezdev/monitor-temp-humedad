#include <assert.h>
#include <stdio.h>
#include "../RecoveryPolicy.h"
#include "../config.h"

using Action = RecoveryPolicy::Action;

static Action step(RecoveryPolicy &policy, uint32_t now,
                   bool configured = true, bool healthy = false) {
  return policy.update(now, configured, healthy, WIFI_RETRY_MS,
                       WIFI_RADIO_RESET_MS, WIFI_RESTART_MS);
}

int main() {
  RecoveryPolicy policy;
  const uint32_t start = 1234;
  policy.begin(start);
  assert(step(policy, start + WIFI_RETRY_MS - 1) == Action::None);
  assert(step(policy, start + WIFI_RETRY_MS) == Action::Reconnect);
  assert(step(policy, start + WIFI_RETRY_MS + 1) == Action::None);
  assert(step(policy, start + WIFI_RADIO_RESET_MS) == Action::ResetRadio);
  assert(step(policy, start + WIFI_RADIO_RESET_MS + WIFI_RETRY_MS) == Action::Reconnect);
  assert(step(policy, start + 2 * WIFI_RADIO_RESET_MS) == Action::ResetRadio);
  // Los reintentos y el reinicio de radio no posponen el reinicio total.
  assert(step(policy, start + WIFI_RESTART_MS) == Action::Restart);

  policy.begin(start);
  const uint32_t recovered = start + WIFI_RESTART_MS - 1;
  assert(step(policy, recovered, true, true) == Action::None);
  assert(step(policy, recovered + 1) == Action::None);
  assert(step(policy, recovered + WIFI_RETRY_MS) == Action::Reconnect);
  assert(step(policy, recovered + WIFI_RESTART_MS) == Action::Restart);

  // Sin SSID, mantener el modo lector serial sin bucles de reinicio.
  policy.begin(start);
  assert(step(policy, start + WIFI_RESTART_MS, false) == Action::None);
  assert(step(policy, start + 2 * WIFI_RESTART_MS, false) == Action::None);

  // Cruce de millis() por cero tras ~49,7 dias.
  const uint32_t rolloverStart = UINT32_MAX - 1000;
  policy.begin(rolloverStart);
  assert(step(policy, rolloverStart + uint32_t(WIFI_RETRY_MS) - 1) == Action::None);
  assert(step(policy, rolloverStart + uint32_t(WIFI_RETRY_MS)) == Action::Reconnect);
  assert(step(policy, rolloverStart + uint32_t(WIFI_RADIO_RESET_MS)) == Action::ResetRadio);
  assert(step(policy, rolloverStart + uint32_t(WIFI_RESTART_MS)) == Action::Restart);
  puts("OK: reconexion, reinicio de radio/placa, recuperacion, sin SSID y rollover");
}
