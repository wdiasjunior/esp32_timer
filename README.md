# ESP32 Relay Timer

Custom firmware for an ESP32-C3 Super Mini that controls a relay via Home Assistant over MQTT.

Disclaimer: this was completely vibe coded and I don't really care about whether or not it isn't since I just needed a smart switch with time schedules and to be configurable through Home Assistant. Use at your own risk.

## Features

- **Three modes**: OFF / ON / AUTO — controllable from Home Assistant
- **Scheduled auto mode**: Multiple configurable time ranges (e.g. 07:00-10:00, 14:00-18:00)
- **MQTT Discovery**: Device auto-registers in Home Assistant — no manual YAML needed
- **Persistent state**: Mode and schedules survive reboots (stored in flash)
- **Standalone operation**: Keeps running in last mode even if Home Assistant is down

## TODO

- [ ] Add a temperature sensor.

## Hardware

- ESP32-C3 Super Mini
- Arduino relay shield (single relay)

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

### 2. Configure the firmware

Edit `esp32_timer/config.h` with your:
- Wi-Fi SSID and password
- MQTT broker credentials (see step 3)
- Relay GPIO pin (default: GPIO 10)
- Timezone offset (default: UTC-3)

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
You should see a **Relay Timer** device with three entities:
- **Mode** — select dropdown (OFF / ON / AUTO)
- **Relay** — binary sensor showing relay state
- **Schedule** — sensor showing active time ranges

### 6. Optional: Dashboard card

```yaml
type: entities
title: Relay Timer
entities:
  - entity: select.relay_timer_mode
  - entity: binary_sensor.relay_timer_relay
  - entity: sensor.relay_timer_schedule
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

## MQTT Topics

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `esp32timer/relay_timer_01/mode/state` | Device → HA | Current mode |
| `esp32timer/relay_timer_01/mode/set` | HA → Device | Set mode |
| `esp32timer/relay_timer_01/relay/state` | Device → HA | Relay ON/OFF |
| `esp32timer/relay_timer_01/schedule/state` | Device → HA | Schedule display |
| `esp32timer/relay_timer_01/schedule/set` | HA → Device | Update schedule |
| `esp32timer/relay_timer_01/availability` | Device → HA | Online/offline |
