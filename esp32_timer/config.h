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
#define TOPIC_AVAILABILITY MQTT_PREFIX "/availability"
