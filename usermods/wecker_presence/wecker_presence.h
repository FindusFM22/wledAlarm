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

  bool     lastPresent   = true;
  uint32_t lastCheckMs   = 0;
  bool     checkedToday  = false;
  uint8_t  lastCheckDay  = 255;

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
    char fqdn[72];
    snprintf(fqdn, sizeof(fqdn), "%s.local", hostname);
    IPAddress ip;
    bool found = WiFi.hostByName(fqdn, ip, 3000);
    DEBUG_PRINTF_P(PSTR("[WeckerPresence] %s → %s\n"),
      fqdn, found ? ip.toString().c_str() : "not found");
    return found;
  }

public:
  void setup() override {}

  void loop() override {
    if (!enabled) return;
    if (!WLED_CONNECTED) return;
    if (timers.empty()) return;

    // Run once per day, checkMins before the earliest alarm
    uint8_t today = weekday(localTime);
    if (today != lastCheckDay) {
      checkedToday = false;
      lastCheckDay = today;
    }
    if (checkedToday) return;

    int earliest = earliestTimerMinute();
    if (earliest < 0) return;

    int nowMin  = hour(localTime) * 60 + minute(localTime);
    int target  = earliest - checkMins;
    if (target < 0) target = 0;

    // Fire within a 1-minute window around target
    if (nowMin < target || nowMin > target + 1) return;

    checkedToday = true;
    bool present = checkPresence();

    if (!present && lastPresent) {
      DEBUG_PRINTLN(F("[WeckerPresence] Not home — disabling timers"));
      disableTimers();
    } else if (present && !lastPresent) {
      DEBUG_PRINTLN(F("[WeckerPresence] Home again — rebuilding timers"));
      rebuildTimers();
    }
    lastPresent = present;
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
};
