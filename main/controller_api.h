#ifndef CONTROLLER_API_H
#define CONTROLLER_API_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/*
 * HIL controller API: USB device inventory, hub per-port power, the analog
 * I2C mux, an MCP endpoint for agents, and optional Bearer token auth.
 * Served by http_server.c; see GET /api/schema for the endpoint list.
 */

typedef struct {
    int status;                 /* HTTP status code */
    char *body;                 /* malloc'd, may be NULL for an empty body */
    size_t len;
    const char *content_type;   /* NULL means JSON */
} api_response_t;

/* Load the API token. */
esp_err_t controller_api_init(void);

/* Route a request.  Returns false if the URL is not handled here.
   `auth` is the Authorization header value (may be NULL). */
bool controller_api_handle(const char *method, const char *url, const char *auth,
                           const char *body, size_t body_len, api_response_t *resp);

/* Whether a request carrying `auth` may mutate state (true when no token is set). */
bool controller_api_authorized(const char *auth);

/* Reason phrase for an HTTP status code. */
const char *controller_api_status_text(int status);

#endif
