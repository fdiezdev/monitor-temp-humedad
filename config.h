#pragma once

// Numeros GPIO, segun el cableado soldado.
constexpr int I2C_SDA_PIN = 4;
constexpr int I2C_SCL_PIN = 3;
constexpr unsigned char AHT10_ADDRESS = 0x38;
constexpr unsigned long SAMPLE_INTERVAL_MS = 10000;
constexpr unsigned long WIFI_RETRY_MS = 30000;
constexpr unsigned long WIFI_RADIO_RESET_MS = 120000;
constexpr unsigned long WIFI_RESTART_MS = 300000;
constexpr unsigned long WATCHDOG_TIMEOUT_MS = 30000;
constexpr unsigned long HTTP_TIMEOUT_MS = 4000;
constexpr char DEVICE_ID[] = "esp32c3-aht10-01";
constexpr char INFLUX_MEASUREMENT[] = "ambiente_aht10";
constexpr char NTP_SERVER_1[] = "pool.ntp.org";
constexpr char NTP_SERVER_2[] = "time.cloudflare.com";
