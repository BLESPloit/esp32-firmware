#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

// ─── Per-role path structs ────────────────────────────────────────────────────
// Each is heap-allocated only if that role exists in the manifest.
// Caller frees with the matching _free() + free().

typedef struct {
    char *advertisement; // advertisement definitions, by default adv.json
    char *entry;      // roles.peripheral.entry  (Lua)
    char *interface;  // roles.peripheral.interface
    bool show_adv_address; 
} device_role_peripheral_t;

typedef struct {
    char *entry;      // roles.central.entry  (Lua)
    char *menu;       // roles.central.menu
} device_role_central_t;

typedef struct {
    char *entry;      // roles.observer.entry  (Lua)
} device_role_observer_t;

// ─── Full resolved paths for a device ────────────────────────────────────────
// All pointers are heap-allocated absolute LittleFS paths, or NULL if absent.
// Caller frees with device_paths_free() + free().

typedef struct {
    char *base;        // /littlefs/devices/{id}
    char *profile;     // top-level "profile" key (ble.json) — shared across roles
    char *vars;
    char *uuids;

    // Assets (optional)
    char *graphics;    // assets.graphics
    char *icon;        // assets.icon  (absolute path, for internal use)

    char *advertisement;

    // Roles — NULL if role not defined in manifest
    device_role_peripheral_t *peripheral;
    device_role_central_t    *central;
    device_role_observer_t   *observer;
} device_paths_t;

// ─── Device metadata ─────────────────────────────────────────────────────────

typedef struct {
    char *folder;
    char *name;
    char *description;
    char *notes;
    char *author;
    char *author_url;
    char *model_url; 
    char *source_url;
    char *icon_url;
} device_meta_t;

// ─── Lifecycle ───────────────────────────────────────────────────────────────

void device_paths_free(device_paths_t *p);
void device_meta_free(device_meta_t *m);

// ─── Load manifest ───────────────────────────────────────────────────────────

// Returns heap-allocated cJSON tree — caller must cJSON_Delete().
cJSON *manifest_load(const char *device_id);

// ─── Resolve all paths ───────────────────────────────────────────────────────
// Resolves everything in one pass: profile, assets, and all roles present.
// Only roles defined in the manifest get a non-NULL sub-struct.
// Returns NULL on failure (manifest missing or unparseable).
device_paths_t *manifest_resolve(const char *device_id);

// ─── Resolve a single named key → absolute path (heap string) ────────────────
// sub: "profile" | "interface" | "menu" | "observer_entry"
//      "icon" | "graphics" | "adv"
// Returns NULL if not found. Caller must free().
char *manifest_resolve_path(const char *device_id, const char *sub);

// ─── Metadata ────────────────────────────────────────────────────────────────

device_meta_t *manifest_load_meta(const char *device_id);
device_meta_t *manifest_extract_meta(const char *device_id, cJSON *manifest);