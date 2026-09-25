#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define AUTH_TOKEN_LEN 32  // hex chars (128 random bits)

typedef enum {
    AUTH_OK,
    AUTH_BAD_PASSWORD,
    AUTH_LOCKED,  // too many failures, wait before retrying
} auth_result_t;

// Load (or generate) the web password. NVS must already be initialised.
esp_err_t auth_init(void);

// On success writes a new session token (AUTH_TOKEN_LEN + 1 bytes) to token_out.
// On AUTH_LOCKED, *retry_after_s says how long until the next attempt is allowed.
auth_result_t auth_login(const char *password, char *token_out, int *retry_after_s);

bool auth_session_valid(const char *token);
void auth_logout(const char *token);
