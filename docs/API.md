# Blesploit HTTP & WebSocket API

Base URL: `http://<device-ip>/` (HTTP port is the ESP-IDF default unless changed in firmware).  
All JSON bodies use `Content-Type: application/json` where applicable.

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

### Wi‑Fi (stored config; reboot may be required to apply)

| Method | Path | Body (JSON, partial OK) | Response |
|--------|------|---------------------------|----------|
| GET | `/api/wifi` | — | `ssid`, `has_psk`, `mode` (`sta_first` \| `ap_only`), `ap_ssid`, `ap_ssid_effective`, `has_ap_psk` |
| POST | `/api/wifi` | `ssid`, `psk`, `mode`, `ap_ssid`, `ap_psk` (all optional) | `ok`, optional `error` / `note` |

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

## WebSocket

**Endpoint:** `ws://<device-ip>/ws`  
**Framing:** text frames, one JSON object per message.  
**`type` field:** required on every client message; server dispatches on `type`.

### On connect (server → client)

- Replays **saved UI state** (background + elements) for the smart display.
- Unicast: `hello` — `type`, `src` (node id, e.g. `ESP_AABBCC`), `caps`.
- Unicast: `device_status` — `scanning`, `central` (string or null), `peripheral` (string or null).

Broadcasts add **`src`** (originating node) when sent over the network.

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
| `system` | `action`: `reboot` / `restart`, `memory`, `version` | Reboot, or push `memory_status` / `version` |
| `ble_sim_trace` | `enabled` (bool) | Enable or disable GATT trace events |
| `log_filter` | Same keys as `POST /api/log/filter`: optional `enabled`, `info_enabled`, `allowed_info_tags` | Updates wslog filter in RAM (mirrors REST behavior; does not emit a separate “filter snapshot” message) |
| `relay` | See **Relay** below | GATT relay or response delivery |
| `fs` | `cmd`: `ls`, `df`, `exists`, `mkdir`, `delete`, `rmdir`, `rename`, `patch` | File manager over WS (see server file manager source for fields) |

Unhandled `type` values are logged and ignored.

---

### Relay (`type`: `relay`)

**Sim / peripheral side (typical):** forward GATT operations toward a peer that runs central and has called `POST /api/relay/connect` to this device.

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
| `fs_response` | FS over WS | `cmd`, `ok` or `error` |
| `relay` | Relay path | Responses and notifications |

---

### Graphics smart display state

On connect, the server replays **canvas state** only: the last `background` command and each message that carried an element **`id`** (as used by `gfx` draws). Typical scan, relay, status, and log traffic is **not** replayed.
