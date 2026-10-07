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
- **Self-healing**: re-associates Wi-Fi after 5 min without the broker, reboots after 30 min, and a 2-minute loop watchdog resets a stuck board. The clock keeps syncing in the background, so a reboot during an internet outage no longer leaves AUTO dead.
- **Diagnostics in HA**: reset reason, boot count, uptime, free heap, Wi-Fi RSSI, clock-synced flag and a summary of the last crash, so an outage can be diagnosed without a ladder
- **OTA updates**: flash new firmware over Wi-Fi from the Arduino IDE/CLI or with one click from Home Assistant, with automatic rollback if the new build can't reach MQTT
- **MQTT Discovery**: Device auto-registers in Home Assistant — no manual YAML needed
- **Persistent state**: Mode, schedules and temperature limit survive reboots (stored in flash)
- **Standalone operation**: Keeps running in last mode even if Home Assistant is down

## Hardware

- ESP32-C3 Super Mini
- Arduino relay shield (single relay)
- DS18B20 temperature sensor (waterproof probe) on GPIO2, with a **4.7 kΩ pull-up** between the data line and 3.3 V.
  The pull-up is mandatory: GPIO2 is a strapping pin on the ESP32-C3 and must read high at boot, and the 1-Wire bus needs it anyway.

### Power and relay — read this before mounting it out of reach

- **Relay rating vs. load.** The usual Arduino relay shield has 10 A contacts. A 220 V boiler at 2–5 kW draws 9–23 A (double that at 127 V).
  Breaking such a current arcs the contacts every time; eventually they weld (boiler stuck ON, only its own thermostat left) or burn.
  If the heater draws more than ~7 A, let the relay drive a proper **contactor** and let the contactor switch the heater.
- **Power the board from a different circuit than the boiler.** When the relay cuts the heater, the transient on that circuit can sag a
  cheap USB adapter enough to brown-out reset the ESP32, and a tripped breaker takes the board down with the heater. A dedicated
  5 V/1 A adapter on another outlet, a short USB cable, and a 470–1000 µF capacitor across 5 V near the board go a long way.
- An RC snubber or MOV across the relay contacts and keeping the sensor cable away from the mains cable reduce the transients further.

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
- OTA hostname and password (`OTA_PASSWORD` — set your own before the first flash)
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
- **Firmware** — update entity showing the installed version, with an **Install** button (see OTA below)
- **OTA Status** — diagnostic text with the result of the last update
- Diagnostics (device page → Diagnostic section): **Reset Reason**, **Boot Count**, **Uptime**, **Free Heap**, **Wi-Fi RSSI**,
  **Clock Synced**, **Last Crash**

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

### 7. Updating the schedule (and optionally the temperature limit)

Send a JSON payload to the schedule topic via **Developer Tools → Actions**:

```yaml
service: mqtt.publish
data:
  topic: "esp32timer/relay_timer_01/schedule/set"
  payload: '{"ranges":[{"start":"07:00","end":"10:00"},{"start":"14:00","end":"18:00"},{"start":"20:00","end":"23:00"}]}'
```

Payload format:

| Field | Required | Meaning |
|-------|----------|---------|
| `ranges` | yes | List of `{"start":"HH:MM","end":"HH:MM"}` objects, up to 8. `end` is exclusive. Overnight ranges like `{"start":"22:00","end":"06:00"}` are supported. |
| `temp_limit` | no | Temperature limit in °C (20–80). Applies to **all** ranges — there is one global limit, not one per range. |

To change the schedule and the limit in one go:

```yaml
service: mqtt.publish
data:
  topic: "esp32timer/relay_timer_01/schedule/set"
  payload: '{"ranges":[{"start":"07:00","end":"10:00"},{"start":"17:00","end":"23:00"}],"temp_limit":45}'
```

Notes:

- Both settings persist on the device, so you only need to send this once (or whenever you want to change it).
- Leaving `temp_limit` out keeps the current limit. The same value is also exposed as the **Temp Limit** slider in Home Assistant, which is the easier way to change just the limit.
- A `temp_limit` outside 20–80 is rejected and logged on serial; the schedule part of the payload is still applied.
- The **Schedule** sensor shows the active ranges; the **Temp Limit** entity shows the limit in effect.

### 8. How the thermostat behaves

- Heating is allowed by the mode: always in **ON**, only inside a schedule window in **AUTO**, never in **OFF**.
- While heating is allowed, the relay is on until the temperature reaches the limit, then off until it drops 2 °C below the limit (`TEMP_HYSTERESIS`).
- Temperature-driven on/off changes wait at least `RELAY_MIN_ON_MS` / `RELAY_MIN_OFF_MS` (60 s each by default). Mode changes and schedule boundaries switch immediately.
- The sensor is read every 5 s without blocking the main loop. A single bad read is ignored; after 3 in a row (or 30 s without a good read) the sensor is declared faulty and the relay stays on inside the heating window.

## OTA updates (no more climbing into the attic)

The first flash has to happen over USB. After that, every update can go over Wi-Fi.
The default **Default 4MB with spiffs** partition scheme already has two firmware slots, so nothing to change there.

Before any update the relay is switched **off** and stays off until the new firmware boots.
Bump `FW_VERSION` at the top of `esp32_timer.ino` for every build you intend to ship — HA uses it to tell versions apart.

### Option A — straight from the Arduino IDE or CLI

The device announces itself on the network as `relay-timer-01` (`OTA_HOSTNAME`).

- **Arduino IDE 2.x**: Tools → Port → pick the network port `relay-timer-01 at 192.168.x.x` (it appears under "Network ports" once the board is online), then Upload. Enter `OTA_PASSWORD` when asked.
- **arduino-cli**:
  ```bash
  arduino-cli compile --fqbn esp32:esp32:esp32c3 esp32_timer
  arduino-cli upload  --fqbn esp32:esp32:esp32c3 --protocol network -p 192.168.1.XX \
                      --upload-field password=YOUR_OTA_PASSWORD esp32_timer
  ```
  Use the IP shown in the serial log / your router (the hostname is `relay-timer-01`).

### Option B — from Home Assistant (host the .bin on HA, click Install)

1. Build the binary: **Sketch → Export Compiled Binary**. It lands in `esp32_timer/build/esp32.esp32.esp32c3/esp32_timer.ino.bin`
   (with arduino-cli: `arduino-cli compile --fqbn esp32:esp32:esp32c3 -e esp32_timer`).
2. Copy it into HA's `www` folder, e.g. `/config/www/esp32_timer/esp32_timer-1.2.0.bin`. HA serves that folder at `http://<HA_IP>:8123/local/…` with no login.
3. Tell the device a new build exists — publish this **retained** message (Developer Tools → Actions):
   ```yaml
   service: mqtt.publish
   data:
     topic: "esp32timer/relay_timer_01/ota/latest"
     retain: true
     payload: '{"version":"1.2.0","url":"http://192.168.1.70:8123/local/esp32_timer/esp32_timer-1.2.0.bin"}'
   ```
   The **Firmware** entity now shows an update is available (installed 1.1.0 → latest 1.2.0).
4. Click **Install** on the Firmware entity (Settings → Devices → Relay Timer, or the Settings page's update list).
   Progress shows on the entity; **OTA Status** tells you what happened. The board reboots on its own.

You can also skip the entity and push a URL directly — it installs regardless of version, handy for re-flashing:
```yaml
service: mqtt.publish
data:
  topic: "esp32timer/relay_timer_01/ota/set"
  payload: "http://192.168.1.70:8123/local/esp32_timer/esp32_timer-1.2.0.bin"
```
Never publish to `ota/set` with `retain: true`, or the device will try to reinstall on every reconnect.
Only plain `http://` URLs are supported (HA on your LAN is fine). `install` is ignored when the published version equals the running one.

### What if the new firmware is broken?

The bootloader boots a freshly flashed image in "pending verification" mode. The firmware only confirms itself once it has
connected to MQTT again. If that doesn't happen within 10 minutes (`OTA_VERIFY_TIMEOUT_MS`) — because it crashes, boot-loops,
or can't join Wi-Fi — the board reboots and the bootloader falls back to the previous firmware. **OTA Status** will then read
"running vX (a previous update was rolled back)" until the next successful update overwrites that slot.

What rollback cannot save you from: a build that connects to MQTT fine but has a logic bug. Test on the bench first when you
change anything around the relay.

## When the device shows as "unavailable"

"Unavailable" means the broker delivered the board's last-will: the TCP session dropped and the board has not re-announced itself.
In order:

1. **Is the boiler still heating with the board dark?** Welded relay contacts. Cut the boiler breaker.
2. **Check the breaker / RCD** of the board's supply (and of the boiler, if shared). Resetting it also power-cycles the board.
3. **Power-cycle the board** if you can reach its supply. It should be back in HA within a minute. The firmware itself reboots
   after 30 minutes without the broker, so a transient network problem clears on its own.
4. **Read the diagnostics once it is back**: *Reset Reason* tells you why it last booted (`POWERON` = lost power, `BROWNOUT` =
   supply sagged, `PANIC`/`TASK_WDT` = firmware crash, `SW` = self-reboot or OTA). *Last Crash* carries the crashing task, the
   program counter and its return address, e.g. `boot #41: task=loopTask pc=0x4200a1b2 ra=0x42001234 mcause=5 mtval=0x00000000`
   (`mcause` 5/7 = bad load/store address, 2 = illegal instruction; `mtval` is the offending address).
   To map `pc` and `ra` to source lines, export the build (**Sketch → Export Compiled Binary** also writes the `.elf`) and run:
   ```bash
   ~/.arduino15/packages/esp32/tools/riscv32-esp-elf-gcc/*/bin/riscv32-esp-elf-addr2line \
       -pfiaC -e esp32_timer/build/esp32.esp32.esp32c3/esp32_timer.ino.elf 0x4200a1b2 0x42001234
   ```
   The `.elf` must be from the exact build that crashed, so keep the exported build of whatever is installed.
5. If it never comes back after a power-cycle: bring it down, serial monitor at 115200, read the boot log.

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
| `esp32timer/relay_timer_01/ota/state` | Device → HA | JSON for the Firmware update entity (installed/latest version, progress) |
| `esp32timer/relay_timer_01/ota/latest` | HA → Device (retained) | `{"version":"x.y.z","url":"http://…bin"}` describing the newest build |
| `esp32timer/relay_timer_01/ota/set` | HA → Device | `install` or an `http://` URL to flash (do not retain) |
| `esp32timer/relay_timer_01/ota/status` | Device → HA | Human-readable result of the last update |
| `esp32timer/relay_timer_01/diag/reset_reason` | Device → HA | Why the board last booted (`POWERON`, `BROWNOUT`, `PANIC`, `TASK_WDT`, `SW`, …) |
| `esp32timer/relay_timer_01/diag/boot_count` | Device → HA | Boots since first flash (stored in flash) |
| `esp32timer/relay_timer_01/diag/uptime` | Device → HA | Seconds since boot |
| `esp32timer/relay_timer_01/diag/heap` | Device → HA | Free heap in bytes |
| `esp32timer/relay_timer_01/diag/rssi` | Device → HA | Wi-Fi signal in dBm |
| `esp32timer/relay_timer_01/diag/clock_synced` | Device → HA | `ON` once NTP has set the clock (AUTO needs this) |
| `esp32timer/relay_timer_01/diag/last_crash` | Device → HA | Summary of the last core dump, if any |
