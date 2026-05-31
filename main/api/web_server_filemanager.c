#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include "cJSON.h"
#include "common/storage.h"
#include "common/utils.h"
#include "api/web_server_internal.h"
#include "api/web_server.h"

static const char *TAG = "web server filemanager";

// ── Helpers ────────────────────────────────────────────────── 

 static void url_decode(char *dst, const char *src, size_t max_len) {
    size_t i = 0, j = 0;
    while (src[i] != '\0' && j < max_len - 1) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = { src[i+1], src[i+2], '\0' };
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else if (src[i] == '+') {
            dst[j++] = ' '; i++;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

static void build_full_path(char *out, size_t maxlen, const char *relpath) {
    if (!relpath || relpath[0] == '\0' || strcmp(relpath, "/") == 0)
        snprintf(out, maxlen, "/%s", LITTLEFS_LABEL);
    else if (relpath[0] == '/')
        snprintf(out, maxlen, "/%s%s", LITTLEFS_LABEL, relpath);   // already has leading /
    else
        snprintf(out, maxlen, "/%s/%s", LITTLEFS_LABEL, relpath);  // insert missing /
}


static bool is_directory(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0) && S_ISDIR(st.st_mode);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root) {
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, s, strlen(s));
    free(s);
    return r;
}

static bool get_path_param(httpd_req_t *req,
                           char *relpath, size_t relmaxlen,
                           char *fullpath, size_t fullmaxlen) {
    char query[256] = {0}, encoded[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
    if (httpd_query_key_value(query, "path", encoded, sizeof(encoded)) != ESP_OK) return false;
    url_decode(relpath, encoded, relmaxlen);
    if (strstr(relpath, "..")) return false;
    build_full_path(fullpath, fullmaxlen, relpath);
    return true;
}

static int rmdir_recursive(const char *path) {
    DIR *d = opendir(path);
    if (!d) { unlink(path); return 0; }
    struct dirent *e;
    char child[512];
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        int n = snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        if (n < 0 || n >= (int)sizeof(child)) {
            ESP_LOGW(TAG, "rmdir_recursive: path truncated, skipping");
            continue;
        }
        if (is_directory(child)) rmdir_recursive(child);
        else                     unlink(child);
    }
    closedir(d);
    return rmdir(path);
}

/**
 * mkdir_p – create a directory and all missing parent directories.
 * Works on the *full* filesystem path (i.e. already includes LITTLEFS_LABEL prefix).
 * Returns 0 on success, -1 on error (errno is set).
 */
static int mkdir_p(const char *fullpath) {
    char tmp[300];
    int n = snprintf(tmp, sizeof(tmp), "%s", fullpath);
    if (n < 0 || n >= (int)sizeof(tmp)) { errno = ENAMETOOLONG; return -1; }

    /* Walk every component and create it if missing. */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    // Create the final component.
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}


// ── /fs/ls ────────────────────────────────────────────────── 

static void ls_dir_into_array(const char *fullpath, const char *relbase,
                               cJSON *arr, bool recursive) {
    DIR *d = opendir(fullpath);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;

        char childrel[512], childfull[512]; 

        int nr, nf;
        if (strcmp(relbase, "/") == 0)
            nr = snprintf(childrel, sizeof(childrel), "/%s", e->d_name);
        else
            nr = snprintf(childrel, sizeof(childrel), "%s/%s", relbase, e->d_name);

        nf = snprintf(childfull, sizeof(childfull), "%s/%s", fullpath, e->d_name);

        if (nr < 0 || nr >= (int)sizeof(childrel) ||
            nf < 0 || nf >= (int)sizeof(childfull)) {
            ESP_LOGW(TAG, "ls: path truncated, skipping %.40s", e->d_name);
            continue;
        }

        struct stat st;
        bool isdir = is_directory(childfull);
        stat(childfull, &st);

        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", e->d_name);
        cJSON_AddStringToObject(item, "path", childrel);
        cJSON_AddStringToObject(item, "type", isdir ? "dir" : "file");
        if (!isdir) cJSON_AddNumberToObject(item, "size", (double)st.st_size);
        cJSON_AddItemToArray(arr, item);

        if (recursive && isdir)
            ls_dir_into_array(childfull, childrel, arr, true);
    }
    closedir(d);
}

static esp_err_t ls_handler(httpd_req_t *req) {
    char relpath[128] = "/", fullpath[256];
    char query[256] = {0}, encoded[128] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    if (httpd_query_key_value(query, "path", encoded, sizeof(encoded)) == ESP_OK) {
        url_decode(relpath, encoded, sizeof(relpath));
        if (strstr(relpath, ".."))
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
    }
    build_full_path(fullpath, sizeof(fullpath), relpath);

    char recflag[4] = {0};
    bool recursive = false;
    if (httpd_query_key_value(query, "recursive", recflag, sizeof(recflag)) == ESP_OK)
        recursive = (recflag[0] == '1');

    if (!is_directory(fullpath))
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not a directory");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "path", relpath);
    cJSON *arr = cJSON_AddArrayToObject(root, "entries");
    ls_dir_into_array(fullpath, relpath, arr, recursive);
    return send_json(req, root);
}


// ── /fs/df ────────────────────────────────────────────────── 

static esp_err_t df_handler(httpd_req_t *req) {
    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "total_bytes", (double)total);
    cJSON_AddNumberToObject(root, "used_bytes",  (double)used);
    cJSON_AddNumberToObject(root, "free_bytes",  (double)(total - used));
    return send_json(req, root);
}

// ── /fs/exists ────────────────────────────────────────────────── 

static esp_err_t exists_handler(httpd_req_t *req) {
    char relpath[128], fullpath[256];
    if (!get_path_param(req, relpath, sizeof(relpath), fullpath, sizeof(fullpath)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing/invalid path");
    struct stat st;
    cJSON *root = cJSON_CreateObject();
    if (stat(fullpath, &st) == 0) {
        bool isdir = S_ISDIR(st.st_mode);
        cJSON_AddBoolToObject(root, "exists", true);
        cJSON_AddStringToObject(root, "type", isdir ? "dir" : "file");
        if (!isdir) cJSON_AddNumberToObject(root, "size", (double)st.st_size);
    } else {
        cJSON_AddBoolToObject(root, "exists", false);
    }
    return send_json(req, root);
}

// ── /fs/rename ────────────────────────────────────────────────── 

static esp_err_t rename_handler(httpd_req_t *req) {
    char query[512] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");

    char from_enc[128]={0}, to_enc[128]={0};
    char from_rel[128], to_rel[128], from_full[256], to_full[256];
    if (httpd_query_key_value(query, "from", from_enc, sizeof(from_enc)) != ESP_OK ||
        httpd_query_key_value(query, "to",   to_enc,   sizeof(to_enc))   != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing from/to");

    url_decode(from_rel, from_enc, sizeof(from_rel));
    url_decode(to_rel,   to_enc,   sizeof(to_rel));

// don't check for ".." in rename, this way we can move file up, and in littlefs directory traversal is not possible
//    if (strstr(from_rel, "..") || strstr(to_rel, ".."))
//        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");

    build_full_path(from_full, sizeof(from_full), from_rel);
    build_full_path(to_full,   sizeof(to_full),   to_rel);

    if (rename(from_full, to_full) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s: %s", from_full, to_full, strerror(errno));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rename failed");
    }
    ESP_LOGI(TAG, "Renamed %s -> %s", from_full, to_full);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

// ── /fs/rmdir  (recursive) ────────────────────────────────────────────────── 

static esp_err_t rmdir_handler(httpd_req_t *req) {
    char relpath[128], fullpath[256];
    if (!get_path_param(req, relpath, sizeof(relpath), fullpath, sizeof(fullpath)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing/invalid path");
    if (!is_directory(fullpath))
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not a directory");
    if (rmdir_recursive(fullpath) != 0) {
        ESP_LOGE(TAG, "rmdir_recursive %s failed: %s", fullpath, strerror(errno));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rmdir failed");
    }
    ESP_LOGI(TAG, "Recursively removed %s", fullpath);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

// ── /fs/mkdir ────────────────────────────────────────────────── 

static esp_err_t mkdir_handler(httpd_req_t *req) {
    char buffer[512];
    int remaining = req->content_len, received = 0;
    if (remaining >= (int)sizeof(buffer))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
    while (remaining > 0) {
        int r = httpd_req_recv(req, buffer + received, remaining);
        if (r <= 0) { if (r == HTTPD_SOCK_ERR_TIMEOUT) continue; return ESP_FAIL; }
        remaining -= r; received += r;
    }
    buffer[received] = '\0';

    char currentdir[128] = "/", foldername[64] = {0};
    char *token = strtok(buffer, "&");
    while (token) {
        if      (strncmp(token, "current_dir=", 12) == 0) url_decode(currentdir, token+12, sizeof(currentdir));
        else if (strncmp(token, "folder_name=", 12) == 0) url_decode(foldername, token+12, sizeof(foldername));
        token = strtok(NULL, "&");
    }
    if (strlen(foldername) == 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Folder name required");

    char fullpath[256+64]; // dirpath(256) + '/' + foldername(64)
    char dirpath[256];
    build_full_path(dirpath, sizeof(dirpath), currentdir);
    snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, foldername);

    if (mkdir_p(fullpath) != 0) {
        ESP_LOGE(TAG, "mkdir_p %s failed: %s", fullpath, strerror(errno));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create folder");
    }

    char redirect[256];
    snprintf(redirect, sizeof(redirect), "/fs/?path=%s", currentdir);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);

}

// ── /fs/download ────────────────────────────────────────────────── 

 static esp_err_t download_file_handler(httpd_req_t *req) {
    char query[256], path_enc[128], path_dec[128], filepath[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", path_enc, sizeof(path_enc)) != ESP_OK)
        return httpd_resp_send_404(req);
    url_decode(path_dec, path_enc, sizeof(path_dec));
    if (strstr(path_dec, ".."))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
    build_full_path(filepath, sizeof(filepath), path_dec);

    FILE *f = fopen(filepath, "r");
    if (!f) return httpd_resp_send_404(req);

    const char *filename = strrchr(path_dec, '/');
    filename = filename ? filename + 1 : path_dec;
    httpd_resp_set_type(req, "application/octet-stream");
    char disp[256];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%.80s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    char buf[512]; size_t rb;
    while ((rb = fread(buf, 1, sizeof(buf), f)) > 0)
        if (httpd_resp_send_chunk(req, buf, rb) != ESP_OK) { fclose(f); return ESP_FAIL; }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ── /fs/delete ────────────────────────────────────────────────── 

static esp_err_t delete_handler(httpd_req_t *req) {
    char relpath[128], fullpath[256];
    if (!get_path_param(req, relpath, sizeof(relpath), fullpath, sizeof(fullpath)))
        return httpd_resp_send_404(req);
    char query[256]={0}, type[16]="file";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "type", type, sizeof(type));

    int rc = (strcmp(type, "dir") == 0) ? rmdir(fullpath) : unlink(fullpath);
    if (rc != 0)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");

    char parentdir[128];
    strncpy(parentdir, relpath, sizeof(parentdir));
    char *slash = strrchr(parentdir, '/');
    if (slash && slash != parentdir) *slash = '\0'; else strcpy(parentdir, "/");
    char redirect[256];
    snprintf(redirect, sizeof(redirect), "/fs/?path=%s", parentdir);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

// ── /fs/upload ────────────────────────────────────────────────── 

 static bool extract_filename(const char *cd, char *filename, size_t max_len) {
    if (!cd || !filename) return false;
    const char *s = strstr(cd, "filename=\"");
    if (!s) return false;
    s += strlen("filename=\"");
    const char *e = strchr(s, '"');
    if (!e) return false;
    size_t len = e - s;
    if (len == 0 || len >= max_len) return false;
    strncpy(filename, s, len);
    filename[len] = '\0';
    for (size_t i = 0; i < len; i++)
        if (filename[i]=='/' || filename[i]=='\\' || filename[i]==':') filename[i]='_';
    ESP_LOGI(TAG, "Extracted filename: %s", filename);
    return true;
}

static bool extract_form_field_value(const char *cd, const char *field,
                                     char *buf, const char *data,
                                     size_t data_len, size_t max_len) {
    char search[64];
    snprintf(search, sizeof(search), "name=\"%s\"", field);
    if (!strstr(cd, search)) return false;
    size_t n = data_len < max_len - 1 ? data_len : max_len - 1;
    strncpy(buf, data, n);
    buf[n] = '\0';
    while (n > 0 && (buf[n-1]=='\r'||buf[n-1]=='\n'||buf[n-1]==' ')) buf[--n]='\0';
    ESP_LOGI(TAG, "Extracted %s: %s", field, buf);
    return true;
}

static bool extract_boundary(const char *ct, char *boundary, size_t max_len) {
    if (!ct || !boundary) return false;
    const char *s = strstr(ct, "boundary=");
    if (!s) return false;
    s += strlen("boundary=");
    if (*s == '"') {
        s++;
        const char *e = strchr(s, '"');
        if (!e) return false;
        size_t len = e - s;
        if (len >= max_len) return false;
        strncpy(boundary, s, len);
        boundary[len] = '\0';
    } else {
        size_t i = 0;
        while (s[i] && s[i]!=' ' && s[i]!=';' && i < max_len-1) { boundary[i]=s[i]; i++; }
        boundary[i] = '\0';
    }
    ESP_LOGI(TAG, "Extracted boundary: %s", boundary);
    return true;
}

static esp_err_t upload_file_handler(httpd_req_t *req) {
    char boundary[128], full_boundary[130];
    char filename[64] = "unnamed_file";
    char current_dir[128] = "/";
    char filepath[256 + 64];   // dirpath(256) + '/' + filename(64)
    char dirpath[256];
    bool filename_found = false;

    size_t ct_len = httpd_req_get_hdr_value_len(req, "Content-Type");
    if (ct_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing Content-Type");
        return ESP_FAIL;
    }
    char *ct = malloc(ct_len + 1);
    if (!ct) { httpd_resp_send_500(req); return ESP_FAIL; }
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, ct_len+1) != ESP_OK) {
        free(ct); httpd_resp_send_500(req); return ESP_FAIL;
    }
    if (!extract_boundary(ct, boundary, sizeof(boundary))) {
        free(ct);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid boundary");
        return ESP_FAIL;
    }
    free(ct);
    snprintf(full_boundary, sizeof(full_boundary), "--%s", boundary);

    char buffer[1024];
    int remaining = req->content_len;
    bool headers_parsed = false, file_data_started = false, is_file_part = false;
    FILE *f = NULL;
    size_t boundary_len = strlen(full_boundary);
    char line_buffer[512];
    size_t line_pos = 0;
    char content_disposition[256] = {0};

    ESP_LOGI(TAG, "Starting file upload, total size: %d bytes", remaining);

    while (remaining > 0) {
        int rv = httpd_req_recv(req, buffer, MIN(remaining, (int)sizeof(buffer)));
        if (rv <= 0) {
            if (rv == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "Receive error");
            if (f) { fclose(f); unlink(filepath); }
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        remaining -= rv;

        if (!headers_parsed || !file_data_started) {
            for (int i = 0; i < rv; i++) {
                char c = buffer[i];
                if (c != '\r' && c != '\n' && line_pos < sizeof(line_buffer)-1)
                    line_buffer[line_pos++] = c;
                else if (c == '\n') {
                    line_buffer[line_pos] = '\0';
                    if (strstr(line_buffer, full_boundary)) {
                        if (f) { fclose(f); f = NULL; }
                        headers_parsed = false; file_data_started = false;
                        is_file_part = false; line_pos = 0;
                        continue;
                    }
                    if (strstr(line_buffer, "Content-Disposition")) {
                        strncpy(content_disposition, line_buffer, sizeof(content_disposition)-1);
                        content_disposition[sizeof(content_disposition)-1] = '\0';
                        if (strstr(line_buffer, "filename=")) {
                            is_file_part = true;
                            extract_filename(line_buffer, filename, sizeof(filename));
                            filename_found = true;
                        } else {
                            is_file_part = false;
                        }
                    }
                    if (line_pos == 0 && strlen(content_disposition) > 0) {
                        headers_parsed = true;
                        if (is_file_part && filename_found) {
                            build_full_path(dirpath, sizeof(dirpath), current_dir);
                            snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, filename);
                            f = fopen(filepath, "w");
                            if (!f) {
                                ESP_LOGE(TAG, "Failed to open %s", filepath);
                                httpd_resp_send_500(req);
                                return ESP_FAIL;
                            }
                            ESP_LOGI(TAG, "Saving to: %s", filepath);
                            file_data_started = true;
                            i++;
                            if (i < rv) fwrite(&buffer[i], 1, rv - i, f);
                            break;
                        } else {
                            i++;
                            if (i < rv) {
                                int vs = i, ve = i;
                                while (ve < rv && buffer[ve] != '\r') ve++;
                                extract_form_field_value(content_disposition, "current_dir",
                                                         current_dir, &buffer[vs], ve-vs, sizeof(current_dir));
                                headers_parsed = false;
                                content_disposition[0] = '\0';
                                i = ve;
                            }
                        }
                    }
                    line_pos = 0;
                }
            }
        } else if (file_data_started && f) {
            fwrite(buffer, 1, rv, f);
        }
    }

    if (f) {
        long fsz = ftell(f);
        fclose(f);
        f = fopen(filepath, "r+");
        if (f) {
            long trunc = fsz - (long)(boundary_len + 6);
            if (trunc > 0) ftruncate(fileno(f), trunc);
            fclose(f);
        }
        ESP_LOGI(TAG, "File uploaded: %s -> dir: %s", filename, current_dir);
    }

    char redirect[256];
    if (strcmp(current_dir, "/") == 0) strcpy(redirect, "/fs/");
    else snprintf(redirect, sizeof(redirect), "/fs/?path=%s", current_dir);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

// ── JSON deep-merge ────────────────────────────────────────────────── 
// for the "patch" WS command

static void json_deep_merge(cJSON *base, const cJSON *patch) {
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, patch) {
        cJSON *existing = cJSON_GetObjectItemCaseSensitive(base, item->string);
        if (cJSON_IsObject(item) && cJSON_IsObject(existing)) {
            json_deep_merge(existing, item);
        } else {
            cJSON *dup = cJSON_Duplicate(item, true);
            if (existing)
                cJSON_ReplaceItemInObjectCaseSensitive(base, item->string, dup);
            else
                cJSON_AddItemToObject(base, item->string, dup);
        }
    }
}

static char *read_fs_file(const char *fullpath, size_t *out_len) {
    FILE *f = fopen(fullpath, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)sz;
    return buf;
}

static int write_fs_file(const char *fullpath, const char *data, size_t len) {
    FILE *f = fopen(fullpath, "w");
    if (!f) return -1;
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    return (written == len) ? 0 : -1;
}

// ── WebSocket "fs" dispatcher ────────────────────────────────────────────────── 

#define FS_GET_STR(j, key) \
    ({ cJSON *_v = cJSON_GetObjectItemCaseSensitive(j, key); \
       cJSON_IsString(_v) ? _v->valuestring : NULL; })

static char *fs_ws_response(const char *cmd, cJSON *payload, const char *error) {
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "fs_response");
    cJSON_AddStringToObject(resp, "cmd",  cmd ? cmd : "?");
    if (error) {
        cJSON_AddStringToObject(resp, "error", error);
    } else {
        if (payload) {
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, payload)
                cJSON_AddItemToObject(resp, item->string, cJSON_Duplicate(item, true));
        }
        cJSON_AddBoolToObject(resp, "ok", true);
    }
    char *s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return s;
}

static bool fs_ws_handler(const char *type, cJSON *json) {
    if (strcmp(type, "fs") != 0) return false;

    cJSON *cmd_obj = cJSON_GetObjectItemCaseSensitive(json, "cmd");
    if (!cJSON_IsString(cmd_obj)) {
        ESP_LOGW(TAG, "fs WS message missing 'cmd'");
        return true;
    }
    const char *cmd = cmd_obj->valuestring;
    char fullpath[256], fullpath2[256];
    char *resp_str = NULL;

    if (strcmp(cmd, "ls") == 0) {
        const char *path = FS_GET_STR(json, "path");
        if (!path) path = "/";
        if (strstr(path, "..")) goto bad_path;
        build_full_path(fullpath, sizeof(fullpath), path);
        bool recursive = false;
        cJSON *rec = cJSON_GetObjectItemCaseSensitive(json, "recursive");
        if (cJSON_IsNumber(rec)) recursive = (rec->valueint != 0);
        if (cJSON_IsBool(rec))   recursive = cJSON_IsTrue(rec);
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "path", path);
        cJSON *arr = cJSON_AddArrayToObject(result, "entries");
        ls_dir_into_array(fullpath, path, arr, recursive);
        resp_str = fs_ws_response(cmd, result, NULL);
        cJSON_Delete(result);

    } else if (strcmp(cmd, "df") == 0) {
        size_t total=0, used=0;
        esp_littlefs_info(LITTLEFS_LABEL, &total, &used);
        cJSON *result = cJSON_CreateObject();
        cJSON_AddNumberToObject(result, "total_bytes", (double)total);
        cJSON_AddNumberToObject(result, "used_bytes",  (double)used);
        cJSON_AddNumberToObject(result, "free_bytes",  (double)(total-used));
        resp_str = fs_ws_response(cmd, result, NULL);
        cJSON_Delete(result);

    } else if (strcmp(cmd, "exists") == 0) {
        const char *path = FS_GET_STR(json, "path");
        if (!path || strstr(path, "..")) goto bad_path;
        build_full_path(fullpath, sizeof(fullpath), path);
        struct stat st;
        cJSON *result = cJSON_CreateObject();
        if (stat(fullpath, &st) == 0) {
            bool isdir = S_ISDIR(st.st_mode);
            cJSON_AddBoolToObject(result, "exists", true);
            cJSON_AddStringToObject(result, "type", isdir ? "dir" : "file");
            if (!isdir) cJSON_AddNumberToObject(result, "size", (double)st.st_size);
        } else {
            cJSON_AddBoolToObject(result, "exists", false);
        }
        resp_str = fs_ws_response(cmd, result, NULL);
        cJSON_Delete(result);

    } else if (strcmp(cmd, "mkdir") == 0) {
        const char *path = FS_GET_STR(json, "path");
        if (!path || strstr(path, "..")) goto bad_path;
        build_full_path(fullpath, sizeof(fullpath), path);
        if (mkdir_p(fullpath) != 0)
            resp_str = fs_ws_response(cmd, NULL, strerror(errno));
        else { cJSON *r = cJSON_CreateObject(); resp_str = fs_ws_response(cmd, r, NULL); cJSON_Delete(r); }
    } else if (strcmp(cmd, "delete") == 0) {
        const char *path = FS_GET_STR(json, "path");
        if (!path || strstr(path, "..")) goto bad_path;
        build_full_path(fullpath, sizeof(fullpath), path);
        int rc = is_directory(fullpath) ? rmdir(fullpath) : unlink(fullpath);
        if (rc != 0) resp_str = fs_ws_response(cmd, NULL, strerror(errno));
        else { cJSON *r = cJSON_CreateObject(); resp_str = fs_ws_response(cmd, r, NULL); cJSON_Delete(r); }

    } else if (strcmp(cmd, "rmdir") == 0) {
        const char *path = FS_GET_STR(json, "path");
        if (!path || strstr(path, "..")) goto bad_path;
        build_full_path(fullpath, sizeof(fullpath), path);
        if (rmdir_recursive(fullpath) != 0)
            resp_str = fs_ws_response(cmd, NULL, strerror(errno));
        else { cJSON *r = cJSON_CreateObject(); resp_str = fs_ws_response(cmd, r, NULL); cJSON_Delete(r); }

    } else if (strcmp(cmd, "rename") == 0) {
        const char *from = FS_GET_STR(json, "from");
        const char *to   = FS_GET_STR(json, "to");

// don't check for ".." in rename (allows to move file up)
//        if (!from || !to || strstr(from, "..") || strstr(to, "..")) goto bad_path;
        if (!from || !to) goto bad_path;
    
        if (rename(fullpath, fullpath2) != 0)
            resp_str = fs_ws_response(cmd, NULL, strerror(errno));
        else { cJSON *r = cJSON_CreateObject(); resp_str = fs_ws_response(cmd, r, NULL); cJSON_Delete(r); }

    } else if (strcmp(cmd, "patch") == 0) {
        const char *path = FS_GET_STR(json, "path");
        cJSON *patch_obj = cJSON_GetObjectItemCaseSensitive(json, "patch");
        if (!path || strstr(path, "..")) goto bad_path;
        if (!cJSON_IsObject(patch_obj)) {
            resp_str = fs_ws_response(cmd, NULL, "Missing 'patch' object");
            goto done;
        }
        build_full_path(fullpath, sizeof(fullpath), path);
        size_t flen = 0;
        char *raw = read_fs_file(fullpath, &flen);
        cJSON *base = raw ? cJSON_Parse(raw) : NULL;
        free(raw);
        if (!cJSON_IsObject(base)) { cJSON_Delete(base); base = cJSON_CreateObject(); }
        json_deep_merge(base, patch_obj);
        char *out = cJSON_PrintUnformatted(base);
        cJSON_Delete(base);
        if (!out || write_fs_file(fullpath, out, strlen(out)) != 0) {
            free(out);
            resp_str = fs_ws_response(cmd, NULL, "Write failed");
        } else {
            free(out);
            cJSON *r = cJSON_CreateObject();
            resp_str = fs_ws_response(cmd, r, NULL);
            cJSON_Delete(r);
        }

    } else {
        resp_str = fs_ws_response(cmd, NULL, "Unknown command");
    }
    goto done;

bad_path:
    resp_str = fs_ws_response(cmd, NULL, "Invalid path");

done:
    if (resp_str) {
        websocket_broadcast_json_transient(resp_str);
        free(resp_str);
    }
#undef FS_GET_STR
    return true;
}


static esp_err_t filemanager_page_handler(httpd_req_t *req) {
    const char *fragments[] = {
        "head.html",
        "filemanager_body.html" // we close the body here, no need for other js which is in common footer.html
    };
    return serve_html_fragments(req, fragments, 2);
}


// ── Register handlers ────────────────────────────────────────────────── 

uint8_t register_file_handlers_in_web_server(httpd_handle_t *server) {
    uint8_t n = 0;

    ws_register_message_handler(fs_ws_handler);

    httpd_uri_t uris[] = {
        {
            .uri = "/fs",
            .method = HTTP_GET,
            .handler = filemanager_page_handler,
            .user_ctx = NULL,
            .is_websocket = false,
        },
        {
            .uri = "/fs/ls",
            .method = HTTP_GET,
            .handler = ls_handler,
            .user_ctx = NULL,
            .is_websocket = false,
        },
        {
            .uri = "/fs/df",
            .method = HTTP_GET,
            .handler = df_handler,
            .user_ctx = NULL,
            .is_websocket = false,
        },
        {
            .uri = "/fs/exists",
            .method = HTTP_GET,
            .handler = exists_handler,
            .user_ctx = NULL,
            .is_websocket = false,
        },
        {
            .uri = "/fs/rename",
            .method = HTTP_GET,
            .handler = rename_handler,
            .user_ctx = NULL,
            .is_websocket = false,
        },
        {
            .uri = "/fs/rmdir",
            .method = HTTP_GET,
            .handler = rmdir_handler,
            .user_ctx = NULL,
            .is_websocket = false,
        },
        {
            .uri = "/fs/mkdir",
            .method = HTTP_POST,
            .handler = mkdir_handler,
            .user_ctx = NULL,
            .is_websocket = false,
        },
        {
            .uri = "/fs/download",
            .method = HTTP_GET,
            .handler = download_file_handler,
            .user_ctx = NULL,
            .is_websocket = false,
        },
        {
            .uri = "/fs/upload",
            .method = HTTP_POST,
            .handler = upload_file_handler,
            .user_ctx = NULL,
            .is_websocket = false,
        },
        {
            .uri = "/fs/delete",
            .method = HTTP_GET,
            .handler = delete_handler,
            .user_ctx = NULL,
            .is_websocket = false,
        },
    };

    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
        httpd_register_uri_handler(*server, &uris[i]);
        n++;
    }

    ESP_LOGI(TAG, "File manager: %d HTTP handlers + 1 WS handler registered", n);
    return n;
}
