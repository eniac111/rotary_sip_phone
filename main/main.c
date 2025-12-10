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
#include "esp_rtc.h"

#define PULSE_GPIO GPIO_NUM_4
#define HOOK_GPIO GPIO_NUM_5
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "ROTARY_SIP";
static EventGroupHandle_t s_wifi_event_group;
static QueueHandle_t s_digit_queue;
static esp_timer_handle_t s_rotary_timer;
static esp_rtc_handle_t s_rtc_handle;

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
static volatile bool s_sip_registered = false;

static int rtc_event_handler(esp_rtc_event_t event, void *ctx) {
    (void)ctx;
    switch (event) {
        case ESP_RTC_EVENT_REGISTERED:
            s_sip_registered = true;
            ESP_LOGI(TAG, "SIP registered");
            break;
        case ESP_RTC_EVENT_UNREGISTERED:
            s_sip_registered = false;
            ESP_LOGW(TAG, "SIP unregistered");
            break;
        case ESP_RTC_EVENT_INCOMING:
            ESP_LOGI(TAG, "Incoming call");
            break;
        case ESP_RTC_EVENT_AUDIO_SESSION_BEGIN:
            ESP_LOGI(TAG, "SIP audio session started");
            break;
        case ESP_RTC_EVENT_AUDIO_SESSION_END:
        case ESP_RTC_EVENT_HANGUP:
            ESP_LOGI(TAG, "SIP audio session ended");
            break;
        default:
            break;
    }
    return 0;
}

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

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc = (const wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Station disconnected (reason=%d), retrying", disc->reason);
        esp_wifi_connect();
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        if (s_rtc_handle) {
            esp_rtc_service_deinit(s_rtc_handle);
            s_rtc_handle = NULL;
            s_sip_registered = false;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (s_rtc_handle == NULL && s_config.sip_user[0] && s_config.sip_domain[0]) {
            wifi_ap_record_t ap_info = {0};
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                ESP_LOGI(TAG, "WiFi connected to %s, starting SIP", (char *)ap_info.ssid);
            }
            char sip_uri[256];
            const char *user = s_config.sip_extension[0] ? s_config.sip_extension : s_config.sip_user;
            if (s_config.sip_pass[0]) {
                int written = snprintf(sip_uri, sizeof(sip_uri), "sip://%s:%s@%s", user, s_config.sip_pass, s_config.sip_domain);
                if (written < 0 || written >= (int)sizeof(sip_uri)) {
                    ESP_LOGE(TAG, "SIP URI too long");
                    return;
                }
            } else {
                int written = snprintf(sip_uri, sizeof(sip_uri), "sip://%s@%s", user, s_config.sip_domain);
                if (written < 0 || written >= (int)sizeof(sip_uri)) {
                    ESP_LOGE(TAG, "SIP URI too long");
                    return;
                }
            }

            esp_rtc_config_t rtc_cfg = {
                .ctx = NULL,
                .local_addr = NULL,
                .uri = sip_uri,
                .acodec_type = RTC_ACODEC_G711U,
                .vcodec_info = NULL,
                .data_cb = NULL,
                .event_handler = rtc_event_handler,
                .use_public_addr = true,
                .send_options = true,
                .keepalive = 30,
                .crt_bundle_attach = NULL,
                .register_interval = 600,
                .user_agent = "Rotary SIP",
                .fixed_local_port = 0,
            };

            s_rtc_handle = esp_rtc_service_init(&rtc_cfg);
            if (s_rtc_handle) {
                ESP_LOGI(TAG, "SIP client started for %s", user);
            } else {
                ESP_LOGE(TAG, "Failed to initialize SIP");
            }
        }
    }
}

static void wifi_start_sta(void) {
    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, s_config.wifi_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, s_config.wifi_pass, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_err_t stop_ret = esp_wifi_stop();
    if (stop_ret != ESP_ERR_WIFI_NOT_INIT && stop_ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_ERROR_CHECK(stop_ret);
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi_start_sta finished. SSID:%s", s_config.wifi_ssid);
}

static void wifi_start_softap(void) {
    wifi_config_t ap_config = { 0 };
    strlcpy((char *)ap_config.ap.ssid, "RotarySIP", sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Started softAP SSID:%s", ap_config.ap.ssid);
}

static void sip_place_call(const char *number) {
    if (!s_rtc_handle) {
        ESP_LOGW(TAG, "SIP handle not ready; cannot place call to %s", number);
        return;
    }
    if (!s_sip_registered) {
        ESP_LOGW(TAG, "SIP not registered; cannot place call to %s", number);
        return;
    }

    char target[128];
    if (s_config.sip_domain[0]) {
        snprintf(target, sizeof(target), "sip:%s@%s", number, s_config.sip_domain);
    } else {
        strlcpy(target, number, sizeof(target));
    }
    ESP_LOGI(TAG, "Dialing %s via SIP user %s@%s", target, s_config.sip_user, s_config.sip_domain);
    esp_rtc_call(s_rtc_handle, target);
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

static void restart_task(void *param) {
    (void)param;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static int hex_to_int(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    return -1;
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

    // URL decode + to space and %XX hex encoding
    size_t out_pos = 0;
    for (size_t i = 0; i < len && out_pos < out_len - 1; ++i) {
        if (start[i] == '+') {
            out[out_pos++] = ' ';
        } else if (start[i] == '%' && i + 2 < len) {
            int hi = hex_to_int(start[i + 1]);
            int lo = hex_to_int(start[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[out_pos++] = (char)((hi << 4) | lo);
                i += 2;
            } else {
                out[out_pos++] = start[i];
            }
        } else {
            out[out_pos++] = start[i];
        }
    }
    out[out_pos] = '\0';
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
    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read body");
            return ESP_FAIL;
        }
        received += ret;
    }
    parse_field(buf, "wifi_ssid", s_config.wifi_ssid, sizeof(s_config.wifi_ssid));
    parse_field(buf, "wifi_pass", s_config.wifi_pass, sizeof(s_config.wifi_pass));
    parse_field(buf, "sip_user", s_config.sip_user, sizeof(s_config.sip_user));
    parse_field(buf, "sip_pass", s_config.sip_pass, sizeof(s_config.sip_pass));
    parse_field(buf, "sip_domain", s_config.sip_domain, sizeof(s_config.sip_domain));
    parse_field(buf, "sip_extension", s_config.sip_extension, sizeof(s_config.sip_extension));
    save_config();
    if (s_rtc_handle) {
        esp_rtc_service_deinit(s_rtc_handle);
        s_rtc_handle = NULL;
        s_sip_registered = false;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Configuration saved. Rebooting to apply settings...\n");

    xTaskCreate(
        restart_task,
        "restart_task",
        2048,
        NULL,
        tskIDLE_PRIORITY,
        NULL);
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
            while (xQueuePeek(s_digit_queue, &digit, pdMS_TO_TICKS(2000)) != pdTRUE) {
                if (pos > 0 && s_hook_lifted) {
                    if (strcmp(dialed_number, "07780") == 0) {
                        ESP_LOGI(TAG, "Reset code dialed; clearing config and rebooting");
                        memset(&s_config, 0, sizeof(s_config));
                        save_config();
                        esp_restart();
                    }
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
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    load_config();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (!sta_netif || !ap_netif) {
        ESP_LOGE(TAG, "Failed to create default WIFI interfaces");
        return;
    }

    s_wifi_event_group = xEventGroupCreate();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    s_digit_queue = xQueueCreate(16, sizeof(int));
    rotary_init();
    start_webserver();
    if (s_config.wifi_ssid[0] == '\0') {
        wifi_start_softap();
    } else {
        wifi_start_sta();
    }
    xTaskCreate(&dial_task, "dial_task", 4096, NULL, 5, NULL);
}

void app_main(void) {
    ESP_LOGI(TAG, "Booting Rotary SIP Phone");
    app_start();
}
