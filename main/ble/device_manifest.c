#include "device_manifest.h"
#include "common/storage.h"
#include "api/web_server.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "ble/device_parser.h" // read_json_file

static const char *TAG = "device manifest";

// ─── Internal helpers ────────────────────────────────────────────────────────

static char *dup_str(cJSON *item) {
    if (!cJSON_IsString(item) || !item->valuestring || item->valuestring[0] == '\0')
        return NULL;
    return strdup(item->valuestring);
}

static char *join_path(const char *base, char *rel) {
    // rel is consumed (freed) here — avoids caller needing to track it
    if (!rel) return NULL;
    size_t len = strlen(base) + 1 + strlen(rel) + 1;
    char *out = malloc(len);
    if (out) snprintf(out, len, "%s/%s", base, rel);
    free(rel);
    return out;
}

static char *make_base(const char *device_id) {
    size_t len = strlen("/" LITTLEFS_LABEL "/devices/") + strlen(device_id) + 1;
    char *out = malloc(len);
    if (out) snprintf(out, len, "/" LITTLEFS_LABEL "/devices/%s", device_id);
    return out;
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────

void device_paths_free(device_paths_t *p) {
    if (!p) return;
    free(p->base);     p->base     = NULL;
    free(p->profile);  p->profile  = NULL;
    free(p->vars);     p->vars     = NULL;
    free(p->uuids);    p->uuids    = NULL;
    free(p->graphics); p->graphics = NULL;
    free(p->icon);     p->icon     = NULL;

    if (p->peripheral) {
        free(p->advertisement);      p->advertisement      = NULL;
        free(p->peripheral->entry);
        free(p->peripheral->interface);
        free(p->peripheral);
        p->peripheral = NULL;
    }
    if (p->central) {
        free(p->central->entry);
        free(p->central->menu);
        free(p->central);
        p->central = NULL;
    }
    if (p->observer) {
        free(p->observer->entry);
        free(p->observer);
        p->observer = NULL;
    }
}

void device_meta_free(device_meta_t *m) {
    if (!m) return;
    free(m->folder);      m->folder      = NULL;
    free(m->name);        m->name        = NULL;
    free(m->description); m->description = NULL;
    free(m->author);      m->author      = NULL;
    free(m->icon_url);    m->icon_url    = NULL;
    free(m->notes);       m->notes       = NULL;
    free(m->author_url);  m->author_url  = NULL;
    free(m->model_url);   m->model_url   = NULL;
    free(m->source_url);  m->source_url  = NULL;
}

// ─── Load manifest ───────────────────────────────────────────────────────────

cJSON *manifest_load(const char *device_id) {
    char path[MAX_DEVICE_PATH_LEN];
    snprintf(path, sizeof(path),
             "/" LITTLEFS_LABEL "/devices/%s/manifest.json", device_id);
    size_t sz;
    char *buf = read_json_file(path, &sz);
    if (!buf) { ESP_LOGE(TAG, "manifest not found: %s", device_id); return NULL; }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) ESP_LOGE(TAG, "manifest parse error: %s", device_id);
    return root;
}

// ─── Resolve all paths ───────────────────────────────────────────────────────

device_paths_t *manifest_resolve(const char *device_id) {
    cJSON *m = manifest_load(device_id);
    if (!m) return NULL;

    char *base = make_base(device_id);
    if (!base) { cJSON_Delete(m); return NULL; }

    device_paths_t *p = calloc(1, sizeof(device_paths_t));
    if (!p) { free(base); cJSON_Delete(m); return NULL; }
    p->base = base;

    // Shared profile
    p->profile = join_path(base, dup_str(cJSON_GetObjectItem(m, "profile")));

    p->vars = join_path(base, dup_str(cJSON_GetObjectItem(m, "vars")));

    p->uuids = join_path(base, dup_str(cJSON_GetObjectItem(m, "uuids")));

    // Assets (all optional)
    cJSON *assets = cJSON_GetObjectItem(m, "assets");
    if (assets) {
        p->graphics = join_path(base, dup_str(cJSON_GetObjectItem(assets, "graphics")));
        p->icon     = join_path(base, dup_str(cJSON_GetObjectItem(assets, "icon")));
    }

    // Roles — only allocate sub-structs for roles present in the manifest
    cJSON *roles = cJSON_GetObjectItem(m, "roles");
    if (roles) {
        cJSON *periph = cJSON_GetObjectItem(roles, "peripheral");
        if (cJSON_IsObject(periph)) {
            p->peripheral = calloc(1, sizeof(device_role_peripheral_t));
            if (p->peripheral) {
                p->peripheral->advertisement    = join_path(base, dup_str(cJSON_GetObjectItem(periph, "advertisement")));
                p->peripheral->entry     = join_path(base, dup_str(cJSON_GetObjectItem(periph, "entry")));
                p->peripheral->interface = join_path(base, dup_str(cJSON_GetObjectItem(periph, "interface")));
                cJSON *show_addr = cJSON_GetObjectItem(periph, "show_adv_address");
                p->peripheral->show_adv_address = cJSON_IsTrue(show_addr);
            }
        }

        cJSON *central = cJSON_GetObjectItem(roles, "central");
        if (cJSON_IsObject(central)) {
            p->central = calloc(1, sizeof(device_role_central_t));
            if (p->central) {
                p->central->entry = join_path(base, dup_str(cJSON_GetObjectItem(central, "entry")));
                p->central->menu  = join_path(base, dup_str(cJSON_GetObjectItem(central, "menu")));
            }
        }

        cJSON *observer = cJSON_GetObjectItem(roles, "observer");
        if (cJSON_IsObject(observer)) {
            p->observer = calloc(1, sizeof(device_role_observer_t));
            if (p->observer) {
                p->observer->entry = join_path(base, dup_str(cJSON_GetObjectItem(observer, "entry")));
            }
        }
    }

    cJSON_Delete(m);
    ESP_LOGI(TAG, "resolved paths for '%s': peripheral=%s central=%s observer=%s",
             device_id,
             p->peripheral ? "yes" : "no",
             p->central    ? "yes" : "no",
             p->observer   ? "yes" : "no");
    return p;
}

// ─── Resolve single key ───────────────────────────────────────────────────────

char *manifest_resolve_path(const char *device_id, const char *sub) {
    cJSON *m = manifest_load(device_id);
    if (!m) return NULL;

    char *base = make_base(device_id);
    if (!base) { cJSON_Delete(m); return NULL; }

    cJSON *roles  = cJSON_GetObjectItem(m, "roles");
    cJSON *assets = cJSON_GetObjectItem(m, "assets");
    char *rel = NULL;

    if      (strcmp(sub, "profile") == 0 || strcmp(sub, "ble") == 0)
        rel = dup_str(cJSON_GetObjectItem(m, "profile"));
    else if (strcmp(sub, "interface") == 0) {
        cJSON *r = roles ? cJSON_GetObjectItem(roles, "peripheral") : NULL;
        rel = dup_str(r ? cJSON_GetObjectItem(r, "interface") : NULL);
    }
    else if (strcmp(sub, "advertisement") == 0){
        cJSON *r = roles ? cJSON_GetObjectItem(roles, "peripheral") : NULL;
        rel = dup_str(r ? cJSON_GetObjectItem(r, "advertisement") : NULL);
    }
    else if (strcmp(sub, "menu") == 0) {
        cJSON *r = roles ? cJSON_GetObjectItem(roles, "central") : NULL;
        rel = dup_str(r ? cJSON_GetObjectItem(r, "menu") : NULL);
    }
    else if (strcmp(sub, "observer_entry") == 0) {
        cJSON *r = roles ? cJSON_GetObjectItem(roles, "observer") : NULL;
        rel = dup_str(r ? cJSON_GetObjectItem(r, "entry") : NULL);
    }
    else if (strcmp(sub, "icon") == 0)
        rel = dup_str(assets ? cJSON_GetObjectItem(assets, "icon") : NULL);
    else if (strcmp(sub, "graphics") == 0)
        rel = dup_str(assets ? cJSON_GetObjectItem(assets, "graphics") : NULL);

    cJSON_Delete(m);

    char *out = join_path(base, rel);  // join_path frees rel
    free(base);
    if (!out) ESP_LOGW(TAG, "key '%s' not found in manifest: %s", sub, device_id);
    return out;
}

// ─── Metadata ────────────────────────────────────────────────────────────────

device_meta_t *manifest_extract_meta(const char *device_id, cJSON *m) {
    device_meta_t *meta = calloc(1, sizeof(device_meta_t));
    if (!meta) return NULL;

    meta->folder      = strdup(device_id);
    meta->name        = dup_str(cJSON_GetObjectItem(m, "name"));
    meta->description = dup_str(cJSON_GetObjectItem(m, "description"));
    meta->author      = dup_str(cJSON_GetObjectItem(m, "author"));
    meta->notes      = dup_str(cJSON_GetObjectItem(m, "notes"));
    meta->author_url = dup_str(cJSON_GetObjectItem(m, "author_url"));
    meta->model_url  = dup_str(cJSON_GetObjectItem(m, "model_url"));
    meta->source_url = dup_str(cJSON_GetObjectItem(m, "source_url"));

    cJSON *assets = cJSON_GetObjectItem(m, "assets");
    cJSON *icon   = assets ? cJSON_GetObjectItem(assets, "icon") : NULL;
    if (cJSON_IsString(icon) && icon->valuestring[0]) {
        size_t len = strlen("/static//icon") + strlen(device_id) + 1;
        meta->icon_url = malloc(len);
        if (meta->icon_url)
            snprintf(meta->icon_url, len, "/static/%s/icon", device_id);
    }
    return meta;
}

device_meta_t *manifest_load_meta(const char *device_id) {
    cJSON *m = manifest_load(device_id);
    if (!m) return NULL;
    device_meta_t *meta = manifest_extract_meta(device_id, m);
    cJSON_Delete(m);
    return meta;
}