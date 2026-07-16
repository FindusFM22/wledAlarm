#pragma once
#include "wled.h"

/*
 * Wecker Presence Usermod
 *
 * Checks if a device (e.g. iPhone) is present on the network via mDNS.
 * If the device is not found shortly before the alarm fires, the alarm
 * timers are disabled so the light does not turn on when nobody is home.
 *
 * Config (stored in cfg.json under "wecker_presence"):
 *   hostname   : mDNS hostname to look for (without .local), e.g. "Findus-mobile"
 *   checkMins  : how many minutes before earliest alarm to run the check (default 10)
 *   enabled    : enable/disable this usermod
 */

class WeckerPresenceUsermod : public Usermod {
private:
  static const char _name[];

  bool     enabled       = true;
  char     hostname[64]  = "Findus-mobile";
  uint8_t  checkMins     = 10;   // check this many minutes before earliest alarm

  bool     lastPresent    = true;
  bool     checkedToday   = false;
  bool     checkRequested = false;
  bool     nlStarted      = false;
  uint8_t  lastPreset     = 0;

  // Find the earliest fire time (in minutes since midnight) across all enabled timers
  int earliestTimerMinute() {
    int earliest = -1;
    for (const auto& t : timers) {
      if (!t.isEnabled()) continue;
      int m = t.hour * 60 + t.minute;
      if (earliest < 0 || m < earliest) earliest = m;
    }
    return earliest;
  }

  // Disable all WLED timers (called when nobody is home)
  void disableTimers() {
    clearTimers();
    configNeedsWrite = true;
  }

  // Re-enable timers from wecker.json
  void rebuildTimers() {
    if (!WLED_FS.exists(F("/wecker.json"))) return;
    File f = WLED_FS.open(F("/wecker.json"), "r");
    if (!f) return;
    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, f)) { f.close(); return; }
    f.close();

    clearTimers();
    int idx = 0;
    for (JsonObject alarm : doc.as<JsonArray>()) {
      if (!alarm["enabled"].as<bool>()) continue;
      int wakeH  = alarm["wakeH"]    | 0;
      int wakeM  = alarm["wakeM"]    | 0;
      int offset = alarm["offsetMin"]| 0;
      uint8_t dow = alarm["dow"]     | 127;
      uint8_t presetId = 240 + idx;

      int fireTotal = ((wakeH * 60 + wakeM - offset) % 1440 + 1440) % 1440;
      uint8_t fireH = fireTotal / 60;
      int8_t  fireM = fireTotal % 60;
      uint8_t weekdays = (dow << 1) | 1;

      addTimer(presetId, fireH, fireM, weekdays, 1, 12, 1, 31);
      idx++;
    }
    configNeedsWrite = true;
  }

  bool checkPresence() {
    // Step 1: resolve hostname via mDNS
    char fqdn[72];
    snprintf(fqdn, sizeof(fqdn), "%s.local", hostname);
    IPAddress ip;
    if (!WiFi.hostByName(fqdn, ip, 3000)) {
      DEBUG_PRINTF_P(PSTR("[WeckerPresence] mDNS %s → not found\n"), fqdn);
      return false;
    }
    DEBUG_PRINTF_P(PSTR("[WeckerPresence] mDNS %s → %s\n"), fqdn, ip.toString().c_str());

    // Step 2: try TCP port 62078 three times — iPhone may be in deep sleep
    for (int attempt = 0; attempt < 3; attempt++) {
      uint32_t t = millis();
      WiFiClient client;
      client.setTimeout(150);
      client.connect(ip, 62078);
      uint32_t elapsed = millis() - t;
      client.stop();
      DEBUG_PRINTF_P(PSTR("[WeckerPresence] TCP attempt %d: %dms\n"), attempt, elapsed);
      if (elapsed < 130) return true; // RST came back fast = device present
      delay(500); // wait before retry
    }
    return false;
  }

public:
  void setup() override {
    server.on(F("/wecker/presence"), HTTP_GET, [this](AsyncWebServerRequest *request){
      // Returns cached result immediately; trigger fresh check for next call
      checkRequested = true;
      String resp = lastPresent
        ? F("{\"present\":true,\"status\":\"zuhause\"}")
        : F("{\"present\":false,\"status\":\"nicht zuhause\"}");
      request->send(200, FPSTR(CONTENT_TYPE_JSON), resp);
    });
  }

  void loop() override {
    if (!enabled) return;
    if (!WLED_CONNECTED) return;

    // Reset nlStarted when preset changes so next wecker preset triggers nightlight again
    if (currentPreset != lastPreset) { nlStarted = false; lastPreset = currentPreset; }

    // On-demand check triggered by HTTP GET /wecker/presence
    if (checkRequested) {
      checkRequested = false;
      lastPresent = checkPresence();
      if (!lastPresent) disableTimers(); else rebuildTimers();
      return;
    }

    if (timers.empty()) return;

    // Find the next alarm that will fire today (or tomorrow if past midnight)
    int earliest = earliestTimerMinute();
    if (earliest < 0) return;

    int nowMin = hour(localTime) * 60 + minute(localTime);
    int target = earliest - checkMins;
    if (target < 0) target = 0;

    // Only check once per alarm — reset when we pass the alarm time
    if (nowMin > earliest + 2) {
      checkedToday = false; // reset after alarm fired, ready for next day
    }

    if (checkedToday) return;

    // Fire within a 1-minute window before the alarm
    if (nowMin < target || nowMin > target + 1) return;

    checkedToday = true;
    bool present = checkPresence();

    if (!present) {
      // Only disable if previously present — fail-safe: keep timers on doubt
      if (lastPresent) {
        // Double-check with a second pass before disabling
        delay(2000);
        present = checkPresence();
      }
      if (!present && lastPresent) {
        DEBUG_PRINTLN(F("[WeckerPresence] Not home — disabling timers"));
        disableTimers();
      }
    } else if (present && !lastPresent) {
      DEBUG_PRINTLN(F("[WeckerPresence] Home again — rebuilding timers"));
      rebuildTimers();
    }
    lastPresent = present;
  }

  // Called after a preset is applied — start nightlight if it's a wecker preset
  void onStateChange(uint8_t mode) override {
    if (!enabled) return;
    if (currentPreset <= 0 || nlStarted) return;
    if (!WLED_FS.exists(F("/wecker.json"))) return;

    File wf = WLED_FS.open(F("/wecker.json"), "r");
    if (!wf) return;
    DynamicJsonDocument doc(2048);
    bool isWecker = false;
    if (!deserializeJson(doc, wf)) {
      for (JsonObject alarm : doc.as<JsonArray>()) {
        if (alarm["enabled"].as<bool>() && (alarm["preset"] | 0) == currentPreset) {
          isWecker = true; break;
        }
      }
    }
    wf.close();

    if (isWecker) {
      nightlightActive    = true;
      nightlightActiveOld = false;
      nightlightDelayMins = 60;
      nightlightMode      = NL_MODE_FADE;
      nightlightTargetBri = 0;
      nlStarted = true;
      DEBUG_PRINTLN(F("[WeckerPresence] Wecker preset fired — starting 60min nightlight"));
    }
  }

  void addToConfig(JsonObject& root) override {
    JsonObject top = root.createNestedObject(FPSTR(_name));
    top["enabled"]   = enabled;
    top["hostname"]  = hostname;
    top["checkMins"] = checkMins;
  }

  bool readFromConfig(JsonObject& root) override {
    JsonObject top = root[FPSTR(_name)];
    if (top.isNull()) return false;
    getJsonValue(top["enabled"],   enabled);
    getJsonValue(top["checkMins"], checkMins);
    if (top["hostname"].is<const char*>())
      strlcpy(hostname, top["hostname"].as<const char*>(), sizeof(hostname));
    return true;
  }

  void addToJsonInfo(JsonObject& root) override {
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");
    JsonArray arr = user.createNestedArray(F("Presence"));
    arr.add(lastPresent ? F("zuhause") : F("nicht zuhause"));
  }

  uint16_t getId() override { return 0xAA01; }

  // Public for on-demand test via HTTP
  bool testNow() {
    bool present = checkPresence();
    lastPresent = present;
    if (!present) disableTimers();
    else rebuildTimers();
    return present;
  }
};
