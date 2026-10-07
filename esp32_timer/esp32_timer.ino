#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <time.h>
#include <ArduinoOTA.h>
#include <HTTPUpdate.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <esp_core_dump.h>
#include <esp_system.h>
#include <esp_timer.h>
#include "config.h"

// Bump this on every release. Shown in HA (device page + Firmware update entity)
// and compared against the version published on the ota/latest topic.
#define FW_VERSION "1.2.0"

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

DeviceAddress tempAddr;                       // resolved once, re-resolved after a fault
bool          tempAddrKnown         = false;
bool          tempConversionPending = false;  // conversion requested, result not read yet
uint8_t       tempFailCount         = 0;      // consecutive bad reads
bool          tempDistrust85        = false;  // reject one 85.0 (power-on value) after discovery
float         lastPublishedTemp     = NAN;

bool prevHeatGate = false;    // last evaluated "heating permitted by mode/schedule"
Mode prevEvalMode = MODE_OFF; // mode seen at last relay evaluation

unsigned long lastStatePublish     = 0;
unsigned long lastWifiAttempt      = 0;
unsigned long lastMqttAttempt      = 0;
unsigned long lastTempRead         = 0;
unsigned long tempConversionStart  = 0;
unsigned long lastGoodTempRead     = 0;
unsigned long lastRelayChange      = 0;

// OTA
char          otaLatestVersion[32] = "";     // from retained ota/latest
char          otaLatestUrl[200]    = "";
char          otaPendingUrl[200]   = "";     // URL to install on the next loop pass
char          otaStatus[128]       = "";     // last human-readable OTA status
bool          otaRequested         = false;
bool          otaPendingVerify     = false;  // running a freshly flashed image, rollback still armed
bool          otaArduinoBegun      = false;
int           otaLastPct           = -1;
unsigned long otaBootMillis        = 0;

// Diagnostics / resilience
uint32_t      bootCount           = 0;
char          resetReasonStr[24]  = "UNKNOWN";
char          lastCrash[192]      = "";     // summary of a core dump found at boot (kept until next crash)
bool          ntpStarted          = false;
unsigned long lastMqttOkMs        = 0;      // last time mqtt.connected() was true
bool          wifiResetDone       = false;  // one Wi-Fi re-association per MQTT outage

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

// Single validation path for every source of a new limit (HA number, schedule JSON, NVS).
bool applyTempLimit(float tl, bool persist) {
    if (isnan(tl) || tl < TEMP_LIMIT_MIN || tl > TEMP_LIMIT_MAX) return false;
    tempLimit = tl;
    if (persist) saveTempLimit();
    return true;
}

void loadTempLimit() {
    prefs.begin("timer", true);
    float stored = prefs.getFloat("tempLim", DEFAULT_TEMP_LIMIT);
    prefs.end();
    if (!applyTempLimit(stored, false)) {
        Serial.printf("Stored temp limit %.1f out of range, using default\n", stored);
        tempLimit = DEFAULT_TEMP_LIMIT;
    }
}

// ---------------------------------------------------------------------------
// Temperature sensor
// ---------------------------------------------------------------------------

void publishTemperature();
void publishTempFault();

// Conversion time for the configured resolution, plus a little margin.
unsigned long tempConversionMs() {
    switch (TEMP_RESOLUTION) {
        case 9:  return 94  + 20;
        case 10: return 188 + 20;
        case 11: return 375 + 20;
        default: return 750 + 20;
    }
}

void tempReadFailed() {
    if (tempFailCount < 255) tempFailCount++;
    if (!tempSensorValid) return;

    bool tooMany = tempFailCount >= TEMP_FAIL_COUNT;
    bool stale   = millis() - lastGoodTempRead > TEMP_STALE_MS;
    if (tooMany || stale) {
        tempSensorValid = false;
        tempAddrKnown   = false; // re-search the bus on recovery (sensor may have been swapped)
        Serial.printf("Temperature sensor disconnected! (%u consecutive failures)\n", tempFailCount);
        if (mqtt.connected()) {
            publishTemperature(); // publishes "None" -> HA shows unknown
            publishTempFault();
        }
    }
}

void tempReadOk(float temp) {
    tempFailCount    = 0;
    lastGoodTempRead = millis();
    currentTemp      = temp;

    bool justRecovered = !tempSensorValid;
    if (justRecovered) {
        tempSensorValid = true;
        Serial.println("Temperature sensor connected");
    }

    if (mqtt.connected()) {
        if (justRecovered) publishTempFault();
        if (isnan(lastPublishedTemp) || fabsf(temp - lastPublishedTemp) >= TEMP_PUBLISH_DELTA) {
            publishTemperature();
        }
    }
}

// Phase 1: find the sensor if needed and kick off a conversion (non-blocking).
void startTempConversion() {
    lastTempRead = millis();

    if (!tempAddrKnown) {
        if (!tempSensor.getAddress(tempAddr, 0)) {
            tempReadFailed();
            return;
        }
        tempAddrKnown  = true;
        tempDistrust85 = true;
        tempSensor.setResolution(tempAddr, TEMP_RESOLUTION);
        Serial.print("DS18B20 found at ");
        for (uint8_t i = 0; i < 8; i++) Serial.printf("%02X", tempAddr[i]);
        Serial.println();
    }

    tempSensor.requestTemperaturesByAddress(tempAddr);
    tempConversionPending = true;
    tempConversionStart   = millis();
}

// Phase 2: once the conversion time has elapsed, read and validate the result.
void finishTempConversion() {
    tempConversionPending = false;
    float temp = tempSensor.getTempC(tempAddr);

    // -127 = no response / CRC error. Out-of-datasheet values are bus noise.
    // Exactly 85.0 is the DS18B20 power-on value: reject it once right after (re)discovery,
    // then accept it, so a genuinely 85 °C tank is still measured (and stays above the limit).
    bool powerOnValue = (temp == 85.0f) && tempDistrust85;
    tempDistrust85 = false;
    bool bad = (temp == DEVICE_DISCONNECTED_C) || temp < -55.0f || temp > 125.0f || powerOnValue;

    if (bad) tempReadFailed();
    else     tempReadOk(temp);
}

// Called every loop pass. Never blocks the loop for more than a bus transaction.
void serviceTemperature() {
    unsigned long now = millis();
    if (tempConversionPending) {
        if (now - tempConversionStart >= tempConversionMs()) finishTempConversion();
    } else if (now - lastTempRead >= TEMP_READ_INTERVAL) {
        startTempConversion();
    }
}

// ---------------------------------------------------------------------------
// Relay control (thermostat + dwell protection)
// ---------------------------------------------------------------------------

bool isInSchedule();
bool clockValid();
const char* modeToString(Mode m);

// Thermostat with hysteresis. Sensor fault => heat, and let the boiler's own
// mechanical thermostat be the cap (debounced in tempReadFailed()).
bool thermostatWantsHeat() {
    if (!tempSensorValid) return true;
    if (relayState) return currentTemp < tempLimit;                     // keep heating until limit
    return currentTemp < (tempLimit - TEMP_HYSTERESIS);                 // resume below limit - hysteresis
}

// OFF  -> relay off
// ON   -> schedule override: heat regardless of time, still capped by the temp limit
// AUTO -> heat only inside a schedule window, capped by the temp limit
void evaluateRelay() {
    bool heatGate = (currentMode == MODE_ON) ||
                    (currentMode == MODE_AUTO && isInSchedule());
    bool desired  = heatGate && thermostatWantsHeat();

    // A change caused by the mode or the schedule window applies immediately.
    // A change caused only by temperature (or sensor validity) respects the dwell times,
    // so a noisy sensor cannot short-cycle the boiler.
    bool immediate = (currentMode != prevEvalMode) || (heatGate != prevHeatGate);
    prevEvalMode = currentMode;
    prevHeatGate = heatGate;

    if (desired == relayState) return;

    if (!immediate) {
        unsigned long minDwell = relayState ? RELAY_MIN_ON_MS : RELAY_MIN_OFF_MS;
        if (millis() - lastRelayChange < minDwell) return;
    }

    // Publish the new state *before* driving the pin and give the frame a moment to leave.
    // Switching the boiler load can produce a transient that resets the board; this way HA
    // still ends up with the correct retained relay state.
    if (mqtt.connected()) {
        mqtt.publish(TOPIC_RELAY_STATE, desired ? "ON" : "OFF", true);
        delay(20);
    }
    setRelay(desired);
    lastRelayChange = millis();
    Serial.printf("Relay %s (%s, temp %.1f, limit %.1f)\n",
                  relayState ? "ON" : "OFF", modeToString(currentMode),
                  tempSensorValid ? currentTemp : NAN, tempLimit);
}

// ---------------------------------------------------------------------------
// Schedule evaluation
// ---------------------------------------------------------------------------

bool isInSchedule() {
    if (!clockValid()) return false;

    time_t t = time(nullptr);
    struct tm timeinfo;
    localtime_r(&t, &timeinfo);

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
    if (doc["temp_limit"].is<float>()) {
        float tl = doc["temp_limit"].as<float>();
        if (applyTempLimit(tl, true)) {
            Serial.printf("Temp limit updated to %.1f°C\n", tempLimit);
        } else {
            Serial.printf("Temp limit %.1f rejected (range %.0f-%.0f)\n", tl, TEMP_LIMIT_MIN, TEMP_LIMIT_MAX);
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
        lastPublishedTemp = currentTemp;
    } else {
        // HA's MQTT sensor maps the literal string "None" to state "unknown"
        mqtt.publish(TOPIC_TEMP_STATE, "None", true);
        lastPublishedTemp = NAN;
    }
}

void publishTempFault() {
    mqtt.publish(TOPIC_TEMP_FAULT_STATE, tempSensorValid ? "OFF" : "ON", true);
}

void publishTempLimit() {
    char buf[8];
    dtostrf(tempLimit, 1, 1, buf);
    mqtt.publish(TOPIC_TEMP_LIMIT_STATE, buf, true);
}

// JSON consumed by the HA "update" entity.
void publishOtaState(bool inProgress, int pct) {
    if (!mqtt.connected()) return;
    JsonDocument doc;
    doc["installed_version"] = FW_VERSION;
    doc["latest_version"]    = otaLatestVersion[0] ? otaLatestVersion : FW_VERSION;
    doc["title"]             = DEVICE_NAME " firmware";
    doc["in_progress"]       = inProgress;
    if (inProgress) doc["update_percentage"] = pct;
    else            doc["update_percentage"] = nullptr;
    char buf[256];
    serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(TOPIC_OTA_STATE, buf, true);
}

void publishOtaStatus() {
    if (mqtt.connected()) mqtt.publish(TOPIC_OTA_STATUS, otaStatus, true);
}

void publishClockSynced() {
    mqtt.publish(TOPIC_DIAG_CLOCK, timeSynced ? "ON" : "OFF", true);
}

void publishDiagnostics() {
    char buf[24];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)(esp_timer_get_time() / 1000000ULL));
    mqtt.publish(TOPIC_DIAG_UPTIME, buf, true);
    snprintf(buf, sizeof(buf), "%u", (unsigned)ESP.getFreeHeap());
    mqtt.publish(TOPIC_DIAG_HEAP, buf, true);
    snprintf(buf, sizeof(buf), "%d", (int)WiFi.RSSI());
    mqtt.publish(TOPIC_DIAG_RSSI, buf, true);
    snprintf(buf, sizeof(buf), "%u", (unsigned)bootCount);
    mqtt.publish(TOPIC_DIAG_BOOT_COUNT, buf, true);
    mqtt.publish(TOPIC_DIAG_RESET_REASON, resetReasonStr, true);
    if (lastCrash[0]) mqtt.publish(TOPIC_DIAG_LAST_CRASH, lastCrash, true);
    publishClockSynced();
}

void setOtaStatus(const char* msg) {
    strlcpy(otaStatus, msg, sizeof(otaStatus));
    Serial.printf("OTA: %s\n", otaStatus);
    publishOtaStatus();
}

void publishAllState() {
    publishModeState();
    publishRelayState();
    publishScheduleState();
    publishTemperature();
    publishTempFault();
    publishTempLimit();
    publishOtaState(false, 0);
    publishOtaStatus();
    publishDiagnostics();
}

// ---------------------------------------------------------------------------
// Clock (NTP)
//
// configTime() only *starts* SNTP; lwIP keeps retrying on its own (DNS failures
// included), so we never block waiting for it. We just poll whether the system
// clock holds a plausible date. This matters after a power blip: the ESP boots in
// seconds, the router's internet comes back a minute later.
// ---------------------------------------------------------------------------

void startNtp() {
    configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
    ntpStarted = true;
    Serial.println("NTP client started");
}

bool clockValid() {
    time_t now = time(nullptr);
    if (now < 1700000000) return false;         // before 2023-11: clock was never set
    if (!timeSynced) {
        timeSynced = true;
        struct tm t;
        localtime_r(&now, &t);
        Serial.printf("Clock synced: %02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
        if (mqtt.connected()) publishClockSynced();
    }
    return true;
}

// ---------------------------------------------------------------------------
// Diagnostics: why did we boot, and are we still healthy?
// ---------------------------------------------------------------------------

const char* resetReasonToString(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:    return "POWERON";
        case ESP_RST_EXT:        return "EXT";
        case ESP_RST_SW:         return "SW";
        case ESP_RST_PANIC:      return "PANIC";
        case ESP_RST_INT_WDT:    return "INT_WDT";
        case ESP_RST_TASK_WDT:   return "TASK_WDT";
        case ESP_RST_WDT:        return "WDT";
        case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:   return "BROWNOUT";
        case ESP_RST_SDIO:       return "SDIO";
        case ESP_RST_USB:        return "USB";
        case ESP_RST_JTAG:       return "JTAG";
        case ESP_RST_EFUSE:      return "EFUSE";
        case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
        case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
        default:                 return "UNKNOWN";
    }
}

void diagBoot() {
    strlcpy(resetReasonStr, resetReasonToString(esp_reset_reason()), sizeof(resetReasonStr));

    prefs.begin("timer", false);
    bootCount = prefs.getUInt("boots", 0) + 1;
    prefs.putUInt("boots", bootCount);
    prefs.end();

    // A panic or watchdog trip leaves a core dump in the coredump partition. Summarise it,
    // publish it, and erase it so the next crash is not masked by this one.
    if (esp_core_dump_image_check() == ESP_OK) {
        esp_core_dump_summary_t* sum = (esp_core_dump_summary_t*)calloc(1, sizeof(esp_core_dump_summary_t));
        if (sum && esp_core_dump_get_summary(sum) == ESP_OK) {
            // RISC-V cannot unwind on-device; pc + ra give the faulting instruction and its
            // caller, which addr2line against the matching .elf resolves to source lines.
            snprintf(lastCrash, sizeof(lastCrash),
                     "boot #%u: task=%s pc=0x%08x ra=0x%08x mcause=%u mtval=0x%08x",
                     (unsigned)(bootCount - 1), sum->exc_task, (unsigned)sum->exc_pc,
                     (unsigned)sum->ex_info.ra, (unsigned)sum->ex_info.mcause,
                     (unsigned)sum->ex_info.mtval);
        } else {
            snprintf(lastCrash, sizeof(lastCrash), "boot #%u: core dump present but unreadable",
                     (unsigned)(bootCount - 1));
        }
        free(sum);
        esp_core_dump_image_erase();
    }

    Serial.printf("Boot #%u, reset reason %s\n", (unsigned)bootCount, resetReasonStr);
    if (lastCrash[0]) Serial.printf("Previous crash: %s\n", lastCrash);
}

// Main-loop watchdog. The core's task watchdog only watches the IDLE task, so a loop that
// is logically stuck but still calls delay() would never be reset. Subscribe the loop task:
// a stall of LOOP_WDT_TIMEOUT_MS panics, writes a core dump, and reboots.
void setupLoopWatchdog() {
    esp_task_wdt_config_t cfg = {
        .timeout_ms     = LOOP_WDT_TIMEOUT_MS,
        .idle_core_mask = 1,
        .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&cfg);
    esp_task_wdt_add(NULL);
}

// Self-healing when HA/the broker cannot be reached for a long time.
void serviceConnectivity() {
    unsigned long now = millis();
    if (mqtt.connected()) {
        lastMqttOkMs  = now;
        wifiResetDone = false;
        return;
    }
    unsigned long offline = now - lastMqttOkMs;

    // Wi-Fi says "connected" but MQTT has been unreachable for a while: the radio may be in
    // the known associated-but-deaf state. Re-associate once per outage; the normal
    // reconnect path in loop() brings it back.
    if (WiFi.status() == WL_CONNECTED && !wifiResetDone && offline > NET_WIFI_RESET_MS) {
        wifiResetDone = true;
        Serial.println("No MQTT for a while with Wi-Fi up: re-associating Wi-Fi");
        WiFi.disconnect(true);
    }

    // Still nothing after a long time: reboot. The relay comes up OFF and AUTO/ON re-evaluate
    // within seconds, so the cost is a short heating pause.
    if (offline > NET_REBOOT_MS && !otaRequested) {
        Serial.println("No MQTT for too long: rebooting");
        delay(100);
        ESP.restart();
    }
}

// ---------------------------------------------------------------------------
// OTA updates
//
// Three layers:
//  1. ArduinoOTA  - upload straight from the Arduino IDE / arduino-cli over Wi-Fi.
//  2. HTTP pull   - HA (or anyone on MQTT) publishes "install" or an http:// URL to
//                   ota/set; the device downloads the .bin and flashes it.
//  3. Rollback    - the bootloader boots a new image in "pending verify" state. We
//                   only mark it valid once MQTT is up again. If that doesn't happen
//                   within OTA_VERIFY_TIMEOUT_MS we reboot and the bootloader falls
//                   back to the previous image, so a bad build can't strand the board.
// The relay is forced OFF for the duration of any update (the loop is blocked).
// ---------------------------------------------------------------------------

void otaBootCheck() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        otaPendingVerify = true;
        otaBootMillis    = millis();
        snprintf(otaStatus, sizeof(otaStatus), "booted v%s, waiting for MQTT to confirm", FW_VERSION);
    } else if (esp_ota_get_last_invalid_partition() != NULL) {
        snprintf(otaStatus, sizeof(otaStatus), "running v%s (a previous update was rolled back)", FW_VERSION);
    } else {
        snprintf(otaStatus, sizeof(otaStatus), "running v%s", FW_VERSION);
    }
    Serial.printf("OTA: %s, partition %s\n", otaStatus, running ? running->label : "?");
}

// Called once MQTT is connected: the new image has proven it can talk to HA.
void otaConfirmIfPending() {
    if (!otaPendingVerify) return;
    esp_ota_mark_app_valid_cancel_rollback();
    otaPendingVerify = false;
    setOtaStatus("v" FW_VERSION " installed and verified");
}

void otaReportProgress(size_t cur, size_t total) {
    esp_task_wdt_reset();
    if (total == 0) return;
    int pct = (int)((uint64_t)cur * 100 / total);
    if (pct / 10 != otaLastPct / 10) {
        otaLastPct = pct;
        Serial.printf("OTA: %d%%\n", pct);
        publishOtaState(true, pct);
    }
}

void otaBegin(const char* what) {
    if (mqtt.connected()) {
        mqtt.publish(TOPIC_RELAY_STATE, "OFF", true);
        delay(20);
    }
    setRelay(false); // safe state: the loop is blocked until the update ends
    otaLastPct = -1;
    setOtaStatus(what);
    publishOtaState(true, 0);
}

void performHttpOta(const char* url) {
    if (strncmp(url, "http://", 7) != 0) {
        setOtaStatus("only http:// URLs are supported");
        return;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "downloading %s", url);
    otaBegin(buf);

    WiFiClient client;
    httpUpdate.rebootOnUpdate(false);
    httpUpdate.setLedPin(LED_PIN, LOW);
    httpUpdate.onProgress([](int cur, int total) { otaReportProgress(cur, total); });

    t_httpUpdate_return ret = httpUpdate.update(client, url, FW_VERSION);

    switch (ret) {
        case HTTP_UPDATE_OK:
            setOtaStatus("flashed OK, rebooting into new firmware");
            publishOtaState(false, 0);
            if (mqtt.connected()) {
                mqtt.publish(TOPIC_AVAILABILITY, "offline", true);
                mqtt.disconnect();
            }
            delay(500);
            ESP.restart();
            break;
        case HTTP_UPDATE_NO_UPDATES:
            setOtaStatus("server said no update available (HTTP 304)");
            publishOtaState(false, 0);
            break;
        default:
            snprintf(buf, sizeof(buf), "failed: %s (%d)",
                     httpUpdate.getLastErrorString().c_str(), httpUpdate.getLastError());
            setOtaStatus(buf);
            publishOtaState(false, 0);
            break;
    }
}

void setupArduinoOta() {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]() { otaBegin("IDE upload started"); });
    ArduinoOTA.onProgress([](unsigned int cur, unsigned int total) { otaReportProgress(cur, total); });
    ArduinoOTA.onEnd([]() {
        setOtaStatus("IDE upload done, rebooting into new firmware");
        publishOtaState(false, 0);
        if (mqtt.connected()) mqtt.publish(TOPIC_AVAILABILITY, "offline", true);
    });
    ArduinoOTA.onError([](ota_error_t e) {
        const char* what =
            e == OTA_AUTH_ERROR    ? "auth failed"    :
            e == OTA_BEGIN_ERROR   ? "begin failed"   :
            e == OTA_CONNECT_ERROR ? "connect failed" :
            e == OTA_RECEIVE_ERROR ? "receive failed" : "end failed";
        char buf[64];
        snprintf(buf, sizeof(buf), "IDE upload failed: %s", what);
        setOtaStatus(buf);
        publishOtaState(false, 0);
    });

    ArduinoOTA.begin();
    otaArduinoBegun = true;
    Serial.printf("ArduinoOTA ready on %s.local (%s)\n", OTA_HOSTNAME, WiFi.localIP().toString().c_str());
}

// Runs every loop pass.
void serviceOta() {
    if (WiFi.status() == WL_CONNECTED) {
        if (!otaArduinoBegun) setupArduinoOta();
        ArduinoOTA.handle();
    }

    if (otaRequested) {
        otaRequested = false;
        performHttpOta(otaPendingUrl);
    }

    if (otaPendingVerify && millis() - otaBootMillis > OTA_VERIFY_TIMEOUT_MS) {
        Serial.println("OTA: new firmware never reached MQTT, rebooting so the bootloader rolls back");
        delay(100);
        ESP.restart();
    }
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
        dev["sw"]    = FW_VERSION;
    };

    char buf[640];

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
        doc["stat_cla"]   = "measurement";
        doc["unit_of_meas"] = "\u00b0C";
        doc["sug_dsp_prc"] = 1;
        addDevice(doc["dev"].to<JsonObject>());
        serializeJson(doc, buf);
        mqtt.publish("homeassistant/sensor/" DEVICE_ID "/temperature/config", buf, true);
    }

    // 4b) Temperature sensor fault (diagnostic)
    {
        JsonDocument doc;
        doc["name"]       = "Temp Sensor Fault";
        doc["uniq_id"]    = DEVICE_ID "_temp_fault";
        doc["stat_t"]     = TOPIC_TEMP_FAULT_STATE;
        doc["avty_t"]     = TOPIC_AVAILABILITY;
        doc["pl_on"]      = "ON";
        doc["pl_off"]     = "OFF";
        doc["dev_cla"]    = "problem";
        doc["ent_cat"]    = "diagnostic";
        addDevice(doc["dev"].to<JsonObject>());
        serializeJson(doc, buf);
        mqtt.publish("homeassistant/binary_sensor/" DEVICE_ID "/temp_fault/config", buf, true);
    }

    // 5) Temperature limit (number slider)
    {
        JsonDocument doc;
        doc["name"]       = "Temp Limit";
        doc["uniq_id"]    = DEVICE_ID "_temp_limit";
        doc["cmd_t"]      = TOPIC_TEMP_LIMIT_SET;
        doc["stat_t"]     = TOPIC_TEMP_LIMIT_STATE;
        doc["avty_t"]     = TOPIC_AVAILABILITY;
        doc["min"]        = TEMP_LIMIT_MIN;
        doc["max"]        = TEMP_LIMIT_MAX;
        doc["step"]       = 0.5;
        doc["dev_cla"]    = "temperature";
        doc["unit_of_meas"] = "\u00b0C";
        doc["ic"]         = "mdi:thermometer-alert";
        addDevice(doc["dev"].to<JsonObject>());
        serializeJson(doc, buf);
        mqtt.publish("homeassistant/number/" DEVICE_ID "/temp_limit/config", buf, true);
    }

    // 6) Firmware update entity
    {
        JsonDocument doc;
        doc["name"]       = "Firmware";
        doc["uniq_id"]    = DEVICE_ID "_firmware";
        doc["stat_t"]     = TOPIC_OTA_STATE;
        doc["cmd_t"]      = TOPIC_OTA_SET;
        doc["pl_inst"]    = "install";
        doc["avty_t"]     = TOPIC_AVAILABILITY;
        doc["dev_cla"]    = "firmware";
        doc["ent_cat"]    = "config";
        addDevice(doc["dev"].to<JsonObject>());
        serializeJson(doc, buf);
        mqtt.publish("homeassistant/update/" DEVICE_ID "/firmware/config", buf, true);
    }

    // 7) OTA status text (diagnostic)
    {
        JsonDocument doc;
        doc["name"]       = "OTA Status";
        doc["uniq_id"]    = DEVICE_ID "_ota_status";
        doc["stat_t"]     = TOPIC_OTA_STATUS;
        doc["avty_t"]     = TOPIC_AVAILABILITY;
        doc["ic"]         = "mdi:update";
        doc["ent_cat"]    = "diagnostic";
        addDevice(doc["dev"].to<JsonObject>());
        serializeJson(doc, buf);
        mqtt.publish("homeassistant/sensor/" DEVICE_ID "/ota_status/config", buf, true);
    }

    // 8) Diagnostics
    auto diagSensor = [&](const char* component, const char* name, const char* idSuffix,
                          const char* topic, const char* icon, const char* devCla,
                          const char* unit, const char* statCla) {
        JsonDocument doc;
        doc["name"]    = name;
        char uid[64];
        snprintf(uid, sizeof(uid), "%s_%s", DEVICE_ID, idSuffix);
        doc["uniq_id"] = uid;
        doc["stat_t"]  = topic;
        doc["avty_t"]  = TOPIC_AVAILABILITY;
        doc["ent_cat"] = "diagnostic";
        if (icon)    doc["ic"]           = icon;
        if (devCla)  doc["dev_cla"]      = devCla;
        if (unit)    doc["unit_of_meas"] = unit;
        if (statCla) doc["stat_cla"]     = statCla;
        if (strcmp(component, "binary_sensor") == 0) { doc["pl_on"] = "ON"; doc["pl_off"] = "OFF"; }
        addDevice(doc["dev"].to<JsonObject>());
        serializeJson(doc, buf);
        char topicBuf[96];
        snprintf(topicBuf, sizeof(topicBuf), "homeassistant/%s/%s/%s/config", component, DEVICE_ID, idSuffix);
        mqtt.publish(topicBuf, buf, true);
    };
    diagSensor("sensor",        "Reset Reason", "reset_reason", TOPIC_DIAG_RESET_REASON, "mdi:restart",       nullptr,           nullptr, nullptr);
    diagSensor("sensor",        "Boot Count",   "boot_count",   TOPIC_DIAG_BOOT_COUNT,   "mdi:counter",       nullptr,           nullptr, "total_increasing");
    diagSensor("sensor",        "Uptime",       "uptime",       TOPIC_DIAG_UPTIME,       "mdi:timer-outline", "duration",        "s",     nullptr);
    diagSensor("sensor",        "Free Heap",    "heap",         TOPIC_DIAG_HEAP,         "mdi:memory",        "data_size",       "B",     "measurement");
    diagSensor("sensor",        "Wi-Fi RSSI",   "rssi",         TOPIC_DIAG_RSSI,         nullptr,             "signal_strength", "dBm",   "measurement");
    diagSensor("sensor",        "Last Crash",   "last_crash",   TOPIC_DIAG_LAST_CRASH,   "mdi:bug",           nullptr,           nullptr, nullptr);
    diagSensor("binary_sensor", "Clock Synced", "clock_synced", TOPIC_DIAG_CLOCK,        "mdi:clock-check",   nullptr,           nullptr, nullptr);

    Serial.println("HA discovery published");
}

// ---------------------------------------------------------------------------
// MQTT callback
// ---------------------------------------------------------------------------

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    char msg[512];
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
            if (newMode == MODE_ON && clockValid() && !isInSchedule()) {
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
        if (applyTempLimit(tl, true)) {
            Serial.printf("Temp limit changed to %.1f°C\n", tempLimit);
        } else {
            Serial.printf("Temp limit '%s' rejected (range %.0f-%.0f)\n", msg, TEMP_LIMIT_MIN, TEMP_LIMIT_MAX);
        }
        publishTempLimit();
    }
    else if (strcmp(topic, TOPIC_OTA_LATEST) == 0) {
        // Retained JSON describing the newest available build. Empty payload clears it.
        JsonDocument doc;
        if (len == 0 || deserializeJson(doc, msg)) {
            otaLatestVersion[0] = '\0';
            otaLatestUrl[0]     = '\0';
        } else {
            strlcpy(otaLatestVersion, doc["version"] | "", sizeof(otaLatestVersion));
            strlcpy(otaLatestUrl,     doc["url"]     | "", sizeof(otaLatestUrl));
        }
        Serial.printf("OTA latest: v'%s' at '%s'\n", otaLatestVersion, otaLatestUrl);
        publishOtaState(false, 0);
    }
    else if (strcmp(topic, TOPIC_OTA_SET) == 0) {
        if (strncmp(msg, "http", 4) == 0) {
            // Explicit URL: always install, no version check (manual override / re-flash same version)
            strlcpy(otaPendingUrl, msg, sizeof(otaPendingUrl));
            otaRequested = true;
        } else if (strcmp(msg, "install") == 0) {
            if (otaLatestUrl[0] == '\0') {
                setOtaStatus("install requested but nothing published on ota/latest");
            } else if (strcmp(otaLatestVersion, FW_VERSION) == 0) {
                setOtaStatus("already running v" FW_VERSION ", bump the version to reinstall");
            } else {
                strlcpy(otaPendingUrl, otaLatestUrl, sizeof(otaPendingUrl));
                otaRequested = true;
            }
        } else {
            setOtaStatus("unknown OTA command (expected 'install' or an http:// URL)");
        }
    }
}

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------

void connectWifi() {
    if (WiFi.status() == WL_CONNECTED) return;

    Serial.printf("Connecting to Wi-Fi '%s'", WIFI_SSID);
    WiFi.persistent(false);        // never write credentials to flash on each begin()
    WiFi.setAutoReconnect(true);
    WiFi.setHostname(OTA_HOSTNAME);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        esp_task_wdt_reset();
        Serial.print(".");
        digitalWrite(LED_PIN, !digitalRead(LED_PIN)); // blink LED
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nWi-Fi connected — IP: %s\n", WiFi.localIP().toString().c_str());
        digitalWrite(LED_PIN, LOW);
        if (!ntpStarted) startNtp();
    } else {
        Serial.println("\nWi-Fi connection failed, will retry...");
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
        mqtt.subscribe(TOPIC_OTA_SET);
        mqtt.subscribe(TOPIC_OTA_LATEST);
        mqtt.publish(TOPIC_AVAILABILITY, "online", true);
        publishDiscovery();
        publishAllState();
        otaConfirmIfPending();
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
    Serial.println("\n=== Relay Timer v" FW_VERSION " starting ===");
    otaBootCheck();
    diagBoot();
    setupLoopWatchdog();

    // GPIO
    pinMode(RELAY_PIN, OUTPUT);
    setRelay(false);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH); // LED on during boot

    // Temperature sensor: non-blocking conversions, address resolved on first use
    tempSensor.begin();
    tempSensor.setWaitForConversion(false);

    // Load saved state
    loadMode();
    loadSchedules();
    loadAutoRevert();
    loadTempLimit();
    Serial.printf("Loaded mode: %s, schedules: %s, autoRevert: %d, temp limit: %.1f°C\n",
                  modeToString(currentMode), scheduleToString().c_str(),
                  autoRevertToAuto, tempLimit);

    // Connect (NTP is started inside connectWifi and syncs in the background)
    connectWifi();
    if (WiFi.status() == WL_CONNECTED) {
        connectMqtt();
    }
    lastMqttOkMs = millis(); // start the offline clock now, not at power-on

    // Prime one temperature reading so the first relay evaluation has real data
    // (otherwise AUTO/ON would briefly force the relay on under the "sensor invalid" rule).
    startTempConversion();
    if (tempConversionPending) {
        delay(tempConversionMs());
        finishTempConversion();
    }
    Serial.printf("Initial temperature: %s\n",
                  tempSensorValid ? String(currentTemp, 1).c_str() : "sensor not found");

    digitalWrite(LED_PIN, LOW); // LED off when ready
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void loop() {
    esp_task_wdt_reset();

    // Reconnect Wi-Fi if needed
    if (WiFi.status() != WL_CONNECTED) {
        if (millis() - lastWifiAttempt > 10000) {
            lastWifiAttempt = millis();
            connectWifi();
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
    serviceConnectivity();
    serviceOta();

    // Auto-revert ON → AUTO when schedule starts
    if (currentMode == MODE_ON && autoRevertToAuto && isInSchedule()) {
        currentMode = MODE_AUTO;
        autoRevertToAuto = false;
        saveMode();
        saveAutoRevert();
        Serial.println("Auto-reverted from ON to AUTO (schedule started)");
        if (mqtt.connected()) publishModeState();
    }

    serviceTemperature();
    evaluateRelay();

    // Periodic state re-publish
    if (mqtt.connected() && millis() - lastStatePublish > 60000) {
        lastStatePublish = millis();
        publishAllState();
    }

    delay(500);
}
