# AHT10 + ESP32-C3 Super Mini

Firmware independiente: lee temperatura y humedad cada 10 segundos, las muestra por serial USB a **115200 baudios** y las envía a InfluxDB. El pinout corresponde al sensor ya soldado.

## Conexiones

| AHT10 | ESP32-C3 Super Mini |
| --- | --- |
| VCC / VIN | 3V3 |
| GND | GND |
| SDA | **GPIO4** |
| SCL | **GPIO3** |

Bus I2C a 100 kHz, dirección `0x38`. Usar pull-ups a 3,3 V si el módulo no las incorpora. Los números son GPIO, no posiciones físicas del conector.

## Archivos

- `AHT10Monitor.ino`: firmware, incluido el controlador AHT10 por I2C con tiempos de espera limitados.
- `config.h`: pines, intervalo de lectura, nombre de dispositivo y nombre de medición.
- `secrets.h`: credenciales a completar; excluido de Git por el `.gitignore` de esta carpeta.
- `secrets.example.h`: plantilla vacía para recrear `secrets.h` después de clonar el repositorio.

## Configurar y cargar con Arduino IDE

1. Instalar **esp32 by Espressif Systems**, versión **3.x**, desde el gestor de placas. El firmware usa las librerías incluidas en ese paquete; no necesita librerías externas.
2. Abrir `AHT10Monitor.ino` desde esta carpeta.
3. Completar `secrets.h`:
   - `WIFI_SSID` y `WIFI_PASSWORD`: red Wi-Fi de **2,4 GHz**. Para una red abierta, contraseña vacía.
   - `INFLUX_URL`: URL base del servidor accesible desde la placa, por ejemplo `http://192.168.1.100:8086`. No usar `localhost` ni agregar `/api/v2/write`.
   - `INFLUX_TOKEN`: token con permiso de escritura sobre el destino.
   - `INFLUX_BUCKET`: bucket en InfluxDB 2/Cloud Serverless, o nombre de la base en InfluxDB 3 Core/Enterprise.
   - `INFLUX_ORG`: organización para InfluxDB 2/Cloud. En Core/Enterprise puede quedar vacío.
   - `INFLUX_CA_CERT`: para HTTPS, certificado raíz de la CA del servidor en formato PEM. Para HTTP, vacío.
4. Seleccionar placa **ESP32C3 Dev Module**, **USB CDC On Boot: Enabled**, flash **4 MB** (confirmar la capacidad de tu variante) y el puerto USB de la placa. Las demás opciones pueden quedar por defecto.
5. Cargar y abrir el monitor serial a **115200**. Si no entra en modo de carga: mantener **BOOT**, pulsar y soltar **RESET**, soltar **BOOT** y volver a cargar. Después de cargar, pulsar RESET si hace falta y volver a seleccionar el puerto.

No hace falta modificar el pinout. Para distinguir varias placas, cambiar `DEVICE_ID` en `config.h` antes de cargar cada una. Usar nombres sin saltos de línea.

Ejemplo para pegar un certificado (reemplazar el contenido por el PEM real):

```cpp
constexpr char INFLUX_CA_CERT[] = R"PEM(-----BEGIN CERTIFICATE-----
CONTENIDO_DEL_CERTIFICADO_CA
-----END CERTIFICATE-----
)PEM";
```

La conexión HTTPS verifica el certificado y requiere hora sincronizada por NTP. Hasta sincronizar, continúan las lecturas serial pero no se envían muestras. Ajustar los servidores NTP en `config.h` si la red requiere un servidor propio. HTTP transmite el token sin cifrar: usarlo únicamente en una red de confianza. No se imprimen contraseñas ni tokens en serial.

## Datos en InfluxDB

Se utiliza `POST /api/v2/write`, compatible con InfluxDB 2 y con la API compatible de InfluxDB 3 Core, Enterprise y Cloud Serverless. No incluye autenticación de InfluxDB 1.x. El bucket/base debe existir y permitir escritura con el token configurado.

Medición/tabla predeterminada: **`ambiente_aht10`**. Tag: **`device`**.

| Campo | Tipo | Unidad |
| --- | --- | --- |
| `temperatura_c` | float | °C |
| `humedad_pct` | float | % de humedad relativa |
| `rssi_dbm` | integer | dBm |

Ejemplo de line protocol:

```text
ambiente_aht10,device=esp32c3-aht10-01 temperatura_c=24.50,humedad_pct=51.20,rssi_dbm=-62i
```

El servidor asigna la hora UTC de recepción. Esta tabla es independiente de `sensores_chiller`: el firmware no modifica los reportes del chiller.

## Comportamiento y diagnóstico

- Una medición válida se imprime antes del intento de envío. Sin credenciales, funciona como lector serial.
- Se reintenta la conexión Wi-Fi cada 30 segundos; a los 2 y 4 minutos sin conexión con IP se reinicia la radio. Al alcanzar 5 minutos se reinicia la placa y comienza nuevamente la recuperación. Si falla el sensor, se intenta reinicializarlo en el siguiente ciclo.
- Una lectura inválida no se publica. El código limita las esperas I2C y el estado ocupado del sensor.
- Cada muestra se envía una sola vez. Si falla Wi-Fi, NTP o HTTP, se descarta; no hay cola persistente ni recuperación de datos históricos.
- Las operaciones HTTP/TLS son síncronas y pueden demorar el siguiente ciclo durante una falla de red. El intervalo de 10 segundos es nominal.
- HTTP 2xx: envío aceptado. 400: revisar destino/configuración/formato; 401/403: revisar token y permisos; 404: revisar URL y destino; 429: límite del servidor; 5xx: falla del servidor. Un valor negativo indica fallo de transporte; revisar red, URL, CA y hora NTP.
- Si no hay lecturas: revisar alimentación, masa común, SDA=4/SCL=3 y dirección `0x38`. Si no hay salida USB: revisar puerto y `USB CDC On Boot`, y reiniciar la placa.

## Watchdog y recuperación automática

El **Task Watchdog de ESP-IDF** supervisa la tarea que ejecuta `setup()` y `loop()`, con un timeout de **30 segundos** y `trigger_panic=true`. Solo se alimenta al terminar cada ciclo completo, además de la alimentación inicial de configuración. Una llamada bloqueada a I2C, Wi-Fi, DNS, HTTP/TLS o serial que impida completar el ciclo dispara el watchdog. También se vigila la tarea idle. Se verifica que la configuración del core reinicie ante un panic, y un error al configurar/alimentar el watchdog aborta en vez de continuar sin protección.

La desconexión Wi-Fi es diferente de un cuelgue: mientras el programa responde, sigue leyendo el sensor y aplica la recuperación progresiva. `RecoveryPolicy.h` controla los plazos sin que los reintentos los reinicien; se considera saludable tener `WL_CONNECTED` y una IP local distinta de cero. Al recuperarse la conexión, comienza un plazo nuevo para la próxima falla. Sin SSID configurado, se mantiene el modo serial sin reinicios por Wi-Fi, pero con watchdog activo.

Configurar en `config.h`: `WATCHDOG_TIMEOUT_MS`, `WIFI_RETRY_MS`, `WIFI_RADIO_RESET_MS` y `WIFI_RESTART_MS`. Los tiempos de recuperación son nominales: se evalúan al pasar por `loop()`. Al arrancar se imprime el motivo de reset de ESP-IDF (por ejemplo, 6 = Task Watchdog, 3 = reinicio por software).

Una red con IP pero sin acceso a InfluxDB no equivale a una desconexión Wi-Fi: el firmware vuelve a intentar enviar con cada muestra. Una petición que se cuelgue queda cubierta por el watchdog; errores HTTP que retornan normalmente no provocan reinicios. Si el router permanece apagado o la contraseña es incorrecta, habrá un nuevo reinicio cada 5 minutos hasta recuperar la conexión.

### Pruebas en la placa pendientes

1. Con la placa enviando datos, apagar el punto de acceso: verificar reintentos cada 30 s, reinicio de radio a los 2 min y reinicio completo a los 5 min. Volver a encender el punto de acceso y comprobar la reanudación del envío.
2. Para probar el watchdog en un firmware temporal, insertar `delay(45000);` al comienzo de `loop()`. Debe reiniciar aproximadamente a los 30 s aunque el scheduler siga activo. Quitar esa línea y volver a cargar el firmware normal. No usar OpenOCD/JTAG durante esta prueba, porque el debugger puede deshabilitar los watchdogs.
3. Desconectar el sensor y verificar que informa error sin quedar bloqueado; volver a conectarlo y comprobar recuperación. Apagar InfluxDB dejando Wi-Fi activo y comprobar que siguen las lecturas y los intentos de envío.

El watchdog permite recuperar bloqueos de software; no garantiza recuperación de fallas de alimentación o hardware.

## Verificación realizada

Firmware con watchdog compilado y enlazado para `esp32:esp32:esp32c3:CDCOnBoot=cdc` con Arduino ESP32 **3.3.10**: 1.125.933 bytes de programa (85%) y 38.516 bytes de variables globales (11%). Las pruebas de recuperación indicadas abajo pasaron.

La política de recuperación incluye pruebas de los límites temporales, reconexión, reinicio de radio/placa, recuperación exitosa, ausencia de SSID y desbordamiento de `millis()`. Ejecutarlas desde esta carpeta con:

```sh
c++ -std=c++11 -Wall -Wextra -Werror tests/recovery_test.cpp -o /tmp/aht10-recovery-test
/tmp/aht10-recovery-test
```

No se probó con hardware ni contra un servidor real.

En esta Mac, el ejecutable `ctags` de Arduino instalado tiene una arquitectura incompatible. Para verificar se desactivó únicamente la generación automática de prototipos mediante `--build-property tools.ctags.cmd.path=/usr/bin/true`; el código define las funciones antes de usarlas y se compiló y enlazó completo con el compilador real de ESP32-C3. No hace falta ese ajuste en una instalación normal de Arduino IDE.

## Referencias

- [Escritura compatible con InfluxDB 3 Core](https://docs.influxdata.com/influxdb3/core/api/write-data/).
- [Escritura HTTP en InfluxDB 2](https://docs.influxdata.com/influxdb/v2/write-data/developer-tools/api/).
- [Configuración USB CDC de Espressif](https://docs.espressif.com/projects/arduino-esp32/en/latest/tutorials/cdc_dfu_flash.html).
- [Watchdogs del ESP32-C3](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/system/wdts.html).
- [Referencia del protocolo AHT10 de Adafruit](https://github.com/adafruit/Adafruit_AHTX0).
