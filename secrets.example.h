#pragma once

// Completar antes de cargar. Este archivo esta excluido de Git.
constexpr char WIFI_SSID[] = "";
constexpr char WIFI_PASSWORD[] = "";

// URL base sin /api/v2/write. Ej.: http://192.168.1.100:8086
// Para Cloud, usar la URL HTTPS indicada por InfluxDB.
constexpr char INFLUX_URL[] = "";
constexpr char INFLUX_TOKEN[] = "";  // Token con permiso de escritura.
constexpr char INFLUX_BUCKET[] = ""; // Bucket (v2/Cloud) o database (v3 Core).
constexpr char INFLUX_ORG[] = "";    // Organizacion v2/Cloud; Core la ignora.

// Para HTTPS: pegar el certificado PEM de la CA que firma al servidor.
// Se pueden concatenar certificados PEM. Para HTTP, dejar vacio.
// No se deshabilita la verificacion TLS.
constexpr char INFLUX_CA_CERT[] = R"PEM()PEM";
