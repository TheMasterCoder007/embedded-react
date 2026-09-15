/*
 * Copyright 2026 Cory Lamming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * WiFi station, saved settings and SNTP (see network.h).
 */

#include "network.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char* TAG = "network";

#define NVS_NAMESPACE "er_net"
#define DEFAULT_TIME_ZONE "UTC0"

/* Scan records the driver hands over; more than NETWORK_SCAN_MAX, since several can share a name. */
#define SCAN_RECORDS 32

/* Reconnect backoff. With the network gone, every attempt is a full channel scan, so retry with a growing
   gap rather than scanning back to back. */
#define RETRY_MIN_MS 2000U
#define RETRY_MAX_MS 60000U

/* Logs a failed setup call and gives up: the UI keeps running without WiFi. */
#define CHECK(call)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        const esp_err_t check_err_ = (call);                                                                           \
        if (check_err_ != ESP_OK)                                                                                      \
        {                                                                                                              \
            ESP_LOGE(TAG, "%s failed: %s", #call, esp_err_to_name(check_err_));                                        \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)

/* Written by the WiFi/IP events, read by the frame loop. Short copies, so a spinlock. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static NetworkStatus s_status;
static NetworkAp* s_aps; /* the last scan, in PSRAM */
static int s_ap_count = -1;

static wifi_ap_record_t* s_records; /* event task only, in PSRAM */
static NetworkAp* s_scan_buf;       /* event task only, in PSRAM */

static atomic_bool s_scanning;
static atomic_bool s_resume_after_scan; /* a connect attempt paused for a scan */
static atomic_bool s_synced;            /* SNTP → network_poll() */
static atomic_bool s_got_ip;            /* IP_EVENT_STA_GOT_IP → network_poll() */

static esp_timer_handle_t s_retry_timer;
static uint32_t s_retry_ms = RETRY_MIN_MS; /* event and timer tasks */
static bool s_started;

/* Frame-loop task only. The pending network is saved once it connects. */
static bool s_time_known;
static char s_pending_ssid[33];
static char s_pending_pass[65];
static char s_tz[64] = DEFAULT_TIME_ZONE;

/** @brief Updates the station's state. */
static void set_state(NetworkState state, NetworkFail fail)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = state;
    s_status.fail = fail;
    portEXIT_CRITICAL(&s_lock);
}

/** @brief Whether a network is chosen (the driver has one to join). */
static bool has_network(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool yes = s_status.ssid[0] != '\0';
    portEXIT_CRITICAL(&s_lock);
    return yes;
}

/** @brief Maps a disconnect reason to what the app can tell the user. */
static NetworkFail classify(uint8_t reason)
{
    switch (reason)
    {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
            return NETWORK_FAIL_AUTH;
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return NETWORK_FAIL_NOT_FOUND;
        default:
            return NETWORK_FAIL_OTHER;
    }
}

/** @brief Joins the chosen network now, or once a running scan finishes. */
static void join(void)
{
    if (atomic_load(&s_scanning))
    {
        atomic_store(&s_resume_after_scan, true);
        return;
    }
    esp_wifi_connect();
}

/** @brief Retry-timer callback. */
static void retry_join(void* arg)
{
    (void)arg;
    if (has_network())
    {
        join();
    }
}

/** @brief Copies the driver's scan records into s_aps: named networks only, one per name, strongest first. */
static void take_scan_results(void)
{
    uint16_t n = SCAN_RECORDS;
    if (esp_wifi_scan_get_ap_records(&n, s_records) != ESP_OK)
    {
        n = 0;
    }
    int count = 0;
    for (int i = 0; i < (int)n; i++)
    {
        const wifi_ap_record_t* r = &s_records[i];
        if (r->ssid[0] == '\0')
        {
            continue;
        }
        int at = -1;
        for (int j = 0; j < count; j++)
        {
            if (strcmp(s_scan_buf[j].ssid, (const char*)r->ssid) == 0)
            {
                at = j;
                break;
            }
        }
        if (at < 0)
        {
            if (count == NETWORK_SCAN_MAX)
            {
                continue; /* the driver sorts strongest first, so what is left is weaker */
            }
            at = count++;
            memcpy(s_scan_buf[at].ssid, r->ssid, sizeof(s_scan_buf[at].ssid));
            s_scan_buf[at].rssi = INT8_MIN;
        }
        if (r->rssi > s_scan_buf[at].rssi)
        {
            s_scan_buf[at].rssi = r->rssi;
            s_scan_buf[at].secure = r->authmode != WIFI_AUTH_OPEN && r->authmode != WIFI_AUTH_OWE;
        }
    }
    /* Insertion sort, strongest first: a merged name can have moved up. */
    for (int i = 1; i < count; i++)
    {
        const NetworkAp ap = s_scan_buf[i];
        int j = i - 1;
        while (j >= 0 && s_scan_buf[j].rssi < ap.rssi)
        {
            s_scan_buf[j + 1] = s_scan_buf[j];
            j--;
        }
        s_scan_buf[j + 1] = ap;
    }
    portENTER_CRITICAL(&s_lock);
    memcpy(s_aps, s_scan_buf, (size_t)count * sizeof(NetworkAp));
    s_ap_count = count;
    portEXIT_CRITICAL(&s_lock);
}

/** @brief WiFi + IP events, on the default event loop's task. */
static void on_net_event(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        if (has_network())
        {
            join();
        }
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        const wifi_event_sta_disconnected_t* ev = (const wifi_event_sta_disconnected_t*)data;
        if (ev->reason == WIFI_REASON_ASSOC_LEAVE || !has_network())
        {
            return; /* we left on purpose: for another network, a scan, or forget */
        }
        ESP_LOGW(TAG, "not connected (reason %d), retrying in %u s", (int)ev->reason, (unsigned)(s_retry_ms / 1000U));
        set_state(NETWORK_FAILED, classify(ev->reason));
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, (uint64_t)s_retry_ms * 1000U);
        s_retry_ms = (s_retry_ms * 2U > RETRY_MAX_MS) ? RETRY_MAX_MS : s_retry_ms * 2U;
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE)
    {
        take_scan_results();
        atomic_store(&s_scanning, false);
        if (atomic_exchange(&s_resume_after_scan, false) && has_network())
        {
            esp_wifi_connect();
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t* ev = (const ip_event_got_ip_t*)data;
        ESP_LOGI(TAG, "connected, ip " IPSTR, IP2STR(&ev->ip_info.ip));
        set_state(NETWORK_CONNECTED, NETWORK_FAIL_NONE);
        s_retry_ms = RETRY_MIN_MS;
        atomic_store(&s_got_ip, true);
        esp_netif_sntp_start(); /* (re)starts SNTP, so a reconnect also resyncs */
    }
}

/** @brief SNTP sync notification: the system clock has just been set. */
static void on_time_sync(struct timeval* tv)
{
    (void)tv;
    atomic_store(&s_synced, true);
}

/** @brief Reads a string key, leaving @p out alone when it is missing or does not fit. */
static void read_key(nvs_handle_t h, const char* key, char* out, size_t cap)
{
    size_t len = cap;
    char tmp[65];
    if (cap <= sizeof(tmp) && nvs_get_str(h, key, tmp, &len) == ESP_OK)
    {
        memcpy(out, tmp, len);
    }
}

/** @brief Writes string keys and commits; @p value NULL erases @p key. */
static void write_keys(const char* const* keys, const char* const* values, int n)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    for (int i = 0; err == ESP_OK && i < n; i++)
    {
        err = values[i] ? nvs_set_str(h, keys[i], values[i]) : nvs_erase_key(h, keys[i]);
        if (err == ESP_ERR_NVS_NOT_FOUND)
        {
            err = ESP_OK; /* erasing a key that was never set */
        }
    }
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    if (err != ESP_ERR_NVS_NOT_FOUND || n > 0)
    {
        nvs_close(h);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "saving settings failed: %s", esp_err_to_name(err));
    }
}

/** @brief Applies s_tz. */
static void apply_time_zone(void)
{
    setenv("TZ", s_tz, 1);
    tzset();
}

/** @brief Builds a station config; a full-length SSID or PSK has no terminator. */
static void make_config(wifi_config_t* cfg, const char* ssid, const char* password)
{
    memset(cfg, 0, sizeof(*cfg));
    const size_t sl = strlen(ssid);
    const size_t pl = strlen(password);
    memcpy(cfg->sta.ssid, ssid, (sl < sizeof(cfg->sta.ssid)) ? sl : sizeof(cfg->sta.ssid));
    memcpy(cfg->sta.password, password, (pl < sizeof(cfg->sta.password)) ? pl : sizeof(cfg->sta.password));
    cfg->sta.threshold.authmode = (pl > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg->sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
}

bool network_start(void)
{
    /* NVS holds what the app saves: the network and the time zone. WiFi keeps nothing of its own there. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    CHECK(err);
    char pass[65] = "";
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK)
    {
        read_key(h, "tz", s_tz, sizeof(s_tz));
        read_key(h, "ssid", s_status.ssid, sizeof(s_status.ssid)); /* no other task yet: no lock */
        read_key(h, "pass", pass, sizeof(pass));
        nvs_close(h);
    }
    apply_time_zone();

    s_records = heap_caps_malloc(SCAN_RECORDS * sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM);
    s_scan_buf = heap_caps_calloc(NETWORK_SCAN_MAX, sizeof(NetworkAp), MALLOC_CAP_SPIRAM);
    s_aps = heap_caps_calloc(NETWORK_SCAN_MAX, sizeof(NetworkAp), MALLOC_CAP_SPIRAM);
    if (!s_records || !s_scan_buf || !s_aps)
    {
        ESP_LOGE(TAG, "no PSRAM for scan results");
        return false;
    }

    CHECK(esp_netif_init());
    CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    CHECK(esp_wifi_init(&init_cfg));

    const esp_timer_create_args_t retry_args = {.callback = retry_join, .name = "wifi_retry"};
    CHECK(esp_timer_create(&retry_args, &s_retry_timer));
    CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_net_event, NULL, NULL));
    CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_net_event, NULL, NULL));
    CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (s_status.ssid[0] != '\0')
    {
        wifi_config_t cfg;
        make_config(&cfg, s_status.ssid, pass);
        CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
        memset(&cfg, 0, sizeof(cfg));
        s_status.state = NETWORK_CONNECTING;
    }
    memset(pass, 0, sizeof(pass));

    /* Started on each IP_EVENT_STA_GOT_IP. Nothing waits on it: the frame loop polls. */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_ER_SNTP_SERVER);
    sntp_cfg.start = false;
    sntp_cfg.wait_for_sync = false;
    sntp_cfg.sync_cb = on_time_sync;
    CHECK(esp_netif_sntp_init(&sntp_cfg));

    CHECK(esp_wifi_start());
    s_started = true;
    if (s_status.ssid[0] != '\0')
    {
        ESP_LOGI(TAG, "joining \"%s\"", s_status.ssid);
    }
    return true;
}

bool network_scan_start(void)
{
    if (!s_started)
    {
        return false;
    }
    if (atomic_load(&s_scanning))
    {
        return true;
    }
    NetworkStatus st;
    network_status(&st);
    if (st.state == NETWORK_CONNECTING || st.state == NETWORK_FAILED)
    {
        /* A connect attempt holds the radio: pause it until the scan is done. */
        esp_timer_stop(s_retry_timer);
        atomic_store(&s_resume_after_scan, true);
        esp_wifi_disconnect();
    }
    atomic_store(&s_scanning, true);
    const wifi_scan_config_t cfg = {.show_hidden = false};
    const esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK)
    {
        atomic_store(&s_scanning, false);
        ESP_LOGW(TAG, "scan not started: %s", esp_err_to_name(err));
        if (atomic_exchange(&s_resume_after_scan, false) && has_network())
        {
            esp_wifi_connect();
        }
        return false;
    }
    return true;
}

int network_scan_results(NetworkAp* out, int max)
{
    if (atomic_load(&s_scanning))
    {
        return -1;
    }
    portENTER_CRITICAL(&s_lock);
    const int n = (s_ap_count < max) ? s_ap_count : max;
    if (n > 0)
    {
        memcpy(out, s_aps, (size_t)n * sizeof(NetworkAp));
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}

bool network_connect(const char* ssid, const char* password)
{
    const size_t sl = strlen(ssid);
    const size_t pl = strlen(password);
    if (!s_started || sl == 0 || sl > 32 || pl > 64 || (pl > 0 && pl < 8))
    {
        return false;
    }
    wifi_config_t cfg;
    make_config(&cfg, ssid, password);
    esp_timer_stop(s_retry_timer);
    s_retry_ms = RETRY_MIN_MS;
    esp_wifi_disconnect(); /* leave the current network; reported as ASSOC_LEAVE, which is ignored */
    const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    memset(&cfg, 0, sizeof(cfg));
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return false;
    }
    memcpy(s_pending_ssid, ssid, sl + 1);
    memcpy(s_pending_pass, password, pl + 1);
    portENTER_CRITICAL(&s_lock);
    memcpy(s_status.ssid, ssid, sl + 1);
    s_status.state = NETWORK_CONNECTING;
    s_status.fail = NETWORK_FAIL_NONE;
    portEXIT_CRITICAL(&s_lock);
    atomic_store(&s_got_ip, false);
    ESP_LOGI(TAG, "joining \"%s\"", ssid);
    join();
    return true;
}

void network_forget(void)
{
    if (!s_started)
    {
        return;
    }
    esp_timer_stop(s_retry_timer);
    atomic_store(&s_resume_after_scan, false);
    memset(s_pending_ssid, 0, sizeof(s_pending_ssid));
    memset(s_pending_pass, 0, sizeof(s_pending_pass));
    portENTER_CRITICAL(&s_lock);
    memset(s_status.ssid, 0, sizeof(s_status.ssid));
    s_status.state = NETWORK_IDLE;
    s_status.fail = NETWORK_FAIL_NONE;
    portEXIT_CRITICAL(&s_lock);
    esp_wifi_disconnect();
    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    static const char* const keys[] = {"ssid", "pass"};
    static const char* const none[] = {NULL, NULL};
    write_keys(keys, none, 2);
    ESP_LOGI(TAG, "network forgotten");
}

void network_status(NetworkStatus* out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
}

bool network_poll(int64_t* epoch_ms)
{
    if (atomic_exchange(&s_got_ip, false) && s_pending_ssid[0] != '\0')
    {
        static const char* const keys[] = {"ssid", "pass"};
        const char* const values[] = {s_pending_ssid, s_pending_pass};
        write_keys(keys, values, 2);
        ESP_LOGI(TAG, "saved \"%s\"", s_pending_ssid);
        memset(s_pending_ssid, 0, sizeof(s_pending_ssid));
        memset(s_pending_pass, 0, sizeof(s_pending_pass));
    }
    if (!atomic_exchange(&s_synced, false))
    {
        return false;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *epoch_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    s_time_known = true;

    struct tm local;
    char stamp[40];
    localtime_r(&tv.tv_sec, &local);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S %Z", &local);
    ESP_LOGI(TAG, "time synced: %s", stamp);
    return true;
}

bool network_time_known(void)
{
    return s_time_known;
}

int32_t network_utc_offset_s(void)
{
    const time_t now = time(NULL);
    struct tm local;
    struct tm utc;
    localtime_r(&now, &local);
    gmtime_r(&now, &utc);
    /* The two are under a day apart, so a different year means one day either way. */
    int days = local.tm_yday - utc.tm_yday;
    if (local.tm_year != utc.tm_year)
    {
        days = (local.tm_year > utc.tm_year) ? 1 : -1;
    }
    return ((days * 24 + (local.tm_hour - utc.tm_hour)) * 60 + (local.tm_min - utc.tm_min)) * 60
           + (local.tm_sec - utc.tm_sec);
}

void network_time_zone(char* out, size_t cap)
{
    strlcpy(out, s_tz, cap);
}

bool network_set_time_zone(const char* tz)
{
    const size_t n = strlen(tz);
    if (n == 0 || n >= sizeof(s_tz))
    {
        return false;
    }
    memcpy(s_tz, tz, n + 1);
    apply_time_zone();
    static const char* const keys[] = {"tz"};
    const char* const values[] = {s_tz};
    write_keys(keys, values, 1);
    ESP_LOGI(TAG, "time zone %s", s_tz);
    return true;
}
