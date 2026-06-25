#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <time.h>
#include "config.h"

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

enum Mode : uint8_t { MODE_OFF = 0, MODE_ON = 1, MODE_AUTO = 2 };

struct TimeRange {
    uint8_t startHour;
    uint8_t startMin;
    uint8_t endHour;
    uint8_t endMin;
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
Preferences  prefs;
OneWire      oneWire(TEMP_SENSOR_PIN);
DallasTemperature tempSensor(&oneWire);

Mode      currentMode        = MODE_OFF;
bool      relayState         = false;
bool      timeSynced         = false;
bool      autoRevertToAuto   = false; // ON set outside schedule → revert to AUTO when schedule starts
uint8_t   scheduleCount      = 0;
TimeRange schedules[MAX_SCHEDULES];

float currentTemp     = -127.0;
float tempLimit       = DEFAULT_TEMP_LIMIT;
bool  tempSensorValid = false;

unsigned long lastStatePublish = 0;
unsigned long lastWifiAttempt  = 0;
unsigned long lastMqttAttempt  = 0;
unsigned long lastTempRead     = 0;

// ---------------------------------------------------------------------------
// Relay
// ---------------------------------------------------------------------------

void setRelay(bool on) {
    relayState = on;
    if (RELAY_ACTIVE_HIGH)
        digitalWrite(RELAY_PIN, on ? HIGH : LOW);
    else
        digitalWrite(RELAY_PIN, on ? LOW : HIGH);
}

// ---------------------------------------------------------------------------
// NVS persistence
// ---------------------------------------------------------------------------

void saveMode() {
    prefs.begin("timer", false);
    prefs.putUChar("mode", (uint8_t)currentMode);
    prefs.end();
}

void loadMode() {
    prefs.begin("timer", true);
    currentMode = (Mode)prefs.getUChar("mode", MODE_OFF);
    prefs.end();
}

void saveSchedules() {
    prefs.begin("timer", false);
    prefs.putUChar("schedCnt", scheduleCount);
    prefs.putBytes("scheds", schedules, sizeof(TimeRange) * scheduleCount);
    prefs.end();
}

void loadSchedules() {
    prefs.begin("timer", true);
    scheduleCount = prefs.getUChar("schedCnt", 0);
    if (scheduleCount > MAX_SCHEDULES) scheduleCount = MAX_SCHEDULES;
    if (scheduleCount > 0) {
        prefs.getBytes("scheds", schedules, sizeof(TimeRange) * scheduleCount);
    }
    prefs.end();

    // Default schedule if none configured
    if (scheduleCount == 0) {
        schedules[0] = {7, 0, 23, 0};
        scheduleCount = 1;
    }
}

void saveAutoRevert() {
    prefs.begin("timer", false);
    prefs.putBool("autoRev", autoRevertToAuto);
    prefs.end();
}

void loadAutoRevert() {
    prefs.begin("timer", true);
    autoRevertToAuto = prefs.getBool("autoRev", false);
    prefs.end();
}

void saveTempLimit() {
    prefs.begin("timer", false);
    prefs.putFloat("tempLim", tempLimit);
    prefs.end();
}

void loadTempLimit() {
    prefs.begin("timer", true);
    tempLimit = prefs.getFloat("tempLim", DEFAULT_TEMP_LIMIT);
    prefs.end();
}

// ---------------------------------------------------------------------------
// Temperature sensor
// ---------------------------------------------------------------------------

void readTemperature() {
    tempSensor.requestTemperatures();
    float temp = tempSensor.getTempCByIndex(0);

    if (temp == DEVICE_DISCONNECTED_C) {
        if (tempSensorValid) {
            Serial.println("Temperature sensor disconnected!");
            tempSensorValid = false;
        }
        return;
    }

    currentTemp = temp;
    if (!tempSensorValid) {
        tempSensorValid = true;
        Serial.println("Temperature sensor connected");
    }

    if (mqtt.connected()) {
        char buf[8];
        dtostrf(currentTemp, 1, 1, buf);
        mqtt.publish(TOPIC_TEMP_STATE, buf, true);
    }
}

// ---------------------------------------------------------------------------
// Schedule evaluation
// ---------------------------------------------------------------------------

bool isInSchedule() {
    if (!timeSynced) return false;

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return false;

    uint16_t now = timeinfo.tm_hour * 60 + timeinfo.tm_min;

    for (uint8_t i = 0; i < scheduleCount; i++) {
        uint16_t start = schedules[i].startHour * 60 + schedules[i].startMin;
        uint16_t end   = schedules[i].endHour   * 60 + schedules[i].endMin;

        if (start <= end) {
            // Normal range (e.g. 07:00–23:00)
            if (now >= start && now < end) return true;
        } else {
            // Overnight range (e.g. 22:00–06:00)
            if (now >= start || now < end) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Schedule JSON parsing
// ---------------------------------------------------------------------------

bool parseScheduleJson(const char* json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("Schedule JSON error: %s\n", err.c_str());
        return false;
    }

    JsonArray ranges = doc["ranges"];
    if (ranges.isNull() || ranges.size() == 0) {
        Serial.println("Schedule: no ranges found");
        return false;
    }

    uint8_t count = 0;
    TimeRange temp[MAX_SCHEDULES];

    for (JsonObject r : ranges) {
        if (count >= MAX_SCHEDULES) break;

        const char* startStr = r["start"];
        const char* endStr   = r["end"];
        if (!startStr || !endStr) continue;

        int sh, sm, eh, em;
        if (sscanf(startStr, "%d:%d", &sh, &sm) != 2) continue;
        if (sscanf(endStr,   "%d:%d", &eh, &em) != 2) continue;

        if (sh < 0 || sh > 23 || sm < 0 || sm > 59) continue;
        if (eh < 0 || eh > 23 || em < 0 || em > 59) continue;

        temp[count++] = {(uint8_t)sh, (uint8_t)sm, (uint8_t)eh, (uint8_t)em};
    }

    if (count == 0) {
        Serial.println("Schedule: no valid ranges parsed");
        return false;
    }

    scheduleCount = count;
    memcpy(schedules, temp, sizeof(TimeRange) * count);

    // Optional temp_limit in the same payload
    if (doc.containsKey("temp_limit")) {
        float tl = doc["temp_limit"];
        if (tl >= 20.0 && tl <= 80.0) {
            tempLimit = tl;
            saveTempLimit();
            Serial.printf("Temp limit updated to %.1f°C\n", tempLimit);
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Schedule to string (for HA sensor)
// ---------------------------------------------------------------------------

String scheduleToString() {
    String s;
    for (uint8_t i = 0; i < scheduleCount; i++) {
        if (i > 0) s += ", ";
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d-%02d:%02d",
                 schedules[i].startHour, schedules[i].startMin,
                 schedules[i].endHour,   schedules[i].endMin);
        s += buf;
    }
    return s;
}

// ---------------------------------------------------------------------------
// Mode helpers
// ---------------------------------------------------------------------------

const char* modeToString(Mode m) {
    switch (m) {
        case MODE_ON:   return "ON";
        case MODE_AUTO: return "AUTO";
        default:        return "OFF";
    }
}

Mode stringToMode(const char* s) {
    if (strcmp(s, "ON")   == 0) return MODE_ON;
    if (strcmp(s, "AUTO") == 0) return MODE_AUTO;
    return MODE_OFF;
}

// ---------------------------------------------------------------------------
// MQTT publishing
// ---------------------------------------------------------------------------

void publishModeState() {
    mqtt.publish(TOPIC_MODE_STATE, modeToString(currentMode), true);
}

void publishRelayState() {
    mqtt.publish(TOPIC_RELAY_STATE, relayState ? "ON" : "OFF", true);
}

void publishScheduleState() {
    String s = scheduleToString();
    mqtt.publish(TOPIC_SCHED_STATE, s.c_str(), true);
}

void publishTemperature() {
    if (tempSensorValid) {
        char buf[8];
        dtostrf(currentTemp, 1, 1, buf);
        mqtt.publish(TOPIC_TEMP_STATE, buf, true);
    }
}

void publishTempLimit() {
    char buf[8];
    dtostrf(tempLimit, 1, 1, buf);
    mqtt.publish(TOPIC_TEMP_LIMIT_STATE, buf, true);
}

void publishAllState() {
    publishModeState();
    publishRelayState();
    publishScheduleState();
    publishTemperature();
    publishTempLimit();
}

// ---------------------------------------------------------------------------
// HA MQTT Discovery
// ---------------------------------------------------------------------------

void publishDiscovery() {
    // Shared device block
    auto addDevice = [](JsonObject dev) {
        JsonArray ids = dev["ids"].to<JsonArray>();
        ids.add(DEVICE_ID);
        dev["name"]  = DEVICE_NAME;
        dev["mdl"]   = "ESP32-C3 Super Mini";
        dev["mf"]    = "Custom";
    };

    char buf[512];

    // 1) Mode select entity
    {
        JsonDocument doc;
        doc["name"]       = "Mode";
        doc["uniq_id"]    = DEVICE_ID "_mode";
        doc["cmd_t"]      = TOPIC_MODE_SET;
        doc["stat_t"]     = TOPIC_MODE_STATE;
        doc["avty_t"]     = TOPIC_AVAILABILITY;
        JsonArray opts    = doc["options"].to<JsonArray>();
        opts.add("OFF"); opts.add("ON"); opts.add("AUTO");
        addDevice(doc["dev"].to<JsonObject>());
        serializeJson(doc, buf);
        mqtt.publish("homeassistant/select/" DEVICE_ID "/mode/config", buf, true);
    }

    // 2) Relay binary sensor
    {
        JsonDocument doc;
        doc["name"]       = "Relay";
        doc["uniq_id"]    = DEVICE_ID "_relay";
        doc["stat_t"]     = TOPIC_RELAY_STATE;
        doc["avty_t"]     = TOPIC_AVAILABILITY;
        doc["pl_on"]      = "ON";
        doc["pl_off"]     = "OFF";
        doc["dev_cla"]    = "power";
        addDevice(doc["dev"].to<JsonObject>());
        serializeJson(doc, buf);
        mqtt.publish("homeassistant/binary_sensor/" DEVICE_ID "/relay/config", buf, true);
    }

    // 3) Schedule sensor
    {
        JsonDocument doc;
        doc["name"]       = "Schedule";
        doc["uniq_id"]    = DEVICE_ID "_schedule";
        doc["stat_t"]     = TOPIC_SCHED_STATE;
        doc["avty_t"]     = TOPIC_AVAILABILITY;
        doc["ic"]         = "mdi:clock-outline";
        addDevice(doc["dev"].to<JsonObject>());
        serializeJson(doc, buf);
        mqtt.publish("homeassistant/sensor/" DEVICE_ID "/schedule/config", buf, true);
    }

    // 4) Temperature sensor
    {
        JsonDocument doc;
        doc["name"]       = "Temperature";
        doc["uniq_id"]    = DEVICE_ID "_temperature";
        doc["stat_t"]     = TOPIC_TEMP_STATE;
        doc["avty_t"]     = TOPIC_AVAILABILITY;
        doc["dev_cla"]    = "temperature";
        doc["unit_of_meas"] = "\u00b0C";
        addDevice(doc["dev"].to<JsonObject>());
        serializeJson(doc, buf);
        mqtt.publish("homeassistant/sensor/" DEVICE_ID "/temperature/config", buf, true);
    }

    // 5) Temperature limit (number slider)
    {
        JsonDocument doc;
        doc["name"]       = "Temp Limit";
        doc["uniq_id"]    = DEVICE_ID "_temp_limit";
        doc["cmd_t"]      = TOPIC_TEMP_LIMIT_SET;
        doc["stat_t"]     = TOPIC_TEMP_LIMIT_STATE;
        doc["avty_t"]     = TOPIC_AVAILABILITY;
        doc["min"]        = 20;
        doc["max"]        = 80;
        doc["step"]       = 0.5;
        doc["unit_of_meas"] = "\u00b0C";
        doc["ic"]         = "mdi:thermometer-alert";
        addDevice(doc["dev"].to<JsonObject>());
        serializeJson(doc, buf);
        mqtt.publish("homeassistant/number/" DEVICE_ID "/temp_limit/config", buf, true);
    }

    Serial.println("HA discovery published");
}

// ---------------------------------------------------------------------------
// MQTT callback
// ---------------------------------------------------------------------------

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    char msg[256];
    unsigned int len = min(length, (unsigned int)(sizeof(msg) - 1));
    memcpy(msg, payload, len);
    msg[len] = '\0';

    Serial.printf("MQTT recv [%s]: %s\n", topic, msg);

    if (strcmp(topic, TOPIC_MODE_SET) == 0) {
        Mode newMode = stringToMode(msg);
        if (newMode != currentMode) {
            currentMode = newMode;
            saveMode();

            // Track if ON was set outside schedule for auto-revert
            if (newMode == MODE_ON && timeSynced && !isInSchedule()) {
                autoRevertToAuto = true;
            } else {
                autoRevertToAuto = false;
            }
            saveAutoRevert();

            Serial.printf("Mode changed to %s (autoRevert=%d)\n",
                          modeToString(currentMode), autoRevertToAuto);
        }
        publishModeState();
    }
    else if (strcmp(topic, TOPIC_SCHED_SET) == 0) {
        if (parseScheduleJson(msg)) {
            saveSchedules();
            Serial.printf("Schedule updated: %s\n", scheduleToString().c_str());
        }
        publishScheduleState();
        publishTempLimit();
    }
    else if (strcmp(topic, TOPIC_TEMP_LIMIT_SET) == 0) {
        float tl = atof(msg);
        if (tl >= 20.0 && tl <= 80.0) {
            tempLimit = tl;
            saveTempLimit();
            Serial.printf("Temp limit changed to %.1f°C\n", tempLimit);
        }
        publishTempLimit();
    }
}

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------

void connectWifi() {
    if (WiFi.status() == WL_CONNECTED) return;

    Serial.printf("Connecting to Wi-Fi '%s'", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        Serial.print(".");
        digitalWrite(LED_PIN, !digitalRead(LED_PIN)); // blink LED
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nWi-Fi connected — IP: %s\n", WiFi.localIP().toString().c_str());
        digitalWrite(LED_PIN, LOW);
    } else {
        Serial.println("\nWi-Fi connection failed, will retry...");
    }
}

// ---------------------------------------------------------------------------
// NTP
// ---------------------------------------------------------------------------

void syncNtp() {
    configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
    Serial.print("Syncing NTP");

    struct tm timeinfo;
    int attempts = 0;
    while (!getLocalTime(&timeinfo) && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (getLocalTime(&timeinfo)) {
        timeSynced = true;
        Serial.printf("\nTime synced: %02d:%02d:%02d\n",
                      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        Serial.println("\nNTP sync failed, will use schedule when time becomes available");
    }
}

// ---------------------------------------------------------------------------
// MQTT connection
// ---------------------------------------------------------------------------

void connectMqtt() {
    if (mqtt.connected()) return;

    Serial.print("Connecting to MQTT...");
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setBufferSize(1024);
    mqtt.setCallback(mqttCallback);

    if (mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASSWORD,
                     TOPIC_AVAILABILITY, 1, true, "offline")) {
        Serial.println(" connected");
        mqtt.subscribe(TOPIC_MODE_SET);
        mqtt.subscribe(TOPIC_SCHED_SET);
        mqtt.subscribe(TOPIC_TEMP_LIMIT_SET);
        mqtt.publish(TOPIC_AVAILABILITY, "online", true);
        publishDiscovery();
        publishAllState();
    } else {
        Serial.printf(" failed (rc=%d), will retry...\n", mqtt.state());
    }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== Relay Timer starting ===");

    // GPIO
    pinMode(RELAY_PIN, OUTPUT);
    setRelay(false);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH); // LED on during boot

    // Temperature sensor
    tempSensor.begin();
    tempSensor.setResolution(12);

    // Load saved state
    loadMode();
    loadSchedules();
    loadAutoRevert();
    loadTempLimit();
    Serial.printf("Loaded mode: %s, schedules: %s, autoRevert: %d, temp limit: %.1f°C\n",
                  modeToString(currentMode), scheduleToString().c_str(),
                  autoRevertToAuto, tempLimit);

    // Connect
    connectWifi();
    if (WiFi.status() == WL_CONNECTED) {
        syncNtp();
        connectMqtt();
    }

    digitalWrite(LED_PIN, LOW); // LED off when ready
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void loop() {
    // Reconnect Wi-Fi if needed
    if (WiFi.status() != WL_CONNECTED) {
        if (millis() - lastWifiAttempt > 10000) {
            lastWifiAttempt = millis();
            connectWifi();
            if (WiFi.status() == WL_CONNECTED && !timeSynced) {
                syncNtp();
            }
        }
    }

    // Reconnect MQTT if needed
    if (WiFi.status() == WL_CONNECTED && !mqtt.connected()) {
        if (millis() - lastMqttAttempt > 5000) {
            lastMqttAttempt = millis();
            connectMqtt();
        }
    }

    mqtt.loop();

    // Auto-revert ON → AUTO when schedule starts
    if (currentMode == MODE_ON && autoRevertToAuto && timeSynced && isInSchedule()) {
        currentMode = MODE_AUTO;
        autoRevertToAuto = false;
        saveMode();
        saveAutoRevert();
        Serial.println("Auto-reverted from ON to AUTO (schedule started)");
        if (mqtt.connected()) publishModeState();
    }

    if (millis() - lastTempRead > TEMP_READ_INTERVAL) {
        lastTempRead = millis();
        readTemperature();
    }

    // Evaluate desired relay state
    bool desiredRelay = false;
    switch (currentMode) {
        case MODE_OFF:
            desiredRelay = false;
            break;
        case MODE_ON:
            desiredRelay = true;
            break;
        case MODE_AUTO:
            if (!isInSchedule()) {
                desiredRelay = false;
            } else if (!tempSensorValid) {
                // Sensor failed — let the boiler's manual thermostat handle it
                desiredRelay = true;
            } else if (relayState) {
                // Currently heating — keep on until temp reaches limit
                desiredRelay = currentTemp < tempLimit;
            } else {
                // Currently off — turn back on when temp drops below (limit - hysteresis)
                desiredRelay = currentTemp < (tempLimit - TEMP_HYSTERESIS);
            }
            break;
    }

    if (desiredRelay != relayState) {
        setRelay(desiredRelay);
        Serial.printf("Relay %s\n", relayState ? "ON" : "OFF");
        if (mqtt.connected()) publishRelayState();
    }

    // Periodic state re-publish
    if (mqtt.connected() && millis() - lastStatePublish > 60000) {
        lastStatePublish = millis();
        publishAllState();
    }

    delay(500);
}
