# Blesploit HTTP & WebSocket API

Base URL: `http://<device-ip>/` (HTTP port is the ESP-IDF default unless changed in firmware).  
When connected over **USB CDC-NCM**, the device is also reachable at **`http://192.168.5.1/`** (DHCP assigns the host `192.168.5.x`). WiFi AP/STA continues to work in parallel (`192.168.4.1` in AP mode, etc.).  
All JSON bodies use `Content-Type: application/json` where applicable.

**Console:** composite USB exposes **CDC-ACM** (serial REPL) and **CDC-NCM** (Ethernet). See **[Serial console](#serial-console)** below. Use the ACM port for `idf.py monitor` and console commands (`wifi`, `version`, …).

**Mobile clients** should use the **WebSocket API** for BLE control, file I/O, and **Wi‑Fi configuration** (`type`: `wifi`). REST JSON endpoints remain for **browser** compatibility (device editor, Wi‑Fi index page, file manager HTML). Device JSON on mobile is read/written via **`fs` whole-file transfer** (`read_start` / `write_start`). The browser Wi‑Fi modal uses **REST** `GET/POST /api/wifi`; serial **CDC-ACM** also accepts `wifi` console commands and `ws '{...}'` JSON injection.

---

## REST

### Pages (HTML)

| Method | Path | Description |
|--------|------|-------------|
| GET | `/` | Index |
| GET | `/devices` | Device list UI |
| GET | `/device/*` | Device editor UI (`/device/<folder>`) |
| GET | `/sim/*` | Simulation UI |
| GET | `/central/*` | Central UI |
| GET | `/scan` | Scanner UI |
| GET | `/fs` | File manager UI |

### JSON / config

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/devices` | JSON array of devices (manifest metadata + `folder`) |
| GET | `/api/device/<id>/<resource>` | Load JSON; `resource` is manifest key or alias (`info`, `manifest`, `adv`, `ble`, `interface`, …). Optional `.json` suffix is stripped. |
| PATCH | `/api/device/<id>/<resource>` | Partial update of that JSON file (editor / manifest-resolved paths). |

### Wi‑Fi (stored config; reboot required to apply runtime changes)

NVS key **`enabled`** controls whether **WiFi** starts at boot (`wifi -f` / `disable` turns WiFi off; **USB NCM stays up**). Passwords are never returned on GET or WS queries.

| Method | Path | Body (JSON, partial OK) | Response |
|--------|------|---------------------------|----------|
| GET | `/api/wifi` | — | `{ "config": {…}, "status": {…} }` — see **Wi‑Fi config / status objects** below |
| POST | `/api/wifi` | `ssid`, `psk`, `mode`, `ap_ssid`, `ap_psk` (all optional) | `ok`, optional `error` / `note` (`note`: reboot to apply). Saving both STA `ssid` and non-empty `psk` sets `enabled` true. No `reboot` flag on REST. |

**`config` object (NVS):**

| Field | Type | Notes |
|-------|------|--------|
| `enabled` | bool | WiFi enabled at next boot |
| `mode` | string | `sta_first` or `ap_only` |
| `ssid` | string | STA SSID (may be empty) |
| `has_psk` | bool | STA password stored (value not returned) |
| `has_sta_creds` | bool | Both STA ssid and psk present |
| `ap_ssid` | string | Custom soft-AP SSID; empty → MAC-based default |
| `ap_ssid_effective` | string | SSID AP mode would broadcast |
| `has_ap_psk` | bool | Custom AP password stored |

**`status` object (runtime, read-only):**

| Field | Type | Notes |
|-------|------|--------|
| `wifi_running` | bool | WiFi driver initialized and mode not null |
| `active_mode` | string | `sta`, `ap`, or `off` |
| `current_ip` | string | WiFi only: STA → AP → `(not connected)` (USB is in `usb.ip`) |
| `primary_iface` | string \| null | `WIFI_STA_DEF`, `WIFI_AP_DEF`, or null (not USB) |
| `sta` | object | `up`, `connected`, `ssid`, `ip`, `rssi` (when connected) |
| `ap` | object | `up`, `ssid_effective`, `ip`, `clients` |
| `usb` | object | `up`, `ip`, `link_up` |
| `http_server` | bool | HTTP/WS server started |
| `pending_reboot` | bool | Always `false` (apply-after-reboot is implicit) |

### Log filter — wslog (controls WebSocket `log` stream)

Firmware uses **`WS_LOGI` / `WS_LOGW` / `WS_LOGE`** (`web_server.h`) so lines also go through `wslog_send()` and may appear as WebSocket messages. Plain **`ESP_LOGx`** calls are UART-only unless bridged elsewhere.

Filtering applies before broadcast.

**Defaults:** `enabled` true, `info_enabled` false, `allowed_info_tags` empty.

**Rules:**

- **`enabled` false** — no `log` messages are sent.
- **Level `E` / `W`** — forwarded whenever wslog is enabled (same `tag` string as `ESP_LOG*`).
- **Level `I`** — forwarded only if `info_enabled` is true **and** either `allowed_info_tags` is empty (all INFO tags) **or** the tag matches any entry (substring match, max 16 tags, 31 chars each).

| Method | Path | Body | Response |
|--------|------|------|----------|
| GET | `/api/log/filter` | — | `enabled`, `info_enabled`, `allowed_info_tags`[] |
| POST | `/api/log/filter` | JSON; fields optional: `enabled`, `info_enabled`, `allowed_info_tags` (string array) | `{"status":"ok"}` |

### Relay (outbound WebSocket client to another node)

| Method | Path | Body | Response |
|--------|------|------|----------|
| POST | `/api/relay/connect` | `{"target":"<ip-or-host>"}` | `status`: `connected` \| `failed` |
| GET | `/api/relay/disconnect` | — | `{"status":"disconnected"}` |

### LittleFS file manager

| Method | Path | Query / body | Description |
|--------|------|----------------|-------------|
| GET | `/fs/ls` | `path`, optional `recursive=1` | Directory listing (JSON) |
| GET | `/fs/df` | — | Free / used space (JSON) |
| GET | `/fs/exists` | `path` | Stat path (JSON) |
| GET | `/fs/download` | `path` | File download |
| POST | `/fs/upload` | `multipart/form-data` | Upload file |
| POST | `/fs/mkdir` | Form body: `current_dir=…&folder_name=…` (URL-encoded) | Create directory |
| GET | `/fs/delete` | `path`, `type=file\|dir` | Delete file or empty dir |
| GET | `/fs/rmdir` | `path` | Recursive directory delete |
| GET | `/fs/rename` | `from`, `to` | Rename / move |

### Static assets

| Method | Path | Description |
|--------|------|-------------|
| GET | `/static/<file>` | Shared static file from LittleFS `/html` (if exists) or embedded bundle |
| GET | `/static/<device_id>/<file>` | Device asset: `/devices/<device_id>/assets/<file>`, or manifest-resolved name without extension |

---

## Serial console

USB composite device: **CDC-ACM** (serial REPL) and **CDC-NCM** (USB Ethernet at `192.168.5.1`). iOS USB-C may enumerate NCM; Lightning iPhones typically do not.

**USB NCM DHCP (local link only):** The ESP DHCP server on USB Ethernet assigns hosts **192.168.5.2–8** on **192.168.5.0/24** and does **not** advertise a default gateway (no DHCP option 3). Use this link to reach the device at `http://192.168.5.1/` while the host keeps **Wi‑Fi as the internet path** when both interfaces are up. DNS offering is disabled via ESP-IDF APIs; note that lwIP may still include DHCP option 6 pointing at `192.168.5.1` — removing option 6 entirely would require an lwIP patch.

**Boot timing (STA-first):** WiFi STA starts before USB NCM enumerates, but STA connect is **non-blocking** — USB Ethernet and the web UI come up while STA is still trying. If STA fails or times out (~30s), soft-AP fallback runs in the background; USB NCM is unaffected.

### USB console mode (NVS)

| Mode | NVS `usbjtag` | Serial port | USB Ethernet |
|------|---------------|-------------|--------------|
| **TinyUSB** (default) | `0` | CDC-ACM (VID `303A`, PID `0x400x`) | CDC-NCM enabled |
| **JTAG** | `1` | USB Serial/JTAG (VID `303A`, PID `0x1001`) | Disabled |

Serial command **`usb-console jtag`** switches to USB Serial/JTAG at next boot; **`usb-console tinyusb`** restores TinyUSB CDC + NCM. **ROM download mode:** hold **BOOT** and press **RESET**.

### Common commands

| Command | Description |
|---------|-------------|
| `help` | List registered commands |
| `version` | Firmware version, project name, build time, IDF version |
| `wifi` | Show or set WiFi NVS settings (same fields as REST `/api/wifi`) |
| `ws '<json>'` | Inject a WebSocket JSON message (see WebSocket section) |
| `usb-console <jtag\|tinyusb>` | Switch USB console mode (reboot required) |
| `reboot` | Restart firmware |

---

## WebSocket

**Endpoint:** `ws://<device-ip>/ws`  
**Framing:** text frames, one JSON object per message.  
**`type` field:** required on every client message; server dispatches on `type`.

### On connect (server → client)

- Replays **saved UI state** (background + elements) for the smart display.
- Unicast: `hello` — `type`, `src` (node id, e.g. `ESP_AABBCC`), `caps`.
- Unicast: `device_status` — `scanning`, `central` (string or null), `peripheral` (string or null).
- Unicast: `wifi_status` — full runtime `status` object (same fields as REST `/api/wifi` `status`).

Broadcasts add **`src`** (originating node) when sent over the network.

### Request correlation (`req_id`)

Optional top-level field on client messages. Use **`req_id`** when the client expects a direct reply that could be ambiguous without correlation (e.g. concurrent `fs` or `wifi` operations). The server **echoes** `req_id` on direct replies (`fs_response`, `fs_chunk`, `relay_response`, `log_filter_response`, **`wifi_response`**, and `type` `smp` command acks). Server-initiated broadcasts (`scan_device`, `log`, `gfx`, `smp` progress events, …) never include `req_id`.

Distinct from relay **`seq`**, node **`src`/`dst`**, and UI element **`id`** on `gfx` / `sim_button` / `central`.

---

### Client → server (by `type`)

| type | Payload | Action |
|------|---------|--------|
| `hello` | optional `src` | Logged; no state change |
| `status` | — | Broadcast `device_status` to all clients |
| `scanner` | `action`: `start` (`connectable` bool), `stop`, `status`, `connect` | BLE scan control; `connect` uses `addr`, optional `read_values`, `pairing_mode`, `strategy`, `pin`, `save_result`, `open_central`, and when `open_central` also `auto_on_auth`, `pin_policy` (see **SMP**) |
| `central` | `action`: `start` (`device`), `stop` (optional `keep_services` bool), `status`, `menu_select` (`id`) | Library central. `keep_services` retains the in-RAM GATT map and keeps library Lua/UI loaded for a same-device reconnect. See **Central session vs GAP**. |
| `sim` | `action`: `start` (`device`), `stop`, `status`, `autostart` (`enabled` bool, `device` when enabling) | Peripheral simulation; `stop` also disables auto-start for the stopped device |
| `sim_button` | `id` | Runs Lua hook for simulated button |
| `devices` | `action`: `list` | Broadcasts `devices_list` |
| `system` | `action`: see **System (WS)** below | Reboot, memory, version |
| `ble_sim_trace` | `enabled` (bool) | Enable or disable GATT trace events |
| `log_filter` | `action`: `get` (optional `req_id`), or set fields: `enabled`, `info_enabled`, `allowed_info_tags` | Get filter snapshot or update wslog filter |
| `relay` | See **Relay** below | GATT relay, connect/disconnect, or response delivery (not pairing) |
| `smp` | See **SMP** below | Live-central pairing config / initiate / passkey IO |
| `fs` | `cmd`: see **File system (WS)** below | File manager over WS |
| `wifi` | `action`: see **Wi‑Fi (WS)** below | NVS config + runtime status; optional `reboot` on mutations |

Unhandled `type` values are logged and ignored.

### Central session vs GAP

Two outbound types — do not treat them as the same:

- **`central_status`**: library session start/stop. `status` is `started` or `stopped`; `device` is the library folder or `null`. Emitted on `central` `start` / `stop` / `status`. Scanner `open_central` does **not** emit `central_status` `"started"`. After any `stop` (including `keep_services`), `central_status` stays `stopped` until the next **`central` `start`**. GATT map and on-device Lua/UI may still be retained after `keep_services`. `device_status.central` uses the same folder string (null when stopped).
- **`central`**: live GAP / pairing UI from `send_update_central_status_to_ws`. `status` is `connecting` / `connected` / `disconnected` / `pairing` (also `scanning`). Soft stop already emits `"disconnected"` from GAP. This is a broadcast, not a command reply.

---

### System (`type`: `system`)

| `action` | Behavior | REST equivalent |
|----------|----------|----------------|
| `reboot`, `restart` | Normal firmware restart (broadcasts `memory_status` first) | — |
| `memory` | Broadcast transient `memory_status` (heap stats) | — |
| `version` | Broadcast transient `version` (firmware / IDF / build time) | — |

**Example:**

```json
{"type":"system","action":"reboot"}
```

No JSON reply is sent before reboot (connection drops). `memory` and `version` push server broadcasts to all WS clients (see **`version`** in [Server → client](#server--client-by-type)).

---

### Wi‑Fi (`type`: `wifi`)

Mobile / serial clients use WebSocket (or serial `ws '{"type":"wifi",…}'`) for Wi‑Fi setup. Responses use **`type`: `wifi_response`**, echo **`action`** and optional **`req_id`**.

| `action` | Fields | Response |
|----------|--------|----------|
| `get_config` | optional `req_id` | `ok`, `config` object (NVS; same fields as REST `config`) |
| `status` | optional `req_id` | `ok`, `status` object (runtime; same fields as REST `status`) |
| `get` | optional `req_id` | `ok`, both `config` and `status` (recommended on connect) |
| `set` | partial: `mode`, `ssid`, `psk`, `ap_ssid`, `ap_psk`, `enabled`; optional `reboot` (bool), `req_id` | `ok`, `note` or `rebooting`; or `error`. Writes NVS; does not apply until reboot unless `reboot`: true |
| `enable` | optional `reboot`, `req_id` | Sets `enabled` true, saves NVS |
| `disable` | optional `reboot`, `req_id` | Sets `enabled` false (WiFi off at boot; USB NCM unchanged) |

**`mode` values (set):** `sta_first`, `sta`, `ap_only`, `ap`, or `0` / `1`.

**Partial update rules (set):** same as REST POST — empty `psk` keeps existing STA password; empty `ap_ssid` / `ap_psk` clears custom AP overrides; non-empty `ssid` required when `ssid` key is present. Saving both STA ssid and psk auto-sets `enabled` true.

**Server-initiated pushes:** firmware broadcasts `wifi_status` (transient) when WiFi or USB link state changes (STA connect/disconnect, AP start, USB mount/unmount). Shape:

```json
{"type":"wifi_status","status":{ ... same fields as REST status ... }}
```

**Example — full snapshot:**

```json
{"type":"wifi","action":"get","req_id":1}
```

```json
{"type":"wifi_response","action":"get","req_id":1,"ok":true,
 "config":{"enabled":true,"mode":"sta_first","ssid":"MyNet","has_psk":true,
           "ap_ssid":"","ap_ssid_effective":"BLESPLO.it_AABBCC","has_ap_psk":false,
           "has_sta_creds":true},
 "status":{"wifi_running":true,"active_mode":"sta","current_ip":"10.0.0.5",
           "primary_iface":"WIFI_STA_DEF",
           "sta":{"up":true,"connected":true,"ssid":"MyNet","ip":"10.0.0.5","rssi":-42},
           "ap":{"up":false,"ssid_effective":"BLESPLO.it_AABBCC","ip":null,"clients":0},
           "usb":{"up":true,"ip":"192.168.5.1","link_up":true},
           "http_server":true,"pending_reboot":false}}
```

**Example — disable WiFi and reboot:**

```json
{"type":"wifi","action":"disable","req_id":2,"reboot":true}
```

```json
{"type":"wifi_response","action":"disable","req_id":2,"ok":true,"rebooting":true}
```

---

### File system (`type`: `fs`)

Paths are LittleFS-relative (e.g. `/devices/pixel_buds/ble.json`). `..` is rejected on most commands.

| `cmd` | Fields | Response |
|-------|--------|----------|
| `ls` | `path`, optional `recursive` | `fs_response` with `entries` |
| `df` | — | `fs_response` with space stats |
| `exists` | `path` | `fs_response` |
| `mkdir` | `path` | `fs_response` |
| `delete` | `path` | `fs_response` |
| `rmdir` | `path` | `fs_response` |
| `rename` | `from`, `to` | `fs_response` |
| `patch` | `path`, `patch` (object) | Generic JSON deep-merge into file |
| `read_start` | `path`, `req_id` | `fs_response` with `size`; opens read session |
| `read_chunk` | `req_id`, optional `seq` | `fs_chunk` with `seq`, `data` (base64), `eof` |
| `write_start` | `path`, optional `size`, `req_id` | `fs_response`; then send chunks |
| `write_chunk` | `req_id`, `data` (base64), optional `seq` | `fs_response` with `bytes` written so far |
| `write_end` | `req_id` | `fs_response` with final `bytes` |

**Download sequence (pull-based):** `read_start` → `fs_response` (`size`) → repeat: send `read_chunk` (same `req_id`, optional `seq`) → receive `fs_chunk` (`cmd`: `read`, `seq`, `data` base64, `eof`) → stop when `eof: true`. One active read session at a time; a new `read_start` aborts any stale session. Empty files: first `read_chunk` returns `data: ""`, `eof: true`.

**Upload sequence:** `write_start` → `write_chunk` (repeat) → `write_end`. Raw chunk size 384 bytes (512 base64 chars). One active upload at a time.

**Mobile device JSON workflow:** `devices` list → `fs read_start` on `/devices/<folder>/<file>.json` → edit locally → `fs write_*` upload whole file.

---

### Relay (`type`: `relay`)

**Connect / disconnect (central node):**

| `action` | Fields | Response |
|----------|--------|----------|
| `connect` | `target` (IP/host), optional `req_id` | `relay_response`: `status` `connected` \| `failed` |
| `disconnect` | optional `req_id` | `relay_response`: `status` `disconnected` |

REST equivalents: `POST /api/relay/connect`, `GET /api/relay/disconnect`.

**Sim / peripheral side (typical):** forward GATT operations toward a peer that runs central and has connected to this device.

| Field | Meaning |
|-------|---------|
| `action` | `read`, `write`, `write_noresp`, `read_desc`, `subscribe`, `unsubscribe`, or responses: `read_rsp`, `write_rsp`, `subscribe_rsp`, `read_desc_rsp`, `notify_rx`, `indicate_rx` |
| `svc`, `chr` | UUID strings |
| `data` | Hex string for writes / read response payload |
| `seq` | Correlates request/response |
| `indicate` | Bool for subscribe action (notify vs indicate) |
| `desc` | Descriptor UUID for `read_desc` |
| `src` | Optional requester id |

**Central side (incoming over outbound relay client):** the firmware also accepts the same `relay` request shapes on the **client** connection to the peer (see `web_server_relay.c`).

Protected characteristics return a `*_rsp` immediately (e.g. `status` **261** = `0x105` Insufficient Authentication; **271** = `0x10F` Insufficient Encryption). Firmware does **not** auto-retry the GATT op after pairing. The app re-issues the same `relay` `read`/`write`/`subscribe` after a successful `smp` `pairing_complete`.

---

### SMP (`type`: `smp`)

GATT stays on `relay`. Pairing commands and progress use `smp`. Discovery-time pairing (scanner `connect` without keeping the link as a long-lived central session) also emits `type` `smp` progress, with a slimmer payload (`event`, `conn_handle`, `status`, `detail` only).

**`conn_handle`:** NimBLE’s first connection is **`0`**. Missing / none is **`0xFFFF`**, not `0`.

#### Client → server

| `action` | Fields | Ack (`type`: `smp`) |
|----------|--------|---------------------|
| `pair_config` | optional `strategy`, `pin`, `auto_on_auth`, `pin_policy`, `req_id` | `event` and `action` `pair_config`, `status` `ok` |
| `pair` | optional `req_id` | `event` and `action` `pair`, `status` `ok` \| `failed` (initiate return code) |
| `pair_io` | optional `conn_handle` (else current central handle), `passkey` (number, 0–999999), `accept` (bool, numeric comparison), `cancel` (bool), `req_id` | `event` and `action` `pair_io`, `status` `ok` \| `failed` |

Unknown `action` is logged and ignored (no ack).

**`pair_config` / scanner `connect` fields** (also applied from `scanner` `connect` when `open_central` is true):

| Field | Type | Default | Meaning |
|-------|------|---------|---------|
| `strategy` | number | `0` | See table below. Live central **`AUTO` (4) is coerced to Legacy Just Works**; discovery `AUTO` still cycles strategies. |
| `pin` | number | unset until sent; scanner `connect` uses **123456** if omitted | Six-digit passkey for `pin_policy` 1 |
| `auto_on_auth` | bool | `true` | After a relay ATT auth/encrypt/key-size error, start pairing without a separate `pair` |
| `pin_policy` | number | `1` | `0` always prompt the client; `1` inject configured `pin` if set, else prompt |

**`strategy` values:**

| Value | Name |
|-------|------|
| `0` | Legacy Just Works (NoIO, SC disabled) |
| `1` | SC Just Works (NoIO, SC enabled) |
| `2` | Legacy PIN (KeyboardOnly, SC disabled) |
| `3` | SC PIN (KeyboardOnly, SC enabled) |
| `4` | AUTO (discovery ladder; live central → treated as `0`) |

**Scanner `connect` pairing_mode** (discovery, not live-central SMP): `0` none, `1` probe only, `2` complete pairing.

**Live-central flow (phone as GATT client via ESP):**

1. `scanner` `connect` with `open_central`: true (optional `strategy` / `pin` / `auto_on_auth` / `pin_policy`). Does **not** emit `central_status` `"started"`.
2. `relay` `read` / `write` / `subscribe` as usual.
3. On ATT auth error: `relay` `*_rsp` with `status` 261/271/…, then if `auto_on_auth` an `smp` `pairing_needed` and auto-`pair`.
4. On success: `smp` `pairing_complete` with `status` `0`; app retries the GATT op.
5. On failure: `smp` `pairing_complete` (or `encryption_failed` / `pairing_failed` / `pairing_cancelled`); app may `pair_config` + `pair` with the next strategy.
6. If `passkey_action` `action` is `input` or `numeric_comparison` and a prompt is required, reply with `pair_io` within **25 s** (`timeout_ms` on that event).

**Examples:**

```json
{"type":"scanner","action":"connect","addr":"AA:BB:CC:DD:EE:FF","open_central":true,"read_values":false,"save_result":false,"pairing_mode":0,"strategy":0,"pin":123456,"auto_on_auth":true,"pin_policy":1}
```

```json
{"type":"smp","action":"pair_config","strategy":2,"pin":123456,"auto_on_auth":true,"pin_policy":1,"req_id":10}
```

```json
{"type":"smp","event":"pair_config","action":"pair_config","status":"ok","req_id":10}
```

```json
{"type":"smp","action":"pair"}
```

```json
{"type":"smp","action":"pair_io","conn_handle":0,"passkey":123456}
```

```json
{"type":"smp","action":"pair_io","conn_handle":0,"accept":true}
```

```json
{"type":"smp","action":"pair_io","cancel":true}
```

#### Server → client (progress)

Live-central broadcasts are transient. Common fields: `type` `smp`, `event`, `conn_handle`, numeric `status`, `strategy`, `pin_policy`. Optional: `detail`, `action` (passkey kind), `passkey`, `timeout_ms`, `svc`, `chr`, `seq`. Broadcasts include `src` when sent over the network.

| `event` | When | `status` / notes |
|---------|------|------------------|
| `pairing_needed` | Relay GATT hit insufficient auth/enc/key size | Host ATT-wrapped code (261, 271, …); `detail` `insufficient_auth`; includes `svc`, `chr`, `seq` |
| `pairing_initiated` | `ble_gap_security_initiate` accepted | `0`; `detail` is strategy name |
| `pairing_response_received` | Peer Pairing Response PDU | `0` |
| `passkey_action` | SMP IO | `action` / `detail`: `just_works`, `display`, `input`, `numeric_comparison`, or `oob`. `display` / `numeric_comparison` include `passkey`. Prompt cases include `timeout_ms` 25000 |
| `pin_injected` | Configured PIN sent (`pin_policy` 1) | `0` |
| `pairing_complete` | SMP finished | **`status` is a raw SMP reason** (`BLE_SM_ERR_*`), not a NimBLE host code. `0` success; `8` unspecified; `3` authentication requirements; `5` pairing not supported; … `detail` is the decoded string |
| `encryption_failed` | `ENC_CHANGE` failed | **Host** `app_status` (`BLE_HS_*`, or `0x400`/`0x500` + SM reason). `6` is truly no memory |
| `pairing_failed` | Disconnect during pair, or IO inject failure | Host / disconnect reason; `detail` e.g. `disconnected`, `initiate_failed` |
| `pairing_cancelled` | `pair_io` `cancel` or 25 s prompt timeout | `status` `-1`; `detail` `cancel` or `timeout` |

Do not treat `pairing_complete` `status` `8` as out-of-memory. That value is **SM unspecified**. Host “no memory” is `6` on `encryption_failed` / initiate failures.

Firmware may also broadcast `{"type":"central","status":"pairing"}` then `"connected"` around an attempt (local UI). Pairing state for the app is the `smp` events.

Discovery (scanner connect) may additionally emit `encryption_success`, `pairing_initiate_failed`, `pin_inject_failed`, `oob_not_supported`, and similar `event` names with the slim payload (no `strategy` / `pin_policy` fields).

---

### Server → client (by `type`)

| type | When | Notes |
|------|------|--------|
| `hello` | New client | Unicast |
| `device_status` | Connect / client `status` | Scan + active central + peripheral ids |
| `wifi_status` | Connect (unicast) / connectivity change (broadcast) | Full WiFi/USB runtime `status`; no `req_id` |
| `scan_status` | Scan start/stop/status | May be stateful or transient (`result`, `count`, …) |
| `scan_device` | Advertisements | `update`, `addr`, `name`, `adv_data` hex, RSSI, flags, … |
| `connection_progress` | Discovery / connection | `phase`, `status`, `addr`, `detail` |
| `scan_discovery_result` | After connect + discover | `addr`, `rc`, `viable`, `services`; optional `cached`: true when GATT map was reused (no ATT walk) |
| `central_status` | Library `central` start/stop | `status`: `started` / `stopped`; `device` folder or `null`. Not emitted by scanner `open_central`. Stays `stopped` after `keep_services` until the next `central` `start`. |
| `central` | Live GAP / pairing UI | `status`: `connecting` / `connected` / `disconnected` / `pairing` (also `scanning`). Soft stop emits `disconnected`. Broadcast; not a command reply. |
| `smp` | Pairing progress or command ack | See **SMP**. Acks have `event`+`action`+string `status` (`ok`/`failed`) and optional `req_id`. Progress has numeric `status` and `event` names above |
| `sim_status` | Simulation start/stop | `status`, `device`, optional `adv`, optional `autostart` (bool), `autostart_device` (string or null), `autostart_blocked` (bool) |
| `devices_list` | List refresh | `devices` array |
| `gfx` | Lua / graphics | `cmd`: `png`, `svg`, `background`, `color`, `text`, `notification`, `clear`, `remove` + layout fields |
| `log` | wslog (`wslog_send`) after filter passes | `level`: `E` \| `W` \| `I`; `tag`; `msg` (quotes/newlines/backslashes sanitized). May include `src` like other broadcasts. |
| `memory_status` | `system` action | Heap stats |
| `version` | `system` action | Firmware / IDF / build time |
| `ble_sim_trace` | Optional | GATT traffic debug |
| `fs_response` | FS over WS | `cmd`, `ok` or `error`, optional `req_id` |
| `fs_chunk` | FS file read | `cmd`: `read`, `seq`, `data` (base64), `eof`, optional `req_id` |
| `relay_response` | Relay connect/disconnect | `action`, `status`, optional `req_id` |
| `log_filter_response` | `log_filter` action `get` | Filter fields + optional `req_id` |
| `wifi_response` | `wifi` actions | `action`, `ok`, optional `req_id`, `config` / `status` / `note` / `rebooting` / `error` |
| `relay` | Relay GATT path | Responses and notifications |

---

### Graphics smart display state

On connect, the server replays **canvas state** only: the last `background` command and each message that carried an element **`id`** (as used by `gfx` draws). Typical scan, relay, status, and log traffic is **not** replayed.

**Asset paths (`gfx` `png` / `svg`):** firmware sends a **short device asset key**, not an HTTP URL or a full LittleFS path.

**Wire format:** `{device_id}/{filename}` — the filename comes from the device’s `graphics.json` `file` field (e.g. `lightbulb.svg`), without an `assets/` prefix.

Example WS message:

```json
{"type":"gfx","cmd":"svg","id":"lightbulb","svg":"blesploit_lightbulb/lightbulb.svg", ...}
```

**Canonical LittleFS path** (same layout LVGL uses on the device):

```
/devices/{device_id}/assets/{filename}
```

Example: `blesploit_lightbulb/lightbulb.svg` → `/devices/blesploit_lightbulb/assets/lightbulb.svg`

| Client | How to resolve `{device_id}/{filename}` |
|--------|----------------------------------------|
| Browser (HTTP) | `GET /static/{device_id}/{filename}` — the static handler maps this to `/devices/{device_id}/assets/{filename}` |
| Mobile / WS-only | `fs read_start` on `/devices/{device_id}/assets/{filename}`; reassemble chunks, cache, render locally |

Legacy WS messages may still carry a full `/static/...` URL from older firmware; clients may accept those for backward compatibility when replaying saved canvas state.
