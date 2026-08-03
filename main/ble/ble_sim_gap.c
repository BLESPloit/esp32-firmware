#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "console/console.h"
#include "services/gap/ble_svc_gap.h"
#include "ble/ble_discovery.h"
#include "ble/ble_scan.h"
#include "ble/ble_sim_internal.h"
#include "ble/device_parser.h"
#include "graphics/graphics.h"
#include "interface/interface_sim.h"
#include "lua/lua_hook.h"
#include "common/storage.h"
#include "common/utils.h"
#include "ble/ble_sim.h"
#include "api/web_server.h"

static const char *TAG = "BLE_sim - GAP";

extern device_config_t config;  // storage.c
extern ble_server_t   *ble_server;

uint16_t sim_conn_handle = BLE_HS_CONN_HANDLE_NONE;

int ble_sim_gap_event(struct ble_gap_event *event, void *arg);


// ── Address handling ────────────────────────────────────────────────── 

static int ble_set_random_addr(uint16_t instance, ble_addr_t addr)
{
    char addr_str[18];
    snprintf(addr_str, sizeof(addr_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             addr.val[5], addr.val[4], addr.val[3],
             addr.val[2], addr.val[1], addr.val[0]);
    int rc = ble_gap_ext_adv_set_addr(instance, &addr);
    if (rc != 0) {
        WS_LOGE(TAG, "Could not set address %s: %d", addr_str, rc);
        return rc;
    }
    WS_LOGI(TAG, "Set random address %s", addr_str);
    return 0;
}

static int ble_generate_and_set_random_addr(uint16_t instance, ble_adv_params_t *p)
{
    ble_addr_t addr;
    int rc = ble_hs_id_gen_rnd(1, &addr);
    if (rc != 0) return rc;
    // write back so status broadcast can report the actual address
    snprintf(p->bd_addr, sizeof(p->bd_addr), "%02X:%02X:%02X:%02X:%02X:%02X",
             addr.val[5], addr.val[4], addr.val[3],
             addr.val[2], addr.val[1], addr.val[0]);
    return ble_set_random_addr(instance, addr);
}

// Sets a specific random address from "AA:BB:CC:DD:EE:FF" string
static int ble_set_addr_from_string(uint16_t instance, const char *addr_str)
{
    if (!addr_str) {
        WS_LOGE(TAG, "Address string is NULL");
        return -1;
    }
    int bytes[6];
    if (sscanf(addr_str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &bytes[5], &bytes[4], &bytes[3],
               &bytes[2], &bytes[1], &bytes[0]) != 6) {
        WS_LOGE(TAG, "Invalid MAC address format: %s", addr_str);
        return ESP_FAIL;
    }
    ble_addr_t addr;
    for (int i = 0; i < 6; i++) addr.val[i] = (uint8_t)bytes[i];
    addr.type = BLE_ADDR_RANDOM;    // NimBLE requires RANDOM for ext adv
    int rc = ble_gap_ext_adv_set_addr(instance, &addr);
    if (rc != 0) {
        WS_LOGE(TAG, "Could not set address %s: %d", addr_str, rc);
        return rc;
    }
    WS_LOGI(TAG, "Set address %s", addr_str);
    return ESP_OK;
}


static const char *addr_type_to_str(ble_adv_addr_type_t t)
{
    switch (t) {
        case BLE_ADV_ADDR_RANDOM_GENERATED: return "random_generated";
        case BLE_ADV_ADDR_RANDOM_SPECIFIC:  return "random_specific";
        case BLE_ADV_ADDR_PUBLIC:           return "public";
        case BLE_ADV_ADDR_PUBLIC_HARDWARE:  return "public_hardware";
        default:                            return "unknown";
    }
}

// ── Helpers: PHY/PDU type strings ──────────────────────────────────────


static uint8_t phy_to_nimble(ble_adv_phy_t phy)
{
    switch (phy) {
        case BLE_ADV_PHY_2M:    return BLE_HCI_LE_PHY_2M;
        case BLE_ADV_PHY_CODED: return BLE_HCI_LE_PHY_CODED;
        default:                return BLE_HCI_LE_PHY_1M;
    }
}

static const char *get_adv_pdu_type_str(uint16_t props)
{
    if (props & BLE_HCI_ADV_SCAN_RSP_MASK)   return "SCAN_RSP";
    if (!(props & BLE_HCI_ADV_LEGACY_MASK))  return "ADV_EXT_IND";
    bool connectable = (props & BLE_HCI_ADV_CONN_MASK)   != 0;
    bool directed    = (props & BLE_HCI_ADV_DIRECT_MASK) != 0;
    bool scannable   = (props & BLE_HCI_ADV_SCAN_MASK)   != 0;
    if (connectable && directed) return "ADV_DIRECT_IND";
    if (connectable)             return "ADV_IND";
    if (scannable)               return "ADV_SCAN_IND";
    return "ADV_NONCONN_IND";
}


// ── Buffer helpers ────────────────────────────────────────────────── 

static esp_err_t ble_adv_params_alloc_bufs(ble_adv_params_t *p,
                                            uint16_t adv_len,
                                            uint16_t rsp_len)
{
    if (adv_len > 0) {
        p->adv_data = malloc(adv_len);
        if (!p->adv_data) return ESP_ERR_NO_MEM;
    }
    if (rsp_len > 0) {
        p->scan_rsp = malloc(rsp_len);
        if (!p->scan_rsp) { free(p->adv_data); p->adv_data = NULL; return ESP_ERR_NO_MEM; }
    }
    p->owns_buffers = true;
    return ESP_OK;
}

void ble_adv_params_free_bufs(ble_adv_params_t *p)
{
    if (p->owns_buffers) {
        free(p->adv_data);
        free(p->scan_rsp);
        if (p->rotation) {
            for (uint8_t i = 0; i < p->rotation->count; i++)
                free(p->rotation->payloads[i].adv_data);
            free(p->rotation->payloads);
            free(p->rotation);
        }
    }
    p->adv_data   = NULL; p->adv_data_len = 0;
    p->scan_rsp   = NULL; p->scan_rsp_len = 0;
    p->rotation   = NULL;
    p->owns_buffers = false;
}

void ble_adv_set_free(ble_adv_set_t *set)
{
    if (!set || !set->instances) return;
    for (uint8_t i = 0; i < set->count; i++) {
        ble_adv_instance_t *inst = &set->instances[i];
        if (inst->rotation_timer) {
            xTimerStop(inst->rotation_timer, 0);
            xTimerDelete(inst->rotation_timer, 0);
            inst->rotation_timer = NULL;
        }
        ble_adv_params_free_bufs(&inst->params);
    }
    free(set->instances);
    set->instances = NULL;
    set->count = 0;
}


// ── Advertising rotation timer ────────────────────────────────────────────────── 

static void rotation_timer_cb(TimerHandle_t xTimer)
{
    ble_adv_instance_t *inst = (ble_adv_instance_t *)pvTimerGetTimerID(xTimer);
    ble_adv_params_t   *p    = &inst->params;
    ble_adv_rotation_t *rot  = p->rotation;
    if (!rot || rot->count == 0) return;

    rot->current_idx = (rot->current_idx + 1) % rot->count;
    ble_adv_payload_t *payload = &rot->payloads[rot->current_idx];

    struct os_mbuf *om = os_msys_get_pkthdr(payload->adv_data_len, 0);
    if (!om) { ESP_LOGE(TAG, "rotation: OOM"); return; }
    if (os_mbuf_append(om, payload->adv_data, payload->adv_data_len) != 0) {
        os_mbuf_free_chain(om);
        WS_LOGE(TAG, "rotation: mbuf append failed");
        return;
    }
    int rc = ble_gap_ext_adv_set_data(inst->instance, om);
    // NimBLE takes ownership of om even on failure
    if (rc != 0)
        WS_LOGE(TAG, "rotation: ble_gap_ext_adv_set_data failed %d", rc);
}


// ── Profile parsing ────────────────────────────────────────────────── 

static ble_adv_addr_type_t parse_addr_type(const char *s)
{
    if (!s) return BLE_ADV_ADDR_RANDOM_SPECIFIC;   // safe default
    if (strcmp(s, "random_generated") == 0) return BLE_ADV_ADDR_RANDOM_GENERATED;
    if (strcmp(s, "public")           == 0) return BLE_ADV_ADDR_PUBLIC;
    if (strcmp(s, "public_hardware")  == 0) return BLE_ADV_ADDR_PUBLIC_HARDWARE;    
    return BLE_ADV_ADDR_RANDOM_SPECIFIC;           // "random_specific" or unknown
}

static ble_adv_phy_t parse_phy(const char *s)
{
    if (!s) return BLE_ADV_PHY_1M;
    if (strcmp(s, "2M")    == 0) return BLE_ADV_PHY_2M;
    if (strcmp(s, "coded") == 0) return BLE_ADV_PHY_CODED;
    return BLE_ADV_PHY_1M;
}

// Returns true and fills *inst on success; false on skip/error.
// Loads all profiles (including disabled), we may want to dynamically enable them later
static bool parse_adv_profile_entry(cJSON *item, ble_adv_instance_t *inst, uint8_t instance_idx)
{
    ble_adv_params_t *p = &inst->params;
    memset(p, 0, sizeof(*p));
    inst->instance       = instance_idx;
    inst->running        = false;
    inst->rotation_timer = NULL;
    p->source            = BLE_ADV_SOURCE_PROFILE;
    p->owns_buffers      = true;

    // id
    cJSON *id = cJSON_GetObjectItem(item, "id");
    if (cJSON_IsString(id))
        strlcpy(p->id, id->valuestring, sizeof(p->id));
    else
        snprintf(p->id, sizeof(p->id), "profile%d", instance_idx);

    // GAP booleans — map directly to ble_gap_ext_adv_params
    p->legacy_pdu  = cJSON_IsTrue(cJSON_GetObjectItem(item, "legacy_pdu"));
    p->connectable = cJSON_IsTrue(cJSON_GetObjectItem(item, "connectable"));
    p->scannable   = cJSON_IsTrue(cJSON_GetObjectItem(item, "scannable"));

    // channel_map (default: all three)
    cJSON *ch = cJSON_GetObjectItem(item, "channel_map");
    p->channel_map = cJSON_IsNumber(ch) ? (uint8_t)ch->valueint : 0x07;

    // PHY
    cJSON *pphy = cJSON_GetObjectItem(item, "primary_phy");
    cJSON *sphy = cJSON_GetObjectItem(item, "secondary_phy");
    p->primary_phy   = parse_phy(cJSON_IsString(pphy) ? pphy->valuestring : NULL);
    p->secondary_phy = parse_phy(cJSON_IsString(sphy) ? sphy->valuestring : NULL);

    // tx_power (default: 127 = controller default)
    cJSON *txp = cJSON_GetObjectItem(item, "tx_power");
    p->tx_power = cJSON_IsNumber(txp) ? (int8_t)txp->valueint : 127;

    // adv_interval_ms → single value, convert to 0.625 ms units
    cJSON *itvl = cJSON_GetObjectItem(item, "adv_interval_ms");
    if (cJSON_IsNumber(itvl)) {
        p->itvl = (uint16_t)(itvl->valuedouble * 1.6f);   // ms → 0.625ms units
    } else {
        p->itvl = (uint16_t)(100 * 1.6f);                 // 100ms default
    }

    // addr_type + bd_addr
    cJSON *at = cJSON_GetObjectItem(item, "addr_type");
    p->addr_type = parse_addr_type(cJSON_IsString(at) ? at->valuestring : NULL);
    cJSON *ba = cJSON_GetObjectItem(item, "bd_addr");
    if (cJSON_IsString(ba) && strlen(ba->valuestring) > 0) {
        strlcpy(p->bd_addr, ba->valuestring, sizeof(p->bd_addr));
    } else if (ble_server->devinfo && p->addr_type != BLE_ADV_ADDR_PUBLIC_HARDWARE) {
        strlcpy(p->bd_addr, ble_server->devinfo->bd_addr, sizeof(p->bd_addr));
    } else {
        // No devinfo / public_hardware without profile bd_addr — TX addr comes from controller
        if (p->addr_type != BLE_ADV_ADDR_PUBLIC_HARDWARE)
            p->addr_type = BLE_ADV_ADDR_PUBLIC_HARDWARE;
        p->bd_addr[0] = '\0';
    }


    // static adv_data_hex
    cJSON *adv_hex = cJSON_GetObjectItem(item, "adv_data_hex");
    if (cJSON_IsString(adv_hex) && strlen(adv_hex->valuestring) > 0) {
        uint16_t len = strlen(adv_hex->valuestring) / 2;
        p->adv_data = malloc(len);
        if (!p->adv_data) goto oom;
        p->adv_data_len = hex_string_to_bytes(adv_hex->valuestring, p->adv_data, len);
    }

    // static scan_rsp_hex
    cJSON *rsp_hex = cJSON_GetObjectItem(item, "scan_rsp_hex");
    if (cJSON_IsString(rsp_hex) && strlen(rsp_hex->valuestring) > 0) {
        uint16_t len = strlen(rsp_hex->valuestring) / 2;
        p->scan_rsp = malloc(len);
        if (!p->scan_rsp) goto oom;
        p->scan_rsp_len = hex_string_to_bytes(rsp_hex->valuestring, p->scan_rsp, len);
    }

    // Optional rotation block
    cJSON *rot_json = cJSON_GetObjectItem(item, "rotation");
    if (cJSON_IsObject(rot_json)) {
        cJSON *payloads_json = cJSON_GetObjectItem(rot_json, "payloads");
        int payload_count = cJSON_IsArray(payloads_json)
                            ? cJSON_GetArraySize(payloads_json) : 0;
        if (payload_count >= 2) {
            ble_adv_rotation_t *rot = calloc(1, sizeof(ble_adv_rotation_t));
            if (!rot) goto oom;

            cJSON *iv = cJSON_GetObjectItem(rot_json, "interval_ms");
            rot->interval_ms = cJSON_IsNumber(iv) ? (uint32_t)iv->valuedouble : 1000;
            rot->count       = (uint8_t)payload_count;
            rot->current_idx = 0;
            rot->payloads    = calloc(rot->count, sizeof(ble_adv_payload_t));
            if (!rot->payloads) { free(rot); goto oom; }

            int idx = 0;
            cJSON *entry;
            cJSON_ArrayForEach(entry, payloads_json) {
                cJSON *hex = cJSON_GetObjectItem(entry, "adv_data_hex");
                if (!cJSON_IsString(hex) || strlen(hex->valuestring) == 0) {
                    WS_LOGE(TAG, "rotation payload %d missing adv_data_hex", idx);
                    // free partial and fall through without rotation
                    for (int j = 0; j < idx; j++) free(rot->payloads[j].adv_data);
                    free(rot->payloads); free(rot);
                    rot = NULL;
                    break;
                }
                uint16_t len = strlen(hex->valuestring) / 2;
                rot->payloads[idx].adv_data = malloc(len);
                if (!rot->payloads[idx].adv_data) {
                    for (int j = 0; j < idx; j++) free(rot->payloads[j].adv_data);
                    free(rot->payloads); free(rot);
                    goto oom;
                }
                rot->payloads[idx].adv_data_len =
                    hex_string_to_bytes(hex->valuestring, rot->payloads[idx].adv_data, len);
                idx++;
            }
            p->rotation = rot;

            // Always seed from payloads[0] — top-level adv_data_hex is ignored
            // when rotation is present.
            free(p->adv_data);
            uint16_t flen = rot->payloads[0].adv_data_len;
            p->adv_data = malloc(flen);
            if (!p->adv_data) goto oom;
            memcpy(p->adv_data, rot->payloads[0].adv_data, flen);
            p->adv_data_len = flen;

        } else {
            WS_LOGW(TAG, "Profile '%s': rotation needs >= 2 payloads, ignoring", p->id);
        }
    }

    return true;

oom:
    WS_LOGE(TAG, "OOM parsing profile '%s'", p->id);
    ble_adv_params_free_bufs(p);
    return false;
}

// Load advertising profiles
static esp_err_t adv_load_from_profiles(const char *device_folder)
{
    char path[256];
    snprintf(path, sizeof(path), "/" LITTLEFS_LABEL "/devices/%s/peripheral/" PROFILE_ADV_FILE, device_folder);

    size_t fsize;
    char *buf = read_json_file(path, &fsize);
    if (!buf) return ESP_ERR_NOT_FOUND;

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        const char *err = cJSON_GetErrorPtr();
        if (err) WS_LOGE(TAG, "JSON parse error near: %.20s", err);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *profiles = cJSON_GetObjectItem(root, "profiles");
    if (!cJSON_IsArray(profiles)) {
        WS_LOGE(TAG, "No profiles array in %s", path);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    // count all profiles (not just enabled)
    uint8_t total_count = cJSON_GetArraySize(profiles);
    if (total_count == 0) {
        WS_LOGW(TAG, "No profiles in %s", path);
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    ble_server->adv_set.instances = calloc(total_count, sizeof(ble_adv_instance_t));
    if (!ble_server->adv_set.instances) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    uint8_t inst_idx = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, profiles) {
        if (inst_idx >= total_count) break;
        ble_adv_instance_t *inst = &ble_server->adv_set.instances[inst_idx];

        // Parse regardless of enabled flag
        if (parse_adv_profile_entry(item, inst, inst_idx)) {

            // Store the initially_enabled flag from JSON
            inst->initially_enabled = cJSON_IsTrue(cJSON_GetObjectItem(item, "enabled"));

            inst_idx++;
        }
    }
    ble_server->adv_set.count = inst_idx;

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Loaded %d adv profiles from %s", inst_idx, path);
    return ESP_OK;
}

esp_err_t ble_adv_resolve(const char *device_folder)
{
    ble_adv_set_free(&ble_server->adv_set);

    if (adv_load_from_profiles(device_folder) == ESP_OK) return ESP_OK;

    // Devinfo fallback — only valid when a ble.json profile was loaded
    if (!ble_server->devinfo) {
        WS_LOGE(TAG, "ble_adv_resolve: no adv profiles and no devinfo — cannot advertise");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Fallback: single instance borrowing from devinfo
    ble_server->adv_set.instances = calloc(1, sizeof(ble_adv_instance_t));
    if (!ble_server->adv_set.instances) return ESP_ERR_NO_MEM;
    ble_server->adv_set.count = 1;

    ble_adv_instance_t *inst = &ble_server->adv_set.instances[0];
    inst->instance       = 0;
    inst->running        = false;
    inst->rotation_timer = NULL;

    ble_adv_params_t   *p  = &inst->params;
    ble_devinfo_t      *di = ble_server->devinfo;
    memset(p, 0, sizeof(*p));
    p->source      = BLE_ADV_SOURCE_DEVINFO;
    p->owns_buffers = false;   // borrowed from devinfo

    p->adv_data     = di->adv_data;
    p->adv_data_len = di->adv_data_len;
    p->scan_rsp     = di->scan_rsp;
    p->scan_rsp_len = di->scan_rsp_len;

    // optional fallback to "deviceinfo" from ble.json (we should use peripheral/adv.json instead)
    if (strcmp(di->pdu_type, "ADV_EXT_IND") == 0) {
        p->legacy_pdu  = false;
        p->connectable = true; // hardcoded for now, should work for majority of cases
        p->scannable   = false;
        // need to make sure the scan response is empty for extended otherwise advertising won't start
        p->scan_rsp     = NULL;
        p->scan_rsp_len = 0;
    } else if (strcmp(di->pdu_type, "ADV_IND") == 0) {
        p->legacy_pdu  = true;
        p->connectable = true;
        p->scannable   = (di->scan_rsp_len > 0);
    } else if (strcmp(di->pdu_type, "ADV_SCAN_IND") == 0) {
        p->legacy_pdu  = true;
        p->connectable = false;
        p->scannable   = true;
    } else if (strcmp(di->pdu_type, "ADV_NONCONN_IND") == 0) {
        p->legacy_pdu  = true;
        p->connectable = false;
        p->scannable   = false;
    } else {
        // safe default
        p->legacy_pdu  = true;
        p->connectable = true;
        p->scannable   = (di->scan_rsp_len > 0);
    }

    p->channel_map = 0x07;
    p->primary_phy   = BLE_ADV_PHY_1M;
    p->secondary_phy = BLE_ADV_PHY_1M;
    p->tx_power    = 127;
    p->itvl        = (uint16_t)(100 * 1.6f);
    p->addr_type   = BLE_ADV_ADDR_RANDOM_SPECIFIC;
    strlcpy(p->id,      "devinfo",    sizeof(p->id));
    strlcpy(p->bd_addr, di->bd_addr,  sizeof(p->bd_addr));

    ESP_LOGI(TAG, "Adv resolved: devinfo fallback (%d bytes)", p->adv_data_len);
    return ESP_OK;
}


cJSON *ble_adv_set_to_json(void)
{
    if (!ble_server || !ble_server->adv_set.instances || ble_server->adv_set.count == 0)
        return NULL;

    cJSON *arr = cJSON_CreateArray();
    for (uint8_t i = 0; i < ble_server->adv_set.count; i++) {
        ble_adv_instance_t *inst = &ble_server->adv_set.instances[i];
        ble_adv_params_t   *p    = &inst->params;

        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id",       p->id);
        cJSON_AddNumberToObject(obj, "instance", inst->instance);
        cJSON_AddBoolToObject  (obj, "running",  inst->running);
        cJSON_AddStringToObject(obj, "bd_addr",  p->bd_addr);
        cJSON_AddStringToObject(obj, "addr_type", addr_type_to_str(p->addr_type)); 
        cJSON_AddItemToArray(arr, obj);
    }
    return arr;
}

// ── Advertising start/stop ────────────────────────────────────────────────── 

esp_err_t ble_adv_instance_start(ble_adv_instance_t *inst)
{
    ble_adv_params_t *p = &inst->params;

    if (ble_gap_ext_adv_active(inst->instance)) {
        ESP_LOGI(TAG, "Instance %d (%s) already advertising", inst->instance, p->id);
        return ESP_OK;
    }

    // --- Resolve own_addr_type before configure ---
    uint8_t own_addr_type;
    switch (p->addr_type) {
        case BLE_ADV_ADDR_PUBLIC_HARDWARE:
            own_addr_type = BLE_OWN_ADDR_PUBLIC;
            break;
        default:
            own_addr_type = BLE_OWN_ADDR_RANDOM;
            break;
    }

    struct ble_gap_ext_adv_params adv_params = {
        .legacy_pdu    = p->legacy_pdu,
        .connectable   = p->connectable,
        .scannable     = p->scannable,
        .directed      = 0,
        .channel_map   = p->channel_map,
        .own_addr_type = own_addr_type,
        .filter_policy = BLE_HCI_ADV_FILT_NONE,
        .primary_phy   = phy_to_nimble(p->primary_phy),
        .secondary_phy = phy_to_nimble(p->secondary_phy),
        .tx_power      = p->tx_power,
        .itvl_min      = p->itvl,
        .itvl_max      = p->itvl,
        .sid           = inst->instance,
    };

    int rc = ble_gap_ext_adv_configure(inst->instance, &adv_params, NULL,
                                       ble_sim_gap_event, NULL);
    if (rc != 0) {
        WS_LOGE(TAG, "ble_gap_ext_adv_configure failed rc=%d "
                 "(legacy=%d conn=%d scan=%d pphy=%d sphy=%d)",
                 rc, adv_params.legacy_pdu, adv_params.connectable,
                 adv_params.scannable, adv_params.primary_phy, adv_params.secondary_phy);
        return ESP_FAIL;
    }

    // Set address
    switch (p->addr_type) {
        case BLE_ADV_ADDR_RANDOM_GENERATED:
            rc = ble_generate_and_set_random_addr(inst->instance, p);
            break;
        case BLE_ADV_ADDR_RANDOM_SPECIFIC:
            rc = ble_set_addr_from_string(inst->instance, p->bd_addr);
            break;
        case BLE_ADV_ADDR_PUBLIC:
            ESP_LOGW(TAG, "Profile '%s': public addr not spoofable, using bd_addr as random", p->id);
            rc = ble_set_addr_from_string(inst->instance, p->bd_addr);
            break;
        case BLE_ADV_ADDR_PUBLIC_HARDWARE: {
            uint8_t hw_addr[6];
            rc = 0;
            if (ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, hw_addr, NULL) == 0) {
                snprintf(p->bd_addr, sizeof(p->bd_addr),
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         hw_addr[5], hw_addr[4], hw_addr[3],
                         hw_addr[2], hw_addr[1], hw_addr[0]);
            }
            break;
        }
    }
    if (rc != 0) {
        WS_LOGE(TAG, "Failed to set address for profile '%s', aborting!", p->id);
        return ESP_FAIL;
    }

    // Set adv data 
    struct os_mbuf *data = os_msys_get_pkthdr(p->adv_data_len, 0);
    if (!data) {
        WS_LOGE(TAG, "Failed to alloc adv data mbuf (instance %d)", inst->instance);
        return ESP_ERR_NO_MEM;
    }
    rc = os_mbuf_append(data, p->adv_data, p->adv_data_len);
    if (rc != 0) { os_mbuf_free_chain(data); return rc; }
    rc = ble_gap_ext_adv_set_data(inst->instance, data);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_ext_adv_set_data failed rc=%d (instance=%d "
                 "legacy=%d conn=%d scan=%d adv_data_len=%d)",
                 rc, inst->instance,
                 p->legacy_pdu, p->connectable, p->scannable, p->adv_data_len);
        return rc;
    }

    // Set scan response (if present)
    if (p->scan_rsp && p->scan_rsp_len > 0) {
        struct os_mbuf *rsp = os_msys_get_pkthdr(p->scan_rsp_len, 0);
        if (!rsp) {
            ESP_LOGE(TAG, "Failed to alloc scan rsp mbuf (instance %d)", inst->instance);
            return ESP_ERR_NO_MEM;
        }
        rc = os_mbuf_append(rsp, p->scan_rsp, p->scan_rsp_len);
        if (rc != 0) { os_mbuf_free_chain(rsp); return rc; }
        rc = ble_gap_ext_adv_rsp_set_data(inst->instance, rsp);
        if (rc != 0) {
            ESP_LOGE(TAG, "Error setting scan response %d", rc);
            return rc;
        }
    }

    // Start
    rc = ble_gap_ext_adv_start(inst->instance, 0, 0);
    if (rc != 0) {
        WS_LOGE(TAG, "Error starting adv (instance %d): %d", inst->instance, rc);
        return rc;
    }
    inst->running = true;
    WS_LOGI(TAG, "Instance %d (%s) started — %s", inst->instance, p->id,
             p->rotation ? "rotating" : "static");

    // Start rotation timer if needed
    if (p->rotation && p->rotation->count >= 2) {
        TickType_t period = pdMS_TO_TICKS(p->rotation->interval_ms);
        inst->rotation_timer = xTimerCreate(
            "adv_rot", period, pdTRUE,
            (void *)inst, rotation_timer_cb);
        if (inst->rotation_timer)
            xTimerStart(inst->rotation_timer, 0);
        else
            ESP_LOGE(TAG, "Failed to create rotation timer for instance %d", inst->instance);
    }

    return ESP_OK;
}

esp_err_t ble_start_advertising(void)
{
    if (!ble_server->adv_set.instances || ble_server->adv_set.count == 0) {
        WS_LOGE(TAG, "adv_set not resolved — call ble_adv_resolve first");
        return ESP_ERR_INVALID_STATE;
    }

    for (uint8_t i = 0; i < ble_server->adv_set.count; i++) {
        ble_adv_instance_t *inst = &ble_server->adv_set.instances[i];

        if (!inst->initially_enabled) {
            WS_LOGI(TAG, "Skipping disabled profile '%s' (instance %d)",
                     inst->params.id, inst->instance);
            continue;
        }

        esp_err_t err = ble_adv_instance_start(inst);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}


bool is_currently_advertising(void)
{
    return ble_gap_adv_active();
}

static void ble_sim_maybe_restart_advertising(const char *event_name)
{
    (void)event_name;
    if (ble_sim_teardown_active()) {
        return;
    }
    if (!ble_server) {
        return;
    }
    ble_start_advertising();
}


// ── BLE simulation GAP event handler ────────────────────────────────────────────────── 

int ble_sim_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        WS_LOGI(TAG, "connection %s; status=%d",
                 event->connect.status == 0 ? "established" : "failed",
                 event->connect.status);
        if (event->connect.status == 0) {
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            assert(rc == 0);
            ble_print_conn_desc(&desc);

            // Stop all rotation timers while connected
            for (uint8_t i = 0; i < ble_server->adv_set.count; i++) {
                ble_adv_instance_t *inst = &ble_server->adv_set.instances[i];
                if (inst->rotation_timer)
                    xTimerStop(inst->rotation_timer, 0);
            }

            if (ble_server->pairing_info &&
                ble_server->pairing_info->initiate_pairing_on_connection) {
                WS_LOGI(TAG, "Initiating pairing immediately");
                rc = ble_gap_security_initiate(event->connect.conn_handle);
                if (rc != 0)
                    WS_LOGE(TAG, "Failed to initiate security: %d", rc);
            }

            sim_conn_handle = event->connect.conn_handle;
            lua_call_handler_async("on_connected", NULL);
        } else {
            // Connection failed — resume advertising
            sim_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ble_sim_maybe_restart_advertising("CONNECT_failed");
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        WS_LOGI(TAG, "disconnect; reason=%d", event->disconnect.reason);
        ble_print_conn_desc(&event->disconnect.conn);

        // Restart rotation timers
        for (uint8_t i = 0; i < ble_server->adv_set.count; i++) {
            ble_adv_instance_t *inst = &ble_server->adv_set.instances[i];
            if (inst->rotation_timer)
                xTimerStart(inst->rotation_timer, 0);
        }
        ble_sim_maybe_restart_advertising("DISCONNECT");
        sim_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        lua_call_handler_async("on_disconnected", NULL);
        break;

    case BLE_GAP_EVENT_CONN_UPDATE:
        WS_LOGI(TAG, "connection updated; status=%d", event->conn_update.status);
        rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
        assert(rc == 0);
        ble_print_conn_desc(&desc);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        WS_LOGI(TAG, "conn update request — accepting");
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        WS_LOGW(TAG, "advertise complete; reason=%d", event->adv_complete.reason);
        ble_sim_maybe_restart_advertising("ADV_COMPLETE");
        break;

    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        WS_LOGI(TAG, "PHY update complete");
        return 0;

    // SMP / Security events
    case BLE_GAP_EVENT_PASSKEY_ACTION:
    case BLE_GAP_EVENT_ENC_CHANGE:
    case BLE_GAP_EVENT_REPEAT_PAIRING:
    case BLE_GAP_EVENT_IDENTITY_RESOLVED:
    case BLE_GAP_EVENT_PARING_COMPLETE:
        return ble_sim_smp_event(event, arg);

    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint16_t data_len = OS_MBUF_PKTLEN(event->notify_rx.om);
        WS_LOGI(TAG, "notify_rx; conn=%d attr=%d len=%d indication=%d",
                 event->notify_rx.conn_handle, event->notify_rx.attr_handle,
                 data_len, event->notify_rx.indication);
        if (data_len > 0) {
            uint8_t data[data_len];
            os_mbuf_copydata(event->notify_rx.om, 0, data_len, data);
            ESP_LOG_BUFFER_HEX(TAG, data, data_len);
        }
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_TX: {
        WS_LOGI(TAG, "notify_tx; conn=%d attr=%d status=%d indication=%d",
                 event->notify_tx.conn_handle, event->notify_tx.attr_handle,
                 event->notify_tx.status, event->notify_tx.indication);
        if (event->notify_tx.status == 0) {
            struct os_mbuf *om;
            rc = ble_att_svr_read_local(event->notify_tx.attr_handle, &om);
            if (rc == 0) {
                uint16_t data_len = OS_MBUF_PKTLEN(om);
                if (data_len > 0) {
                    uint8_t data[data_len];
                    os_mbuf_copydata(om, 0, data_len, data);
                    ESP_LOG_BUFFER_HEX(TAG, data, data_len);
                }
                os_mbuf_free_chain(om);
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_SUBSCRIBE: {
        WS_LOGI(TAG, "subscribe; conn=%d attr=%d reason=%d "
                 "prev_notify=%d cur_notify=%d prev_indicate=%d cur_indicate=%d",
                 event->subscribe.conn_handle, event->subscribe.attr_handle,
                 event->subscribe.reason,
                 event->subscribe.prev_notify, event->subscribe.cur_notify,
                 event->subscribe.prev_indicate, event->subscribe.cur_indicate);

        const char *action =
            (event->subscribe.cur_notify == 0 && event->subscribe.cur_indicate == 0)
                ? "unsubscribe"
                : "subscribe";

        char svc_uuid[37] = {0};
        char chr_uuid[37] = {0};
        ble_sim_lookup_chr_by_val_handle(event->subscribe.attr_handle, svc_uuid, chr_uuid);

        web_ble_sim_trace_emit_subscribe(
            event->subscribe.conn_handle,
            event->subscribe.attr_handle,
            svc_uuid,
            chr_uuid,
            action,
            (uint8_t)event->subscribe.reason,
            (uint8_t)event->subscribe.prev_notify,
            (uint8_t)event->subscribe.cur_notify,
            (uint8_t)event->subscribe.prev_indicate,
            (uint8_t)event->subscribe.cur_indicate);

        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        WS_LOGI(TAG, "mtu update; conn=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        return 0;

    default:
        WS_LOGW(TAG, "unhandled GAP event %d", event->type);
        return 0;
    }
    return 0;
}
