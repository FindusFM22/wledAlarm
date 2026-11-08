# Light alarm

An independent WLED light alarm for ESP32 and ESP8266, with direct phone reachability checks. No FRITZ!Box login, router credentials, phone app, Home Assistant or cloud is required.

## Setup

1. Include `wecker_presence` in `custom_usermods`. For an ESP32-WROOM-32(S), copy the `light_alarm_esp32` example from `platformio_override.sample.ini` into your local `platformio_override.ini`.
2. Build the web assets, then firmware:
   ```sh
   npm ci
   npm run build
   pio run -e light_alarm_esp32
   ```
3. Configure your actual LED type, count, colour order, data pin and safe current limit in WLED. This feature does **not** change GPIO allocation or bus configuration.
4. In **Time & Macros**, enable NTP and select your timezone (Central European for Germany). The example provides these defaults for a fresh installation; existing stored settings take precedence.
5. Reserve a fixed IPv4 for your phone in your router. For an iPhone, use a **fixed private Wi-Fi address** for that network, rather than disabling address privacy everywhere. [Apple documentation](https://support.apple.com/102509).
6. Open `/wecker.htm`, enter the phone IPv4 under **Phone presence setup**, save, and check its status.
7. Add an alarm and test **Preview · 30 sec**. Also test reachability with the phone locked overnight before relying on automatic alarms.

There is no automatic flashing or router configuration. No router account is stored or used.

## Behaviour

- The selected wake time means **target brightness is reached at that time**.
- Each alarm has a rise duration (0–120 minutes), hold duration (0–180 minutes), fade-out duration (0–60 minutes) and snooze duration (1–30 minutes).
- The default RGBW sunrise blends a dim warm colour into the strip's white channel. The actual white temperature comes from the strip, not a software CCT setting. Use a white-channel mode in WLED that permits manual white values.
- Alternatively, a preset supplies the **first visual segment's colours and effect**, applied to the currently active segments. Preset macros, playlists, custom API commands and saved geometry are intentionally not executed. A missing/non-visual preset falls back to sunrise. Ordinary preset use elsewhere in WLED is unchanged.
- Recurring weekdays refer to the **wake date**, including when the rise begins the previous evening. One-shot alarms have an explicit date.
- A skipped, started or overlapping occurrence is consumed individually. A recurring alarm remains enabled for the next date.
- **Stop** ends the alarm. A normal WLED brightness/colour/preset change also ends it, without the old alarm later overwriting that manual change.
- **Snooze** is dark, followed by a one-minute rise and the normal hold/fade phases. Repeated snoozes have a hard maximum runtime.
- Preview runs a 20-second rise, 5-second hold and 5-second fade, then restores the previous visual state. Manual control cancels restoration. Stop an existing WLED nightlight/playlist before previewing.
- Removing or disabling a running alarm ends it. A scheduled alarm takes priority over a manual preview.
- WLED's regular timer table is neither rewritten nor disabled.

## Phone checks and their limits

Checks are bounded, non-blocking and confined to the ESP's own IPv4 subnet. All scheduling windows refer to **light start**, not wake time:

| Situation | Automatic check interval |
|---|---|
| No enabled alarm starting within 12 hours, or clock not initialized | Paused |
| Alarm starting within 12 hours | 15 minutes |
| Last 30 minutes before light start | 5 minutes |
| Automatic alarm running, including snooze | 10 minutes |
| Manual check or changing the phone IP | Immediate when idle; coalesced with an in-flight round, with a 5-second rate limit |

A round sends a valid direct-unicast reverse-PTR mDNS question to UDP port 5353, plus an ARP request. If no reply arrives, it re-queries at approximately 5, 12, 22, 30, 40, 50 and 60 seconds within the same receive window. Delayed replies remain matchable: retries do not reset the DNS token, evidence or transaction. A verified reply ends the round immediately. Five seconds after the last UDP attempt, one ICMP echo and a TCP attempt are added only if still needed. The round is bounded to 75 seconds, with an 80-second receive-window ceiling. Sending a packet alone is never positive evidence. Opening the alarm page does not trigger phone probes when there is no upcoming alarm; its normal HTTP status requests are separate.

This wake-and-recheck burst addresses observed delayed responses after repeated manual tests. It is not a guarantee that an iPhone wakes or responds. Normal 15/5/10-minute background intervals remain unchanged; up to eight nearby UDP questions are grouped into one round, not eight independent polling cycles. The longer worst-case receive period may increase radio activity, so battery savings still need measurement.

| Check | Positive evidence | Limits |
|---|---|---|
| UDP/mDNS | A bounded, matching reverse-address response from the phone's IP and UDP port 5353 | Not all iOS/network combinations reply. This is a protocol query, not a guaranteed wake-up mechanism. |
| ICMP error | Port Unreachable from the phone's IP quoting this UDP endpoint and target; DNS token checked when quoted | Routers' errors, unrelated endpoints and arbitrary ICMP packets are rejected. |
| ICMP ping | One fallback echo reply | Phones may stop answering while sleeping, disconnected or using power saving. |
| TCP port 62078 | A connection **or an actual TCP reset** | The port is commonly useful for Apple devices, but not guaranteed. Quick generic errors are not accepted as presence. No application payload is sent. |
| Active ARP | An actual ARP reply addressed to this interface, for the phone's IP, during the current round | A small transparent observer on the public netif input callback inspects 42 bytes and forwards the original packet exactly once. It never flushes or trusts the cache. Custom network wrappers may make this observation unavailable. |

Any fresh positive evidence creates a **60-minute confirmation lease**. Failed checks no longer shorten that lease, and repeated errors/UI refreshes never extend it. The UI separately shows the latest check, last real reply, remaining lease and adaptive interval. A retained confirmation is highlighted as a grace period, not presented as a fresh reply. When the lease expires, a recent failed round means unreachable; DNS/local network/allocation errors mean unknown. Neither authorizes a new alarm. A running automatic alarm then fades out over three seconds. Preview is explicit manual control and does not require presence.

This intentionally trades fast departure detection for fewer sleeping-phone false negatives: **leaving after a successful check can remain undetected for up to an hour**. Increasing the grace to an entire night would also authorize alarms after an evening departure; it is not treated as a reliable presence solution.

A due alarm with a configured but unknown phone waits up to 90 seconds after its planned light start and may request one targeted retry, not continuous fast polling. The five-minute late-start limit still applies. After boot or changing the phone target, the first pending check may finish even if the rise has already begun. An expired confirmation with a recent no-response result skips immediately.

### Energy impact

The old fixed 30-second interval meant nominally 120 rounds/hour. The normal 15-minute interval means 4 rounds/hour, about **97% fewer check opportunities**, and most successful rounds avoid ping/TCP entirely. The ESP still runs WLED/Wi-Fi normally; this is not MCU deep sleep. A specific iPhone battery saving cannot be inferred from packet counts: radio wake time, background traffic, signal and access-point behaviour matter. Compare overnight behaviour and battery use on the actual phone. See [Apple's network energy guidance](https://developer.apple.com/library/archive/documentation/Performance/Conceptual/EnergyGuide-iOS/EnergyandNetworking.html).

### Check logs

The RAM ring retains **96 completed rounds**: time, manual/automatic trigger, duration, UDP send count, response delay, method and result/error. At a 15-minute interval this is approximately 24 hours; shorter pre-alarm/running intervals reduce that coverage. Manual tests consume entries too. The normal status/UI response shows only the latest 12, plus retained count/capacity and RAM size.

Use **Load full night log** or `GET /wecker/log?offset=0&limit=24` for all retained data. Pages have at most 24 entries, newest first, with `generation`, `total` and `nextOffset`; require the same generation across pages for a consistent read. A dedicated mutex protects the bounded page copy, and serialization occurs after releasing it. No extra phone probes are triggered by log retrieval. Logs are cleared by reboot, never written to flash per probe, and do not prove whether the iPhone display was on or off.

The gateway, ESP itself, network/broadcast addresses, multicast and off-subnet targets are rejected. Hostnames use asynchronous unicast DNS; prefer the reserved IPv4 because `.local` mDNS is not a reliable substitute here.

**These checks measure a device, not a person.** If you leave the phone at home, the alarm may still run. If the phone sleeps through all checks, the alarm may be skipped even while you are home. More retries or combining probes cannot eliminate that common failure mode. Guest networks, client isolation and IPv6-only connectivity are unsuitable. This light alarm is not a guaranteed wake-up device.

## Storage and restart policy

`/wecker.json` holds a version-2 object with a revision, enabled flag, phone target and up to 16 alarms. The firmware validates the complete candidate before applying it, writes `/wecker.tmp.json`, checks the write length and atomically replaces the live file with LittleFS rename. An invalid/failed save leaves the previous configuration in service. Errors are visible in the UI.

A damaged configuration is not used for automatic alarms. If a due occurrence's execution marker cannot be persisted, it is not started; bounded retries may catch up when storage recovers.

Before execution, the occurrence's wake date is persisted. This prevents duplicate alarms after reboot or during the repeated DST hour. A reboot interrupts a running alarm; an already consumed occurrence is **not replayed**. A not-yet-consumed alarm can start partway through its rise, or up to five minutes after the wake time. Old alarms are not replayed hours later. A cold boot without NTP, a manually supplied clock or a supported RTC waits for a valid time.

The missing spring-forward hour shifts a scheduled time forward by the one-hour gap; the repeated fall-back hour uses its first occurrence. Once started, phase durations use monotonic time, independent of subsequent NTP corrections.

Legacy array-format files are migrated after a valid clock is available. IDs, labels, weekdays, presets and lead durations are retained; old one-shot times receive their next wake date. The original list is preserved as `/wecker-legacy.json`. The new hold/fade semantics replace the old immediate 60-minute fade-out. Legacy phone targets are read from the old usermod configuration when present. Do not run an old frontend against the new API: unversioned list updates are rejected.

## API

| Endpoint | Purpose |
|---|---|
| `GET /wecker.json` | Current, versioned configuration; no-store response. |
| `POST /wecker.json` | Complete validated configuration with the revision read by the client. |
| `GET /wecker/status` | Clock, presence evidence, next occurrence, running phase and recent events. |
| `GET /wecker/log?offset=0&limit=24` | Paginated 96-entry RAM history; up to 24 entries per read. |
| `POST /wecker/control` | `{"action":"stop"}`, `snooze`, `skip`, `presence-check`, or `{"action":"preview","id":"..."}`. |
| `GET /wecker/result?job=...` | Result of a queued operation. |

Manual configuration writes and `/wecker/control` commands retain WLED's configured settings PIN policy. A POST returns **202 with a job ID**, not a premature success. Poll the result until it is no longer 202: 200 confirms completion, 409 indicates a revision/operation conflict, and invalid input or storage errors are rejected. Only after a confirmed save should a client replace its local configuration. The browser refreshes revisions and does not silently overwrite another browser's changes. Raw file-editor modifications are not hot-reloaded; use the API/UI.

## iPhone next-wake sync (ESP32)

This is a separate **single, one-shot slot**, in addition to the 16 manual alarms. It accepts an explicit next wake instant, not an Apple alarm ID or the complete Clock alarm list. The server does not assume that a particular iOS version exposes enabled status, a next-occurrence date, or the Health sleep schedule. Verify those capabilities on the actual phone before relying on a Shortcut to choose the correct occurrence.

The feature is **disabled after installation**. Existing manual alarms, their revision, phone target, presets and LED configuration are unchanged. Manual alarms remain active until you deliberately disable them. Both sources share the existing presence checks and one light runner; a manual alarm wins an exact start-time tie, and overlaps are consumed rather than replayed.

| Endpoint | Purpose |
|---|---|
| `GET /wecker/sync/iphone` | Current slot status; also included as `iphone` in `/wecker/status`. |
| `POST /wecker/sync/iphone` | Replace or explicitly clear the single iPhone wake instant. |
| `GET /wecker/sync/iphone/result?job=...&boot=...` | Follow the receipt's `resultUrl` to read sync/profile job results. |
| `GET /wecker/sync/iphone/profile` | Read the independent light profile and its revision. |
| `POST /wecker/sync/iphone/profile` | Set the profile/enable flag, or copy one manual alarm's light settings once. |

**All of these new endpoints intentionally require no token, login, PIN or authorization header.** They trust direct clients in the ESP's local IPv4 subnet. Off-subnet, unset, network and broadcast addresses are rejected. This is a network restriction, not authentication of a particular phone/person: any device on the trusted subnet can change the managed slot and its profile. Ordinary WLED and manual-alarm access policies are not changed. The former token endpoint no longer exists.

Both POSTs return **202** with `job`, `confirmed: false` and `resultUrl`. Follow that URL until HTTP **200** and `confirmed: true`; only then is the operation durably committed. Result URLs include a public boot identifier; it is **not an access credential**. A missing/old identifier returns `410 job_from_other_boot`, rather than confirming a recycled job ID after reboot. Errors retain the previous confirmed state. Results expire from the bounded eight-job RAM ring and do not survive reboot; after an uncertain sync result, read the saved status or resend the exact payload. Do not generate a new `capturedAt` just to retry an old reading.

### Payload and ordering

```json
{
  "capturedAt": "2026-11-08T22:15:00.123+01:00",
  "wakeAt": "2026-11-09T07:00:00+01:00"
}
```

- Both fields are required; no other fields are accepted. Capture the timestamp when reading the phone's alarm data, **before** any network wait/retry.
- Use RFC 3339 timestamps with `Z` or an explicit `±HH:MM` offset. Dates are limited to 2024–2099, real-world offsets to ±14 hours. Leap seconds, unknown `-00:00` offsets, timezone-free dates and more than three fractional digits are rejected. `capturedAt` supports milliseconds; `wakeAt` must have whole-second precision (an optional `.000` is accepted).
- The explicit UTC instant is authoritative, even during the second repeated DST hour. The ESP's timezone affects display, not the received instant. The device computes light start from the independent profile's rise duration.
- A valid device clock is required for a new update. A new capture may be at most 24 hours old or 5 minutes ahead of the device. A new wake instant must be in the future and at most 8 days away.
- An older capture is rejected with `409 stale_sync`. The same capture with different data is `409 sync_conflict`. Equivalent timestamps with different offsets compare as the same instant.
- An exact retry is acknowledged without another flash write, even after execution or expiry. A fresher capture of the same consumed occurrence advances ordering but **never re-arms it**.
- Client writes are limited to one per second (`429 sync_rate_limited`, retry after one second). Execution markers are not rate-limited.

Only an explicit successful no-alarm reading clears the slot:

```json
{"capturedAt":"2026-11-08T22:15:00+01:00","wakeAt":null}
```

Missing/invalid fields, malformed JSON (including trailing garbage and NUL escapes), a read error, or a network error must **not** be converted to this payload. A clear or replacement ends a running iPhone alarm, but never a running manual alarm. Clearing/disabling the managed slot retains its ordering and execution watermarks.

### Profile setup

Read `/wecker/sync/iphone/profile`, then POST the returned **profile revision**, not the manual configuration revision:

```json
{
  "revision": 0,
  "enabled": true,
  "profile": {
    "label": "iPhone",
    "rampMin": 30,
    "holdMin": 30,
    "fadeMin": 5,
    "snoozeMin": 10,
    "brightness": 180,
    "white": 220,
    "scene": "sunrise",
    "preset": 0
  }
}
```

Alternatively, `{"revision":0,"enabled":true,"copyFrom":"existing-manual-id"}` copies that alarm's light settings once. It does not import its ID, recurrence or execution dates and does not remove/disable the manual alarm. Profile edits affect the next start, not a running light curve; disabling the phone profile ends its current run. Use the current revision from GET; conflicts return `409 profile_revision_conflict`. A phone sync never changes the profile revision.

### Terminal sync example

These examples use `curl` and `jq`. Enable/configure the independent profile once, then send just the next wake instant from the phone. No authorization header or global configuration revision is needed for a sync.

```sh
WLED='http://192.168.178.32'
CAPTURED=$(date -u +'%Y-%m-%dT%H:%M:%SZ')
WAKE_AT='2026-11-09T07:00:00+01:00' # Choose your actual future wake instant.
BODY=$(jq -cn --arg captured "$CAPTURED" --arg wake "$WAKE_AT" \
  '{capturedAt:$captured,wakeAt:$wake}')
RECEIPT=$(curl --noproxy '*' -fsS \
  -H 'Content-Type: application/json' \
  --data "$BODY" "$WLED/wecker/sync/iphone")
RESULT_URL=$(printf '%s\n' "$RECEIPT" | jq -er '.resultUrl')

# Repeat while HTTP 202; only 200 / confirmed:true confirms a durable save.
curl --noproxy '*' -fsS \
  "$WLED$RESULT_URL" | jq

curl --noproxy '*' -fsS "$WLED/wecker/sync/iphone" | jq
```

The status distinguishes `disabled`, `empty`, `scheduled`, `waiting_for_time`, `started`, `completed`, `skipped`, `stopped`, `cancelled`, `interrupted`, `consumed`, `expired` and storage unavailability. `occurrenceState` retains the saved state even when the integration/master is disabled. ISO timestamps are UTC; the additional `*Local` numeric values are only for WLED's UI. There is no automatic repeat tomorrow without another sync.

### Persistence and security

The profile, last capture, wake instant and monotonic consumed-wake watermark are stored together in one bounded version-2 JSON blob in ESP32 NVS namespace `weckersync`, key `state`. This uses NVS's replacement/recovery semantics, without modifying partition layouts or other namespaces. There are no access credentials in the record. Former version-1 records are rewritten without their obsolete credential fields while preserving profile and replay protection. Ordinary firmware-only OTA retains NVS.

The watermark is committed **before** taking LED ownership. A reboot never replays a consumed occurrence, including an overlapping, unreachable or manually skipped one. Completed/started outcome writes are diagnostic; if such a write fails, the required consumption marker still prevents replay. New dates at or before the consumed UTC watermark are rejected unless merely acknowledging the same consumed slot. Storage corruption disables this integration, not the manual alarms.

NVS is not included in the JSON/filesystem configuration backup. Filesystem-only resets do not erase this namespace; clear/disable the managed slot before transferring the controller. Use this credential-free API **only in an isolated, trusted home network**. Do not publish it through port forwarding or a local reverse proxy: the subnet check cannot identify an external caller hidden behind such a proxy. HTTP is not encrypted. ESP8266 retains the normal manual alarm feature, reports `supported:false` here, and rejects sync writes with `501 sync_unsupported`.

## Architecture and validation

- `light_alarm_model.h`: bounded Gregorian scheduling, DST resolution, monotonic runner and presence confidence.
- `light_alarm_config.h`: shared ingress/filesystem validation and versioned serialization.
- `light_alarm_store.h`: durable configuration replacement and legacy preservation.
- `light_alarm_sync.h`, `light_alarm_sync_store.h`: explicit UTC phone sync, local-network access, independent profile and NVS replay/ordering persistence on ESP32.
- `light_alarm_presence.h`: asynchronous ping/TCP/ARP probes; callbacks publish atomics, never touch LEDs or files.
- `light_alarm_udp.h`, `light_alarm_probe_codec.h`: bounded direct-unicast mDNS/ICMP/ARP evidence and a transparent packet observer. Networking state is initialized in lwIP context; receive buffers and PCBs are bounded/reused.
- `light_alarm_atomic.h`, `light_alarm_lock.h`: ESP32 task synchronization and ESP8266 cooperative callback compatibility.
- `light_alarm_wled.h`: visual ownership, preview restoration and WLED renderer integration.
- `wecker_presence.cpp`: bounded command queue, immutable HTTP snapshots and orchestration in the normal WLED loop.
- `wled00/data/wecker.{htm,js,css}`: UI, built into `html_wecker.h`; no generated headers are committed.

Run `npm test` for native C++ scheduling/runner/input tests and UI/build tests. Compile both `esp32dev` **with this usermod included** and `nodemcuv2`. The dedicated example is `pio run -e light_alarm_esp32`.

Optional native memory/undefined-behaviour checks: `LIGHT_ALARM_SANITIZE=1 node --test tools/light-alarm-test.js` with a compiler that supports AddressSanitizer and UBSan.

An optional browser integration check serves compiled assets against mocked endpoints:

```sh
PLAYWRIGHT_MODULE=/path/to/playwright node tools/light-alarm-browser-check.cjs
```

Before relying on the alarm, check on the physical device: overnight locked phone, phone outside the home, temporary Wi-Fi/router loss, invalid preset fallback, actual RGBW channel output/current limiting, stop/snooze, restart during a rise, midnight scheduling and simultaneous browser edits.

### References and attribution

- Calendar conventions: [RFC 3339 §5.7](https://www.rfc-editor.org/rfc/rfc3339#section-5.7).
- Sync timestamp format: [RFC 3339 §5.6](https://www.rfc-editor.org/rfc/rfc3339#section-5.6).
- Sync storage/recovery: [Espressif NVS](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32/api-reference/storage/nvs_flash.html); public boot identifiers: [Espressif hardware RNG](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32/api-reference/system/random.html).
- Brightness curve: [cubic smoothstep](https://en.wikipedia.org/wiki/Smoothstep).
- ICMP sessions: [Espressif ICMP Echo API](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/icmp_echo.html).
- ESP8266 SDK callback contract: [ESP8266Ping](https://github.com/dancol90/ESP8266Ping/blob/master/src/ESP8266Ping.cpp); its blocking wait is not used.
- UDP/ARP idea: [iPhone Detect scanner](https://github.com/mudape/iphonedetect/blob/1643961c21c4f9e42ecf7ab23e5e3a4f2e94cb43/custom_components/iphonedetect/scanner.py). Its Linux `REACHABLE` neighbour lookup is not available on the ESP; this implementation observes actual replies instead.
- Protocol formats: [RFC 6762 §5.5](https://www.rfc-editor.org/rfc/rfc6762#section-5.5), [RFC 826](https://www.rfc-editor.org/rfc/rfc826), [RFC 792](https://www.rfc-editor.org/rfc/rfc792).
- Visual/preset integration follows WLED's `wled00/led.cpp`, `wled00/presets.cpp` and `wled00/FX_fcn.cpp`.
- The new source sections carry AI attribution markers for review.
