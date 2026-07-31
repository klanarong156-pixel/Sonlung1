# SmartFarm Pro

Production-oriented Smart Farm firmware and dashboard for ESP8266 and ESP32. The project preserves the original MQTT pump, relay, DHT11, RTC, schedule, and web-dashboard features while splitting the firmware into reusable modules.

## Analysis Report

### Folder structure
- `config/config.h` central compile-time settings, pin maps, MQTT defaults, timing constants, and security defaults.
- `core/` contains WiFi, MQTT, REST API, OTA, watchdog, scheduler, storage, Telegram stub, and logging services.
- `drivers/` contains hardware drivers for relay, DHT, RTC, and soil expansion placeholder.
- `web/` contains LittleFS dashboard assets: `index.html`, `style.css`, `app.js`, and PWA `manifest.json`.
- `main.cpp` is the shared firmware entry point.
- `Smartfarm_Refactored.ino` is an Arduino IDE wrapper around `main.cpp`.
- `platformio.ini` defines ESP8266 and ESP32 build environments.

### Dependencies and versions
PlatformIO dependencies are pinned as semver ranges:
- WiFiManager `^2.0.17`
- PubSubClient `^2.8`
- DHT sensor library `^1.4.6`
- Adafruit Unified Sensor `^1.1.14`
- RTClib `^2.1.4`
- ArduinoOTA from each ESP Arduino core
- LittleFS from each ESP Arduino core

### Compile errors found in the uploaded project
- Watchdog used `WDTO_8S` without including the AVR watchdog header and the requested ESP8266 signature was not used.
- The project was a single ESP8266-only sketch and could not compile for ESP32 because it directly included `ESP8266WiFi.h` and NodeMCU `D*` pins only.
- The web dashboard was a monolithic HTML file and was not arranged for LittleFS upload.
- No PlatformIO manifest existed.

### Runtime bugs and blocking code
- WiFiManager was configured in blocking captive-portal mode.
- MQTT reconnect logic existed, but no generalized service recovery architecture existed.
- Relay state persistence used EEPROM only and did not include backup configuration files.
- Manual pump control could bypass zone state reconciliation in some MQTT paths.

### Memory problems
- The previous web app and firmware used many dynamic `String` operations and large inline HTML/JavaScript payloads.
- MQTT payload buffers were small for growth and not centralized.
- Configuration was stored as raw EEPROM data only, with no backup copy.

### Security problems
- MQTT credentials and dashboard unlock password were hard-coded legacy defaults.
- The old dashboard used local-only password checks.
- API authentication did not exist.
- TLS certificate validation was disabled for MQTT to preserve compatibility with constrained boards; production deployments should install a CA certificate.

### Architecture overview
- MQTT publishes retained relay/pump state and JSON telemetry to `farm/data`, with LWT on `farm/status` and birth on `farm/birth`.
- REST API exposes `/api/status`, `/api/control`, `/api/config`, `/api/schedule`, `/api/reboot`, `/api/restart`, `/api/log`, and `/api/history`.
- Relay control is centralized in `RelayController`.
- Sensor reads are non-blocking and interval-based in `DhtDriver`.
- OTA uses ArduinoOTA callbacks and web dashboard OTA status guidance.
- WiFi uses static hostname, auto reconnect, RSSI reporting, and reconnect counter.
- Storage uses LittleFS with a binary config and backup config.
- Scheduler supports multiple configured schedules, zones, holiday mode, RTC operation, and non-blocking checks.

## Optimization Report
- Removed all `delay()` usage.
- Added ESP8266 `ESP.wdtEnable(8000)` and cooperative watchdog feeds.
- Split large functions into services and drivers.
- Added reusable logger, storage, relay, sensor, WiFi, MQTT, API, OTA, and scheduler modules.
- Moved dashboard assets to LittleFS-friendly files.

## Security Report
- API write operations require an auth token header or token argument.
- Dashboard escapes rendered text.
- Input validation is present for relay IDs and state writes.
- Password hash constant is centralized for replacement.
- MQTT credentials remain configurable in `config/config.h`; rotate before production.

## Compile Instructions

### PlatformIO
```bash
pio run -e esp8266
pio run -e esp32
pio run -t uploadfs -e esp8266
```

### Arduino IDE
1. Open `Smartfarm_Refactored.ino`.
2. Install ESP8266 or ESP32 board support.
3. Install the dependencies listed above.
4. Select the correct board.
5. Upload the sketch.

## Test Procedure
1. Build ESP8266 and ESP32 targets.
2. Upload LittleFS web assets.
3. Boot device and configure WiFi through `SmartFarm_Setup` if needed.
4. Verify `/api/status` returns JSON.
5. Toggle each relay with `/api/control` using header `X-Auth-Token: admin`.
6. Verify MQTT topics: `farm/status`, `farm/birth`, `farm/data`, `farm/relay/+/status`, and `farm/pump/status`.
7. Disconnect WiFi/MQTT and verify automatic reconnect and retained status recovery.
8. Run ArduinoOTA upload and confirm automatic reboot.

## Git Commit Messages
- `refactor: modularize smart farm firmware`
- `feat: add littlefs dashboard and rest api`
- `chore: add platformio esp8266 esp32 builds`

## Changelog
- Added PlatformIO dual-target support.
- Added modular `config`, `core`, `drivers`, and `web` folder structure.
- Added REST API and modern responsive dashboard.
- Added non-blocking WiFi/MQTT/sensor/scheduler loops.
- Added LittleFS configuration backup.
- Added ArduinoOTA service.
- Replaced deprecated watchdog usage with `ESP.wdtEnable(8000)` on ESP8266.
