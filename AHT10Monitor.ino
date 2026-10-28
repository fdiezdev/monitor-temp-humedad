/* ESP32-C3 Super Mini + AHT10: serial USB y escritura HTTP a InfluxDB.
 * Solo requiere el core Arduino ESP32; driver I2C con esperas acotadas.
 * API /api/v2/write: InfluxDB 2 y versiones de InfluxDB 3 compatibles.
 */
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <sdkconfig.h>
#include "config.h"
#include "secrets.h"
#include "RecoveryPolicy.h"

// El panic del watchdog debe reiniciar, no quedarse detenido en un debugger.
#if !defined(CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT) && !defined(CONFIG_ESP_SYSTEM_PANIC_SILENT_REBOOT)
#error "Seleccionar una configuracion de panic que reinicie el ESP32"
#endif
static_assert(WIFI_RETRY_MS < WIFI_RADIO_RESET_MS && WIFI_RADIO_RESET_MS < WIFI_RESTART_MS,
              "Los plazos de recuperacion WiFi deben ser crecientes");
static_assert(WATCHDOG_TIMEOUT_MS > HTTP_TIMEOUT_MS + 5000,
              "Dejar margen para HTTP y el handshake TLS");

bool sensorReady = false;
bool busReady = false;
bool influxConfigured = false;
bool useTls = false;
bool wasConnected = false;
uint32_t lastSampleMs = 0;
RecoveryPolicy wifiRecovery;
String writeUrl;

void configureWatchdog() {
  esp_task_wdt_config_t config = {};
  config.timeout_ms = WATCHDOG_TIMEOUT_MS;
  config.idle_core_mask = (1U << portNUM_PROCESSORS) - 1;
  config.trigger_panic = true;
  esp_err_t result = esp_task_wdt_reconfigure(&config);
  if (result == ESP_ERR_INVALID_STATE) result = esp_task_wdt_init(&config);
  ESP_ERROR_CHECK(result);
  // setup() y loop() se ejecutan en la misma tarea de Arduino.
  if (esp_task_wdt_status(nullptr) != ESP_OK) {
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
  }
  ESP_ERROR_CHECK(esp_task_wdt_reset());
}

// AHT10: comandos reset=0xBA, calibracion=0xE1, medicion=0xAC.
bool sensorCommand(const uint8_t *data, size_t length) {
  Wire.beginTransmission(AHT10_ADDRESS);
  Wire.write(data, length);
  return Wire.endTransmission() == 0;
}

bool sensorStatus(uint8_t &status) {
  if (Wire.requestFrom(AHT10_ADDRESS, static_cast<uint8_t>(1)) != 1) return false;
  status = Wire.read();
  return true;
}

bool waitForSensor(uint8_t &status) {
  const uint32_t start = millis();
  do {
    if (!sensorStatus(status)) return false;
    if (!(status & 0x80)) return true;
    delay(5);
  } while (millis() - start < 500);
  return false;
}

bool initSensor() {
  const uint8_t reset[] = {0xBA};
  const uint8_t calibrate[] = {0xE1, 0x08, 0x00};
  uint8_t status = 0;
  if (!sensorCommand(reset, sizeof(reset))) return false;
  delay(40);
  if (!waitForSensor(status)) return false;
  if (!sensorCommand(calibrate, sizeof(calibrate))) return false;
  delay(20);
  return waitForSensor(status) && (status & 0x08);
}

bool readSensor(float &temperature, float &humidity) {
  const uint8_t measure[] = {0xAC, 0x33, 0x00};
  if (!sensorCommand(measure, sizeof(measure))) return false;
  delay(80);
  uint8_t status = 0;
  if (!waitForSensor(status) || !(status & 0x08)) return false;
  if (Wire.requestFrom(AHT10_ADDRESS, static_cast<uint8_t>(6)) != 6) return false;
  uint8_t data[6];
  for (uint8_t &value : data) value = Wire.read();
  if ((data[0] & 0x80) || !(data[0] & 0x08)) return false;
  const uint32_t rawH = (uint32_t(data[1]) << 12) |
                        (uint32_t(data[2]) << 4) | (data[3] >> 4);
  const uint32_t rawT = (uint32_t(data[3] & 0x0F) << 16) |
                        (uint32_t(data[4]) << 8) | data[5];
  humidity = rawH * (100.0f / 1048576.0f);
  temperature = rawT * (200.0f / 1048576.0f) - 50.0f;
  return temperature >= -40.0f && temperature <= 85.0f &&
         humidity >= 0.0f && humidity <= 100.0f;
}

String urlEncode(const char *value) {
  const char hex[] = "0123456789ABCDEF";
  String result;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(value); *p; ++p) {
    const unsigned char c = *p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      result += char(c);
    } else {
      result += '%'; result += hex[c >> 4]; result += hex[c & 15];
    }
  }
  return result;
}

String escapeLineName(const char *value) {
  String result;
  for (const char *p = value; *p; ++p) {
    if (*p == ',' || *p == ' ' || *p == '=' || *p == '\\') result += '\\';
    result += *p;
  }
  return result;
}

void serviceWifi() {
  const bool connected = WiFi.status() == WL_CONNECTED &&
                         WiFi.localIP() != IPAddress(0, 0, 0, 0);
  if (connected && !wasConnected) {
    Serial.println("[WiFi] Conectado");
    configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);
  } else if (!connected && wasConnected) {
    Serial.println("[WiFi] Conexion perdida; las lecturas serial continuan");
  }
  wasConnected = connected;
  const RecoveryPolicy::Action action = wifiRecovery.update(
      millis(), WIFI_SSID[0] != '\0', connected,
      WIFI_RETRY_MS, WIFI_RADIO_RESET_MS, WIFI_RESTART_MS);
  switch (action) {
    case RecoveryPolicy::Action::Restart:
      Serial.println("[WiFi] Plazo sin conexion agotado; reiniciando placa");
      delay(100);
      ESP.restart();
      break;
    case RecoveryPolicy::Action::ResetRadio:
      Serial.println("[WiFi] Reiniciando radio y reconectando");
      WiFi.disconnect(true, false); // Apagar radio sin borrar credenciales.
      WiFi.mode(WIFI_OFF);
      delay(100);
      WiFi.mode(WIFI_STA);
      WiFi.setAutoReconnect(true);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      break;
    case RecoveryPolicy::Action::Reconnect:
      Serial.println("[WiFi] Reintentando conexion");
      WiFi.disconnect(false, false);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      break;
    case RecoveryPolicy::Action::None:
      break;
  }
}

void publishReading(float temperature, float humidity) {
  if (!influxConfigured) return;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[InfluxDB] Sin WiFi; muestra no enviada");
    return;
  }
  if (useTls && time(nullptr) < 1700000000) {
    Serial.println("[InfluxDB] Esperando hora NTP para validar TLS; muestra no enviada");
    return;
  }

  // Sin timestamp: el servidor asigna la hora de recepcion UTC.
  String line = escapeLineName(INFLUX_MEASUREMENT) + ",device=" + escapeLineName(DEVICE_ID);
  line += " temperatura_c=" + String(temperature, 2);
  line += ",humedad_pct=" + String(humidity, 2);
  line += ",rssi_dbm=" + String(WiFi.RSSI()) + "i";

  WiFiClient plain;
  WiFiClientSecure secure;
  if (useTls) {
    secure.setCACert(INFLUX_CA_CERT);
    secure.setHandshakeTimeout(5); // Segundos.
  }
  WiFiClient &transport = useTls ? static_cast<WiFiClient &>(secure) : plain;
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(transport, writeUrl)) {
    Serial.println("[InfluxDB] No se pudo iniciar HTTP");
    return;
  }
  http.addHeader("Authorization", String("Token ") + INFLUX_TOKEN);
  http.addHeader("Content-Type", "text/plain; charset=utf-8");
  const int code = http.POST(line);
  if (code >= 200 && code < 300) {
    Serial.printf("[InfluxDB] Enviado (HTTP %d)\n", code);
  } else {
    // No imprimir headers, URL ni respuesta: pueden contener datos sensibles.
    Serial.printf("[InfluxDB] Fallo (%d); muestra descartada\n", code);
  }
  http.end();
}

void setup() {
  configureWatchdog(); // Vigila tambien inicializacion, I2C y WiFi de setup().
  Serial.begin(115200);
  const uint32_t start = millis();
  while (!Serial && millis() - start < 1500) delay(10);
  Serial.println("\n=== AHT10 / ESP32-C3 Super Mini ===");
  Serial.printf("[Sistema] Motivo de arranque: %d | Watchdog: %lu ms\n",
                static_cast<int>(esp_reset_reason()), WATCHDOG_TIMEOUT_MS);
  Serial.printf("[I2C] SDA=GPIO%d, SCL=GPIO%d\n", I2C_SDA_PIN, I2C_SCL_PIN);
  busReady = Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 100000);
  Wire.setTimeOut(50);
  delay(40);
  if (!busReady) Serial.println("[I2C] No se pudo iniciar el bus");

  writeUrl = INFLUX_URL;
  while (writeUrl.endsWith("/")) writeUrl.remove(writeUrl.length() - 1);
  useTls = writeUrl.startsWith("https://");
  influxConfigured = (useTls || writeUrl.startsWith("http://")) &&
                     INFLUX_BUCKET[0] && INFLUX_TOKEN[0];
  if (useTls && !INFLUX_CA_CERT[0]) {
    influxConfigured = false;
    Serial.println("[Config] HTTPS requiere INFLUX_CA_CERT en secrets.h");
  }
  writeUrl += "/api/v2/write?precision=s&bucket=" + urlEncode(INFLUX_BUCKET);
  if (INFLUX_ORG[0]) writeUrl += "&org=" + urlEncode(INFLUX_ORG);
  if (!influxConfigured) Serial.println("[Config] Completar secrets.h; solo salida serial");

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  wifiRecovery.begin(millis());
  if (WIFI_SSID[0]) WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  else Serial.println("[Config] Falta WIFI_SSID en secrets.h");
  lastSampleMs = millis() - SAMPLE_INTERVAL_MS;
}

void loop() {
  serviceWifi();
  if (millis() - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = millis();
    if (!busReady) {
      busReady = Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 100000);
      Wire.setTimeOut(50);
    }
    if (busReady && !sensorReady) sensorReady = initSensor();
    float temperature = 0, humidity = 0;
    if (sensorReady && readSensor(temperature, humidity)) {
      Serial.printf("[AHT10] Temperatura: %.2f C | Humedad: %.2f %%\n", temperature, humidity);
      publishReading(temperature, humidity);
    } else {
      sensorReady = false;
      Serial.println("[AHT10] Error de lectura; revisar conexion. Se reintentara.");
    }
  }
  delay(5);
  // Alimentar SOLO al completar todo el ciclo. Nunca desde un timer, otra
  // tarea ni dentro de los reintentos: eso ocultaria un bloqueo de este loop.
  ESP_ERROR_CHECK(esp_task_wdt_reset());
}
