## Runtime and standard library

ESP32 peripheral scripts run with a restricted standard library, in order to save the memory footprint:

- `_G`
- `string`
- `math`
- `table`

Do not assume Luaj JSE libraries such as `io` or `os` are available here.

Scripts are loaded by `lua_init_persistent_minimal()` in the firmware Lua VM. 

---

## Globals

| Global | Source |
|--------|--------|
| `vars` | Table loaded from device `vars.json` (strings, numbers, bools). |
| `uuids` | String map of symbolic names to UUID strings from the device UUID map JSON via `lua_uuids_inject`. |

---

## Common helpers

These helpers are available in the ESP32 peripheral Lua VM.

| Function | Description |
|----------|-------------|
| `delay(seconds, func_name)` | Schedule global function `func_name` with zero arguments. |
| `bin_to_hex(binary)` → string | Convert binary data to uppercase hex. |
| `hex_to_bin(hex)` → binary | Decode even-length hex input to binary. Invalid input may hard-fail. |
| `get_time()` → integer | Returns `time(NULL)`. This is only useful if the device has valid wall-clock time set. |
| `vars_save()` → integer | Serializes the `vars` back to json |


### Crypto

| Function | Description |
|----------|-------------|
| `aes_ecb_encrypt(key16, block16)` → binary | AES-128-ECB encrypt with exactly 16-byte key and block. |
| `aes_ecb_decrypt(key16, block16)` → binary | AES-128-ECB decrypt. |
| `sha256(data)` → binary | Full 32-byte SHA-256 digest. |
| `sha256_first_16(data)` → binary | First 16 bytes of SHA-256.  |
| `ecdh_generate_keypair()` → `priv32, pub64` | Generates a secp256r1 keypair; `pub64` is `X || Y` without the `0x04` prefix.  |
| `ecdh_compute_shared(priv32, peer_pub64)` → `shared32` | Computes a 32-byte ECDH shared secret.  |
| `random_bytes(n)` → binary | Returns hardware RNG output for `n` in the range 1..1024. |

---

## Graphics

These functions bind to the LVGL / WebSocket simulation UI.

| Function | Description |
|----------|-------------|
| `gfx_set_background(color_uint)` | Set background color, typically `0xRRGGBB`.  |
| `gfx_show(id)` | Render an existing element by id.  |
| `gfx_set_position(id [, align_str] [, x, y [, w [, h]]])` | Set anchor and offsets; `align_str` defaults to `center`.  |
| `gfx_set_color(id, color_uint)` | Recolor an existing element.  |
| `gfx_remove(id)` | Remove an element.  |
| `gfx_render_text(id [, align_str] [, x, y] [, color])` | Create or update layout for a text id.  |
| `gfx_update_text(id, text [, color])` | Change the text payload.  |
| `gfx_print_notification(text [, align] [, x, y] [, color] [, size] [, duration_ms])` | Show a toast-style notification.  |

Supported align strings: `top_left`, `top_center`, `top_right`, `middle_left`, `middle_right`, `bottom_left`, `bottom_center`, `bottom_right`; any other value falls back to `center`. 

---

## BLE peripheral

These functions are available only in the ESP32 peripheral simulation runtime.

| Function | Description |
|----------|-------------|
| `ble_notify(svc_uuid, chr_uuid, hex_str)` → `ok [, err]` | Decode `hex_str`, update the characteristic backing value, and notify subscribers.  |
| `ble_notify_raw(svc_uuid, chr_uuid, hex_str)` → `ok [, err]` | Send a notify PDU without updating the backing store; requires connectivity and does not honor CCCD.  |
| `adv_set_data(profile_id, adv_hex [, scan_rsp_hex])` → bool | Replace raw advertising data, with optional scan response, for the advertising profile id.  |
| `get_adv_bd_addr(profile_id)` → `addr | nil [, err]` | Return the resolved TX address string when available.  |
| `adv_enable(profile_id)` → `ok [, err]` | Start an advertising instance.  |
| `adv_disable(profile_id)` → `ok [, err]` | Stop an advertising instance.  |
| `get_mtu()` → int | Return the negotiated ATT MTU, defaulting to 23 if unknown.  |
| `set_preferred_mtu(mtu)` → `ok [, err]` | Request MTU in the allowed range 23..517.  |

---

## Dynamic GATT hooks

Peripheral GATT `dynamic` hooks in JSON, such as `on_read` and `on_write`, can call into Lua. See `ble_sim_gatt.c` for relay-prefixed forms. 

---

## Notes

- All Lua runs on a dedicated task. BLE notify posts `on_notify` asynchronously via a queue. 
- Many functions use multiple return values such as `ok, err` or `nil, err`, and some failures may use `luaL_error`. 
- Prefer entries in `uuids` for readability; UUID strings are normalized internally for lookup. 