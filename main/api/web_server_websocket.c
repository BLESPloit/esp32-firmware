#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h> 
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "cJSON.h"

#include "interface/interface_central.h"
#include "interface/interface_sim.h"
#include "common/storage.h"
#include "api/web_server.h"
#include "api/web_server_internal.h"

static const char *TAG = "web server websocket";

extern device_config_t config;

static int ws_client_fds[MAX_WS_CLIENTS] = {0};
static SemaphoreHandle_t ws_clients_mutex = NULL;
static SemaphoreHandle_t ws_send_mutex = NULL;

#define WS_FIN_BIT  0x80U
#define WS_MASK_BIT 0x80U


// Maintain state for websocket
typedef struct {
    char id[24];
    char json[180];
    bool active;
} element_state_t;  

typedef struct {
    element_state_t elements[MAX_STATE_ELEMENTS];
    char background_cmd[100];
    SemaphoreHandle_t mutex;
} smart_state_t;

static smart_state_t g_smart_state = {0};
static char g_node_id[12] = {0};   // "ESP_AABBCC\0" = 11 chars

#define MAX_WS_MSG_HANDLERS 15
static ws_message_handler_fn_t g_msg_handlers[MAX_WS_MSG_HANDLERS];
static int                     g_msg_handler_count = 0;


// used in ws_broadcast, smart_state_replay
extern httpd_handle_t server;

// forward declarations
static void smart_state_update(const char* json_cmd);
static esp_err_t smart_state_replay(int fd);
// void smart_state_remove_element(const char* element_id); -> exposed in web_server_internal.h
static void smart_state_clear(void);
static esp_err_t ws_broadcast_with_state(const char* message, bool save_to_state, bool inject_src);
static esp_err_t ws_send_frame_locked(int fd, httpd_ws_frame_t *frame);
static void ws_evict_client(int fd);
static esp_err_t ws_queue_broadcast(const char *json_str, bool save_to_state);
static void ws_on_connect_cb(void *arg);

typedef struct {
    int fd;
} ws_connect_deferred_t;


const char *ws_get_node_id(void) { return g_node_id; }

void ws_register_message_handler(ws_message_handler_fn_t fn) {
    if (g_msg_handler_count < MAX_WS_MSG_HANDLERS)
        g_msg_handlers[g_msg_handler_count++] = fn;
    else
        ESP_LOGW(TAG, "WS handler table full, dropping handler");
}


// Initialize mutex 
void init_websocket_mutex(void) {
    ws_clients_mutex = xSemaphoreCreateMutex();
    ws_send_mutex = xSemaphoreCreateMutex();
}

static size_t ws_build_frame_header(uint8_t *hdr, size_t hdr_cap, const httpd_ws_frame_t *frame)
{
    if (!hdr || hdr_cap < 2 || !frame) {
        return 0;
    }

    hdr[0] = WS_FIN_BIT | (frame->type & 0x0fU);
    size_t tx_len;

    if (frame->len <= 125) {
        hdr[1] = frame->len & 0x7fU;
        tx_len = 2;
    } else if (frame->len < UINT16_MAX) {
        hdr[1] = 126;
        hdr[2] = (frame->len >> 8U) & 0xffU;
        hdr[3] = frame->len & 0xffU;
        tx_len = 4;
    } else {
        if (hdr_cap < 10) {
            return 0;
        }
        hdr[1] = 127;
        uint8_t shift_idx = sizeof(uint64_t) - 1;
        uint64_t len64 = frame->len;
        for (int8_t idx = 2; idx <= 9; idx++) {
            hdr[idx] = (len64 >> (shift_idx * 8U)) & 0xffU;
            shift_idx--;
        }
        tx_len = 10;
    }

    hdr[1] &= ~WS_MASK_BIT;
    return tx_len;
}

static esp_err_t ws_send_frame_locked(int fd, httpd_ws_frame_t *frame)
{
    if (!server || !ws_send_mutex || !frame) {
        return ESP_FAIL;
    }

    uint8_t hdr[10];
    size_t hdr_len = ws_build_frame_header(hdr, sizeof(hdr), frame);
    if (hdr_len == 0) {
        return ESP_FAIL;
    }

    size_t total = hdr_len + frame->len;
    uint8_t *buf = malloc(total);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(buf, hdr, hdr_len);
    if (frame->len > 0 && frame->payload) {
        memcpy(buf + hdr_len, frame->payload, frame->len);
    }

    xSemaphoreTake(ws_send_mutex, portMAX_DELAY);
    int sent = httpd_socket_send(server, fd, (const char *)buf, total, 0);
    xSemaphoreGive(ws_send_mutex);

    free(buf);
    return (sent >= 0 && (size_t)sent == total) ? ESP_OK : ESP_FAIL;
}

// populate node ID (3 last bytes of BDADDR)
void ws_node_init(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(g_node_id, sizeof(g_node_id), "ESP_%02X%02X%02X",
             mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Node ID: %s", g_node_id);
}


static void send_hello(int fd) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "hello");
    cJSON_AddStringToObject(j, "src",  g_node_id);
    cJSON_AddStringToObject(j, "caps", "observer,central,peripheral"); 
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return;
    httpd_ws_frame_t pkt = {0};
    pkt.payload = (uint8_t *)s;
    pkt.len     = strlen(s);
    pkt.type    = HTTPD_WS_TYPE_TEXT;
    if (ws_send_frame_locked(fd, &pkt) != ESP_OK) {
        ws_evict_client(fd);
    }
    free(s);
}


static char *build_device_status_json(void) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "device_status");
    cJSON_AddBoolToObject(j, "scanning", is_ble_scanning());
    const char *central = web_central_get_device();
    const char *peripheral = web_sim_get_device();
    central    ? cJSON_AddStringToObject(j, "central", central)
               : cJSON_AddNullToObject(j, "central");
    peripheral ? cJSON_AddStringToObject(j, "peripheral", peripheral)
               : cJSON_AddNullToObject(j, "peripheral");
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return s; // caller must free
}

static void send_device_status(int fd) {
    char *s = build_device_status_json();
    if (!s) return;
    httpd_ws_frame_t pkt = {.payload=(uint8_t*)s, .len=strlen(s), .type=HTTPD_WS_TYPE_TEXT};
    if (ws_send_frame_locked(fd, &pkt) != ESP_OK) {
        ws_evict_client(fd);
    }
    free(s);
}

static void send_device_status_broadcast(void) {
    char *s = build_device_status_json();
    if (!s) return;
    websocket_broadcast_json_transient(s); // transient - not saved to smart state
    free(s);
}

//
// Client tracking
//

// Add client to tracking list
static void add_ws_client(int fd) {
    if (!ws_clients_mutex) return;

    xSemaphoreTake(ws_clients_mutex, portMAX_DELAY);

    // Evict any existing entry with this exact fd (OS reused it → old one is dead)
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_client_fds[i] == fd) {
            ESP_LOGI(TAG, "Evicting stale entry for reused fd=%d at slot %d", fd, i);
            ws_client_fds[i] = 0;
        }
    }

    // Insert into free slot
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_client_fds[i] == 0) {
            ws_client_fds[i] = fd;
            ESP_LOGI(TAG, "Client %d connected (fd=%d)", i, fd);
            xSemaphoreGive(ws_clients_mutex);

            ws_connect_deferred_t *conn = malloc(sizeof(ws_connect_deferred_t));
            if (conn) {
                conn->fd = fd;
                if (httpd_queue_work(server, ws_on_connect_cb, conn) != ESP_OK) {
                    free(conn);
                    ESP_LOGW(TAG, "Failed to queue connect work for fd=%d", fd);
                }
            }
            return;
        }
    }

    xSemaphoreGive(ws_clients_mutex);
    ESP_LOGE(TAG, "No free slot for fd=%d even after eviction!", fd);
}


// Remove client from tracking list
static void remove_ws_client(int fd) {
    if (!ws_clients_mutex) return;
    
    xSemaphoreTake(ws_clients_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_client_fds[i] == fd) {
            ws_client_fds[i] = 0;
            ESP_LOGI(TAG, "Client %d disconnected (fd=%d)", i, fd);
            break;
        }
    }
    xSemaphoreGive(ws_clients_mutex);
}

static void ws_evict_client(int fd)
{
    remove_ws_client(fd);
    if (server) {
        httpd_sess_trigger_close(server, fd);
    }
}

static void ws_on_connect_cb(void *arg)
{
    ws_connect_deferred_t *conn = (ws_connect_deferred_t *)arg;
    if (!conn) {
        return;
    }
    int fd = conn->fd;
    free(conn);

    smart_state_replay(fd);
    send_hello(fd);
    send_device_status(fd);
}

// Injects "src" into a JSON string. Returns heap-allocated string, caller must free.
// Returns NULL on error (caller should fall back to original message).
// Note: this is costly deserialize/serialize, don't use in limited stack and too frequently
static char *ws_inject_src(const char *json_in) {
    cJSON *root = cJSON_Parse(json_in);
    if (!root) return NULL;
    cJSON_DeleteItemFromObject(root, "src");
    cJSON_AddStringToObject(root, "src", g_node_id);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}


// Built-in handler for transport-level and interface messages
// Registered first in smart_state_init() or a new ws_handlers_init()
static bool builtin_ws_handler(const char *type, cJSON *json) {

    if (strcmp(type, "hello") == 0) {
        cJSON *src = cJSON_GetObjectItemCaseSensitive(json, "src");
        if (cJSON_IsString(src))
            ESP_LOGI(TAG, "Hello from peer: %s", src->valuestring);
        return true;
    }

    if (strcmp(type, "status") == 0) {
        send_device_status_broadcast();
//        web_scan_broadcast_status();
//        web_central_broadcast_status();
//        web_sim_broadcast_status();
        return true;
    }

    return false;  // not handled
}

esp_err_t parse_websocket_message(const char *payload) {
    cJSON *json = cJSON_Parse(payload);
    if (!json) {
        ESP_LOGE(TAG, "JSON parse error: %s", cJSON_GetErrorPtr() ?: "unknown");
        return ESP_FAIL;
    }

    cJSON *type_obj = cJSON_GetObjectItemCaseSensitive(json, "type");
    if (!cJSON_IsString(type_obj)) {
        ESP_LOGE(TAG, "Missing/invalid 'type' field");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    const char *type = type_obj->valuestring;
    ESP_LOGI(TAG, "WS message type: '%s'", type);

    bool handled = false;
    for (int i = 0; i < g_msg_handler_count && !handled; i++) {
        handled = g_msg_handlers[i](type, json);
    }
    if (!handled)
        ESP_LOGW(TAG, "No handler for WS type: '%s'", type);

    cJSON_Delete(json);
    return ESP_OK;
}


typedef struct {
    char *json;
    bool  save_to_state;
} ws_deferred_msg_t;

static void ws_deferred_send_cb(void *arg)
{
    ws_deferred_msg_t *m = (ws_deferred_msg_t *)arg;
    if (!m) {
        return;
    }
    ws_broadcast_with_state(m->json, m->save_to_state, true);
    free(m->json);
    free(m);
}

static esp_err_t ws_queue_broadcast(const char *json_str, bool save_to_state)
{
    if (!server || !json_str) {
        return ESP_FAIL;
    }

    ws_deferred_msg_t *m = malloc(sizeof(ws_deferred_msg_t));
    if (!m) {
        return ESP_ERR_NO_MEM;
    }

    m->json = strdup(json_str);
    if (!m->json) {
        free(m);
        ESP_LOGW(TAG, "ws_queue_broadcast: strdup failed");
        return ESP_ERR_NO_MEM;
    }
    m->save_to_state = save_to_state;

    esp_err_t r = httpd_queue_work(server, ws_deferred_send_cb, m);
    if (r != ESP_OK) {
        free(m->json);
        free(m);
    }
    return r;
}

esp_err_t websocket_broadcast_json(const char *json_str)
{
    return ws_queue_broadcast(json_str, true);
}

esp_err_t websocket_broadcast_json_transient(const char *json_str)
{
    return ws_queue_broadcast(json_str, false);
}

esp_err_t websocket_broadcast_json_presrc_safe(const char *json_str)
{
    return ws_queue_broadcast(json_str, false);
}

// WebSocket handler
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        // New WebSocket connection
        int fd = httpd_req_to_sockfd(req);
 
        ESP_LOGI(TAG, "WebSocket handshake done, fd=%d", fd);

        // Wait for client to be ready (otherwise race condition and the first message is sent before the handshake finishes)
        vTaskDelay(pdMS_TO_TICKS(200));  // 200ms delay
        ESP_LOGI(TAG, "Client %d ready", fd);
        add_ws_client(fd);

        return ESP_OK;
    }

    int fd = httpd_req_to_sockfd(req);
    ESP_LOGI(TAG, "ws_handler called, fd=%d", fd);


    // Handle incoming WebSocket frames
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    
    // Get frame length
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        // distinguish errno to understand why socket is dead
        ESP_LOGE(TAG, "recv_frame (probe) failed: %d, errno=%d (%s), fd=%d",
                 ret, errno, strerror(errno), fd);
        remove_ws_client(fd); 
        return ret;
    }

    ESP_LOGI(TAG, "Frame probe: type=%d len=%d fd=%d", ws_pkt.type, ws_pkt.len, fd);

    if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
        // Do NOT call recv_frame again — send pong manually (avoids esp-idf #8803 double-read bug)
        ESP_LOGI(TAG, "PING received, sending PONG fd=%d", fd);
        ws_pkt.type = HTTPD_WS_TYPE_PONG;
        // payload=NULL, len=0 already set by memset — correct for empty pong
        ret = ws_send_frame_locked(fd, &ws_pkt);
        if (ret != ESP_OK) {
            ws_evict_client(fd);
        }
        ESP_LOGI(TAG, "PONG sent ret=%d fd=%d", ret, fd);
        return ret;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "CLOSE frame (probe), removing fd=%d", fd);
        remove_ws_client(fd);
        return ESP_OK;
    }

    if (ws_pkt.len == 0) {
        // pong or other zero-len non-text frame — nothing to do
        ESP_LOGI(TAG, "Zero-length non-control frame type=%d, ignoring fd=%d", ws_pkt.type, fd);
        return ESP_OK;
    }    

    // Allocate buffer
    uint8_t *buf = calloc(1, ws_pkt.len + 1);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes, fd=%d", ws_pkt.len, fd);
        return ESP_ERR_NO_MEM;
    }
    ws_pkt.payload = buf;
    
    // Receive frame
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "recv_frame (payload) failed: %d, errno=%d (%s), fd=%d",  ret, errno, strerror(errno), fd);
        free(buf);
        remove_ws_client(fd);
        return ret;
    }

    ESP_LOGI(TAG, "Frame received: type=%d len=%d fd=%d", ws_pkt.type, ws_pkt.len, fd);

    // Handle close frame
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "CLOSE frame, removing fd=%d", fd);
        remove_ws_client(fd);
        free(buf);
        return ESP_OK;
    }

    if (ws_pkt.type != HTTPD_WS_TYPE_TEXT) {
        ESP_LOGW(TAG, "Non-text frame type=%d, ignoring fd=%d", ws_pkt.type, fd);
        free(buf);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Received from client: %s", ws_pkt.payload);


     // Parse the JSON message
    ret = parse_websocket_message((const char *)ws_pkt.payload);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse ws message");
    }

    free(buf);
    return ESP_OK;
}


// Broadcast with optional state saving
static esp_err_t ws_broadcast_with_state(const char *message, bool save_to_state, bool inject_src) {

    if (save_to_state) {
        smart_state_update(message);
    }

    char *stamped = inject_src ? ws_inject_src(message) : NULL;
    const char *to_send = stamped ? stamped : message;

    if (!config.net_enabled.value.u8) {
        printf("[WS-OUT]%s\n", to_send);
        free(stamped);
        return ESP_OK;
    }

    if (!server || !ws_clients_mutex) {
        ESP_LOGE(TAG, "Can't broadcast ws!");
        free(stamped);
        return ESP_FAIL;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t *)to_send;
    ws_pkt.len     = strlen(to_send);
    ws_pkt.type    = HTTPD_WS_TYPE_TEXT;

    // ── Snapshot fd list; release mutex before any I/O ──────────────
    int snapshot[MAX_WS_CLIENTS];
    xSemaphoreTake(ws_clients_mutex, portMAX_DELAY);
    memcpy(snapshot, ws_client_fds, sizeof(snapshot));
    xSemaphoreGive(ws_clients_mutex);                  // ← released before send

    int sent_count = 0;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (snapshot[i] == 0) continue;

        httpd_ws_client_info_t client_info = httpd_ws_get_fd_info(server, snapshot[i]);
        if (client_info != HTTPD_WS_CLIENT_WEBSOCKET) {
            ws_evict_client(snapshot[i]);              // safe — mutex not held
            continue;
        }

        esp_err_t ret = ws_send_frame_locked(snapshot[i], &ws_pkt);
        if (ret == ESP_OK) {
            sent_count++;
        } else {
            ESP_LOGW(TAG, "Failed to send to client %d: %d", i, ret);
            ws_evict_client(snapshot[i]);              // safe — mutex not held
        }
    }

    ESP_LOGI(TAG, "Broadcast %s to %d clients", to_send, sent_count);
    free(stamped);
    return ESP_OK;
}

// ── Maintain state ────────────────────────────────────────────────── 

void smart_state_init(void) {
    g_smart_state.mutex = xSemaphoreCreateMutex();
    memset(g_smart_state.elements, 0, sizeof(g_smart_state.elements));
//    strcpy(g_smart_state.background_cmd, "{\"type\":\"background\",\"color\":\"#FFFFFF\"}");

    ws_register_message_handler(builtin_ws_handler); 
}



// Update or add element state
static void smart_state_update(const char* json_cmd) {
    if (!g_smart_state.mutex) return;
    
    cJSON* json = cJSON_Parse(json_cmd);
    if (!json) {
        ESP_LOGW(TAG, "Failed to parse state command");
        return;
    }
    
    cJSON* type_obj = cJSON_GetObjectItem(json, "type");
    if (!type_obj) {
        cJSON_Delete(json);
        return;
    }
    
    const char* type = type_obj->valuestring;
    
    xSemaphoreTake(g_smart_state.mutex, portMAX_DELAY);
    
    // Handle background separately
    if (strcmp(type, "background") == 0) {
        strncpy(g_smart_state.background_cmd, json_cmd, sizeof(g_smart_state.background_cmd) - 1);
        g_smart_state.background_cmd[sizeof(g_smart_state.background_cmd) - 1] = '\0';
        ESP_LOGD(TAG, "State: background updated");
        xSemaphoreGive(g_smart_state.mutex);
        cJSON_Delete(json);
        return;
    }
    
    // Get element ID
    cJSON* id_obj = cJSON_GetObjectItem(json, "id");
    if (!id_obj) {
        xSemaphoreGive(g_smart_state.mutex);
        cJSON_Delete(json);
        return;
    }
    
    const char* id = id_obj->valuestring;
    
    // Find existing element or empty slot
    int slot = -1;
    for (int i = 0; i < MAX_STATE_ELEMENTS; i++) {
        if (g_smart_state.elements[i].active && 
            strcmp(g_smart_state.elements[i].id, id) == 0) {
            slot = i;
            ESP_LOGD(TAG, "State: updating element '%s' at slot %d", id, i);
            break;
        }
    }
    
    if (slot == -1) {
        // Find empty slot
        for (int i = 0; i < MAX_STATE_ELEMENTS; i++) {
            if (!g_smart_state.elements[i].active) {
                slot = i;
                ESP_LOGD(TAG, "State: adding element '%s' at slot %d", id, i);
                break;
            }
        }
    }
    
    if (slot != -1) {
        strncpy(g_smart_state.elements[slot].id, id, sizeof(g_smart_state.elements[slot].id) - 1);
        g_smart_state.elements[slot].id[sizeof(g_smart_state.elements[slot].id) - 1] = '\0';
        
        strncpy(g_smart_state.elements[slot].json, json_cmd, sizeof(g_smart_state.elements[slot].json) - 1);
        g_smart_state.elements[slot].json[sizeof(g_smart_state.elements[slot].json) - 1] = '\0';
        
        g_smart_state.elements[slot].active = true;
    } else {
        ESP_LOGW(TAG, "State buffer full! Cannot add element '%s'", id);
    }
    
    xSemaphoreGive(g_smart_state.mutex);
    cJSON_Delete(json);
}

//
// Replay smart state to client
//
static esp_err_t smart_state_replay(int fd) {
    if (!g_smart_state.mutex) return ESP_FAIL;
    
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    xSemaphoreTake(g_smart_state.mutex, portMAX_DELAY);
    
    // Send background first
    ws_pkt.payload = (uint8_t *)g_smart_state.background_cmd;
    ws_pkt.len = strlen(g_smart_state.background_cmd);
    if (ws_pkt.len > 0 && ws_send_frame_locked(fd, &ws_pkt) != ESP_OK) {
        xSemaphoreGive(g_smart_state.mutex);
        ws_evict_client(fd);
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    // Send all active elements
    for (int i = 0; i < MAX_STATE_ELEMENTS; i++) {
        if (g_smart_state.elements[i].active) {
            ws_pkt.payload = (uint8_t *)g_smart_state.elements[i].json;
            ws_pkt.len = strlen(g_smart_state.elements[i].json);
            if (ws_send_frame_locked(fd, &ws_pkt) != ESP_OK) {
                xSemaphoreGive(g_smart_state.mutex);
                ws_evict_client(fd);
                return ESP_FAIL;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    
    xSemaphoreGive(g_smart_state.mutex);
    
    ESP_LOGI(TAG, "Smart state replayed to fd=%d", fd);
    return ESP_OK;
}

// Remove element from state
void smart_state_remove_element(const char* element_id) {
    if (!g_smart_state.mutex) return;
    
    xSemaphoreTake(g_smart_state.mutex, portMAX_DELAY);
    
    for (int i = 0; i < MAX_STATE_ELEMENTS; i++) {
        if (g_smart_state.elements[i].active && 
            strcmp(g_smart_state.elements[i].id, element_id) == 0) {
            
            ESP_LOGI(TAG, "Removing element '%s' from state", element_id);
            g_smart_state.elements[i].active = false;
            memset(g_smart_state.elements[i].id, 0, sizeof(g_smart_state.elements[i].id));
            memset(g_smart_state.elements[i].json, 0, sizeof(g_smart_state.elements[i].json));
            break;
        }
    }
    
    xSemaphoreGive(g_smart_state.mutex);
}

// Clear all state
static void smart_state_clear(void) {
    if (!g_smart_state.mutex) return;
    
    xSemaphoreTake(g_smart_state.mutex, portMAX_DELAY);
    
    for (int i = 0; i < MAX_STATE_ELEMENTS; i++) {
        if (g_smart_state.elements[i].active) {
            g_smart_state.elements[i].active = false;
            memset(g_smart_state.elements[i].id, 0, sizeof(g_smart_state.elements[i].id));
            memset(g_smart_state.elements[i].json, 0, sizeof(g_smart_state.elements[i].json));
        }
    }  
   
    xSemaphoreGive(g_smart_state.mutex);
    
    ESP_LOGI(TAG, "State cleared");
}


// ── Register handlers ────────────────────────────────────────────────── 
uint8_t register_websocket_handler_in_web_server(httpd_handle_t *server)
{
    static const httpd_uri_t ws_uri = {
        .uri        = "/ws",
        .method     = HTTP_GET,
        .handler    = ws_handler,
        .user_ctx   = NULL,
        .is_websocket = true,                    // handle as websocket
        .handle_ws_control_frames = false        // Handle ping/pong/close frames manually

    };
    uint8_t ret = httpd_register_uri_handler(*server, &ws_uri);

//    ESP_LOGI(TAG, "Websocket handler registered, status code=%d", ret);
    return ESP_OK;
}
