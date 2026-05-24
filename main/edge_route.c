// Edge router client. See edge_route.h.
//
// We use the IDF crt bundle for TLS (same as scadable_client) so the
// device trusts Let's Encrypt without any per-device cert provisioning
// on this side of the call.
//
// The HTTP body is small (~120 bytes) — we collect it in a fixed
// buffer via the ON_DATA event so we don't need to deal with chunked
// reads. If the body would ever exceed our buffer, we truncate and
// the JSON parse fails — caller treats that as "edge unreachable" and
// keeps the previous route, which is the safe default.

#include "edge_route.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#ifndef SCADABLE_EDGE_URL
#define SCADABLE_EDGE_URL "https://app.scadable.com/v1/route"
#endif

#define EDGE_BODY_MAX 512

static const char *TAG = "edge_route";

static char s_common_name[64] = "";

void edge_route_init(const char *common_name) {
    if (!common_name) return;
    strncpy(s_common_name, common_name, sizeof(s_common_name) - 1);
    s_common_name[sizeof(s_common_name) - 1] = '\0';
}

typedef struct {
    char *buf;
    int   cap;
    int   len;
} body_ctx_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt) {
    body_ctx_t *ctx = (body_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx && evt->data && evt->data_len > 0) {
        int room = ctx->cap - 1 - ctx->len;
        if (room <= 0) return ESP_OK;
        int n = evt->data_len < room ? evt->data_len : room;
        memcpy(ctx->buf + ctx->len, evt->data, n);
        ctx->len += n;
        ctx->buf[ctx->len] = '\0';
    }
    return ESP_OK;
}

esp_err_t edge_route_fetch(char *host_out, size_t host_n,
                           int  *port_out,
                           char *region_out, size_t region_n) {
    if (!host_out || !host_n || !port_out) return ESP_ERR_INVALID_ARG;

    char body[EDGE_BODY_MAX];
    body[0] = '\0';
    body_ctx_t ctx = { .buf = body, .cap = sizeof(body), .len = 0 };

    esp_http_client_config_t cfg = {
        .url               = SCADABLE_EDGE_URL,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = http_event_cb,
        .user_data         = &ctx,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;

    if (s_common_name[0]) {
        esp_http_client_set_header(cli, "X-Device-CN", s_common_name);
    }

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET %s: %s", SCADABLE_EDGE_URL, esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "GET %s: status %d", SCADABLE_EDGE_URL, status);
        return ESP_FAIL;
    }
    if (ctx.len <= 0) {
        ESP_LOGW(TAG, "empty body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGW(TAG, "parse failed: %.*s", ctx.len, body);
        return ESP_FAIL;
    }

    cJSON *host_j   = cJSON_GetObjectItemCaseSensitive(root, "mqtt_host");
    cJSON *port_j   = cJSON_GetObjectItemCaseSensitive(root, "mqtt_port");
    cJSON *region_j = cJSON_GetObjectItemCaseSensitive(root, "region");

    esp_err_t out = ESP_FAIL;
    if (cJSON_IsString(host_j) && host_j->valuestring && cJSON_IsNumber(port_j)) {
        strncpy(host_out, host_j->valuestring, host_n - 1);
        host_out[host_n - 1] = '\0';
        *port_out = port_j->valueint;
        if (region_out && region_n > 0) {
            if (cJSON_IsString(region_j) && region_j->valuestring) {
                strncpy(region_out, region_j->valuestring, region_n - 1);
                region_out[region_n - 1] = '\0';
            } else {
                region_out[0] = '\0';
            }
        }
        ESP_LOGI(TAG, "route: %s:%d (region=%s)", host_out, *port_out,
                 region_out ? region_out : "");
        out = ESP_OK;
    } else {
        ESP_LOGW(TAG, "missing mqtt_host/mqtt_port in: %.*s", ctx.len, body);
    }

    cJSON_Delete(root);
    return out;
}
