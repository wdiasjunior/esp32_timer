#pragma once

// --- Wi-Fi ---
#define WIFI_SSID     "WIFI_SSID"
#define WIFI_PASSWORD "WIFI_PASSWORD"

// --- MQTT Broker (Mosquitto on Home Assistant) ---
#define MQTT_HOST     "MQTT_HOST_IP_ADDRESS"
#define MQTT_PORT     1883
#define MQTT_USER     "mqtt_user"
#define MQTT_PASSWORD "mqtt_pass"

// --- Device identity ---
#define DEVICE_NAME "Relay Timer"
#define DEVICE_ID   "relay_timer_01"

// --- GPIO ---
#define RELAY_PIN          10     // GPIO connected to relay IN — adjust to your wiring
#define RELAY_ACTIVE_HIGH  false  // true = HIGH energizes relay; false = LOW energizes relay
#define LED_PIN            8      // Onboard LED on most ESP32-C3 Super Mini boards
#define TEMP_SENSOR_PIN    2      // GPIO2 — DS18B20 data pin

// --- Temperature ---
#define DEFAULT_TEMP_LIMIT 45.0   // Default target temperature (°C)
#define TEMP_LIMIT_MIN     20.0   // Lowest limit accepted from HA / NVS (°C)
#define TEMP_LIMIT_MAX     80.0   // Highest limit accepted from HA / NVS (°C)
#define TEMP_HYSTERESIS    2.0    // Relay turns back on when temp drops this much below limit
#define TEMP_READ_INTERVAL 5000   // Start a sensor conversion every 5 seconds (ms)
#define TEMP_RESOLUTION    10     // DS18B20 bits: 9=0.5°C/94ms, 10=0.25°C/188ms, 11=0.125°C/375ms, 12=0.0625°C/750ms
#define TEMP_FAIL_COUNT    3      // Consecutive bad reads before the sensor is declared disconnected
#define TEMP_STALE_MS      30000  // No good read for this long also declares the sensor disconnected (ms)
#define TEMP_PUBLISH_DELTA 0.1    // Publish a new reading only when it moved at least this much (°C)

// --- Relay protection ---
// Minimum time the relay stays in a state before a *temperature-driven* change is allowed.
// Mode changes and schedule start/end still switch immediately.
#define RELAY_MIN_ON_MS    60000  // (ms)
#define RELAY_MIN_OFF_MS   60000  // (ms)

// --- NTP ---
#define NTP_SERVER     "pool.ntp.org"
#define GMT_OFFSET_SEC -10800 // UTC-3 (Brazil)
#define DST_OFFSET_SEC 0      // No DST adjustment

// --- Schedule ---
#define MAX_SCHEDULES 8 // Maximum number of time ranges for AUTO mode

// --- MQTT topics ---
#define MQTT_PREFIX        "esp32timer/" DEVICE_ID
#define TOPIC_MODE_STATE   MQTT_PREFIX "/mode/state"
#define TOPIC_MODE_SET     MQTT_PREFIX "/mode/set"
#define TOPIC_RELAY_STATE  MQTT_PREFIX "/relay/state"
#define TOPIC_SCHED_STATE  MQTT_PREFIX "/schedule/state"
#define TOPIC_SCHED_SET    MQTT_PREFIX "/schedule/set"
#define TOPIC_AVAILABILITY     MQTT_PREFIX "/availability"
#define TOPIC_TEMP_STATE       MQTT_PREFIX "/temperature/state"
#define TOPIC_TEMP_LIMIT_STATE MQTT_PREFIX "/temp_limit/state"
#define TOPIC_TEMP_LIMIT_SET   MQTT_PREFIX "/temp_limit/set"
#define TOPIC_TEMP_FAULT_STATE MQTT_PREFIX "/temp_fault/state"
