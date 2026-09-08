# ESPMaster HTTP API

Reference for every HTTP endpoint ESPMaster exposes, against the current `/api/`-unified
endpoint layout.

## Conventions

- **Base URL**: `http://<device-ip>/` (mDNS/hostname not currently configured - find the IP via
  your router's DHCP client list, or the serial log at boot).
- **Auth**: most endpoints are open on the local network (no login system on this device). A
  handful of state-changing endpoints require the OTA password via an `X-OTA-Password` header,
  noted individually below - see [Authentication](#authentication).
- **Request bodies**: `POST` endpoints take `application/x-www-form-urlencoded` bodies (i.e.
  `key=value&key2=value2`, the same as an HTML form submit) unless noted as multipart file upload.
- **Responses**: format varies per endpoint (JSON, plain text, or HTML) - called out individually.
  `/api/diag`'s `heap`/`tcp` checks are deliberately plain text rather than JSON to avoid any
  allocation on a heap-constrained device; see that entry.
- **CORS**: not configured - calls from a page served by a different origin will be blocked by
  the browser. Fine for `curl`/scripts/same-origin use, not for a third-party web page.
- All endpoints below are on this ESP8266 build's default flash layout (`eesz=1M128`); nothing
  here is affected by that choice, it's just how this device happens to be built (see
  `firmware/README.md`).
- **curl examples**: every endpoint below has one, using `192.168.1.50` as a placeholder for the
  device's actual IP - substitute your own throughout.

## Authentication

If an OTA password is set (see [`GET/POST /api/ota-password`](#get-apiota-password)), these
endpoints require it via an `X-OTA-Password: <password>` request header:

- `POST /api/web-file`
- `POST /api/units/firmware`
- `POST /api/units/firmware-push`
- `POST /api/firmware-start`
- `POST /api/firmware-chunk`
- `POST /api/firmware-finish`

If no password is set (empty string), these endpoints accept requests unauthenticated. Every
other endpoint below has no auth check at all, including `GET /api/ota-password` itself
(deliberate design choice - this device has no separate login system, see the comment in
`ESPMaster.ino` on that endpoint).

## Endpoint index

| Method | Path | Purpose |
| --- | --- | --- |
| GET | [`/`](#get-) | Main web UI page |
| GET | [`/api/settings`](#get-apisettings) | Current device settings |
| POST | [`/api/settings`](#post-apisettings) | Update alignment/mode/text/speed/schedule/countdown |
| POST | [`/api/text`](#post-apitext) | Lightweight text push (automations/webhooks) |
| GET | [`/api/diag?type=`](#get-apidiag) | Diagnostics: `health`\|`heap`\|`tcp`\|`test` |
| GET | [`/api/log`](#get-apilog) | Recent in-memory serial log |
| GET | [`/api/crash-log`](#get-apicrash-log) | Persisted on-device crash log |
| DELETE | [`/api/crash-log`](#delete-apicrash-log) | Clear the crash log |
| GET | [`/api/i2c-scan`](#get-apii2c-scan) | Raw I2C bus scan (legacy diagnostic) |
| GET | [`/api/units`](#get-apiunits) | Live status + offset for every unit |
| POST | [`/api/units/rescan`](#post-apiunitsrescan) | Re-run the I2C unit scan |
| POST | [`/api/units/offset`](#post-apiunitsoffset) | Set a unit's calibration offset |
| POST | [`/api/units/reset`](#post-apiunitsreset) | Recalibrate every unit |
| POST | [`/api/web-file`](#post-apiweb-file) | Replace `index.html` in place |
| POST | [`/api/units/firmware`](#post-apiunitsfirmware) | Upload a Unit firmware image |
| POST | [`/api/units/firmware-push`](#post-apiunitsfirmware-push) | Push uploaded firmware to a unit over I2C |
| POST | [`/api/units/bootloader-probe`](#post-apiunitsbootloader-probe) | Diagnostic bootloader probe, writes nothing |
| POST | [`/api/firmware-start`](#post-apifirmware-start) | Begin a browser-based ESPMaster firmware update |
| POST | [`/api/firmware-chunk`](#post-apifirmware-chunk) | Upload one chunk of the firmware |
| POST | [`/api/firmware-finish`](#post-apifirmware-finish) | Finalize the update, reboot on success |
| POST | [`/api/debug-mode`](#post-apidebug-mode) | Enable/disable debug mode |
| POST | [`/api/reboot`](#post-apireboot) | Reboot the device |
| DELETE | [`/api/scheduled-messages`](#delete-apischeduled-messages) | Remove a scheduled message |
| POST | [`/api/ota-mode`](#post-apiota-mode) | Start Arduino-IDE OTA mode |
| POST | [`/api/wifi/reset`](#post-apiwifireset) | Erase WiFi config, reboot into AP setup |
| GET | [`/api/ota-password`](#get-apiota-password) | Read the current OTA password |
| POST | [`/api/ota-password`](#post-apiota-password) | Change the OTA password |
| GET | [`/api/timezone`](#get-apitimezone) | Read the current timezone |
| POST | [`/api/timezone`](#post-apitimezone) | Change the timezone |

---

### `GET /`

Serves the web UI (`index.html` from LittleFS) - the single-page app covering both the main
clock view and the Administration view. Not a data endpoint; response is `text/html`.

If `DEBUG_ENABLE` is compiled in (it is, by default) and debug mode is currently on (see
[`POST /api/debug-mode`](#post-apidebug-mode)), this instead serves a minimal startup-debug HTML
page showing the live boot log, with a button that calls `POST /api/debug-mode` (`enabled=false`)
to return to the normal page.

```bash
curl http://192.168.1.50/
```

---

### `GET /api/settings`

Current device state - the main page's own settings and the source of truth for alignment,
speed, mode, WiFi status, etc.

**Response** `200 application/json`:
```json
{
  "timezoneOffset": 420,
  "unitCount": 3,
  "alignment": "left",
  "flapSpeed": "80",
  "deviceMode": "text",
  "version": "2.3.0-Scott",
  "lastTimeReceivedMessageDateTime": "16 Aug 26 09:12:04",
  "lastWrittenText": "",
  "countdownToDateUnix": 0,
  "wifiStatus": "Connected",
  "wifiRssi": -69,
  "wifiIp": "192.168.1.51",
  "scheduledMessages": [
    { "scheduledDateTimeUnix": 1755331200, "message": "Hello", "showIndefinitely": false }
  ],
  "wifiSettingsResettable": true,
  "otaEnabled": false,
  "otaPassword": "splitflap",
  "timezone": "Europe/Berlin"
}
```
`otaPassword`/`timezone` duplicate `GET /api/ota-password`/`GET /api/timezone` (same values, same
unauthenticated-by-design reasoning) - folded in here so the Administration view can populate its
displays from this one already-fetched response instead of two more separate requests.
`otaEnabled` reflects the `OTA_ENABLE` compile flag (currently `false` in this build - see
[`POST /api/ota-mode`](#post-apiota-mode)). `wifiSettingsResettable` reflects `WIFI_USE_DIRECT`
being off (AP-config mode).

```bash
curl http://192.168.1.50/api/settings
```

---

### `POST /api/settings`

Updates one or more device settings in a single request - this is what the main page's form
submits. Same resource as the `GET` above, different method.

**Body params** (all optional - only send what changed):

| Param | Values | Notes |
| --- | --- | --- |
| `alignment` | `left` \| `center` \| `right` | |
| `deviceMode` | `text` \| `clock` \| `date` \| `countdown` | |
| `flapSpeed` | `1`-`100` | Validated server-side even though the UI slider already clamps this |
| `inputText` | up to 500 chars | Only takes effect if `deviceMode` is (or becomes) `text` |
| `scheduleEnabled` | `on` | If present, adds a **new** scheduled message instead of changing `deviceMode`/`inputText` directly - see below |
| `scheduleShowIndefinitely` | `on` | Only relevant when `scheduleEnabled=on` |
| `scheduledDateTimeUnix` | unix timestamp | When to show the scheduled message |
| `countdownDateTimeUnix` | unix timestamp | Countdown target, only relevant for `deviceMode=countdown` |

When `scheduleEnabled=on` is present, this call adds a new entry to the scheduled-message queue
(via `inputText` + `scheduledDateTimeUnix` + `scheduleShowIndefinitely`) rather than changing the
live display immediately - remove one later via
[`DELETE /api/scheduled-messages`](#delete-apischeduled-messages).

**Response:**
- `200 text/plain` `"OK"` on success.
- `400 text/plain` with a human-readable error on validation failure (e.g. an invalid
  `alignment`/`deviceMode` value, `flapSpeed` out of `1`-`100`, `inputText` over 500 chars, or a
  non-numeric date field).

```bash
curl -X POST http://192.168.1.50/api/settings \
  --data "deviceMode=text&inputText=HELLO&alignment=center&flapSpeed=80"
```

---

### `POST /api/text`

Lightweight alternative to `POST /api/settings` for automations (webhooks, Home Assistant, phone
shortcuts, ...) that just want to show a message - one required param, one optional param, instead
of the full settings form. Same unauthenticated design as `/api/settings` above.

**Body params:**

| Param | Values | Notes |
| --- | --- | --- |
| `text` | up to 500 chars | Required. |
| `duration` | `0`-`3600` (seconds) | Optional, default `0`. |

- `duration` omitted or `0`: **permanent** - switches the display into Text mode and persists that
  mode change, same net effect as `POST /api/settings` with `deviceMode=text&inputText=<text>`.
- `duration` > `0`: **temporary** - shows the message immediately for that many seconds, then
  automatically reverts to whatever the display was already showing (current `deviceMode` is left
  untouched entirely) - the same "hold, then let the normal per-second mode loop take back over"
  behavior a non-indefinite scheduled message already uses.

**Response:**
- `200 text/plain` `"OK"` on success.
- `400 text/plain` if `text` is missing/over 500 chars, or `duration` is missing a number, negative,
  or over `3600`.

```bash
# Permanent - switches into Text mode and stays
curl -X POST http://192.168.1.50/api/text --data "text=HELLO WORLD"

# Temporary - shows for 10s, then reverts to whatever was showing before
curl -X POST http://192.168.1.50/api/text --data "text=BRB&duration=10"
```

---

### `GET /api/diag`

Four pure-diagnostic checks sharing one registered route, picked via `?type=` - each
`webServer.on()` registration costs a small permanent heap allocation that's never freed, and this
device has very little room to spare, so these were consolidated rather than left as four separate
endpoints (formerly `/api/health`, `/api/heap`, `/api/tcp-status`, `/api/test`).

**Query param** `type` (required): `health` | `heap` | `tcp` | `test`.

**`type=health`** - trivial liveness check.
**Response** `200 text/plain`: `Healthy`

**`type=heap`** - free-heap diagnostic, added while chasing a real out-of-memory crash.
**Deliberately plain text, not JSON** - zero allocation risk of its own (stack-only `char`
buffer), which is the entire point on a heap-constrained device.
**Response** `200 text/plain`, e.g.: `free=16480 frag=1% maxBlock=16368 ...`
- `free` - bytes currently free on the heap.
- `frag` - heap fragmentation percentage (`ESP.getHeapFragmentation()`).
- `maxBlock` - largest single contiguous free block available (bytes).

**`type=tcp`** - live TCP connection-pool diagnostic (this ESP8266's lwIP hard-caps at 5
simultaneous TCP connections). Same "plain text, zero allocation" reasoning as `heap` above.
**Response** `200 text/plain`, e.g.: `active=1/5 timeWait=0/5 bound=0 listen=1`
- `active` - currently active (open, in-flight) TCP connections, out of the hard limit of 5.
- `timeWait` - connections in TCP TIME-WAIT after closing, out of a *separate* 5-slot pool (does
  not compete with `active` for headroom).
- `bound` - bound-but-not-yet-connected PCBs (normally 0).
- `listen` - listening sockets (normally 1 - the web server itself).

**`type=test`** - minimal connectivity/latency check, predates most other diagnostics on this
list.
**Response** `200 application/json`: `{ "status": "ok", "time": 123456 }` - `time` is `millis()`
(milliseconds since boot), not a wall-clock timestamp.

`400 text/plain` if `type` is missing or not one of the four values above.

```bash
curl "http://192.168.1.50/api/diag?type=health"
curl "http://192.168.1.50/api/diag?type=heap"
curl "http://192.168.1.50/api/diag?type=tcp"
curl "http://192.168.1.50/api/diag?type=test"
```

---

### `GET /api/log`

Last up to 50 entries from the in-memory serial log ring buffer.

**Response** `200 application/json`:
```json
{
  "count": 214,
  "logs": [
    { "message": "DEBUG: Status = WiFi Connected", "timestamp": 4821 }
  ]
}
```
`count` is the *total* number of log lines ever recorded this boot (not just how many are
returned); `timestamp` is milliseconds since boot.

Returns `503 text/plain` instead ("Too little free heap right now to safely build the log
response - try again shortly") if free heap is below 4000 bytes at request time, rather than
risking an allocation failure.

```bash
curl http://192.168.1.50/api/log
```

---

### `GET /api/crash-log`

The persisted on-device crash log - survives even if a live serial connection wasn't available at
the moment of the crash. One JSON object per line (JSON Lines format), oldest first.

**Response** `200 application/jsonl` - raw file content, or an empty body if no crash log exists
yet.

```bash
curl http://192.168.1.50/api/crash-log
```

---

### `DELETE /api/crash-log`

Clears the persisted crash log file.

**Response** `200 text/plain`: `Crash log cleared`

```bash
curl -X DELETE http://192.168.1.50/api/crash-log
```

---

### `GET /api/i2c-scan`

Legacy raw I2C bus scanner - pings every address 0-15 directly (not limited to addresses already
known as units) and reports what answered. Mostly superseded by
[`GET /api/units`](#get-apiunits) for normal use; still useful for diagnosing address/wiring
issues at a lower level. Response is deferred to `loop()` (real I2C traffic never runs inside the
AsyncWebServer callback itself), so it can take a moment.

**Response** `200 application/json`:
```json
{
  "scanTime": 48213,
  "devices": [
    { "address": 0, "status": 0, "statusText": "ready" },
    { "address": 1, "status": 1, "statusText": "busy" }
  ],
  "foundCount": 2,
  "expectedCount": 3
}
```
`expectedCount` is the currently-known `unitsAmount` from the last full scan (see
[`POST /api/units/rescan`](#post-apiunitsrescan)) - a mismatch against `foundCount` suggests a
wiring/address gap. Returns `503 text/plain` if another scan is already in progress.

```bash
curl http://192.168.1.50/api/i2c-scan
```

---

### `GET /api/units`

Live status and calibration offset for every known unit - what the Administration view's Units
card polls. Deferred to `loop()`, same reasoning as `/api/i2c-scan` above. Uses the lightweight
3-byte I2C read (status + offset only) - about half the I2C interrupt cost per unit versus the
6-byte read internal display logic uses, since this endpoint's response was never used for
anything beyond status/offset.

**Query param** `detail` (any value, e.g. `?detail=1`): opts into the heavier 6-byte read instead,
adding `phase`/`progress` to each unit - added as a diagnostic for
watching a Unit that starts rotating with no ESPMaster-issued display job involved (the normal
display-job wait logic's phase/progress logging never runs in that case). Not used by the
Administration page itself - pass it manually (e.g. `curl`) when actively diagnosing.

**Response** `200 application/json`:
```json
{
  "unitsAmount": 3,
  "units": [
    { "index": 0, "online": true, "status": 0, "offset": 100 },
    { "index": 1, "online": true, "status": 1, "offset": 95, "phase": 2, "progress": 340 }
  ]
}
```
- `status`: `0` = ready, `1` = busy/moving.
- `offset` / `status` are `-1` / `-2` respectively for a unit that didn't respond (`online: false`).
- `phase`/`progress` only present with `?detail=1`. `phase`: `0` = idle, `1` = calibrating
  (searching for the hall sensor marker), `2` = rotating to a target letter. `progress`: steps
  done so far in the current phase - watch whether this climbs steadily (points at a mechanical/
  torque issue) or keeps resetting toward 0 (points at the Unit itself repeatedly restarting, e.g.
  from a brown-out reset).

Returns `503 text/plain` if another units request is already in progress.

```bash
curl http://192.168.1.50/api/units

# with phase/progress included:
curl "http://192.168.1.50/api/units?detail=1"
```

---

### `POST /api/units/rescan`

Re-runs the full I2C scan to pick up units connected or fixed after boot, without a reboot.
Updates the device-wide `unitsAmount` used everywhere else. Deferred to `loop()`.

**Response** `200 application/json`:
```json
{ "unitsAmount": 3 }
```
Returns `503 text/plain` if another rescan is already in progress.

```bash
curl -X POST http://192.168.1.50/api/units/rescan
```

---

### `POST /api/units/offset`

Sets a new calibration offset for a single unit; the unit recalibrates (a full rotation to
re-home) immediately.

**Body params:**

| Param | Type | Notes |
| --- | --- | --- |
| `id` | integer | Unit index, `0` to `unitsAmount - 1` |
| `offset` | integer | `0` to `UNIT_STEPS - 1` (2037 on this hardware - 28BYJ-48 stepper steps/revolution) |

**Response:**
- `202 text/plain`: `"Offset sent. Unit is recalibrating now, this can take a few seconds."`
- `400 text/plain` if `id`/`offset` are missing, non-numeric, or out of range.

```bash
curl -X POST http://192.168.1.50/api/units/offset --data "id=0&offset=100"
```

---

### `POST /api/units/reset`

Triggers a full recalibration of every known unit (picked up on the next `loop()` iteration).

**Response** `200 text/plain`: `"Units are resetting/recalibrating now."`

```bash
curl -X POST http://192.168.1.50/api/units/reset
```

---

### `POST /api/web-file`

Replaces `index.html.gz` in place on the device's LittleFS, without touching any other file - lets
the web UI be updated without a full filesystem reflash (which would wipe runtime settings). The
whole UI is one gzip-compressed file (`ESPAsyncWebServer` serves it with `Content-Encoding: gzip`
automatically), so `index.html.gz` is the *only* accepted filename.

**Auth:** requires `X-OTA-Password` header if a password is set.

**Request:** `multipart/form-data` file upload; the uploaded file's name must be exactly
`index.html.gz`.

**Response:**
- `200 text/plain`: `"OK - file updated"`
- `400 text/plain` if the filename isn't `index.html.gz`.
- `401 text/plain` if the password is wrong.

> ⚠️ No filesystem-space check happens before writing - a full partition can silently produce a
> 0-byte file. Verify byte-for-byte after uploading (`GET /` and compare length/content against
> the source).

```bash
gzip -9 -k -c index.html > index.html.gz
curl -X POST http://192.168.1.50/api/web-file \
  -H "X-OTA-Password: splitflap" \
  -F "file=@index.html.gz;filename=index.html.gz;type=application/gzip"
```

---

### `POST /api/units/firmware`

Uploads a compiled `Unit.ino` firmware image (raw AVR flash `.bin`, **not** the ESPMaster's own
firmware - see `Unit/BOOTLOADER_SETUP.md`) to be pushed to a unit afterwards. Stores it, doesn't
push it yet.

**Auth:** requires `X-OTA-Password` header if a password is set.

**Request:** `multipart/form-data`, field name `firmware`.

**Response:**
- `200 text/plain`: `"OK - firmware stored, ready to push to a unit"`
- `401 text/plain` if the password is wrong.

```bash
curl -X POST http://192.168.1.50/api/units/firmware \
  -H "X-OTA-Password: splitflap" \
  -F "firmware=@Unit.bin"
```

---

### `POST /api/units/firmware-push`

Pushes the firmware previously uploaded via `/api/units/firmware` to one unit over I2C (twiboot
protocol). Deferred to `loop()` - a real push can take several seconds to a couple minutes.

**Renamed 2026-08-22** from `/api/units/firmware/push` - that path was a strict prefix-extension
of the upload endpoint above and got silently intercepted by it (ESPAsyncWebServer's route
matching for the two-callback upload form), making it completely unreachable. Always use a
sibling path, never a sub-path of an upload endpoint's URL.

**Auth:** requires `X-OTA-Password` header if a password is set.

**Body params:**

| Param | Type | Notes |
| --- | --- | --- |
| `id` | integer | Target unit index |

**Response** `200` (success) or `500` (failure) `application/json`:
```json
{
  "unitIndex": 0,
  "bootAddress": "0x29",
  "pageSize": 128,
  "totalPages": 42,
  "pagesWritten": 42,
  "success": true,
  "started": true,
  "message": "Firmware written and verified. Unit is starting the new application now."
}
```
On failure, `success: false` and an `error` field instead of `message`/`started` (and possibly
missing `bootAddress`/`pageSize`/`totalPages`/`pagesWritten` if the failure happened before the
unit was even found, e.g. no unit answered in bootloader mode). The critical first flash page
(interrupt vector table) is always written last and only after every other page is verified - a
failed push leaves the old firmware fully intact. `400` if `id` is missing/invalid/out of range
or no firmware has been uploaded yet; `401` on a wrong password; `503` if another push is already
in progress.

```bash
curl -X POST http://192.168.1.50/api/units/firmware-push \
  -H "X-OTA-Password: splitflap" \
  --data "id=0"
```

---

### `POST /api/units/bootloader-probe`

Diagnostic-only: sends a unit into bootloader mode and reports which I2C address it answers on
and its chip info, **without writing anything to flash**. Used to verify the DIP-jumper address
is read live by twiboot. Deferred to `loop()`.

**Body params:**

| Param | Type | Notes |
| --- | --- | --- |
| `id` | integer | Target unit index |

**Response** `200 application/json` on success:
```json
{
  "unitIndex": 0,
  "success": true,
  "bootAddress": 41,
  "bootAddressHex": "0x29",
  "addressOffsetFromBase": 0,
  "signature": "1E 95 0F",
  "pageSize": 128,
  "bootloaderStart": 32256,
  "returnedToApp": true,
  "note": "Nothing was written. The unit was explicitly told to return to its app."
}
```
or `{"unitIndex":N,"success":false,"error":"..."}` if no unit (or more than one) answered in
bootloader mode. `400` if `id` is missing/invalid/out of range; `503` if another probe is already
in progress.

```bash
curl -X POST http://192.168.1.50/api/units/bootloader-probe --data "id=0"
```

---

### `POST /api/firmware-start`

Begins a browser-based ESPMaster firmware update (a compiled `.bin` sketch, no Arduino IDE
required) - three-step, chunked protocol. See `POST /api/firmware-chunk` below for why this
replaced a single-request upload.

**Auth:** requires `X-OTA-Password` header if a password is set.

**Body params:** `size` (integer, total firmware size in bytes).

**Response:**
- `200 text/plain`: `"OK - ready for chunks"`
- `400 text/plain` if `size` is missing/zero.
- `401 text/plain` if the password is wrong.
- `500 text/plain`: `"Failed to start update, check the serial log for details"`
- `503 text/plain` if another firmware upload is already in progress.

```bash
SIZE=$(stat -c%s ESPMaster.ino.bin)
curl -X POST http://192.168.1.50/api/firmware-start \
  -H "X-OTA-Password: splitflap" \
  --data "size=$SIZE"
```

---

### `POST /api/firmware-chunk`

Uploads one chunk of the firmware previously announced via `/api/firmware-start`. Call
repeatedly, once per chunk, **waiting for each response before sending the next chunk** - that
wait is the whole point: the ESP8266 has no way to slow down incoming TCP data on its own, so an
upload sent as fast as the network allows can outrun the flash-write speed no matter how large a
receive buffer is used. Waiting for the response to each chunk lets the request/response
round-trip itself provide backpressure.

**Auth:** requires `X-OTA-Password` header if a password is set.

**Request:** raw binary body, at most 4096 bytes (`OTA_CHUNK_SIZE` in `ESPMaster.ino` - keep the
uploader's chunk size in sync with this).

**Response:**
- `200 text/plain`: `"OK"`
- `400 text/plain` if the chunk is oversized, or no upload is in progress.
- `401 text/plain` if the password is wrong.
- `500 text/plain`: `"Chunk write failed, check the serial log for details"` (aborts the whole
  upload - old firmware stays active; call `/api/firmware-start` again to retry from scratch).

An upload with no chunk/finish activity for 30s is treated as abandoned and cleaned up
automatically (old firmware stays active).

```bash
# One chunk, previously split with e.g. `split -b 4096 ESPMaster.ino.bin chunk_`
curl -X POST http://192.168.1.50/api/firmware-chunk \
  -H "X-OTA-Password: splitflap" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @chunk_aa
```

---

### `POST /api/firmware-finish`

Finalizes an upload after all chunks have been sent successfully. Reboots the device
automatically on success.

**Auth:** requires `X-OTA-Password` header if a password is set.

**Request:** no body needed.

**Response:**
- `200 text/plain`: `"OK - Firmware written, rebooting now"`
- `400 text/plain` if no upload is in progress.
- `401 text/plain` if the password is wrong.
- `500 text/plain`: `"Update failed, check the serial log for details"`

All three endpoints above are only present if `WEB_FIRMWARE_UPDATE_ENABLE` is compiled in (it is,
by default).

```bash
curl -X POST http://192.168.1.50/api/firmware-finish -H "X-OTA-Password: splitflap"
```

---

### `POST /api/debug-mode`

Enables or disables debug mode (startup debug page, error status panel, page-load debug log,
serial debug log at the bottom of the main page). Persisted across reboots.

**Body params:**

| Param | Values |
| --- | --- |
| `enabled` | `true` \| `false` |

**Response** `200 text/plain` - a human-readable confirmation message describing what changed.
`400 text/plain` if `enabled` is missing.

Only present if `DEBUG_ENABLE` is compiled in (it is, by default). Replaces the former
`GET /exit-debug-mode` / `GET /enable-debug-mode` - those were a CSRF-via-GET gap, since closed.

```bash
curl -X POST http://192.168.1.50/api/debug-mode --data "enabled=true"
```

---

### `POST /api/reboot`

Reboots the device (actual reboot happens on the next `loop()` iteration, ~a few hundred ms
later).

**Response** `200 text/html` - a small human-readable "rebooting..." page with a link back home
(meant for a real browser form submission, not programmatic use, though the body content doesn't
matter if you only care about the reboot itself).

```bash
curl -X POST http://192.168.1.50/api/reboot
```

---

### `DELETE /api/scheduled-messages`

Removes one scheduled message.

**Query param:**

| Param | Type |
| --- | --- |
| `id` | The message's `scheduledDateTimeUnix`, as returned in `GET /api/settings`'s `scheduledMessages[]` |

Example: `DELETE /api/scheduled-messages?id=1755331200`

**Response:**
- `202 text/plain`: `"Removed"`
- `400 text/plain` if `id` is missing, non-numeric, or doesn't match any scheduled message.

```bash
curl -X DELETE "http://192.168.1.50/api/scheduled-messages?id=1755331200"
```

---

### `POST /api/ota-mode`

Starts `ArduinoOTA` mode, letting the Arduino IDE flash this device over WiFi (select the new
network port in the IDE's Tools menu, upload as normal). The device reboots automatically once
the IDE upload finishes.

**Response** `200 text/html` - an explanatory page (meant for real navigation).

Only present if `OTA_ENABLE` is compiled in - **currently `false` in this build**, so this
endpoint does not exist right now despite the Administration view's "OTA Update" button still
being wired to call it (a pre-existing inconsistency, not introduced by the Punkt 74 rename -
worth a look in a future session).

```bash
curl -X POST http://192.168.1.50/api/ota-mode
```

---

### `POST /api/wifi/reset`

Erases the stored WiFi configuration and reboots into AP setup mode (connect to the device's own
`Split-Flap-AP` network to reconfigure).

**Response** `200 text/html` - an explanatory page (meant for real navigation).

Only present if `WIFI_USE_DIRECT` is `false` (it is, by default - this device uses WiFiManager's
AP-config flow rather than hardcoded credentials).

```bash
curl -X POST http://192.168.1.50/api/wifi/reset
```

---

### `GET /api/ota-password`

Returns the currently-configured OTA password. **Unauthenticated by design** - this device has no
separate login system, and every other admin action already checks this same shared password
per-request, so hiding it behind itself wouldn't add real protection.

**Response** `200 application/json`:
```json
{ "otaPassword": "splitflap" }
```

```bash
curl http://192.168.1.50/api/ota-password
```

---

### `POST /api/ota-password`

Changes the OTA password. Same resource as the `GET` above.

**Body params:**

| Param | Notes |
| --- | --- |
| `currentPassword` | Must match the current password |
| `newPassword` | Must be non-empty |

**Response:**
- `200 text/plain`: `"OTA password changed"`
- `400 text/plain` if either param is missing or `newPassword` is empty.
- `401 text/plain` if `currentPassword` doesn't match.

```bash
curl -X POST http://192.168.1.50/api/ota-password \
  --data "currentPassword=splitflap&newPassword=newSecret123"
```

---

### `GET /api/timezone`

Returns the currently-configured timezone (TZ database name).

**Response** `200 application/json`:
```json
{ "timezone": "Europe/Berlin" }
```

```bash
curl http://192.168.1.50/api/timezone
```

---

### `POST /api/timezone`

Changes the timezone. Same resource as the `GET` above.

**Body params:**

| Param | Notes |
| --- | --- |
| `timezone` | TZ database name (e.g. `Europe/Berlin`), must be non-empty and resolvable |

**Response:**
- `200 text/plain`: `"Timezone changed"`
- `400 text/plain` if `timezone` is missing/empty, or not recognized by ezTime's timezoned lookup
  (this call needs network access - it queries ezTime's timezone service live, it doesn't just
  parse the string locally).

```bash
curl -X POST http://192.168.1.50/api/timezone --data "timezone=Europe/Berlin"
```
