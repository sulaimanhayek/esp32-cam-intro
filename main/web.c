#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "auth.h"
#include "camera.h"
#include "flash_led.h"
#include "storage.h"

static const char *TAG = "web";

// main/*.html, embedded by EMBED_TXTFILES (null-terminated)
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char login_html_start[] asm("_binary_login_html_start");

#define SESSION_COOKIE   "sid"
#define MAX_LOGIN_BODY   256

#define STREAM_BOUNDARY "123456789000000000000987654321"

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// Read an integer query parameter, or return `def` if it's missing
static int query_int(httpd_req_t *req, const char *key, int def)
{
    char query[64], val[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, key, val, sizeof(val)) == ESP_OK) {
        return atoi(val);
    }
    return def;
}

static void set_security_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "frame-ancestors 'none'");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static bool is_logged_in(httpd_req_t *req)
{
    char token[AUTH_TOKEN_LEN + 1];
    size_t len = sizeof(token);
    return httpd_req_get_cookie_val(req, SESSION_COOKIE, token, &len) == ESP_OK &&
           auth_session_valid(token);
}

// Every API handler starts with this; replies 401 when there's no valid session
static bool require_login(httpd_req_t *req)
{
    set_security_headers(req);
    if (is_logged_in(req)) {
        return true;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    send_json(req, "{\"ok\":false,\"error\":\"login required\"}");
    return false;
}

static esp_err_t redirect(httpd_req_t *req, const char *location)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_send(req, NULL, 0);
}

// Decode an application/x-www-form-urlencoded value in place
static void url_decode(char *s)
{
    char *out = s;
    for (; *s; s++) {
        if (*s == '+') {
            *out++ = ' ';
        } else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = {s[1], s[2], 0};
            *out++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *out++ = *s;
        }
    }
    *out = '\0';
}

static esp_err_t index_handler(httpd_req_t *req)
{
    set_security_headers(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, is_logged_in(req) ? index_html_start : login_html_start);
}

static esp_err_t login_handler(httpd_req_t *req)
{
    set_security_headers(req);
    if (req->content_len == 0 || req->content_len > MAX_LOGIN_BODY) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
    }

    char body[MAX_LOGIN_BODY + 1];
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += n;
    }
    body[got] = '\0';

    char password[MAX_LOGIN_BODY + 1] = "";
    httpd_query_key_value(body, "password", password, sizeof(password));
    url_decode(password);

    char token[AUTH_TOKEN_LEN + 1];
    int retry_after = 0;
    auth_result_t res = auth_login(password, token, &retry_after);
    memset(password, 0, sizeof(password));
    memset(body, 0, sizeof(body));

    if (res == AUTH_OK) {
        // HttpOnly: invisible to scripts. SameSite=Strict: never sent on requests
        // started by other websites, which blocks CSRF against the API.
        char cookie[128];
        snprintf(cookie, sizeof(cookie),
                 SESSION_COOKIE "=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=43200", token);
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);
        return redirect(req, "/");
    }
    if (res == AUTH_LOCKED) {
        char location[32];
        snprintf(location, sizeof(location), "/?e=locked&wait=%d", retry_after);
        return redirect(req, location);
    }
    return redirect(req, "/?e=bad");
}

static esp_err_t logout_handler(httpd_req_t *req)
{
    set_security_headers(req);
    char token[AUTH_TOKEN_LEN + 1];
    size_t len = sizeof(token);
    if (httpd_req_get_cookie_val(req, SESSION_COOKIE, token, &len) == ESP_OK) {
        auth_logout(token);
    }
    httpd_resp_set_hdr(req, "Set-Cookie", SESSION_COOKIE "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    return redirect(req, "/");
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    bool flash = query_int(req, "flash", 0) != 0;
    char name[24];
    char json[64];

    if (!storage_is_mounted()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return send_json(req, "{\"ok\":false,\"error\":\"no SD card\"}");
    }
    if (camera_take_photo(flash, name, sizeof(name)) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json(req, "{\"ok\":false,\"error\":\"capture failed\"}");
    }
    snprintf(json, sizeof(json), "{\"ok\":true,\"file\":\"%s\"}", name);
    return send_json(req, json);
}

static esp_err_t torch_handler(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char json[32];
    flash_led_set(query_int(req, "level", 0));
    snprintf(json, sizeof(json), "{\"level\":%d}", flash_led_get());
    return send_json(req, json);
}

// JSON list of the photos on the SD card
static esp_err_t photos_list_handler(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    DIR *dir = storage_is_mounted() ? opendir(STORAGE_MOUNT_POINT) : NULL;
    if (!dir) {
        return httpd_resp_sendstr(req, "[]");
    }

    httpd_resp_sendstr_chunk(req, "[");
    bool first = true;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "IMG_", 4) != 0) {
            continue;
        }
        char path[300];
        struct stat st = {0};
        snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/%s", ent->d_name);
        stat(path, &st);

        char item[300];
        snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"size\":%ld}",
                 first ? "" : ",", ent->d_name, (long)st.st_size);
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }
    closedir(dir);
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t photo_file_handler(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    const char *name = req->uri + strlen("/photos/");
    if (strncmp(name, "IMG_", 4) != 0 || strchr(name, '/') || strstr(name, "..")) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }

    char path[64];
    snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }

    httpd_resp_set_type(req, "image/jpeg");
    static char buf[4096];  // handlers on one server run sequentially
    size_t n;
    esp_err_t err = ESP_OK;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if ((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) {
            break;
        }
    }
    fclose(f);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY);
    ESP_LOGI(TAG, "Stream client connected");

    esp_err_t err = ESP_OK;
    char part[128];
    while (err == ESP_OK) {
        camera_fb_t *fb = camera_grab();
        if (!fb) {
            ESP_LOGE(TAG, "Frame grab failed");
            err = ESP_FAIL;
            break;
        }
        int len = snprintf(part, sizeof(part),
                           "\r\n--" STREAM_BOUNDARY "\r\n"
                           "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                           (unsigned)fb->len);
        err = httpd_resp_send_chunk(req, part, len);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }
        camera_release(fb);
    }
    ESP_LOGI(TAG, "Stream client disconnected");
    return err;
}

esp_err_t web_start(void)
{
    httpd_handle_t control = NULL;
    httpd_handle_t stream = NULL;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_handler},
        {.uri = "/login", .method = HTTP_POST, .handler = login_handler},
        {.uri = "/logout", .method = HTTP_POST, .handler = logout_handler},
        // State-changing actions are POST-only
        {.uri = "/capture", .method = HTTP_POST, .handler = capture_handler},
        {.uri = "/torch", .method = HTTP_POST, .handler = torch_handler},
        {.uri = "/photos", .method = HTTP_GET, .handler = photos_list_handler},
        {.uri = "/photos/*", .method = HTTP_GET, .handler = photo_file_handler},
    };

    ESP_ERROR_CHECK(auth_init());
    ESP_ERROR_CHECK(httpd_start(&control, &config));
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(control, &routes[i]);
    }

    // The stream handler never returns while a client watches, so it gets its
    // own server (and task) to keep the control API responsive.
    config.server_port = 81;
    config.ctrl_port += 1;
    config.stack_size = 4096;
    const httpd_uri_t stream_route = {.uri = "/stream", .method = HTTP_GET, .handler = stream_handler};
    ESP_ERROR_CHECK(httpd_start(&stream, &config));
    httpd_register_uri_handler(stream, &stream_route);

    ESP_LOGI(TAG, "Web UI on port 80, stream on port 81");
    return ESP_OK;
}
