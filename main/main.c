#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#define PULSE_GPIO GPIO_NUM_4
#define HOOK_GPIO GPIO_NUM_5
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "ROTARY_SIP";
static EventGroupHandle_t s_wifi_event_group;
static QueueHandle_t s_digit_queue;
static esp_timer_handle_t s_rotary_timer;

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char sip_user[65];
    char sip_pass[65];
    char sip_domain[65];
    char sip_extension[16];
} app_config_t;

static app_config_t s_config;
static volatile int s_pulse_count = 0;
static volatile bool s_hook_lifted = false;
static volatile uint64_t s_last_pulse_time = 0;

static void save_config(void) {
    nvs_handle_t nvs;
    if (nvs_open("rotary", NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace");
        return;
    }
    if (nvs_set_blob(nvs, "config", &s_config, sizeof(s_config)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write config");
        nvs_close(nvs);
        return;
    }
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Configuration saved");
}

static void load_config(void) {
    nvs_handle_t nvs;
    size_t size = sizeof(s_config);
    if (nvs_open("rotary", NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_blob(nvs, "config", &s_config, &size) != ESP_OK) {
            memset(&s_config, 0, sizeof(s_config));
        }
        nvs_close(nvs);
    } else {
        memset(&s_config, 0, sizeof(s_config));
    }
}

static esp_err_t wifi_event_handler(void *ctx, system_event_t *event) {
    switch (event->event_id) {
        case SYSTEM_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            esp_wifi_connect();
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            break;
        case SYSTEM_EVENT_STA_GOT_IP:
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, s_config.wifi_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, s_config.wifi_pass, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi_init_sta finished. SSID:%s", s_config.wifi_ssid);
}

static void sip_place_call(const char *number) {
    // Placeholder for actual SIP call initiation using ESP-ADF or PJSIP.
    ESP_LOGI(TAG, "Dialing %s via SIP user %s@%s", number, s_config.sip_user, s_config.sip_domain);
}

static void rotary_timeout(void *arg) {
    int pulses = s_pulse_count;
    s_pulse_count = 0;
    if (pulses == 0) {
        return;
    }
    int digit = pulses % 10;
    if (digit == 0) {
        digit = 0;
    }
    if (s_hook_lifted) {
        xQueueSendFromISR(s_digit_queue, &digit, NULL);
    }
}

static void IRAM_ATTR rotary_isr_handler(void *arg) {
    uint64_t now = esp_timer_get_time();
    s_pulse_count++;
    s_last_pulse_time = now;
    esp_timer_start_once(s_rotary_timer, 70000); // 70ms gap ends digit
}

static void IRAM_ATTR hook_isr_handler(void *arg) {
    s_hook_lifted = gpio_get_level(HOOK_GPIO) == 1;
    s_pulse_count = 0;
    esp_timer_stop(s_rotary_timer);
    xQueueReset(s_digit_queue);
}

static void rotary_init(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << PULSE_GPIO,
        .pull_up_en = 1,
        .pull_down_en = 0
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PULSE_GPIO, rotary_isr_handler, NULL);

    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    io_conf.pin_bit_mask = 1ULL << HOOK_GPIO;
    gpio_config(&io_conf);
    gpio_isr_handler_add(HOOK_GPIO, hook_isr_handler, NULL);

    const esp_timer_create_args_t timer_args = {
        .callback = rotary_timeout,
        .name = "rotary_timeout"
    };
    esp_timer_create(&timer_args, &s_rotary_timer);
}

static const char *HTML_FORM =
"<!DOCTYPE html><html><head><meta charset='utf-8'><title>Rotary SIP</title>" \
"<style>body{font-family:sans-serif;margin:1rem;}label{display:block;margin-top:.6rem;}input{width:100%;padding:.4rem;}</style>" \
"</head><body><h2>Rotary SIP Phone</h2><form method='POST' action='/save'>" \
"<label>WiFi SSID<input name='wifi_ssid' value='%s'/></label>" \
"<label>WiFi Password<input name='wifi_pass' type='password' value='%s'/></label>" \
"<label>SIP User<input name='sip_user' value='%s'/></label>" \
"<label>SIP Password<input name='sip_pass' type='password' value='%s'/></label>" \
"<label>SIP Domain<input name='sip_domain' value='%s'/></label>" \
"<label>SIP Extension<input name='sip_extension' value='%s'/></label>" \
"<button type='submit'>Save</button></form></body></html>";

static void send_form(httpd_req_t *req) {
    char buffer[1024];
    snprintf(buffer, sizeof(buffer), HTML_FORM,
             s_config.wifi_ssid, s_config.wifi_pass,
             s_config.sip_user, s_config.sip_pass,
             s_config.sip_domain, s_config.sip_extension);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buffer, HTTPD_RESP_USE_STRLEN);
}

static void parse_field(const char *body, const char *key, char *out, size_t out_len) {
    char needle[32];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *start = strstr(body, needle);
    if (!start) {
        return;
    }
    start += strlen(needle);
    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    if (len >= out_len) {
        len = out_len - 1;
    }
    // simple URL decode for '+' to space; ignores % encoding for brevity
    for (size_t i = 0; i < len; ++i) {
        out[i] = start[i] == '+' ? ' ' : start[i];
    }
    out[len] = '\0';
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    send_form(req);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req) {
    char buf[512] = {0};
    int total_len = req->content_len;
    if (total_len >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Payload too large");
        return ESP_FAIL;
    }
    int received = httpd_req_recv(req, buf, total_len);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read body");
        return ESP_FAIL;
    }
    parse_field(buf, "wifi_ssid", s_config.wifi_ssid, sizeof(s_config.wifi_ssid));
    parse_field(buf, "wifi_pass", s_config.wifi_pass, sizeof(s_config.wifi_pass));
    parse_field(buf, "sip_user", s_config.sip_user, sizeof(s_config.sip_user));
    parse_field(buf, "sip_pass", s_config.sip_pass, sizeof(s_config.sip_pass));
    parse_field(buf, "sip_domain", s_config.sip_domain, sizeof(s_config.sip_domain));
    parse_field(buf, "sip_extension", s_config.sip_extension, sizeof(s_config.sip_extension));
    save_config();
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

static httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
            .user_ctx = NULL
        };
        httpd_uri_t save = {
            .uri = "/save",
            .method = HTTP_POST,
            .handler = save_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &save);
    }
    return server;
}

static void dial_task(void *pvParameters) {
    char dialed_number[32] = {0};
    size_t pos = 0;
    while (true) {
        int digit = -1;
        if (xQueueReceive(s_digit_queue, &digit, portMAX_DELAY) == pdTRUE) {
            if (digit >= 0 && digit <= 9 && pos < sizeof(dialed_number) - 1) {
                dialed_number[pos++] = '0' + digit;
                dialed_number[pos] = '\0';
                ESP_LOGI(TAG, "Digit: %d, number: %s", digit, dialed_number);
            }
            // simple timeout to dial after 2 seconds idle
            TickType_t start = xTaskGetTickCount();
            while (xQueuePeek(s_digit_queue, &digit, pdMS_TO_TICKS(2000)) != pdTRUE) {
                if (pos > 0 && s_hook_lifted) {
                    sip_place_call(dialed_number);
                }
                pos = 0;
                memset(dialed_number, 0, sizeof(dialed_number));
                break;
            }
        }
    }
}

static void app_start(void) {
    load_config();
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    rotary_init();
    start_webserver();
    wifi_init_sta();
    s_digit_queue = xQueueCreate(16, sizeof(int));
    xTaskCreate(&dial_task, "dial_task", 4096, NULL, 5, NULL);
}

void app_main(void) {
    ESP_LOGI(TAG, "Booting Rotary SIP Phone");
    app_start();
}
