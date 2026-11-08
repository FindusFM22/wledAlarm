# Legacy single-wake iPhone Shortcuts

> [!WARNING]
> This generator does **not** produce the current whole-list client. Its **WLED Alarm Sync** and **Clear** templates write the legacy single-wake API and replace a previously imported iPhone list. For a new installation, use the [manual list-sync Shortcut guide](../../usermods/wecker_presence/iphone-shortcut.md). Do not run both client types in automations.

The generated Setup and Status helpers remain useful for one-time profile activation and read-only inspection. They do not switch a stored list back to single-wake mode.

Developer tooling for this fork's [ESP32 iPhone sync API](../../usermods/wecker_presence/readme.md#iphone-next-wake-sync-esp32). No firmware, LED configuration, router configuration, extra iPhone app or cloud service is changed or required at runtime.

## Generate importable files

On a Mac with Apple's Shortcuts app **signed into iCloud**:

```sh
python3 tools/ios-shortcuts/generate.py --sign
```

Signed files are placed in `build_output/ios-shortcuts/` (gitignored). AirDrop these **top-level `.shortcut` files** to the iPhone, or open them from Files/iCloud Drive. The `unsigned/` files are inspectable source plists, not the files to distribute. Signing uses `shortcuts sign --mode anyone`: Apple receives the generic template for validation, without a real device address, alarm data or contact details. This does not create a public iCloud sharing link.

Without `--sign`, Python alone can generate the unsigned source for inspection. No third-party compiler/library is needed. Do not commit personalized or signed outputs.

If Apple reports `In order to do this, you must be signed into iCloud`, generation still leaves all five unsigned templates and a `manifest.json` with `signed: false`. They are **not ready for iPhone import**. Log into iCloud on a signing Mac and rerun the command; do not distribute top-level files left from an older/incomplete signing run. This tool does not log in, request account credentials, or bypass Apple's signature checks.

Alternatively, double-click the generated **`build_output/ios-shortcuts/Sign on Mac.command`** on an iCloud-signed-in Mac. This standalone helper needs only the adjacent `unsigned/` folder and Apple's `shortcuts` command, not Python or the repository. It signs the five explicit templates, opens the package folder when all succeed, and leaves the signed files ready for AirDrop. Its success message is authoritative for that manual run; the generator's manifest is not updated by the standalone helper. The generated `INSTALL.txt` contains the short installation checklist.

The generator also packages explicit outputs into a ZIP: **`WLED Shortcuts - Mac signing kit.zip`** contains the unsigned templates and standalone signing helper for transfer to a signing Mac; it is **not an iPhone installer**. A successful `--sign` run produces **`WLED Shortcuts - iPhone.zip`** with only the signed Shortcuts and installation checklist. Unzip and send/open the individual signed `.shortcut` files on the phone.

| Shortcut | Purpose |
|---|---|
| **WLED Alarm Check** | Read-only selection test on the iPhone; no WLED requests. |
| **WLED Alarm Setup** | Enable the independent profile, retaining its light settings and using its current revision. |
| **WLED Alarm Sync** | Send the next enabled normal Clock alarm whose label contains the exact, case-sensitive substring `WLED`; an explicitly successful empty reading clears only the iPhone slot. **Preview mode by default.** |
| **WLED Alarm Status** | Read the stored slot and readiness/status flags. |
| **WLED Alarm Clear** | Explicit, confirmed manual clear. Stops a running managed iPhone light alarm, not a manual alarm. |

## Device acceptance: iOS 27.2

These templates target native Clock/Shortcuts actions, not Health's sleep schedule. Their workflow structure and mocked behavior can be tested on the development machine; **this is not an iOS 27.2 device test**. Public Clock schemas are not a guarantee that every iOS build exposes the same Shortcuts property names or value representations. Do not enable live synchronization before the following checks pass on the actual iPhone.

1. Import **Check** and run it with an ordinary enabled Clock alarm such as `WLED Work`. Confirm its selected date/time.
2. Add an earlier enabled alarm **without** `WLED`; it must be ignored.
3. Add an earlier **disabled** `WLED` alarm; it must be ignored. Disable all matching alarms; Check must report no enabled `WLED` alarm.
4. Test a passed one-shot time (tomorrow), multiple matching alarms, and a weekly alarm whose next repeat day is not tomorrow. English/German weekday names and standard daily/weekday/weekend summaries are understood; unknown repeat representations abort rather than guess.
5. The actions expect the native Clock properties **Label**, **Is Enabled**, **Time**, and **Repeat**. If an action is unavailable, fields are missing, or the output is wrong, keep Sync's first Number action at **0**. Export a small diagnostic shortcut from the actual phone using **Find Alarms** and the exposed properties so the template can be adapted. Do not fall back to assuming every returned alarm is enabled.
6. Import the other files and enter the same fixed private IPv4 URL in each import question, e.g. `http://192.168.178.32`. Use the actual device address, not that example. These templates intentionally accept only direct RFC 1918 IPv4 HTTP addresses, without ports/proxies/hostnames.
7. Run **Setup** once, then open `http://<wled-ip>/wecker.htm`. Check the master alarm switch, NTP/timezone, light profile and phone-presence configuration. Setup enables only the independent profile and **does not** enable the master switch or disable existing manual alarms.
8. Import/configure **Sync** with **0** initially. After Check passes, change its **first Number action** to **1**, or answer **1** to the live-sync import question. Run it manually and verify `wakeAt`, `startAt`, `enabled`, `masterEnabled` and `clockValid` in **Status**. Verify a real short test alarm before overnight use.

Only one upcoming iPhone occurrence is stored. A new sync is required for the next day; copying an iPhone alarm's weekly recurrence into WLED is deliberately not part of this API. Existing manual WLED alarms continue to run until deliberately disabled; a manual alarm wins an exact start-time tie. Presence gating and overlap rules still apply.

The candidate date is resolved by iOS's native local-date actions, then transmitted with an explicit offset. Nonexistent DST times are rejected by a round-trip check, rather than silently shifted. Repeated-hour behavior must be checked against Clock on the actual phone. Use a Gregorian English/German calendar. The iPhone's acoustic alarm remains the primary wake-up alarm.

## Automation setup on the iPhone

Add triggers to **WLED Alarm Sync** on the phone, after live mode and permissions have been tested:

- **App → Clock → Is Closed**: sync after changing/enabling/disabling/deleting alarms in Clock.
- **Time of Day**, e.g. every evening at 22:00: resend the current next occurrence even if Clock was not opened.
- **Wi-Fi → home network joined** or **Charger → connected**: optional catch-up after returning home or plugging in overnight.
- An **Alarm → Is Stopped** trigger can sync the next occurrence after a normal wake-up, but is **not** an alarm-edited/enabled trigger and is too late to start today's sunrise.

**Running-light consequence:** the existing API cancels a running managed iPhone light alarm when a sync replaces or clears its wake instant. A post-wake sync can therefore shorten its hold/fade period. Omit the optional Alarm-stopped trigger if that light curve should finish uninterrupted; this does not affect a running manual WLED alarm.

Choose automatic/immediate execution where the phone offers it, allow running when locked, and grant local-network/Clock access during a manual run first. Triggers/permissions are configured on the actual phone; this generator does not install personal automations or change iPhone settings. See [Apple's iOS 27 automation guide](https://support.apple.com/guide/shortcuts/apdfbdbd7123/ios). Menu labels may differ by localization/build.

There is no continuous synchronization while Shortcuts is not running. Edits after the last trigger are not magically propagated. Run Sync after Siri/another app edits alarms, or add an appropriate catch-up trigger. Guest Wi-Fi, client isolation, cellular access, off-subnet VPNs and reverse proxies are not supported by this local-subnet API.

## API and failure behavior

Routes are registered in `usermods/wecker_presence/wecker_presence.cpp:120`:

| Method/path | Use |
|---|---|
| `GET /wecker/sync/iphone` | Read status/readiness. |
| `POST /wecker/sync/iphone` | Send **only** `capturedAt` and `wakeAt` (RFC 3339 with timezone, or an explicit JSON `null` for a clear). |
| `GET /wecker/sync/iphone/result?job=...&boot=...` | Follow the receipt's `resultUrl`, including its boot identifier. |
| `GET /wecker/sync/iphone/profile` | Read the independent profile and its revision. |
| `POST /wecker/sync/iphone/profile` | Enable the profile with its current revision and unchanged light settings. |

No token, PIN, login or authorization header is required; the firmware restricts these routes to direct clients in its own IPv4 subnet. Any device on that trusted subnet can modify the slot. Keep the controller isolated from the public internet.

`capturedAt` is captured **before reading Clock**, not after a network wait. The generated payload is created once, with millisecond capture precision and whole-second wake precision. Both POSTs return a queued receipt, **not success**. The Shortcuts follow only the expected same-origin result path, wait one second between at most 12 result reads, and require `confirmed: true`, version 2 and the matching job before reporting durable storage. A save does not guarantee that the light can run (master/profile enablement, presence and storage/clock readiness are separate).

Clock read/parse errors, unknown enabled/repeat values, network failures, firmware errors, expired receipts and poll timeouts never generate a fallback clear. Native HTTP actions may surface non-2xx responses directly as a Shortcuts error. After a timeout/network error, read Status: the previous operation may already be committed. There is no automatic POST retry; a subsequent fresh run reads Clock again and creates a new capture. Never regenerate `capturedAt` merely to retry an old reading.

## Validation and attribution

```sh
python3 -B tools/ios-shortcuts/test_shortcuts.py
npm test
```

The Python tests interpret the generated native action graph with mocked Clock/network actions. They validate selection, ordering, payloads and confirmation/error paths, but cannot certify Apple's property extraction, import compatibility, date parser or locked-phone permissions. Apple signing is a separate packaging check, not a behavioral certification.

- [Apple: import questions](https://support.apple.com/guide/shortcuts/apdf330fd3a0/ios).
- [Apple: command-line signing](https://support.apple.com/guide/shortcuts-mac/apd455c82f02/mac).
- [Apple: Clock alarm schema](https://developer.apple.com/documentation/appintents/appschema/clockentity/alarm.md).
- [Clock Find Alarms action metadata](https://www.shortcutactions.com/actions/find-alarms).
- [Cherri file-format reference, pinned revision](https://github.com/electrikmilk/cherri/tree/5f556958beca7120c6ccfde506825d8218fc773e), MIT license. Used to understand native serialization, not copied as a compiler/runtime dependency.
- [This fork's payload, persistence and subnet contract](../../usermods/wecker_presence/readme.md#iphone-next-wake-sync-esp32).

New source sections include AI attribution markers.
