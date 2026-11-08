# iPhone Clock list-sync Shortcut

This is the recommended client for [WLED Alarm's ESP32 list API](readme.md#iphone-alarm-list-sync-esp32). It only reads Clock and uploads data. WLED chooses the next occurrence and handles weekdays, dates and the light curve.

The instructions use **English Shortcuts action names**. Native property names and permissions can vary by iOS build; verify them on your actual phone. The Health/Sleep schedule is not included.

## Before building

- Use the same trusted home network as WLED and a fixed controller IPv4.
- Configure NTP/timezone, the alarm master switch and the phone's reserved IPv4 in WLED.
- Enable the independent iPhone light profile once through the [profile API](readme.md#profile-setup), or use **WLED Alarm Setup** from the legacy tools. Setup alone does not select single-wake mode; the legacy **Sync** and **Clear** clients do.
- Keep manual alarms enabled only if you also want them to run.

## Build `WLED Alarm List Sync`

Values described as variables below are **blue action-output tokens**, not literal placeholder text.

### 1. Read the list

Add **Find Alarms** (or **Get All Alarms**). Do not filter names, enabled states or the number of results.

Add **Repeat with Each**, input **Alarms**.

### 2. Map four properties inside the repeat

Add **Format Date**:

- Input: **Repeat Item → Time**.
- Date Format: **Custom**.
- Format String: `HH:mm`.

Add **Dictionary**, with four fields of type **Text**:

| Key | Blue variable |
|---|---|
| `label` | Repeat Item → Label |
| `enabled` | Repeat Item → Is Enabled |
| `time` | Formatted Date |
| `repeat` | Repeat Item → Repeat |

Tap a **Repeat Item** token to select its property. `Yes`/`No` and newline-separated weekdays are accepted; no conversion If blocks are needed.

Add **Get Text from Input**, input **Dictionary**. This must be the **last action before End Repeat**. Do not leave a diagnostic `Name: ...` Text action after it: that would become the repeat's output instead of JSON.

### 3. Assemble a JSON array after End Repeat

Add **Combine Text**:

- Input: **Repeat Results**.
- Separator: **Custom**.
- Separator text: `,`.

Add **Text**: type `[`, insert the blue **Combined Text** token, then type `]`.

An example final body is:

```json
[
  {"label":"Work","enabled":"Yes","time":"07:45","repeat":"Monday\nTuesday\nWednesday\nThursday\nFriday"},
  {"label":"Backup","enabled":"No","time":"07:50","repeat":""}
]
```

This is a format example, not a static list to paste instead of the blue variables. Dictionary serialization handles quotes and Unicode labels.

### 4. Upload the array

Add **Get Contents of URL**:

- URL: `http://<wled-ip>/wecker/sync/iphone/alarms`.
- Method: **POST**.
- Header: `Content-Type` = `application/json`.
- Request Body: **File**.
- File: the **Text output containing the outer brackets**, not bare Combined Text.

No physical file needs to be saved. A JSON-body editor can alternatively send `{"alarms":[...]}` if it supports inserting the complete list as an array variable.

## Confirm the save

The first POST reply is a queued receipt:

```json
{"job":7,"confirmed":false,"resultUrl":"/wecker/sync/iphone/result?job=7&boot=1234567890abcdef"}
```

To see the final result, append:

1. **Get Dictionary Value**: key `resultUrl`, input **Contents of URL**.
2. **Text**: your fixed `http://<wled-ip>` followed directly by the blue **Dictionary Value** token.
3. **Wait**: one second.
4. **Get Contents of URL**: **GET** on that Text URL.

Only `confirmed:true`, HTTP 200 and an empty `error` confirm durable storage. If the operation is still pending, repeat only the GET; a one-second wait is not a guarantee of completion. Preserve the returned `boot` parameter and follow only the controller's expected `/wecker/sync/iphone/result?...` path on the same host.

After an uncertain result, inspect WLED status. Do not retry an old bare-list POST as though it were a fresh reading; re-read Clock first. Never send an empty list as fallback error handling.

## Verify on the phone

1. Run manually and approve any Clock/local-network permissions.
2. Open `/wecker.htm`. **iPhone alarms** should show all uploaded alarms, with the correct times and enabled/disabled states.
3. Check `/wecker/sync/iphone`: `mode:"list"`, the expected count and next wake, `enabled:true`, `masterEnabled:true`, `clockValid:true`, and no storage error.
4. Test one enabled and one disabled alarm, an ordinary unlabelled alarm, repeat weekdays and a non-repeating alarm. WLED uses its own timezone; match it to the phone for home use.
5. Test a short real alarm and locked-phone execution before overnight use. Keep the acoustic iPhone alarm enabled.

If **Is Enabled** or **Repeat** is unavailable, stop and inspect the actual exposed properties. Do not assume every returned alarm is enabled. An explicit successful empty list clears the managed iPhone list; a Clock access/parse failure must not do so.

## Automate changes, not date calculations

Create a personal automation:

**Automation → App → Clock → Is Closed → Run Immediately → WLED Alarm List Sync**.

Older builds may use **Ask Before Running** instead. Permissions and personal automations must be configured on the actual phone.

Stored weekly alarms advance automatically on WLED. You only need another upload when Clock data changes; an optional home-Wi-Fi/charger trigger can catch up. Siri or another app changing alarms does not necessarily close Clock, so sync explicitly after those edits.

> [!WARNING]
> Disable automations using the legacy **WLED Alarm Sync** or **Clear** templates. Their successful single-wake writes replace list mode. The read-only Status and one-time Setup helpers do not replace the list.

For device presence, overlapping alarms, restart behavior and known edge cases, see the [alarm documentation](readme.md).
