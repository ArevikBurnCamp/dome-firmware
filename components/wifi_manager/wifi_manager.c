#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "sdkconfig.h"
#include "esp_heap_caps.h"

#include "storage.h"
#include "wifi_manager.h"
#include "app_config.h"

static const char *TAG = "WIFI_MANAGER";

// Event group to signal when we are connected
static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;
const int WIFI_FAIL_BIT = BIT1;

#define WIFI_MANAGER_MAX_RETRY  5

static int s_retry_num = 0;
static esp_netif_t *sta_netif_instance = NULL;

// Forward declarations
static void start_ap_mode(void);
static void start_sta_mode(const char* ssid, const char* password);
static esp_err_t http_get_handler(httpd_req_t *req);
static esp_err_t http_post_handler(httpd_req_t *req);
static httpd_handle_t start_webserver(void);

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MANAGER_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t get_uri = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = http_get_handler,
        };
        httpd_register_uri_handler(server, &get_uri);

        httpd_uri_t post_uri = {
            .uri      = "/save",
            .method   = HTTP_POST,
            .handler  = http_post_handler,
        };
        httpd_register_uri_handler(server, &post_uri);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

static esp_err_t http_get_handler(httpd_req_t *req)
{
    const char* resp_str = (const char*)
        "<!DOCTYPE html><html><head><title>WiFi Setup</title>"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "</head><body><h1>WiFi Setup</h1>"
        "<form action='/save' method='post'>"
        "SSID: <input type='text' name='ssid'><br>"
        "Password: <input type='password' name='password'><br>"
        "<input type='submit' value='Save'>"
        "</form></body></html>";
    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t http_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret, remaining = req->content_len;

    if (remaining > sizeof(buf) - 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char ssid[32] = {0};
    char password[64] = {0};

    if (httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid)) != ESP_OK ||
        httpd_query_key_value(buf, "password", password, sizeof(password)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form data");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saving SSID: %s", ssid);

    app_config_t temp_config;
    storage_load_config(&temp_config); // Load current config to not lose other settings
    strncpy(temp_config.wifi_ssid, ssid, sizeof(temp_config.wifi_ssid) - 1);
    strncpy(temp_config.wifi_password, password, sizeof(temp_config.wifi_password) - 1);

    storage_save_config(&temp_config); // Save the whole structure back

    httpd_resp_send(req, "Credentials saved. Rebooting...", HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;
}

void start_ap_mode(void)
{
    ESP_LOGI(TAG, "Starting AP Mode");
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_ap());

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "DOME-SETUP",
            .ssid_len = strlen("DOME-SETUP"),
            .channel = 1,
            .password = "password",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s", "DOME-SETUP", "password");
    
    start_webserver();
}

void start_sta_mode(const char* ssid, const char* password)
{
    ESP_LOGI(TAG, "Starting STA Mode");
    
    sta_netif_instance = esp_netif_create_default_wifi_sta();

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(20000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", ssid);
    } else {
        ESP_LOGW(TAG, "Connection failed or timed out. Starting AP mode.");
        ESP_ERROR_CHECK(esp_wifi_stop());
        if (sta_netif_instance) {
            esp_netif_destroy_default_wifi(sta_netif_instance);
            sta_netif_instance = NULL;
        }
        start_ap_mode();
    }
}

void wifi_manager_start(const app_config_t* config)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    if (strlen(config->wifi_ssid) > 0) {
        ESP_LOGI(TAG, "Credentials found, trying to connect...");
        start_sta_mode(config->wifi_ssid, config->wifi_password);
    } else {
        ESP_LOGI(TAG, "No credentials found, starting AP mode.");
        start_ap_mode();
    }
}

EventGroupHandle_t wifi_manager_get_event_group(void)
{
    return wifi_event_group;
}
