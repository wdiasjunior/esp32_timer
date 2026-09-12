# ESP32 Relay Timer

Custom firmware for an ESP32-C3 Super Mini that controls a boiler relay via Home Assistant over MQTT,
with a DS18B20 temperature sensor acting as a thermostat.

Disclaimer: this was completely vibe coded and I don't really care about whether or not it isn't since I just needed a smart switch with time schedules and to be configurable through Home Assistant. Use at your own risk.

## Features

- **Three modes**: OFF / ON / AUTO — controllable from Home Assistant
  - **OFF**: relay off
  - **ON**: schedule override — heats regardless of the time of day, but still stops at the temperature limit
  - **AUTO**: heats only inside the configured time ranges, and stops at the temperature limit
- **Scheduled auto mode**: Multiple configurable time ranges (e.g. 07:00-10:00, 14:00-18:00)
- **Temperature limit**: DS18B20 thermostat with hysteresis (default 45 °C, back on 2 °C below), adjustable from a slider in Home Assistant
- **Relay protection**: temperature-driven switching respects a minimum on/off time (default 60 s) so a noisy sensor can't short-cycle the boiler
- **Sensor fault handling**: after 3 consecutive bad reads the sensor is declared faulty, HA shows the temperature as unknown and a "Temp Sensor Fault" diagnostic turns on. While faulty, the relay is **kept ON** inside the heating window so the boiler's own mechanical thermostat takes over. Change `thermostatWantsHeat()` if you'd rather fail OFF.
- **MQTT Discovery**: Device auto-registers in Home Assistant — no manual YAML needed
- **Persistent state**: Mode, schedules and temperature limit survive reboots (stored in flash)
- **Standalone operation**: Keeps running in last mode even if Home Assistant is down

## Hardware

- ESP32-C3 Super Mini
- Arduino relay shield (single relay)
- DS18B20 temperature sensor (waterproof probe) on GPIO2, with a **4.7 kΩ pull-up** between the data line and 3.3 V.
  The pull-up is mandatory: GPIO2 is a strapping pin on the ESP32-C3 and must read high at boot, and the 1-Wire bus needs it anyway.

## Setup

### 1. Arduino IDE setup

1. Install **Arduino IDE** (2.x recommended)
2. Add ESP32 board support: **File → Preferences → Additional Board Manager URLs** → add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. **Tools → Board → Boards Manager** → search "esp32" → install **esp32 by Espressif Systems**
4. Install libraries via **Sketch → Include Library → Manage Libraries**:
   - **PubSubClient** by Nick O'Leary
   - **ArduinoJson** by Benoit Blanchon (v7.x)
   - **OneWire** by Paul Stoffregen
   - **DallasTemperature** by Miles Burton

### 2. Configure the firmware

Edit `esp32_timer/config.h` with your:
- Wi-Fi SSID and password
- MQTT broker credentials (see step 3)
- Relay GPIO pin (default: GPIO 10) and DS18B20 pin (default: GPIO 2)
- Temperature defaults: limit, hysteresis, allowed range, minimum relay on/off time
- Timezone offset (default: UTC-3)

`config.h` is tracked in git with placeholder credentials. Don't commit your real Wi-Fi password.

### 3. Set up Mosquitto in Home Assistant

1. Go to **Settings → Add-ons → Add-on Store**
2. Install **Mosquitto broker**
3. Start the addon
4. Create an MQTT user: **Settings → People → Users** → add a user (e.g. `mqtt_user`)
5. Add MQTT integration: **Settings → Devices & Services → Add Integration → MQTT** (auto-detects Mosquitto)

### 4. Flash the firmware

1. Open `esp32_timer/esp32_timer.ino` in Arduino IDE
2. Select board: **Tools → Board → esp32 → ESP32C3 Dev Module**
3. Select port: **Tools → Port** → your ESP32's USB port
4. Set **USB CDC On Boot: Enabled** in Tools menu (for serial output over native USB)
5. Click **Upload**
6. Open **Tools → Serial Monitor** at 115200 baud to verify boot sequence

### 5. Verify in Home Assistant

After the ESP32 boots and connects, go to **Settings → Devices & Services → MQTT**.
You should see a **Relay Timer** device with these entities:
- **Mode** — select dropdown (OFF / ON / AUTO)
- **Relay** — binary sensor showing relay state
- **Schedule** — sensor showing active time ranges
- **Temperature** — current water temperature (unknown while the sensor is faulty)
- **Temp Limit** — number slider (20–80 °C, 0.5 °C steps); the relay switches off at this temperature
- **Temp Sensor Fault** — diagnostic binary sensor, on while the DS18B20 is not responding

### 6. Optional: Dashboard card

```yaml
type: entities
title: Relay Timer
entities:
  - entity: select.relay_timer_mode
  - entity: binary_sensor.relay_timer_relay
  - entity: sensor.relay_timer_schedule
  - entity: sensor.relay_timer_temperature
  - entity: number.relay_timer_temp_limit
  - entity: binary_sensor.relay_timer_temp_sensor_fault
```

### 7. Updating the schedule

Send a JSON payload to the schedule topic via **Developer Tools → Actions**:

```yaml
service: mqtt.publish
data:
  topic: "esp32timer/relay_timer_01/schedule/set"
  payload: '{"ranges":[{"start":"07:00","end":"10:00"},{"start":"14:00","end":"18:00"},{"start":"20:00","end":"23:00"}]}'
```

The schedule persists on the device — you only need to send it once (or whenever you want to change it). Overnight ranges like `{"start":"22:00","end":"06:00"}` are supported.

The same payload may carry an optional `"temp_limit": 50` field to set the limit at the same time. Normally you'd just use the **Temp Limit** slider instead.

### 8. How the thermostat behaves

- Heating is allowed by the mode: always in **ON**, only inside a schedule window in **AUTO**, never in **OFF**.
- While heating is allowed, the relay is on until the temperature reaches the limit, then off until it drops 2 °C below the limit (`TEMP_HYSTERESIS`).
- Temperature-driven on/off changes wait at least `RELAY_MIN_ON_MS` / `RELAY_MIN_OFF_MS` (60 s each by default). Mode changes and schedule boundaries switch immediately.
- The sensor is read every 5 s without blocking the main loop. A single bad read is ignored; after 3 in a row (or 30 s without a good read) the sensor is declared faulty and the relay stays on inside the heating window.

## MQTT Topics

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `esp32timer/relay_timer_01/mode/state` | Device → HA | Current mode |
| `esp32timer/relay_timer_01/mode/set` | HA → Device | Set mode |
| `esp32timer/relay_timer_01/relay/state` | Device → HA | Relay ON/OFF |
| `esp32timer/relay_timer_01/schedule/state` | Device → HA | Schedule display |
| `esp32timer/relay_timer_01/schedule/set` | HA → Device | Update schedule |
| `esp32timer/relay_timer_01/availability` | Device → HA | Online/offline |
| `esp32timer/relay_timer_01/temperature/state` | Device → HA | Current temperature (°C), or `None` when the sensor is faulty |
| `esp32timer/relay_timer_01/temp_limit/state` | Device → HA | Current temperature limit |
| `esp32timer/relay_timer_01/temp_limit/set` | HA → Device | Set temperature limit (20–80) |
| `esp32timer/relay_timer_01/temp_fault/state` | Device → HA | `ON` while the sensor is not responding |
