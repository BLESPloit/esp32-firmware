# Lua API (device scripts)

Scripts are loaded by `lua_init_persistent_minimal()` (`main/lua/lua_hook.c`): one main file per **central** or **peripheral (sim)** profile, with a restricted standard library (`_G`, `string`, `math`, `table` only).

Do not assume Lua standard libraries such as `io` or `os` are available.

---

## Globals (when provided by the device manifest / paths)

| Global | Source |
|--------|--------|
| `vars` | Table from device **`vars.json`** (`lua_vars_inject`, before script load); flat string/number/bool keys only. See [DEVICE_JSON.md](DEVICE_JSON.md) (section **vars.json**). |
| `uuids` | Populated from device **`uuids.json`** via `lua_uuids_inject` before script load — each top-level entry should include **`uuid`**; see [DEVICE_JSON.md](DEVICE_JSON.md). |

Peripheral **GATT `dynamic` hooks** in JSON (`on_read`, `on_write`, …) can call into Lua; see `ble_sim_gatt.c` for relay-prefixed forms.

---

## Common (central + peripheral)

| Function | Description |
|----------|-------------|
| `delay(seconds, func_name)` | Schedule global function `func_name` with zero args. |
| `bin_to_hex(binary)` → string | Uppercase hex. |
| `hex_to_bin(hex)` → binary | Even length; accepts hex digits (e.g. `%2x` parsing). |
| `get_time()` → integer | `time(NULL)` — wall clock only if the device has time set (e.g. SNTP). |
| `vars_save()` → bool | Writes global **`vars`** table to manifest **`vars.json`** path; scalar fields only. |
| `gpio_set(name, level)` → ok [, err] | Drive symbolic GPIO **`gpio_a`** or **`gpio_b`** high (1/true) or low (0/false). First use configures the pin as output. |
| `gpio_get(name)` → level \| nil [, err] | Read current level (0/1) for **`gpio_a`** or **`gpio_b`**. |
| **Crypto** | |
| `aes_ecb_encrypt(key16, block16)` → binary | AES-128-ECB encrypt (exactly 16-byte key and block). |
| `aes_ecb_decrypt(key16, block16)` → binary | AES-128-ECB decrypt. |
| `sha256(data)` → 32-byte binary | Full SHA-256 digest. |
| `sha256_first_16(data)` → 16-byte binary | First 16 bytes of SHA-256. |
| `ecdh_generate_keypair()` → priv32, pub64 | secp256r1; `pub64` is X‖Y without `0x04` prefix. |
| `ecdh_compute_shared(priv32, peer_pub64)` → shared32 | 32-byte shared secret. |
| `random_bytes(n)` → binary | Hardware RNG; `n` in 1..1024. |

**GPIO availability** is board/build-dependent (Kconfig). Bare/generic builds default to no named GPIOs (`-1`); M5StickS3 uses GPIO 4/5; LilyGO T-QT Pro uses GPIO 16/17. Unavailable pins return an error at call time.

Example — pulse a line for 5 seconds using `delay`:

```lua
function gpio_a_off()
    gpio_set("gpio_a", 0)
end

gpio_set("gpio_a", 1)
delay(5, "gpio_a_off")
```

---

## Central only

BLE GATT client (blocking, ~3s timeout on read/write):

| Function | Description |
|----------|-------------|
| `ble_connected()` → bool | Connection present. |
| `ble_write(svc_uuid, chr_uuid, data_bin)` → ok [, err] | Write characteristic; `data_bin` is raw bytes. UUIDs accept common string forms; matching is normalized. |
| `ble_read(svc_uuid, chr_uuid)` → data_bin \| nil [, err] | Read characteristic value. |
| `ble_subscribe(svc_uuid, chr_uuid)` → ok [, err] | Enable notify on CCCD (`0x0001`); deliveries go to `on_notify`. |
| `ble_unsubscribe(svc_uuid, chr_uuid)` → ok | Best-effort disable notify. |
| `get_mtu()` → int | Negotiated ATT MTU for the active central or sim link (default 23 if unknown); usable notify/write payload roughly `get_mtu() - 3`. Central auto-exchanges MTU on connect before `on_connected`. |
| `set_preferred_mtu(mtu)` → ok [, err] | Set preferred ATT MTU (range **23 .. 517**); triggers Exchange MTU on an active central connection when possible. |

Define these Lua functions in your script to receive callbacks:

| Function | Role |
|----------|------|
| `on_connected` | Central: called after connect once ATT MTU exchange has completed (or been skipped when already negotiated). |
| `on_notify(svc_uuid, chr_uuid, hex_payload)` | Central: notify/indication from subscribed characteristics (`hex_payload` is hex without spaces). Payload may be truncated for very long PDUs — use `ble_read` if you need the full value. |

Central **`menu.json`** defines static menus, **`on_enter`**, and row actions (`func` or `func(args)`); see [DEVICE_JSON.md](DEVICE_JSON.md) (section **menu.json**).

Interface:

| Function | Description |
|----------|-------------|
| `push_menu(id)` | Push central menu node by id. |
| `pop_menu()` | Pop menu stack. |
| `set_title(text)` | Set UI title. |
| `set_state(key, value)` | Store string UI state keyed by string. |

Graphics helper (central build registers one gfx helper):

| Function | Description |
|----------|-------------|
| `gfx_print_notification(text [, align] [, x, y] [, color] [, size] [, duration_ms])` | Toast-style notification; `align` strings same as below. |

---

## Peripheral (simulation) only

Graphics (bind to LVGL / WebSocket UI):

| Function | Description |
|----------|-------------|
| `gfx_set_background(color_uint)` | Sets background (`color_uint` typically `0xRRGGBB`). |
| `gfx_show(id)` | Render existing element `id`. |
| `gfx_set_position(id [, align_str] [, x, y [, w [, h]]])` | Anchor + offsets (`align_str` defaults to `center`). |
| `gfx_set_color(id, color_uint)` | Recolor element. |
| `gfx_remove(id)` | Remove element. |
| `gfx_render_text(id [, align_str] [, x, y] [, color])` | Create/update layout for text id. |
| `gfx_update_text(id, text [, color])` | Change text payload. |
| `gfx_print_notification(...)` | Same signature as central. |

**Align** strings (`align_str`): `top_left`, `top_center`, `top_right`, `middle_left`, `middle_right`, `bottom_left`, `bottom_center`, `bottom_right`; anything else → `center`.

BLE peripheral (advertising profile **`id`** values come from **`peripheral/adv.json`** — see [DEVICE_JSON.md](DEVICE_JSON.md)):

| Function | Description |
|----------|-------------|
| `ble_notify(svc_uuid, chr_uuid, hex_str)` → ok [, err] | Decode `hex_str`, update characteristic backing value, notify subscribers. |
| `ble_notify_raw(svc_uuid, chr_uuid, hex_str)` → ok [, err] | Send notify PDU without updating backing store; connectivity required. Does not honor CCCD (may transmit unsafely — see source comments). |
| `adv_set_data(profile_id, adv_hex [, scan_rsp_hex])` → bool | Replace raw adv (+ optional scan response) hex for advertising profile id. |
| `get_adv_bd_addr(profile_id)` → addr \| nil [, err] | Resolved TX address string when available. |
| `adv_enable(profile_id)` → ok [, err] | Start advertising instance. |
| `adv_disable(profile_id)` → ok [, err] | Stop instance. |
| `get_mtu()` / `set_preferred_mtu(mtu)` | Same semantics as central; `get_mtu()` reads the sim connection handle. |

---

## Notes

- **Threading:** All Lua runs on a dedicated task; BLE notify posts `on_notify` asynchronously via a queue.
- **Errors:** Many functions use multiple return values (`ok, err` or `nil, err`) or `luaL_error` for hard failures.
- **UUIDs:** Prefer entries in `uuids` for readability; string forms are normalized internally for lookup.
