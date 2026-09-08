#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "cJSON.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"

#include "api/web_server.h"
#include "ble/ble_discovery.h"
#include "ble/ble_central.h"

static const char *TAG = "BLE central - SMP";

#define PAIR_IO_TIMEOUT_MS 25000

const char *get_pairing_strategy_name(pairing_strategy_t strategy);
const char *get_pairing_error_string(int status);
const char *get_sm_error_string(int sm_err);

typedef struct {
    bool auto_on_auth;
    pairing_strategy_t strategy;
    uint32_t pin;
    bool pin_set;
    central_pin_policy_t pin_policy;
} central_smp_cfg_t;

static central_smp_cfg_t s_cfg = {
    .auto_on_auth = true,
    .strategy = PAIRING_STRATEGY_LEGACY_JUST_WORKS,
    .pin = 0,
    .pin_set = false,
    .pin_policy = CENTRAL_PIN_POLICY_CONFIGURED_ELSE_PROMPT,
};

static bool s_pairing;
static bool s_waiting_io;
static bool s_terminal_sent;
static uint8_t s_pending_io_action;
static uint16_t s_pair_conn;
static TimerHandle_t s_io_timer;

static pairing_strategy_t effective_strategy(void)
{
    if (s_cfg.strategy == PAIRING_STRATEGY_AUTO) {
        return PAIRING_STRATEGY_LEGACY_JUST_WORKS;
    }
    return s_cfg.strategy;
}

static void configure_security_for_strategy(pairing_strategy_t strategy)
{
    strategy = (strategy == PAIRING_STRATEGY_AUTO)
                   ? PAIRING_STRATEGY_LEGACY_JUST_WORKS
                   : strategy;

    ESP_LOGI(TAG, "Configuring security for strategy: %s",
             get_pairing_strategy_name(strategy));

    switch (strategy) {
    case PAIRING_STRATEGY_LEGACY_JUST_WORKS:
        ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
        ble_hs_cfg.sm_bonding = 1;
        ble_hs_cfg.sm_mitm = 0;
        ble_hs_cfg.sm_sc = 0;
        ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
        ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
        break;
    case PAIRING_STRATEGY_SC_JUST_WORKS:
        ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
        ble_hs_cfg.sm_bonding = 1;
        ble_hs_cfg.sm_mitm = 0;
        ble_hs_cfg.sm_sc = 1;
        ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
        ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
        break;
    case PAIRING_STRATEGY_LEGACY_PIN:
        ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_ONLY;
        ble_hs_cfg.sm_bonding = 1;
        ble_hs_cfg.sm_mitm = 1;
        ble_hs_cfg.sm_sc = 0;
        ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
        ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
        break;
    case PAIRING_STRATEGY_SC_PIN:
        ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_ONLY;
        ble_hs_cfg.sm_bonding = 1;
        ble_hs_cfg.sm_mitm = 1;
        ble_hs_cfg.sm_sc = 1;
        ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
        ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
        break;
    default:
        ESP_LOGW(TAG, "Unknown strategy, using Legacy Just Works");
        ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
        ble_hs_cfg.sm_bonding = 1;
        ble_hs_cfg.sm_mitm = 0;
        ble_hs_cfg.sm_sc = 0;
        ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
        ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
        break;
    }
}

static void smp_broadcast(const char *event, uint16_t conn_handle, int status,
                          const char *detail, const char *action,
                          bool has_passkey, uint32_t passkey,
                          int timeout_ms,
                          const char *svc, const char *chr,
                          bool has_seq, uint32_t seq)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }

    cJSON_AddStringToObject(root, "type", "smp");
    cJSON_AddStringToObject(root, "event", event ? event : "unknown");
    cJSON_AddNumberToObject(root, "conn_handle", conn_handle);
    cJSON_AddNumberToObject(root, "status", status);
    cJSON_AddNumberToObject(root, "strategy", effective_strategy());
    cJSON_AddNumberToObject(root, "pin_policy", s_cfg.pin_policy);
    if (detail) {
        cJSON_AddStringToObject(root, "detail", detail);
    }
    if (action) {
        cJSON_AddStringToObject(root, "action", action);
    }
    if (has_passkey) {
        cJSON_AddNumberToObject(root, "passkey", passkey);
    }
    if (timeout_ms > 0) {
        cJSON_AddNumberToObject(root, "timeout_ms", timeout_ms);
    }
    if (svc) {
        cJSON_AddStringToObject(root, "svc", svc);
    }
    if (chr) {
        cJSON_AddStringToObject(root, "chr", chr);
    }
    if (has_seq) {
        cJSON_AddNumberToObject(root, "seq", seq);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        websocket_broadcast_json_transient(json);
        free(json);
    }
}

static void smp_broadcast_simple(const char *event, uint16_t conn_handle,
                                 int status, const char *detail)
{
    smp_broadcast(event, conn_handle, status, detail, NULL,
                  false, 0, 0, NULL, NULL, false, 0);
}

static void io_timer_stop(void)
{
    if (s_io_timer) {
        xTimerStop(s_io_timer, 0);
    }
}

static void pairing_idle(void)
{
    io_timer_stop();
    s_pairing = false;
    s_waiting_io = false;
    s_pending_io_action = BLE_SM_IOACT_NONE;
}

static void emit_terminal(const char *event, uint16_t conn_handle, int status,
                          const char *detail, bool sm_done)
{
    if (!s_terminal_sent) {
        s_terminal_sent = true;
        smp_broadcast_simple(event, conn_handle, status, detail);
    }
    io_timer_stop();
    s_waiting_io = false;
    if (sm_done) {
        s_pairing = false;
        s_pending_io_action = BLE_SM_IOACT_NONE;
    }
}

static void io_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    if (!s_waiting_io || s_terminal_sent) {
        return;
    }
    ESP_LOGW(TAG, "pair_io timeout");
    emit_terminal("pairing_cancelled", s_pair_conn, -1, "timeout", true);
}

static void io_timer_start(void)
{
    uint32_t ticks = pdMS_TO_TICKS(PAIR_IO_TIMEOUT_MS);
    if (ticks == 0) {
        ticks = 1;
    }
    if (s_io_timer == NULL) {
        s_io_timer = xTimerCreate("cent_smp_io", ticks, pdFALSE, NULL, io_timer_cb);
        if (s_io_timer == NULL) {
            ESP_LOGE(TAG, "Failed to create pair_io timer");
            return;
        }
    } else {
        xTimerStop(s_io_timer, 0);
    }
    if (xTimerChangePeriod(s_io_timer, ticks, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to arm pair_io timer");
    }
}

void ble_central_smp_reset(void)
{
    pairing_idle();
    s_terminal_sent = false;
    s_pair_conn = BLE_HS_CONN_HANDLE_NONE;
    s_cfg.auto_on_auth = true;
    s_cfg.strategy = PAIRING_STRATEGY_LEGACY_JUST_WORKS;
    s_cfg.pin = 0;
    s_cfg.pin_set = false;
    s_cfg.pin_policy = CENTRAL_PIN_POLICY_CONFIGURED_ELSE_PROMPT;
    ble_sm_set_pairing_rsp_cb(NULL, NULL);
}

static void central_pairing_rsp_cb(uint16_t conn_handle,
                                   const struct ble_sm_pairing_params *rsp,
                                   void *arg)
{
    (void)arg;
    uint16_t ours = ble_central_conn_handle();

    /* Handle 0 is valid (first NimBLE connection). NONE is 0xFFFF. */
    if (ours == BLE_HS_CONN_HANDLE_NONE || conn_handle != ours) {
        ESP_LOGD(TAG, "Pairing response ignored (handle=%u, ours=%u)",
                 (unsigned)conn_handle, (unsigned)ours);
        return;
    }
    if (!rsp) {
        return;
    }

    ESP_LOGI(TAG, "Pairing Response: io_cap=%d authreq=0x%02x max_key=%d "
             "init_kd=0x%02x resp_kd=0x%02x",
             rsp->io_cap, rsp->authreq, rsp->max_enc_key_size,
             rsp->init_key_dist, rsp->resp_key_dist);
    smp_broadcast_simple("pairing_response_received", conn_handle, 0,
                         get_pairing_strategy_name(effective_strategy()));
}

void ble_central_smp_claim_pairing_rsp_cb(void)
{
    ble_sm_set_pairing_rsp_cb(central_pairing_rsp_cb, NULL);
}

void ble_central_smp_apply_json(const cJSON *json)
{
    if (!json) {
        return;
    }

    const cJSON *j_auto = cJSON_GetObjectItemCaseSensitive(json, "auto_on_auth");
    if (cJSON_IsBool(j_auto)) {
        s_cfg.auto_on_auth = cJSON_IsTrue(j_auto);
    }

    const cJSON *j_strat = cJSON_GetObjectItemCaseSensitive(json, "strategy");
    if (cJSON_IsNumber(j_strat)) {
        int v = j_strat->valueint;
        if (v >= PAIRING_STRATEGY_LEGACY_JUST_WORKS && v <= PAIRING_STRATEGY_AUTO) {
            s_cfg.strategy = (pairing_strategy_t)v;
        }
    }

    const cJSON *j_pin = cJSON_GetObjectItemCaseSensitive(json, "pin");
    if (cJSON_IsNumber(j_pin)) {
        int v = j_pin->valueint;
        if (v < 0) {
            v = 0;
        }
        if (v > 999999) {
            v = 999999;
        }
        s_cfg.pin = (uint32_t)v;
        s_cfg.pin_set = true;
    }

    const cJSON *j_pol = cJSON_GetObjectItemCaseSensitive(json, "pin_policy");
    if (cJSON_IsNumber(j_pol)) {
        int v = j_pol->valueint;
        if (v == CENTRAL_PIN_POLICY_PROMPT ||
            v == CENTRAL_PIN_POLICY_CONFIGURED_ELSE_PROMPT) {
            s_cfg.pin_policy = (central_pin_policy_t)v;
        }
    }

    ESP_LOGI(TAG, "SMP config: auto_on_auth=%d strategy=%d pin_set=%d pin=%06lu pin_policy=%d",
             s_cfg.auto_on_auth, s_cfg.strategy, s_cfg.pin_set,
             (unsigned long)s_cfg.pin, s_cfg.pin_policy);
}

int ble_central_smp_initiate(uint16_t conn_handle)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "pair: no connection");
        return BLE_HS_ENOTCONN;
    }
    if (s_pairing) {
        ESP_LOGW(TAG, "pair: already in progress");
        return BLE_HS_EALREADY;
    }

    configure_security_for_strategy(s_cfg.strategy);
    ble_central_smp_claim_pairing_rsp_cb();
    s_pairing = true;
    s_waiting_io = false;
    s_terminal_sent = false;
    s_pair_conn = conn_handle;
    s_pending_io_action = BLE_SM_IOACT_NONE;

    send_update_central_status_to_ws("pairing");
    smp_broadcast_simple("pairing_initiated", conn_handle, 0,
                         get_pairing_strategy_name(effective_strategy()));

    int rc = ble_gap_security_initiate(conn_handle);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_security_initiate failed: %d", rc);
        emit_terminal("pairing_failed", conn_handle, rc, "initiate_failed", true);
        return rc;
    }
    ESP_LOGI(TAG, "Pairing initiated; strategy=%s",
             get_pairing_strategy_name(effective_strategy()));
    return 0;
}

void ble_central_smp_on_auth_error(uint16_t conn_handle, int host_status,
                                   const char *svc, const char *chr, uint32_t seq)
{
    smp_broadcast("pairing_needed", conn_handle, host_status, "insufficient_auth",
                  NULL, false, 0, 0, svc ? svc : "", chr ? chr : "", true, seq);

    if (!s_cfg.auto_on_auth) {
        ESP_LOGI(TAG, "Auth error %d; auto_on_auth disabled", host_status);
        return;
    }
    if (s_pairing) {
        ESP_LOGI(TAG, "Auth error %d; pairing already in progress", host_status);
        return;
    }
    ble_central_smp_initiate(conn_handle);
}

void ble_central_smp_on_disconnect(uint16_t conn_handle, int reason)
{
    io_timer_stop();
    if (s_pairing && !s_terminal_sent) {
        emit_terminal("pairing_failed", conn_handle, reason, "disconnected", true);
    } else {
        pairing_idle();
    }
    s_pair_conn = BLE_HS_CONN_HANDLE_NONE;
}

static int inject_passkey(uint16_t conn_handle, uint8_t action, uint32_t passkey)
{
    struct ble_sm_io io = {0};
    io.action = action;
    io.passkey = passkey;
    int rc = ble_sm_inject_io(conn_handle, &io);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_sm_inject_io passkey failed: %d", rc);
    }
    return rc;
}

static int inject_numcmp(uint16_t conn_handle, int accept)
{
    struct ble_sm_io io = {0};
    io.action = BLE_SM_IOACT_NUMCMP;
    io.numcmp_accept = accept ? 1 : 0;
    int rc = ble_sm_inject_io(conn_handle, &io);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_sm_inject_io numcmp failed: %d", rc);
    }
    return rc;
}

int ble_central_smp_pair_io(uint16_t conn_handle, bool has_passkey, uint32_t passkey,
                            bool has_accept, bool accept, bool cancel)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        conn_handle = s_pair_conn;
    }
    if (!s_waiting_io || s_terminal_sent) {
        ESP_LOGW(TAG, "pair_io: not waiting for IO");
        return BLE_HS_EINVAL;
    }
    if (conn_handle != s_pair_conn) {
        ESP_LOGW(TAG, "pair_io: conn_handle mismatch");
        return BLE_HS_EINVAL;
    }

    if (cancel) {
        io_timer_stop();
        s_waiting_io = false;
        if (s_pending_io_action == BLE_SM_IOACT_NUMCMP) {
            inject_numcmp(conn_handle, 0);
        }
        emit_terminal("pairing_cancelled", conn_handle, -1, "cancel", true);
        return 0;
    }

    int rc = BLE_HS_EINVAL;
    if (s_pending_io_action == BLE_SM_IOACT_INPUT) {
        if (!has_passkey) {
            ESP_LOGW(TAG, "pair_io: missing passkey");
            return BLE_HS_EINVAL;
        }
        rc = inject_passkey(conn_handle, BLE_SM_IOACT_INPUT, passkey);
        if (rc == 0) {
            smp_broadcast_simple("pin_injected", conn_handle, 0, "input");
        }
    } else if (s_pending_io_action == BLE_SM_IOACT_NUMCMP) {
        if (!has_accept) {
            ESP_LOGW(TAG, "pair_io: missing accept");
            return BLE_HS_EINVAL;
        }
        rc = inject_numcmp(conn_handle, accept);
        if (rc == 0) {
            smp_broadcast_simple(accept ? "numeric_comparison_confirmed"
                                        : "numeric_comparison_rejected",
                                 conn_handle, 0,
                                 accept ? "accepted" : "rejected");
        }
    } else {
        ESP_LOGW(TAG, "pair_io: unexpected pending action %d", s_pending_io_action);
        return BLE_HS_EINVAL;
    }

    if (rc == 0) {
        io_timer_stop();
        s_waiting_io = false;
    } else {
        emit_terminal("pairing_failed", conn_handle, rc, "inject_failed", true);
    }
    return rc;
}

static void wait_for_io(uint16_t conn_handle, uint8_t action, const char *action_name,
                        bool has_passkey, uint32_t passkey)
{
    s_waiting_io = true;
    s_pending_io_action = action;
    io_timer_start();
    smp_broadcast("passkey_action", conn_handle, 0, action_name, action_name,
                  has_passkey, passkey, PAIR_IO_TIMEOUT_MS, NULL, NULL, false, 0);
}

int ble_central_smp_handle_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        uint8_t action = event->passkey.params.action;
        uint16_t conn = event->passkey.conn_handle;
        ESP_LOGI(TAG, "PASSKEY_ACTION action=%d conn=0x%04x", action, conn);

        switch (action) {
        case BLE_SM_IOACT_NONE:
            smp_broadcast("passkey_action", conn, 0, "just_works", "just_works",
                          false, 0, 0, NULL, NULL, false, 0);
            break;

        case BLE_SM_IOACT_DISP: {
            uint32_t pk = event->passkey.params.numcmp;
            smp_broadcast("passkey_action", conn, 0, "display", "display",
                          true, pk, 0, NULL, NULL, false, 0);
            rc = inject_passkey(conn, BLE_SM_IOACT_DISP, pk);
            if (rc != 0) {
                emit_terminal("pairing_failed", conn, rc, "display_inject_failed", true);
            }
            break;
        }

        case BLE_SM_IOACT_NUMCMP:
            wait_for_io(conn, BLE_SM_IOACT_NUMCMP, "numeric_comparison",
                        true, event->passkey.params.numcmp);
            break;

        case BLE_SM_IOACT_INPUT:
            if (s_cfg.pin_policy == CENTRAL_PIN_POLICY_CONFIGURED_ELSE_PROMPT &&
                s_cfg.pin_set) {
                smp_broadcast("passkey_action", conn, 0, "input", "input",
                              false, 0, 0, NULL, NULL, false, 0);
                rc = inject_passkey(conn, BLE_SM_IOACT_INPUT, s_cfg.pin);
                if (rc != 0) {
                    emit_terminal("pairing_failed", conn, rc, "pin_inject_failed", true);
                } else {
                    ESP_LOGI(TAG, "Injected configured PIN %06lu",
                             (unsigned long)s_cfg.pin);
                    smp_broadcast_simple("pin_injected", conn, 0,
                                         get_pairing_strategy_name(effective_strategy()));
                }
            } else {
                wait_for_io(conn, BLE_SM_IOACT_INPUT, "input", false, 0);
            }
            break;

        case BLE_SM_IOACT_OOB:
            smp_broadcast("passkey_action", conn, -1, "oob_not_supported", "oob",
                          false, 0, 0, NULL, NULL, false, 0);
            emit_terminal("pairing_failed", conn, -1, "oob_not_supported", true);
            break;

        default:
            emit_terminal("pairing_failed", conn, action, "unknown_passkey_action", true);
            break;
        }
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (!s_pairing) {
            return 0;
        }
        ESP_LOGI(TAG, "ENC_CHANGE status=%d", event->enc_change.status);
        if (event->enc_change.status == 0) {
            emit_terminal("pairing_complete", event->enc_change.conn_handle, 0, "success", true);
        } else {
            emit_terminal("encryption_failed", event->enc_change.conn_handle,
                          event->enc_change.status,
                          get_pairing_error_string(event->enc_change.status), true);
        }
        send_update_central_status_to_ws("connected");
        return 0;

    case BLE_GAP_EVENT_PARING_COMPLETE:
        if (!s_pairing) {
            return 0;
        }
        ESP_LOGI(TAG, "PAIRING_COMPLETE status=%d", event->pairing_complete.status);
        if (event->pairing_complete.status == 0) {
            emit_terminal("pairing_complete", event->pairing_complete.conn_handle,
                          0, "success", true);
        } else {
            emit_terminal("pairing_complete", event->pairing_complete.conn_handle,
                          event->pairing_complete.status,
                          get_sm_error_string(event->pairing_complete.status), true);
        }
        send_update_central_status_to_ws("connected");
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        ESP_LOGI(TAG, "REPEAT_PAIRING conn=0x%04x", event->repeat_pairing.conn_handle);
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc == 0) {
            rc = ble_store_util_delete_peer(&desc.peer_id_addr);
            if (rc != 0) {
                ESP_LOGW(TAG, "delete_peer failed: %d, trying unpair", rc);
                ble_gap_unpair(&desc.peer_id_addr);
            } else {
                ESP_LOGI(TAG, "Deleted old bond, retrying pairing");
            }
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_IDENTITY_RESOLVED:
        ESP_LOGI(TAG, "Identity resolved conn=0x%04x",
                 event->identity_resolved.conn_handle);
        return 0;

    default:
        break;
    }
    return 0;
}
