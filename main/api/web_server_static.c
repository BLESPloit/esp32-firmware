#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h> 

#include "services/gap/ble_svc_gap.h" // BLE_ADDR_PUBLIC ...
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"

#include "ble/ble_sim.h" // load device for simulation
#include "ble/ble_discovery.h"
#include "ble/ble_scan.h"
#include "graphics/graphics.h"
#include "interface/interface_sim.h"
#include "common/storage.h"
#include "common/utils.h"
#include "api/web_server.h"
#include "api/web_server_internal.h"
#include "ble/device_manifest.h"

static const char *TAG = "web server - static";

// ── Embedded files ────────────────────────────────────────────────── 

extern const uint8_t central_css_start[]            asm("_binary_central_css_start");
extern const uint8_t central_css_end[]              asm("_binary_central_css_end");
extern const uint8_t central_body_html_start[]      asm("_binary_central_body_html_start");
extern const uint8_t central_body_html_end[]        asm("_binary_central_body_html_end");
extern const uint8_t common_css_start[]             asm("_binary_common_css_start");
extern const uint8_t common_css_end[]               asm("_binary_common_css_end");
extern const uint8_t common_js_start[]              asm("_binary_common_js_start");
extern const uint8_t common_js_end[]                asm("_binary_common_js_end");
extern const uint8_t devices_body_html_start[]      asm("_binary_devices_body_html_start");
extern const uint8_t devices_body_html_end[]        asm("_binary_devices_body_html_end");
extern const uint8_t editor_html_start[]            asm("_binary_editor_html_start");
extern const uint8_t editor_html_end[]              asm("_binary_editor_html_end");
extern const uint8_t filemanager_body_html_start[]  asm("_binary_filemanager_body_html_start");
extern const uint8_t filemanager_body_html_end[]    asm("_binary_filemanager_body_html_end");
extern const uint8_t footer_html_start[]            asm("_binary_footer_html_start");
extern const uint8_t footer_html_end[]              asm("_binary_footer_html_end");
extern const uint8_t head_html_start[]              asm("_binary_head_html_start");
extern const uint8_t head_html_end[]                asm("_binary_head_html_end");
extern const uint8_t index_html_start[]             asm("_binary_index_html_start");
extern const uint8_t index_html_end[]               asm("_binary_index_html_end");
extern const uint8_t logo_png_start[]               asm("_binary_logo_png_start");
extern const uint8_t logo_png_end[]                 asm("_binary_logo_png_end");
extern const uint8_t logo_svg_start[]               asm("_binary_logo_svg_start");
extern const uint8_t logo_svg_end[]                 asm("_binary_logo_svg_end");
extern const uint8_t scan_css_start[]               asm("_binary_scan_css_start");
extern const uint8_t scan_css_end[]                 asm("_binary_scan_css_end");
extern const uint8_t scan_body_html_start[]         asm("_binary_scan_body_html_start");
extern const uint8_t scan_body_html_end[]           asm("_binary_scan_body_html_end");
extern const uint8_t sim_css_start[]                asm("_binary_sim_css_start");
extern const uint8_t sim_css_end[]                  asm("_binary_sim_css_end");
extern const uint8_t sim_body_html_start[]          asm("_binary_sim_body_html_start");
extern const uint8_t sim_body_html_end[]            asm("_binary_sim_body_html_end");
extern const uint8_t ws_debug_log_js_start[]        asm("_binary_ws_debug_log_js_start");
extern const uint8_t ws_debug_log_js_end[]          asm("_binary_ws_debug_log_js_end");


typedef struct {
    const char        *filename;   // bare name, e.g. "index.html"
    const uint8_t     *start;
    const uint8_t     *end;
} embedded_file_t;

static const embedded_file_t embedded_files[] = {
    { "central.css",            central_css_start,              central_css_end          },
    { "central_body.html",      central_body_html_start,        central_body_html_end    },
    { "common.css",             common_css_start,               common_css_end           },
    { "common.js",              common_js_start,                common_js_end            },
    { "devices_body.html",      devices_body_html_start,        devices_body_html_end    },
    { "editor.html",            editor_html_start,              editor_html_end          },
    { "filemanager_body.html",  filemanager_body_html_start,    filemanager_body_html_end},
    { "footer.html",            footer_html_start,              footer_html_end          },
    { "head.html",              head_html_start,                head_html_end            },
    { "index.html",             index_html_start,               index_html_end           },
    { "logo.png",               logo_png_start,                 logo_png_end             },
    { "logo.svg",               logo_svg_start,                 logo_svg_end             },
    { "scan.css",               scan_css_start,                 scan_css_end             },
    { "scan_body.html",         scan_body_html_start,           scan_body_html_end       },
    { "sim.css",                sim_css_start,                  sim_css_end              },
    { "sim_body.html",          sim_body_html_start,            sim_body_html_end        },
    { "ws_debug_log.js",        ws_debug_log_js_start,          ws_debug_log_js_end      },
    { NULL, NULL, NULL }  // sentinel
};

static const embedded_file_t *find_embedded(const char *filename) {
    for (int i = 0; embedded_files[i].filename; i++) {
        if (strcmp(embedded_files[i].filename, filename) == 0)
            return &embedded_files[i];
    }
    return NULL;
}


// serve in chunks any file from storage (littlefs)
static const char *get_content_type(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".png")  == 0) return "image/png";
    if (strcmp(ext, ".svg")  == 0) return "image/svg+xml";
    if (strcmp(ext, ".css")  == 0) return "text/css";
    if (strcmp(ext, ".js")   == 0) return "application/javascript";
    if (strcmp(ext, ".html") == 0) return "text/html";
    return "application/octet-stream";
}


// serve in chunks any file from storage (littlefs)
static esp_err_t serve_file_from_storage(httpd_req_t *req, const char *filepath, const char *content_type, size_t chunk_size)
{
    struct stat file_stat;
    FILE *f = NULL;
    char *chunk;
    size_t chunksize;

    // Check if file exists
    if (stat(filepath, &file_stat) == -1) {
        ESP_LOGE(TAG, "File does not exist: %s", filepath);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }
    
    // Open file for reading
    f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read file");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, content_type);

    // Allocate chunk buffer
    chunk = malloc(chunk_size);
    if (!chunk) {
        ESP_LOGE(TAG, "Failed to allocate memory for chunk");
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }
    
    // Read and send file in chunks
    do {
        chunksize = fread(chunk, 1, chunk_size, f);
        if (chunksize > 0) {
            if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to send chunk");
                free(chunk);
                fclose(f);
                return ESP_FAIL;
            }
        }
    } while (chunksize > 0);
    
    // Send empty chunk to signal end of response
    httpd_resp_send_chunk(req, NULL, 0);
    
    // Cleanup
    free(chunk);
    fclose(f);
    
    ESP_LOGI(TAG, "File sent successfully: %s (%ld bytes)", filepath, file_stat.st_size);
//    log_memory_usage("After serving file");
    return ESP_OK;

}

static esp_err_t serve_file_embedded_or_storage(httpd_req_t *req, const char *filepath, size_t chunk_size)
{
    if (!filepath) {
        ESP_LOGE(TAG, "serve_file_embedded_or_storage called with NULL filepath");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal error");
    }

    const char *filename = strrchr(filepath, '/');
    filename = filename ? filename + 1 : filepath;
    const char *content_type = get_content_type(filename);

    // --- Try LittleFS first (debug override) ---
    struct stat st;
    if (stat(filepath, &st) == 0) {
        ESP_LOGI(TAG, "[static] Serving '%s' from LittleFS", filepath);
        return serve_file_from_storage(req, filepath, content_type, chunk_size);
    }

    // --- Fall back to embedded firmware ---
    const embedded_file_t *ef = find_embedded(filename);
    if (ef) {
        size_t len = ef->end - ef->start;
        ESP_LOGI(TAG, "[static] Serving '%s' from firmware (%d bytes)", filename, len);
        httpd_resp_set_type(req, content_type);
        httpd_resp_send(req, (const char *)ef->start, len); // direct flash pointer, zero-copy
        return ESP_OK;
    }

    ESP_LOGE(TAG, "[static] '%s' not found in LittleFS or firmware", filepath);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_FAIL;
}

// "stitch" multiple html files
esp_err_t serve_html_fragments(httpd_req_t *req, const char **filenames, int count)
{
    httpd_resp_set_type(req, "text/html");

    for (int i = 0; i < count; i++) {
        char filepath[128];
        snprintf(filepath, sizeof(filepath), "/" LITTLEFS_LABEL "/html/%s", filenames[i]);

        // --- Try LittleFS first (debug override) ---
        FILE *f = fopen(filepath, "rb");
        if (f) {
            ESP_LOGI(TAG, "[fragment] Serving '%s' from LittleFS", filenames[i]);
            char chunk[4096];
            size_t chunksize;
            do {
                chunksize = fread(chunk, 1, sizeof(chunk), f);
                if (chunksize > 0)
                    httpd_resp_send_chunk(req, chunk, chunksize);
            } while (chunksize > 0);
            fclose(f);
            continue;
        }

        // --- Fall back to embedded firmware ---
        const embedded_file_t *ef = find_embedded(filenames[i]);
        if (ef) {
            ESP_LOGI(TAG, "[fragment] Serving '%s' from firmware", filenames[i]);
            httpd_resp_send_chunk(req, (const char *)ef->start, ef->end - ef->start);
            continue;
        }

        // --- Soft error: don't abort the whole page ---
        ESP_LOGE(TAG, "[fragment] '%s' not found in LittleFS or firmware", filenames[i]);
        httpd_resp_sendstr_chunk(req, "<!-- fragment missing -->");
    }

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}


esp_err_t serve_static_json(httpd_req_t *req, char *filepath) {
    return serve_file_embedded_or_storage(req, filepath, 4096);
}

esp_err_t serve_static_html(httpd_req_t *req, const char *filename)
{
    char filepath[128];
    
    // Construct full file path (assuming SPIFFS mounted at /spiffs)
    snprintf(filepath, sizeof(filepath), "/"LITTLEFS_LABEL"/html/%s", filename);
    return serve_file_embedded_or_storage(req, filepath, 4096);
}


static esp_err_t serve_image_with_fallback(httpd_req_t *req, const char *filepath,
                                            const char *content_type,
                                            const char *fallback_embedded_name)
{
    // Try the requested file (LittleFS or embedded)
    struct stat st;
    if (stat(filepath, &st) == 0)
        return serve_file_from_storage(req, filepath, content_type, 4096);

    // Fall back to embedded placeholder
    ESP_LOGW(TAG, "'%s' missing, serving embedded fallback '%s'", filepath, fallback_embedded_name);
    const embedded_file_t *ef = find_embedded(fallback_embedded_name);
    if (ef) {
        httpd_resp_set_type(req, content_type);
        httpd_resp_send(req, (const char *)ef->start, ef->end - ef->start);
        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No image available");
    return ESP_FAIL;
}

esp_err_t serve_static_png(httpd_req_t *req, char *filepath) {
    return serve_image_with_fallback(req, filepath, "image/png", "logo.png");
}

esp_err_t serve_static_svg(httpd_req_t *req, char *filepath) {
    return serve_image_with_fallback(req, filepath, "image/svg+xml", "logo.svg");
}


esp_err_t static_handler(httpd_req_t *req)
{
    const char *uri = req->uri;

    if (strncmp(uri, "/static/", 8) != 0) { httpd_resp_send_404(req); return ESP_FAIL; }

    const char *path_after_static = uri + 8;

    if (*path_after_static == '\0' || strstr(path_after_static, "..")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    const char *slash    = strchr(path_after_static, '/');
    const char *filename = NULL;
    char        filepath[256];

    if (slash && *(slash + 1) != '\0') {
        // Format: /static/{device_id}/file  (with or without extension)
        filename = slash + 1;

        // Reject nested paths beyond one level
        if (strchr(filename, '/')) { httpd_resp_send_404(req); return ESP_FAIL; }

        size_t device_id_len = slash - path_after_static;
        if (device_id_len == 0 || device_id_len >= 64) {
            httpd_resp_send_404(req); return ESP_FAIL;
        }

        char device_id[64];
        strncpy(device_id, path_after_static, device_id_len);
        device_id[device_id_len] = '\0';

        if (strrchr(filename, '.') != NULL) {
            // Has extension — direct asset path
            snprintf(filepath, sizeof(filepath),
                     "/" LITTLEFS_LABEL "/devices/%s/assets/%s", device_id, filename);
        } else {
            // No extension — resolve real path (with extension) from manifest.json
            char *resolved = manifest_resolve_path(device_id, filename);
            if (!resolved) {
                ESP_LOGW(TAG, "Cannot resolve '%s' for device '%s', serving logo.svg", filename, device_id);
                const embedded_file_t *ef = find_embedded("logo.svg");
                if (ef) {
                    httpd_resp_set_type(req, "image/svg+xml");
                    httpd_resp_send(req, (const char *)ef->start, ef->end - ef->start);
                    return ESP_OK;
                }
                httpd_resp_send_404(req);
                return ESP_FAIL;
            }
                    strncpy(filepath, resolved, sizeof(filepath) - 1);
            filepath[sizeof(filepath) - 1] = '\0';
            free(resolved);
            // Re-derive filename so get_content_type() sees the resolved extension
            filename = strrchr(filepath, '/');
            filename = filename ? filename + 1 : filepath;
        }
    } else {
        // Format: /static/file.ext — serve from /html (embedded or LittleFS)
        filename = path_after_static;
        snprintf(filepath, sizeof(filepath),
                 "/" LITTLEFS_LABEL "/html/%s", filename);
    }

    // Validate extension
    const char *content_type = get_content_type(filename);
    if (strcmp(content_type, "application/octet-stream") == 0) {
        httpd_resp_send_404(req); return ESP_FAIL;
    }

    // Route to appropriate image handler (with fallback) or generic handler
    if (strcmp(content_type, "image/png") == 0)
        return serve_static_png(req, filepath);

    if (strcmp(content_type, "image/svg+xml") == 0)
        return serve_static_svg(req, filepath);

    return serve_file_embedded_or_storage(req, filepath, 4096);
}

// ── Register handlers ────────────────────────────────────────────────── 

uint8_t register_static_handlers_in_web_server(httpd_handle_t *server)
{
    uint8_t handler_count = 0;

    httpd_uri_t uri_static = {
        .uri = "/static/*",
        .method = HTTP_GET,
        .handler = static_handler,
        .user_ctx = NULL,
        .is_websocket = false,
    };
    httpd_register_uri_handler(*server, &uri_static);
    handler_count++;

    return handler_count;
}