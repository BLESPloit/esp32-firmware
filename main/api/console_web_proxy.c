#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_system.h"
#include "argtable3/argtable3.h"
#include "linenoise/linenoise.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "mbedtls/base64.h"  // For base64 decode
#include "api/web_server.h"  // parse_websocket_message, websocket_broadcast_json


#define UPLOAD_CHUNK   384   // ~512 base64 chars decode to 384 bytes
#define UPLOAD_MAX_LINE 520  // base64 line + \n overhead
#define SERIAL_HTTP_BUF 1024

// Upload state (persists across REPL lines)
static FILE *upload_file = NULL;
static char  upload_path[256] = {0};
static size_t upload_bytes = 0;
static bool  upload_active = false;


// Mock httpd_req_t (minimal for handlers)
typedef struct {
    int method;
    char uri[256];
    char content[SERIAL_HTTP_BUF];
    size_t content_len;
    char resp_hdr[128];  // Stub
} mock_httpd_req_t;


// 
//  Websocket proxy
// 

static int ws_cmd(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: ws <json_payload>\n");
        printf("  ws '{\"type\":\"button\",\"id\":\"power\"}'\n");
        printf("  ws {\"type\":\"relay\",\"action\":\"read\",\"svc\":\"180F\",\"chr\":\"2A19\"}\n");
        return ESP_OK;
    }

    // Join ALL args after "ws" into single JSON string (handles spaces)
    char payload[512] = {0}; 
    bool first = true;
    for (int i = 1; i < argc; i++) {
        if (!first) strcat(payload, " ");
        strcat(payload, argv[i]);
        first = false;
    }

    printf("[WS-IN] %s\n", payload);  // Debug

    esp_err_t ret = parse_websocket_message(payload);
    if (ret != ESP_OK) {
        printf("[WS-ERR] Parse failed: %d\n", ret);
    }
    return ESP_OK;
}



void register_ws_proxy(void) {
    esp_console_cmd_t cmd = {
        .command = "ws",
        .help    = "Send a WebSocket JSON message via serial",
        .hint    = "'{\"type\":\"...\"}'",
        .func    = ws_cmd,
        .argtable= NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

// 
//  HTTP proxy
// 
/*

// Stub httpd_resp functions (printf to serial)
static esp_err_t mock_resp_send(httpd_req_t *req, const char *data, size_t len) {
    snprintf(proxy_resp, sizeof(proxy_resp), "[RESP]%.*s\n", (int)len, data);
    printf("%s", proxy_resp);
    return ESPOK;
}
static esp_err_t mock_resp_set_type(httpd_req_t *req, const char *type) { return ESPOK; }
*/

/*

// Dispatch to real handlers (add your URIs)
static esp_err_t proxy_dispatch(mock_httpd_req_t *req) {
    httpd_req_t mock = { .method = req->method, .uri = req->uri, 
                         .content_len = req->content_len };
    
    if (strncmp(req->uri, "/api/log/filter", 15) == 0) {
        return wslogsetfilterhandler((httpd_req_t*)&mock);
    } else if (strncmp(req->uri, "/api/simulation/start", 21) == 0) {
        return simulatedevicehandler((httpd_req_t*)&mock);
    } else if (strncmp(req->uri, "/ws", 3) == 0) {
        parsewebsocketmessage(req->content);
        snprintf(proxy_resp, sizeof(proxy_resp), "[RESP]WS OK\n");
        printf("%s", proxy_resp);
        return ESPOK;
    } // Add more: /api/simulation/stop, static files (mock servestaticjson -> printf JSON)
    else {
        snprintf(proxy_resp, sizeof(proxy_resp), "[RESP]404 Not Found: %s\n", req->uri);
        printf("%s", proxy_resp);
        return ESP_FAIL;
    }
}

// HTTP proxy command
static int http_cmd(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: http <GET|POST> <uri> [body]\n");
        return ESP_OK;
    }

    mock_httpd_req_t req = {0};
    req.method = (strcmp(argv[1], "POST") == 0) ? HTTP_POST : HTTP_GET;
    strncpy(req.uri, argv[2], sizeof(req.uri) - 1);

    if (argc > 3) {
        strncpy(req.content, argv[3], sizeof(req.content) - 1);
        req.content_len = strlen(req.content);
    }

    printf("[REQ] %s %s\n", argv[1], argv[2]);  // Colored in app

    esp_err_t ret = proxy_dispatch(&req);
    return (ret == ESPOK) ? ESP_OK : ESP_FAIL;
}


void register_proxy_commands(void) {
    // HTTP proxy
    esp_console_cmd_t http_struct = {
        .command = "http",
        .help = "HTTP proxy over serial (net off)",
        .hint = "<GET|POST> <uri> [json]",
        .func = http_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&http_struct));

*/

// 
//  File upload/download
// 



// Called from REPL line handler - intercept raw lines during upload
// Hook this into esp_console BEFORE normal dispatch (see initialize_console)
bool console_web_proxy_upload_intercept(const char *line) {
    if (!upload_active) return false;  // Not our line

    if (strcmp(line, "[END]") == 0) {
        // Finalize
        fclose(upload_file);
        upload_file = NULL;
        upload_active = false;
        printf("[RESP]{\"status\":\"ok\",\"path\":\"%s\",\"bytes\":%zu}\n",
               upload_path, upload_bytes);
        upload_bytes = 0;
        return true;
    }

    if (strcmp(line, "[ABORT]") == 0) {
        fclose(upload_file);
        remove(upload_path);
        upload_file = NULL;
        upload_active = false;
        printf("[RESP]{\"status\":\"aborted\"}\n");
        return true;
    }

    // Decode base64 chunk
    unsigned char chunk[UPLOAD_CHUNK + 4];
    size_t out_len = 0;
    int ret = mbedtls_base64_decode(chunk, sizeof(chunk), &out_len,
                                    (const unsigned char *)line, strlen(line));
    if (ret != 0) {
        printf("[RESP]{\"status\":\"error\",\"msg\":\"base64 decode failed %d\"}\n", ret);
        fclose(upload_file);
        remove(upload_path);
        upload_file = NULL;
        upload_active = false;
        return true;
    }

    // Write chunk to LittleFS (same as web_server_static chunked write)
    size_t written = fwrite(chunk, 1, out_len, upload_file);
    if (written != out_len) {
        printf("[RESP]{\"status\":\"error\",\"msg\":\"write failed\"}\n");
        fclose(upload_file);
        remove(upload_path);
        upload_file = NULL;
        upload_active = false;
        return true;
    }

    upload_bytes += written;
    printf("[ACK]%zu\n", upload_bytes);  // App uses ACK to pace next chunk
    return true;
}

// upload command: http POST /upload <dest_path>
static int upload_cmd(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: upload <dest_path>\n");
        printf("Then send base64 lines, finish with [END]\n");
        return ESP_OK;
    }
    if (upload_active) {
        printf("[RESP]{\"status\":\"error\",\"msg\":\"upload already active\"}\n");
        return ESP_FAIL;
    }

    snprintf(upload_path, sizeof(upload_path), "%s", argv[1]);
    upload_file = fopen(upload_path, "wb");
    if (!upload_file) {
        printf("[RESP]{\"status\":\"error\",\"msg\":\"cannot open %s\"}\n", upload_path);
        return ESP_FAIL;
    }

    upload_active = true;
    upload_bytes = 0;
    printf("[READY]%s\n", upload_path);  // App starts sending chunks
    return ESP_OK;
}

// Download command: http GET /file <src_path>
// Mirror of web_server_static serve_file_from_storage but to serial
static int download_cmd(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: download <src_path>\n");
        return ESP_OK;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        printf("[RESP]{\"status\":\"error\",\"msg\":\"not found: %s\"}\n", argv[1]);
        return ESP_FAIL;
    }

    struct stat st;
    stat(argv[1], &st);
    printf("[RESP]{\"status\":\"ok\",\"path\":\"%s\",\"size\":%ld}\n",
           argv[1], st.st_size);

    // Chunked base64 output (app decodes)
    unsigned char raw[384];
    unsigned char b64[520];
    size_t b64_len;
    size_t n;
    while ((n = fread(raw, 1, sizeof(raw), f)) > 0) {
        mbedtls_base64_encode(b64, sizeof(b64), &b64_len, raw, n);
        b64[b64_len] = '\0';
        printf("[DATA]%s\n", b64);
    }
    fclose(f);
    printf("[END]\n");
    return ESP_OK;
}

void register_file_commands(void) {
    esp_console_cmd_t upload_struct = {
        .command = "upload",
        .help    = "Upload file to LittleFS via base64 serial chunks",
        .hint    = "<dest_path>",
        .func    = upload_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&upload_struct));

    esp_console_cmd_t download_struct = {
        .command = "download",
        .help    = "Download file from LittleFS via base64 serial chunks",
        .hint    = "<src_path>",
        .func    = download_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&download_struct));
}
