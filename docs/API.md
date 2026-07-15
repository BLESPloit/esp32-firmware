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

Optional top-level field on client messages. Use **`req_id`** when the client expects a direct reply that could be ambiguous without correlation (e.g. concurrent `fs` or `wifi` operations). The server **echoes** `req_id` on direct replies (`fs_response`, `fs_chunk`, `relay_response`, `log_filter_response`, **`wifi_response`**). Server-initiated broadcasts (`scan_device`, `log`, `gfx`, …) never include `req_id`.

Distinct from relay **`seq`**, node **`src`/`dst`**, and UI element **`id`** on `gfx` / `sim_button` / `central`.

---

### Client → server (by `type`)

| type | Payload | Action |
|------|---------|--------|
| `hello` | optional `src` | Logged; no state change |
| `status` | — | Broadcast `device_status` to all clients |
| `scanner` | `action`: `start` (`connectable` bool), `stop`, `status`, `connect` | BLE scan control; `connect` uses `addr`, optional `read_values`, `pairing_mode`, `strategy`, `pin`, `save_result`, `open_central` |
| `central` | `action`: `start` (`device`), `stop`, `status`, `menu_select` (`id`) | Central mode + UI delegation |
| `sim` | `action`: `start` (`device`), `stop`, `status` | Peripheral simulation |
| `sim_button` | `id` | Runs Lua hook for simulated button |
| `devices` | `action`: `list` | Broadcasts `devices_list` |
| `system` | `action`: see **System (WS)** below | Reboot, memory, version |
| `ble_sim_trace` | `enabled` (bool) | Enable or disable GATT trace events |
| `log_filter` | `action`: `get` (optional `req_id`), or set fields: `enabled`, `info_enabled`, `allowed_info_tags` | Get filter snapshot or update wslog filter |
| `relay` | See **Relay** below | GATT relay, connect/disconnect, or response delivery |
| `fs` | `cmd`: see **File system (WS)** below | File manager over WS |
| `wifi` | `action`: see **Wi‑Fi (WS)** below | NVS config + runtime status; optional `reboot` on mutations |

Unhandled `type` values are logged and ignored.

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
| `scan_discovery_result` | After connect + discover | `addr`, `rc`, `viable`, `services` |
| `central_status` | Central start/stop | `status`, `device` |
| `sim_status` | Simulation start/stop | `status`, `device`, optional `adv` |
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
