#pragma once
#ifndef WLED_DISABLE_WECKER
#include "wled.h"
#include "asyncDNS.h"

/*
 * Wecker Presence Usermod (v2 — eigenständig, non-blocking)
 *
 * Verwaltet Wecker-Alarme vollständig im RAM, ohne die globale WLED-
 * Timer-Tabelle zu berühren. wecker.json ist einzige Source of Truth.
 * Reguläre WLED-Zeitschaltungen bleiben unberührt.
 *
 * Registriert HTTP-Routen: GET /wecker.json, POST /wecker.json, GET /wecker/presence
 *
 * Config (cfg.json, Schlüssel "wecker_presence"):
 *   presenceAddr : IP oder Hostname des zu prüfenden Geräts (feste IP empfohlen!)
 *   checkMins    : Minuten vor dem frühesten Alarm für Presence-Check (default 10)
 *   enabled      : Usermod aktiv/inaktiv
 *
 * Hinweis: Hostname-Auflösung via AsyncDNS (lwIP unicast) — .local/mDNS wird
 *   wahrscheinlich NICHT aufgelöst. Feste IP per DHCP-Reservierung verwenden.
 *   DNS-Fehler sind Fail-safe: Wecker feuert im Zweifel trotzdem.
 */

class WeckerPresenceUsermod : public Usermod {
private:
  // ── Alarm-Datenmodell ───────────────────────────────────────────────────────
  struct WeckerAlarm {
    uint8_t  wakeH, wakeM;
    int16_t  offsetMin;   // Vorlauf: Lampe geht offsetMin Minuten VOR Weckzeit an
    uint8_t  dow;         // Wochentag-Bitmaske: bit0=Mo..bit6=So; 0 = einmalig
    uint8_t  preset;
    bool     enabled;
  };
  static const uint8_t MAX_ALARMS = 16;
  WeckerAlarm  alarms[MAX_ALARMS];
  uint8_t      alarmCount   = 0;
  volatile bool alarmsDirty = true;   // POST-Handler setzt true → loadAlarms() im loop

  // ── Timer-Tick ──────────────────────────────────────────────────────────────
  int8_t  lastCheckedMinute = -1;     // wie lastTimerMinute in ntp.cpp:431
  bool    checkTriggeredToday = false;
  bool    armNightlight = false;
  uint8_t armPreset     = 0;

  // ── Presence State-Machine ──────────────────────────────────────────────────
  enum PState : uint8_t { PS_IDLE, PS_RESOLVING, PS_CONNECTING, PS_DONE };
  PState   pState     = PS_IDLE;
  uint32_t pStateSince = 0;
  uint8_t  pRetries   = 0;
  static const uint8_t  PRESENCE_RETRIES     = 2;
  static const uint32_t PRESENCE_DNS_TIMEOUT = 5000;   // ms
  static const uint32_t PRESENCE_TCP_TIMEOUT = 1500;   // ms
  static const uint32_t PRESENCE_RETRY_DELAY = 1000;   // ms zwischen Retries
  static const uint16_t PRESENCE_TCP_PORT    = 62078;

  bool      lastPresent    = true;
  bool      presenceSuppress = false;    // In-RAM: unterdrückt Feuern, KEIN cfg-Write
  bool      checkRequested = false;

  IPAddress                 targetIP;
  std::shared_ptr<AsyncDNS> dnsQuery;
  AsyncClient*              presenceClient = nullptr;
  uint32_t                  retryAfter     = 0;   // millis-Timestamp für Retry-Verzögerung

  // ── Config ──────────────────────────────────────────────────────────────────
  static const char _name[];
  bool    enabled   = true;
  char    presenceAddr[64] = "";   // feste IP oder Hostname (kein .local)
  uint8_t checkMins = 10;

  // ── Interne Hilfsfunktionen ─────────────────────────────────────────────────

  // Fire-Zeit eines Alarms in Minuten seit Mitternacht
  static int fireMinute(const WeckerAlarm& a) {
    return (((int)a.wakeH * 60 + a.wakeM - a.offsetMin) % 1440 + 1440) % 1440;
  }

  // Frühester Fire-Zeitpunkt heute (−1 wenn kein heutiger aktiver Alarm)
  int earliestFireMinuteToday() const {
    byte todayIdx = weekdayMondayFirst() - 1;   // 0=Mo..6=So
    int  earliest = -1;
    for (uint8_t i = 0; i < alarmCount; i++) {
      const WeckerAlarm& a = alarms[i];
      if (!a.enabled) continue;
      if (a.dow != 0 && !((a.dow >> todayIdx) & 1)) continue;
      int m = fireMinute(a);
      if (earliest < 0 || m < earliest) earliest = m;
    }
    return earliest;
  }

  // Spätester Fire-Zeitpunkt heute (für Reset-Fenster nach Alarmen)
  int lastFireMinuteToday() const {
    byte todayIdx = weekdayMondayFirst() - 1;
    int  latest   = -1;
    for (uint8_t i = 0; i < alarmCount; i++) {
      const WeckerAlarm& a = alarms[i];
      if (!a.enabled) continue;
      if (a.dow != 0 && !((a.dow >> todayIdx) & 1)) continue;
      int m = fireMinute(a);
      if (m > latest) latest = m;
    }
    return latest;
  }

  // Alarme aus wecker.json in RAM-Cache laden
  void loadAlarms() {
    alarmCount = 0;
    if (!WLED_FS.exists(F("/wecker.json"))) return;
    File f = WLED_FS.open(F("/wecker.json"), "r");
    if (!f) return;
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, f)) { f.close(); return; }
    f.close();
    for (JsonObject alarm : doc.as<JsonArray>()) {
      if (alarmCount >= MAX_ALARMS) break;
      WeckerAlarm& a = alarms[alarmCount];
      a.wakeH     = alarm["wakeH"]     | 0;
      a.wakeM     = alarm["wakeM"]     | 0;
      a.offsetMin = alarm["offsetMin"] | 0;
      a.dow       = alarm["dow"]       | 127;
      a.preset    = alarm["preset"]    | 1;
      a.enabled   = alarm["enabled"]   | false;
      alarmCount++;
    }
    DEBUG_PRINTF_P(PSTR("[Wecker] Geladen: %d Alarme aus wecker.json\n"), alarmCount);
  }

  // Aktuelle alarm[]-Inhalte als wecker.json-kompatibles JSON schreiben
  // (nur für "einmalig" benötigt; hält dieselbe Reihenfolge und Felder wie das Original)
  void persistAlarms() {
    // Wir lesen die Originaldatei und patchen nur das enabled-Feld des passenden Alarms.
    // Einfachste korrekte Implementierung: Datei neu aufbauen aus RAM-Cache.
    // Felder die wir nicht im Cache haben (id, label) werden NICHT preserviert — daher
    // bevorzugen wir den Ansatz, originale JSON-Datei zu lesen und nur zu patchen.
    if (!WLED_FS.exists(F("/wecker.json"))) return;
    File rf = WLED_FS.open(F("/wecker.json"), "r");
    if (!rf) return;
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, rf)) { rf.close(); return; }
    rf.close();

    // Cache-Index ↔ JSON-Array-Index stimmen überein (beide filtern nicht)
    uint8_t ci = 0;
    for (JsonObject alarm : doc.as<JsonArray>()) {
      if (ci >= alarmCount) break;
      alarm["enabled"] = alarms[ci].enabled;
      ci++;
    }
    File wf = WLED_FS.open(F("/wecker.json"), "w");
    if (wf) { serializeJson(doc, wf); wf.close(); }
  }

  // ── Async Presence State-Machine ────────────────────────────────────────────

  enum PresenceResult : uint8_t { PR_PRESENT, PR_ABSENT, PR_UNKNOWN };

  void startPresenceCheck() {
    if (pState == PS_RESOLVING || pState == PS_CONNECTING) return; // läuft noch
    pRetries = PRESENCE_RETRIES;
    pStateSince = millis();

    // Feste IP? Dann direkt verbinden, kein DNS
    if (targetIP.fromString(presenceAddr)) {
      DEBUG_PRINTF_P(PSTR("[Wecker] Presence: direkte IP %s\n"), presenceAddr);
      beginTcpProbe();
      return;
    }
    // Hostname → AsyncDNS (lwIP unicast, .local nicht garantiert)
    if (presenceAddr[0] != '\0') {
      DEBUG_PRINTF_P(PSTR("[Wecker] Presence: DNS-Lookup für %s\n"), presenceAddr);
      dnsQuery = AsyncDNS::query(presenceAddr, dnsQuery);
      pState = PS_RESOLVING;
    } else {
      // Keine Adresse konfiguriert → kein Check, nicht unterdrücken
      DEBUG_PRINTLN(F("[Wecker] Presence: Adresse nicht konfiguriert, skip"));
    }
  }

  // AsyncClient wird EINMAL angelegt und wiederverwendet (Muster hue.cpp:38-46);
  // niemals delete → keine Lecks, kein Delete-im-Callback.
  bool ensurePresenceClient() {
    if (presenceClient) return true;
    presenceClient = new AsyncClient();
    if (!presenceClient) return false;
    presenceClient->onConnect(&sOnTcpConnect, this);
    presenceClient->onError  (&sOnTcpError,   this);
    return true;
  }

  void beginTcpProbe() {
    if (!ensurePresenceClient()) { finishPresence(PR_UNKNOWN); return; }
    presenceClient->close();   // No-op wenn nicht verbunden; alte Verbindung sicher beenden
    pState      = PS_CONNECTING;
    pStateSince = millis();
    if (!presenceClient->connect(targetIP, PRESENCE_TCP_PORT)) {
      finishPresence(PR_UNKNOWN);   // connect() konnte nicht starten → Fail-safe
    }
  }

  void finishPresence(PresenceResult r) {
    if (presenceClient) presenceClient->close();   // Verbindung beenden, Objekt bleibt (Wiederverwendung)
    pState     = PS_DONE;
    retryAfter = 0;   // späte Success-Callbacks lösen keinen Nach-Retry aus

    switch (r) {
      case PR_PRESENT:
        DEBUG_PRINTLN(F("[Wecker] Presence: zuhause"));
        presenceSuppress = false;
        lastPresent = true;
        break;
      case PR_ABSENT:
        DEBUG_PRINTLN(F("[Wecker] Presence: nicht zuhause — Unterdrückung aktiv"));
        // Fail-safe: nur unterdrücken wenn vorher präsent (bei erstem Boot lastPresent=true)
        if (lastPresent) presenceSuppress = true;
        lastPresent = false;
        break;
      case PR_UNKNOWN:
        // DNS-Fehler oder keine Adresse: Wecker feuert trotzdem
        DEBUG_PRINTLN(F("[Wecker] Presence: unbekannt — feuern trotzdem"));
        break;
    }
    dnsQuery.reset();
  }

  // Wird jede loop()-Iteration aufgerufen — keine delay()
  void servicePresence() {
    if (checkRequested) {
      checkRequested = false;
      startPresenceCheck();
    }

    uint32_t now = millis();

    switch (pState) {
      case PS_IDLE:
      case PS_DONE:
        return;

      case PS_RESOLVING: {
        if (now - pStateSince > PRESENCE_DNS_TIMEOUT) {
          DEBUG_PRINTLN(F("[Wecker] Presence: DNS-Timeout → UNKNOWN"));
          finishPresence(PR_UNKNOWN);
          return;
        }
        if (!dnsQuery) { finishPresence(PR_UNKNOWN); return; }
        switch (dnsQuery->status()) {
          case AsyncDNS::result::Success:
            targetIP = dnsQuery->getIP();
            DEBUG_PRINTF_P(PSTR("[Wecker] Presence: DNS → %s\n"), targetIP.toString().c_str());
            beginTcpProbe();
            break;
          case AsyncDNS::result::Error:
            DEBUG_PRINTLN(F("[Wecker] Presence: DNS-Fehler → UNKNOWN"));
            finishPresence(PR_UNKNOWN);
            break;
          default:
            break; // Busy/Idle → weiter pollen
        }
        break;
      }

      case PS_CONNECTING: {
        // Watchdog: TCP-Timeout → abwesend (oder retry)
        if (now - pStateSince > PRESENCE_TCP_TIMEOUT) {
          DEBUG_PRINTF_P(PSTR("[Wecker] Presence: TCP-Timeout (retry %d)\n"), pRetries);
          if (pRetries > 0) {
            pRetries--;
            if (presenceClient) presenceClient->close();   // wiederverwenden, nicht löschen
            retryAfter = now + PRESENCE_RETRY_DELAY;
            pState = PS_DONE;   // kurz pausieren; retryAfter triggert beginTcpProbe neu
          } else {
            finishPresence(PR_ABSENT);
          }
        }
        break;
      }
    }

    // Retry-Karenz abgelaufen?
    if (pState == PS_DONE && retryAfter != 0 && now >= retryAfter) {
      retryAfter = 0;
      beginTcpProbe();   // direkt mit schon aufgelöster targetIP
    }
  }

  // Statische AsyncClient-Callbacks (arg = this)
  static void sOnTcpConnect(void* arg, AsyncClient*) {
    WeckerPresenceUsermod* self = static_cast<WeckerPresenceUsermod*>(arg);
    DEBUG_PRINTLN(F("[Wecker] Presence: TCP connected → präsent"));
    self->finishPresence(PR_PRESENT);
  }

  static void sOnTcpError(void* arg, AsyncClient*, int8_t error) {
    WeckerPresenceUsermod* self = static_cast<WeckerPresenceUsermod*>(arg);
    uint32_t elapsed = millis() - self->pStateSince;
    DEBUG_PRINTF_P(PSTR("[Wecker] Presence: TCP error %d nach %ums\n"), error, elapsed);
    // Schnelles RST (< 300ms) bedeutet: Gerät wach und hat abgelehnt → präsent
    if (elapsed < 300) {
      self->finishPresence(PR_PRESENT);
    } else {
      if (self->pRetries > 0) {
        self->pRetries--;
        if (self->presenceClient) self->presenceClient->close();   // wiederverwenden, nicht löschen
        self->retryAfter = millis() + PRESENCE_RETRY_DELAY;
        self->pState = PS_DONE;
      } else {
        self->finishPresence(PR_ABSENT);
      }
    }
  }

public:
  void setup() override {
    loadAlarms();
    alarmsDirty = false;

    // ── HTTP-Routen registrieren ──────────────────────────────────────────────

    // GET /wecker.json — Alarmiste ausliefern
    server.on(F("/wecker.json"), HTTP_GET, [](AsyncWebServerRequest *request) {
      if (WLED_FS.exists(F("/wecker.json"))) {
        request->send(WLED_FS, F("/wecker.json"), FPSTR(CONTENT_TYPE_JSON));
      } else {
        request->send(200, FPSTR(CONTENT_TYPE_JSON), F("[]"));
      }
    });

    // POST /wecker.json — Alarmliste speichern (Body-Streaming, kein String-Puffer-Overread)
    server.on(F("/wecker.json"), HTTP_POST,
      [](AsyncWebServerRequest *request){},
      nullptr,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        static File uploadFile;
        if (index == 0) {
          uploadFile = WLED_FS.open(F("/wecker.json"), "w");
        }
        if (uploadFile) {
          uploadFile.write(data, len);
        }
        if (index + len >= total) {
          if (uploadFile) { uploadFile.close(); }
          // Usermod-Singleton reload anstoßen
          WeckerPresenceUsermod* self = static_cast<WeckerPresenceUsermod*>(
            UsermodManager::lookup(0xAA01)
          );
          if (self) self->alarmsDirty = true;
          request->send(200, FPSTR(CONTENT_TYPE_JSON), F("{\"success\":true}"));
        }
      }
    );

    // GET /wecker/presence — gecachten Presence-Status liefern + frischen Check anstoßen
    server.on(F("/wecker/presence"), HTTP_GET, [this](AsyncWebServerRequest *request) {
      checkRequested = true;   // servicePresence() stößt Check in nächster loop-Iteration an
      const char* resp = lastPresent
        ? "{\"present\":true,\"status\":\"zuhause\"}"
        : "{\"present\":false,\"status\":\"nicht zuhause\"}";
      request->send(200, FPSTR(CONTENT_TYPE_JSON), resp);
    });
  }

  void loop() override {
    if (!enabled) return;
    if (!WLED_CONNECTED) return;
    if (localTime < 1704067200) return;   // NTP noch nicht synchron (vor 2024-01-01)

    // Alarm-Cache bei Bedarf nachladen (nach POST)
    if (alarmsDirty) {
      loadAlarms();
      alarmsDirty = false;
    }

    // Nightlight-Latch zurücksetzen wenn Preset sich ändert
    if (currentPreset != lastPreset) { nlStarted = false; lastPreset = currentPreset; }

    // Presence State-Machine vorwärts treiben (non-blocking)
    servicePresence();

    // Minuten-Tick (genau einmal pro Minute)
    byte curMin = minute(localTime);
    if (curMin == (byte)lastCheckedMinute) return;
    lastCheckedMinute = (int8_t)curMin;

    int nowMin    = (int)hour(localTime) * 60 + curMin;
    byte todayIdx = weekdayMondayFirst() - 1;   // 0=Mo..6=So

    // Presence-Check checkMins vor frühestem heutigen Alarm anstoßen
    int earliest = earliestFireMinuteToday();
    if (earliest >= 0 && !checkTriggeredToday) {
      int target = earliest - (int)checkMins;
      if (target < 0) target = 0;
      if (nowMin == target) {
        startPresenceCheck();
        checkTriggeredToday = true;
      }
    }
    // Reset: nach dem letzten Alarm des Tages checkTriggeredToday und Suppress zurücksetzen
    int latest = lastFireMinuteToday();
    if (latest < 0 || nowMin > latest + 2) {
      checkTriggeredToday = false;
      presenceSuppress    = false;
    }

    // Feuer-Scan
    if (presenceSuppress) return;   // niemand zuhause

    for (uint8_t i = 0; i < alarmCount; i++) {
      WeckerAlarm& a = alarms[i];
      if (!a.enabled) continue;
      if (a.dow != 0 && !((a.dow >> todayIdx) & 1)) continue;   // Wochentag prüfen
      if (fireMinute(a) != nowMin) continue;

      // Alarm feuert
      armNightlight = true;
      armPreset     = a.preset;
      applyPreset(a.preset);   // deferred (handlePresets im selben Loop-Durchlauf)
      DEBUG_PRINTF_P(PSTR("[Wecker] Alarm feuert: preset=%d um %02d:%02d\n"),
                     a.preset, (uint8_t)(nowMin/60), (uint8_t)(nowMin%60));

      // "Einmalig" (dow==0): nach dem Feuern deaktivieren + persistieren
      if (a.dow == 0) {
        a.enabled = false;
        persistAlarms();
        alarmsDirty = false;   // wir haben gerade selbst persistiert
      }
      break;   // ein Preset pro Tick (wie checkTimers ntp.cpp:460)
    }
  }

  // Nightlight starten wenn das von uns gefeuerte Preset angewandt wird
  void onStateChange(uint8_t /* mode */) override {
    if (!enabled || !armNightlight) return;
    if (currentPreset != armPreset) return;   // nicht unser Preset
    nightlightActive    = true;
    nightlightActiveOld = false;
    nightlightDelayMins = 60;
    nightlightMode      = NL_MODE_FADE;
    nightlightTargetBri = 0;
    armNightlight = false;
    nlStarted     = true;
    DEBUG_PRINTLN(F("[Wecker] Nightlight gestartet (60 min fade-out)"));
  }

  void addToConfig(JsonObject& root) override {
    JsonObject top = root.createNestedObject(FPSTR(_name));
    top["enabled"]     = enabled;
    top["presenceAddr"] = presenceAddr;
    top["checkMins"]   = checkMins;
  }

  bool readFromConfig(JsonObject& root) override {
    JsonObject top = root[FPSTR(_name)];
    if (top.isNull()) return false;
    getJsonValue(top["enabled"],   enabled);
    getJsonValue(top["checkMins"], checkMins);
    // Migration: alter Schlüssel "hostname" → presenceAddr
    if (top["presenceAddr"].is<const char*>()) {
      strlcpy(presenceAddr, top["presenceAddr"].as<const char*>(), sizeof(presenceAddr));
    } else if (top["hostname"].is<const char*>()) {
      strlcpy(presenceAddr, top["hostname"].as<const char*>(), sizeof(presenceAddr));
    }
    return true;
  }

  void addToJsonInfo(JsonObject& root) override {
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");
    JsonArray arr = user.createNestedArray(F("Presence"));
    arr.add(lastPresent ? F("zuhause") : F("nicht zuhause"));
  }

  uint16_t getId() override { return 0xAA01; }

private:
  // Für Nightlight-Latch (wie bisher)
  uint8_t lastPreset = 0;
  bool    nlStarted  = false;
};

// _name[] wird in wecker_presence.cpp definiert (ODR: genau eine Übersetzungseinheit).

#endif // WLED_DISABLE_WECKER
