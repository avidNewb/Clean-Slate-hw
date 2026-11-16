#include "wifi_manager.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "sdkconfig.h"

#include "settings_store.h"

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT BIT0
#define STA_FAILS_BEFORE_FALLBACK_AP 5

static EventGroupHandle_t s_wifi_events = NULL;
static esp_timer_handle_t s_reconnect_timer = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static SemaphoreHandle_t s_scan_mutex = NULL;

static uint32_t s_backoff_sec = 1;
static uint32_t s_fail_streak = 0;
static char s_ip_str[16] = "0.0.0.0";
static char s_device_id[32] = {0};
static char s_ap_ssid[33] = {0};
static char s_sta_ssid[33] = {0};

static volatile bool s_ap_active = false;
static volatile bool s_ap_pinned = false; /* True when explicitly in initial/manual setup mode */
static volatile bool s_sta_has_creds = false;
static volatile bool s_dns_running = false;
static TaskHandle_t s_dns_task = NULL;

static void dns_captive_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGW(TAG, "DNS socket create failed: errno=%d", errno);
        s_dns_running = false;
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGW(TAG, "DNS bind(53) failed: errno=%d", errno);
        close(sock);
        s_dns_running = false;
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    esp_netif_ip_info_t ip_info = {0};
    uint32_t ap_ip_be = htonl(0xC0A80401); /* 192.168.4.1 fallback */
    if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        ap_ip_be = ip_info.ip.addr;
    }

    uint8_t rx[512];
    uint8_t tx[512];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    ESP_LOGI(TAG, "Captive portal DNS responder started (192.168.4.1)");

    while (s_dns_running) {
        client_len = sizeof(client_addr);
        int len = recvfrom(sock, rx, sizeof(rx), 0,
                           (struct sockaddr *)&client_addr, &client_len);
        if (len < 12) {
            continue;
        }
        /* Find end of QNAME + QTYPE + QCLASS in first question */
        int pos = 12;
        while (pos < len && rx[pos] != 0) {
            uint8_t lbl = rx[pos];
            if ((lbl & 0xC0U) == 0xC0U) {
                pos += 2;
                break;
            }
            pos += 1 + lbl;
        }
        if (pos < len && rx[pos] == 0) {
            pos++;
        }
        pos += 4; /* QTYPE (2) + QCLASS (2) */
        if (pos > len || pos + 16 > (int)sizeof(tx)) {
            continue;
        }

        memcpy(tx, rx, (size_t)pos);
        tx[2] = 0x81; /* QR=1, standard response, RD copied roughly */
        tx[3] = 0x80; /* RA=1, RCODE=0 */
        tx[4] = rx[4];
        tx[5] = rx[5];
        tx[6] = 0x00;
        tx[7] = 0x01; /* ANCOUNT = 1 */
        tx[8] = 0x00;
        tx[9] = 0x00;
        tx[10] = 0x00;
        tx[11] = 0x00;

        uint8_t *ans = tx + pos;
        ans[0] = 0xC0; ans[1] = 0x0C; /* Pointer to QNAME at offset 12 */
        ans[2] = 0x00; ans[3] = 0x01; /* TYPE A */
        ans[4] = 0x00; ans[5] = 0x01; /* CLASS IN */
        ans[6] = 0x00; ans[7] = 0x00; ans[8] = 0x00; ans[9] = 60; /* TTL 60s */
        ans[10] = 0x00; ans[11] = 0x04; /* RDLENGTH 4 */
        memcpy(ans + 12, &ap_ip_be, 4);

        sendto(sock, tx, (size_t)(pos + 16), 0,
               (struct sockaddr *)&client_addr, client_len);
    }

    close(sock);
    s_dns_task = NULL;
    ESP_LOGI(TAG, "Captive portal DNS responder stopped");
    vTaskDelete(NULL);
}

static void start_dns_server(void)
{
    if (s_dns_running) {
        return;
    }
    s_dns_running = true;
    if (xTaskCreate(dns_captive_task, "dns_captive", 3072, NULL, 3, &s_dns_task) != pdPASS) {
        s_dns_running = false;
        s_dns_task = NULL;
    }
}

static void stop_dns_server(void)
{
    s_dns_running = false;
}

static void copy_wifi_field(uint8_t *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0 || !src) {
        return;
    }
    size_t n = strnlen(src, dst_size);
    memcpy(dst, src, n);
}

static esp_err_t enable_softap(bool pinned_setup_mode)
{
    s_ap_pinned = pinned_setup_mode;
    if (s_ap_active) {
        return ESP_OK;
    }

    wifi_config_t ap_cfg = {0};
    copy_wifi_field(ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), s_ap_ssid);
    ap_cfg.ap.ssid_len = (uint8_t)strnlen(s_ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set APSTA failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "set AP cfg failed");
    s_ap_active = true;
    start_dns_server();
    ESP_LOGI(TAG, "SoftAP active: SSID=\"%s\" (http://192.168.4.1/)", s_ap_ssid);
    return ESP_OK;
}

static void disable_softap_if_unpinned(void)
{
    if (!s_ap_active || s_ap_pinned) {
        return;
    }
    stop_dns_server();
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_active = false;
    ESP_LOGI(TAG, "Fallback SoftAP disabled after STA connection");
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (!s_sta_has_creds) {
        return;
    }
    ESP_LOGI(TAG, "Attempting Wi-Fi reconnect...");
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_sta_has_creds) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        snprintf(s_ip_str, sizeof(s_ip_str), "0.0.0.0");

        if (!s_sta_has_creds) {
            return;
        }

        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        s_fail_streak++;
        ESP_LOGW(TAG, "Disconnected (reason=%d, streak=%" PRIu32 "), retrying in %" PRIu32 "s",
                 disc ? disc->reason : -1, s_fail_streak, s_backoff_sec);

        if (s_fail_streak >= STA_FAILS_BEFORE_FALLBACK_AP && !s_ap_active) {
            ESP_LOGW(TAG, "Opening fallback Setup SoftAP (%s) after repeated failures", s_ap_ssid);
            enable_softap(false);
        }

        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, (uint64_t)s_backoff_sec * 1000000ULL);
        s_backoff_sec *= 2;
        if (s_backoff_sec > CONFIG_CS_WIFI_MAX_BACKOFF_SEC) {
            s_backoff_sec = CONFIG_CS_WIFI_MAX_BACKOFF_SEC;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Connected, IP: %s (hostname: %s)", s_ip_str, s_device_id);
        s_backoff_sec = 1;
        s_fail_streak = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        disable_softap_if_unpinned();
    }
}

esp_err_t wifi_manager_init(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_wifi_events != NULL, ESP_ERR_NO_MEM, TAG, "event group alloc failed");
    s_scan_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_scan_mutex != NULL, ESP_ERR_NO_MEM, TAG, "scan mutex alloc failed");

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconn",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_reconnect_timer),
                        TAG, "reconnect timer create failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop create failed");
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (strlen(CONFIG_CS_API_DEVICE_ID) > 0) {
        snprintf(s_device_id, sizeof(s_device_id), "%s", CONFIG_CS_API_DEVICE_ID);
    } else {
        snprintf(s_device_id, sizeof(s_device_id), "clean-slate-%02x%02x%02x",
                 mac[3], mac[4], mac[5]);
    }
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "CleanSlate-%02X%02X", mac[4], mac[5]);

    if (s_sta_netif) {
        esp_netif_set_hostname(s_sta_netif, s_device_id);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL),
        TAG, "register WIFI_EVENT failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL),
        TAG, "register IP_EVENT failed");

    cs_local_settings_t local;
    settings_store_get(&local);
    snprintf(s_sta_ssid, sizeof(s_sta_ssid), "%s", local.wifi_ssid);
    s_sta_has_creds = (local.wifi_ssid[0] != '\0');

    wifi_config_t sta_cfg = {0};
    copy_wifi_field(sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), local.wifi_ssid);
    copy_wifi_field(sta_cfg.sta.password, sizeof(sta_cfg.sta.password), local.wifi_pass);
    sta_cfg.sta.threshold.authmode = (local.wifi_pass[0] == '\0')
        ? WIFI_AUTH_OPEN
        : WIFI_AUTH_WPA_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(s_sta_has_creds ? WIFI_MODE_STA : WIFI_MODE_APSTA),
                        TAG, "set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG, "set STA config failed");

    if (!s_sta_has_creds) {
        wifi_config_t ap_cfg = {0};
        copy_wifi_field(ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), s_ap_ssid);
        ap_cfg.ap.ssid_len = (uint8_t)strnlen(s_ap_ssid, sizeof(ap_cfg.ap.ssid));
        ap_cfg.ap.channel = 1;
        ap_cfg.ap.max_connection = 4;
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "set AP config failed");
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "set_ps failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(s_device_id);
        mdns_instance_name_set(local.device_name[0] ? local.device_name : "Clean Slate");
        mdns_txt_item_t txt[1] = { { "id", s_device_id } };
        mdns_service_add(NULL, "_http", "_tcp", 80, txt, 1);
        ESP_LOGI(TAG, "mDNS responder active: http://%s.local/", s_device_id);
    } else {
        ESP_LOGW(TAG, "mDNS init failed (continuing with IP access only)");
    }

    if (!s_sta_has_creds) {
        s_ap_active = true;
        s_ap_pinned = true;
        start_dns_server();
        ESP_LOGI(TAG, "No Wi-Fi credentials configured; Captive Portal AP started: %s", s_ap_ssid);
    } else {
        ESP_LOGI(TAG, "Wi-Fi STA started (SSID=\"%s\", device_id=\"%s\")",
                 s_sta_ssid, s_device_id);
    }
    return ESP_OK;
}

esp_err_t wifi_manager_enter_setup_mode(void)
{
    esp_timer_stop(s_reconnect_timer);
    s_sta_has_creds = false;
    s_fail_streak = 0;
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
    esp_wifi_disconnect();
    return enable_softap(true);
}

esp_err_t wifi_manager_apply_sta_credentials(const char *ssid, const char *password, bool exit_ap)
{
    ESP_RETURN_ON_FALSE(ssid != NULL && ssid[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "empty SSID");

    esp_timer_stop(s_reconnect_timer);
    s_fail_streak = 0;
    s_backoff_sec = 1;
    snprintf(s_sta_ssid, sizeof(s_sta_ssid), "%s", ssid);
    s_sta_has_creds = true;

    wifi_config_t sta_cfg = {0};
    copy_wifi_field(sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), ssid);
    if (password) {
        copy_wifi_field(sta_cfg.sta.password, sizeof(sta_cfg.sta.password), password);
    }
    sta_cfg.sta.threshold.authmode = (!password || password[0] == '\0')
        ? WIFI_AUTH_OPEN
        : WIFI_AUTH_WPA_PSK;

    esp_wifi_disconnect();
    if (exit_ap) {
        s_ap_pinned = false;
        stop_dns_server();
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA mode failed");
        s_ap_active = false;
    } else {
        s_ap_pinned = false;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG, "set STA cfg failed");
    return esp_wifi_connect();
}

esp_err_t wifi_manager_scan_networks(wifi_scan_entry_t *out_entries,
                                     size_t max_entries,
                                     size_t *out_count)
{
    ESP_RETURN_ON_FALSE(out_entries != NULL && out_count != NULL && max_entries > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid scan args");
    *out_count = 0;
    if (!s_scan_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 80,
        .scan_time.active.max = 180,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        xSemaphoreGive(s_scan_mutex);
        return err;
    }

    uint16_t ap_num = 0;
    err = esp_wifi_scan_get_ap_num(&ap_num);
    if (err != ESP_OK || ap_num == 0) {
        esp_wifi_clear_ap_list();
        xSemaphoreGive(s_scan_mutex);
        return err;
    }

    uint16_t fetch_num = (ap_num > 24U) ? 24U : ap_num;
    wifi_ap_record_t *records = calloc(fetch_num, sizeof(wifi_ap_record_t));
    if (!records) {
        esp_wifi_clear_ap_list();
        xSemaphoreGive(s_scan_mutex);
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_scan_get_ap_records(&fetch_num, records);
    xSemaphoreGive(s_scan_mutex);
    if (err != ESP_OK) {
        free(records);
        return err;
    }

    size_t count = 0;
    for (uint16_t i = 0; i < fetch_num && count < max_entries; i++) {
        const char *ssid = (const char *)records[i].ssid;
        if (!ssid[0]) {
            continue;
        }
        bool dup = false;
        for (size_t j = 0; j < count; j++) {
            if (strcmp(out_entries[j].ssid, ssid) == 0) {
                if (records[i].rssi > out_entries[j].rssi) {
                    out_entries[j].rssi = records[i].rssi;
                }
                dup = true;
                break;
            }
        }
        if (!dup) {
            snprintf(out_entries[count].ssid, sizeof(out_entries[count].ssid), "%s", ssid);
            out_entries[count].rssi = records[i].rssi;
            out_entries[count].open = (records[i].authmode == WIFI_AUTH_OPEN);
            count++;
        }
    }

    free(records);
    *out_count = count;
    return ESP_OK;
}

bool wifi_manager_wait_connected(uint32_t timeout_ms)
{
    if (!s_wifi_events) {
        return false;
    }
    TickType_t ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, ticks);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_manager_is_connected(void)
{
    if (!s_wifi_events) {
        return false;
    }
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_manager_is_ap_mode(void)
{
    return s_ap_active;
}

void wifi_manager_get_ip_str(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return;
    }
    snprintf(buf, buf_len, "%s", s_ip_str);
}

void wifi_manager_get_dns_str(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return;
    }
    buf[0] = '\0';
    if (!s_sta_netif || !wifi_manager_is_connected()) {
        return;
    }
    esp_netif_dns_info_t dns = {0};
    if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
        dns.ip.type == ESP_IPADDR_TYPE_V4 &&
        dns.ip.u_addr.ip4.addr != 0) {
        snprintf(buf, buf_len, IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    }
}

const char *wifi_manager_get_device_id(void)
{
    return s_device_id;
}

const char *wifi_manager_get_ap_ssid(void)
{
    return s_ap_ssid;
}

void wifi_manager_get_sta_ssid(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return;
    }
    snprintf(buf, buf_len, "%s", s_sta_ssid);
}

int wifi_manager_get_rssi(void)
{
    if (!wifi_manager_is_connected()) {
        return 0;
    }
    wifi_ap_record_t info = {0};
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        return info.rssi;
    }
    return 0;
}
