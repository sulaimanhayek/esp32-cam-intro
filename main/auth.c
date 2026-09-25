#include "auth.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "secret.h"

static const char *TAG = "auth";

#define MAX_SESSIONS        4
#define SESSION_TTL_S       (12 * 60 * 60)
#define FREE_ATTEMPTS       5       // failures allowed before lockouts start
#define LOCKOUT_BASE_S      30      // doubles with every further failure
#define LOCKOUT_MAX_S       (15 * 60)

typedef struct {
    char token[AUTH_TOKEN_LEN + 1];
    int64_t expires_us;
} session_t;

static char s_password[65];
static session_t s_sessions[MAX_SESSIONS];
static int s_failures;
static int64_t s_locked_until_us;
static SemaphoreHandle_t s_lock;  // control + stream servers run in different tasks

// Compare without an early exit so response time doesn't reveal how many
// leading characters were right
static bool secure_equals(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    size_t n = la > lb ? la : lb;
    unsigned char diff = la != lb;
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = i < la ? a[i] : 0;
        unsigned char cb = i < lb ? b[i] : 0;
        diff |= ca ^ cb;
    }
    return diff == 0;
}

esp_err_t auth_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    esp_err_t err = secret_get("web_pass", CONFIG_WEB_PASSWORD, s_password, sizeof(s_password));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Web UI password: %s", s_password);
    }
    return err;
}

auth_result_t auth_login(const char *password, char *token_out, int *retry_after_s)
{
    int64_t now = esp_timer_get_time();
    auth_result_t result;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (now < s_locked_until_us) {
        *retry_after_s = (int)((s_locked_until_us - now) / 1000000) + 1;
        result = AUTH_LOCKED;
    } else if (!secure_equals(password, s_password)) {
        s_failures++;
        if (s_failures >= FREE_ATTEMPTS) {
            int shift = s_failures - FREE_ATTEMPTS;
            int64_t lock_s = shift > 10 ? LOCKOUT_MAX_S : (int64_t)LOCKOUT_BASE_S << shift;
            if (lock_s > LOCKOUT_MAX_S) {
                lock_s = LOCKOUT_MAX_S;
            }
            s_locked_until_us = now + lock_s * 1000000;
            ESP_LOGW(TAG, "%d failed logins, locked for %lld s", s_failures, lock_s);
        } else {
            ESP_LOGW(TAG, "Failed login (%d)", s_failures);
        }
        result = AUTH_BAD_PASSWORD;
    } else {
        s_failures = 0;

        // Reuse an expired slot, or evict the one closest to expiring
        session_t *slot = &s_sessions[0];
        for (int i = 1; i < MAX_SESSIONS; i++) {
            if (s_sessions[i].expires_us < slot->expires_us) {
                slot = &s_sessions[i];
            }
        }
        // esp_random() is a true RNG while the WiFi radio is on
        for (int i = 0; i < AUTH_TOKEN_LEN / 8; i++) {
            snprintf(slot->token + i * 8, 9, "%08lx", (unsigned long)esp_random());
        }
        slot->expires_us = now + (int64_t)SESSION_TTL_S * 1000000;
        strlcpy(token_out, slot->token, AUTH_TOKEN_LEN + 1);
        ESP_LOGI(TAG, "Login OK");
        result = AUTH_OK;
    }
    xSemaphoreGive(s_lock);
    return result;
}

bool auth_session_valid(const char *token)
{
    if (strlen(token) != AUTH_TOKEN_LEN) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    bool valid = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].expires_us > now && secure_equals(token, s_sessions[i].token)) {
            valid = true;
        }
    }
    xSemaphoreGive(s_lock);
    return valid;
}

void auth_logout(const char *token)
{
    if (strlen(token) != AUTH_TOKEN_LEN) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (secure_equals(token, s_sessions[i].token)) {
            memset(&s_sessions[i], 0, sizeof(s_sessions[i]));
        }
    }
    xSemaphoreGive(s_lock);
}
