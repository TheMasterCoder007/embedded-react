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

#include <ctype.h>
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

#define MS_PER_SECOND 1000
#define US_PER_MS 1000
#define SECONDS_PER_MINUTE 60
#define MINUTES_PER_HOUR 60
#define HOURS_PER_DAY 24

/* Bytes for the time-sync log line's timestamp. */
#define TIME_STAMP_BYTES 40

#define ARRAY_COUNT(array) ((int)(sizeof(array) / sizeof((array)[0])))

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
static NetworkAp* s_scan_results; /* the last scan, in PSRAM */
static int s_ap_count = -1;

static wifi_ap_record_t* s_records; /* event task only, in PSRAM */
static NetworkAp* s_scan_buffer;    /* event task only, in PSRAM */

static atomic_bool s_scanning;
static atomic_bool s_resume_after_scan; /* a connect attempt paused for a scan */
static atomic_bool s_time_synced;       /* SNTP → network_poll() */
static atomic_bool s_got_ip;            /* IP_EVENT_STA_GOT_IP → network_poll() */

static esp_timer_handle_t s_retry_timer;
static uint32_t s_retry_ms = RETRY_MIN_MS; /* under s_lock */
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static bool s_started;

/* Frame-loop task only. The pending network is saved once it connects. */
static bool s_time_known;
static char s_pending_ssid[NETWORK_SSID_BYTES];
static char s_pending_password[NETWORK_PASSWORD_BYTES];
static char s_time_zone[NETWORK_TIME_ZONE_BYTES] = DEFAULT_TIME_ZONE;

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

/** @brief Returns the delay before the next retry and doubles it for the one after, up to RETRY_MAX_MS. */
static uint32_t next_retry_ms(void)
{
    portENTER_CRITICAL(&s_lock);
    const uint32_t delay_ms = s_retry_ms;
    s_retry_ms = (delay_ms * 2U > RETRY_MAX_MS) ? RETRY_MAX_MS : delay_ms * 2U;
    portEXIT_CRITICAL(&s_lock);
    return delay_ms;
}

/** @brief Sets the retry delay back to RETRY_MIN_MS. */
static void reset_retry(void)
{
    portENTER_CRITICAL(&s_lock);
    s_retry_ms = RETRY_MIN_MS;
    portEXIT_CRITICAL(&s_lock);
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

/** @brief Whether this station can join a network secured this way: open, or a password on WPA2 or WPA3. */
static bool joinable(wifi_auth_mode_t mode)
{
    switch (mode)
    {
        case WIFI_AUTH_OPEN:
        case WIFI_AUTH_OWE:
        case WIFI_AUTH_WPA2_PSK:
        case WIFI_AUTH_WPA_WPA2_PSK:
        case WIFI_AUTH_WPA3_PSK:
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return true;
        default:
            return false; /* WEP and WPA1 are below make_config's WPA2 floor, and there is no EAP */
    }
}

/** @brief Copies the driver's scan records into s_scan_results: joinable named networks, one per name, strongest
 *  first. */
static void take_scan_results(void)
{
    uint16_t record_count = SCAN_RECORDS;
    if (esp_wifi_scan_get_ap_records(&record_count, s_records) != ESP_OK)
    {
        record_count = 0;
    }
    int count = 0;
    for (int index = 0; index < (int)record_count; index++)
    {
        const wifi_ap_record_t* record = &s_records[index];
        if (record->ssid[0] == '\0' || !joinable(record->authmode))
        {
            continue;
        }
        int slot = -1;
        for (int position = 0; position < count; position++)
        {
            if (strcmp(s_scan_buffer[position].ssid, (const char*)record->ssid) == 0)
            {
                slot = position;
                break;
            }
        }
        if (slot < 0)
        {
            if (count == NETWORK_SCAN_MAX)
            {
                continue; /* the driver sorts strongest first, so what is left is weaker */
            }
            slot = count++;
            memcpy(s_scan_buffer[slot].ssid, record->ssid, sizeof(s_scan_buffer[slot].ssid));
            s_scan_buffer[slot].rssi = INT8_MIN;
        }
        if (record->rssi > s_scan_buffer[slot].rssi)
        {
            s_scan_buffer[slot].rssi = record->rssi;
            s_scan_buffer[slot].secure = record->authmode != WIFI_AUTH_OPEN && record->authmode != WIFI_AUTH_OWE;
        }
    }
    /* Insertion sort, strongest first: a merged name can have moved up. */
    for (int index = 1; index < count; index++)
    {
        const NetworkAp network = s_scan_buffer[index];
        int position = index - 1;
        while (position >= 0 && s_scan_buffer[position].rssi < network.rssi)
        {
            s_scan_buffer[position + 1] = s_scan_buffer[position];
            position--;
        }
        s_scan_buffer[position + 1] = network;
    }
    portENTER_CRITICAL(&s_lock);
    memcpy(s_scan_results, s_scan_buffer, (size_t)count * sizeof(NetworkAp));
    s_ap_count = count;
    portEXIT_CRITICAL(&s_lock);
}

/** @brief WiFi + IP events, on the default event loop's task. */
static void on_net_event(void* argument, esp_event_base_t base, int32_t event_id, void* data)
{
    (void)argument;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        if (has_network())
        {
            join();
        }
    }
    else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        const wifi_event_sta_disconnected_t* disconnected = (const wifi_event_sta_disconnected_t*)data;
        if (disconnected->reason == WIFI_REASON_ASSOC_LEAVE || !has_network())
        {
            return; /* we left on purpose: for another network, a scan, or forget */
        }
        const uint32_t retry_ms = next_retry_ms();
        ESP_LOGW(TAG,
                 "not connected (reason %d), retrying in %u s",
                 (int)disconnected->reason,
                 (unsigned)(retry_ms / MS_PER_SECOND));
        set_state(NETWORK_FAILED, classify(disconnected->reason));
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, (uint64_t)retry_ms * US_PER_MS);
    }
    else if (base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE)
    {
        take_scan_results();
        atomic_store(&s_scanning, false);
        if (atomic_exchange(&s_resume_after_scan, false) && has_network())
        {
            esp_wifi_connect();
        }
    }
    else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t* got_ip = (const ip_event_got_ip_t*)data;
        ESP_LOGI(TAG, "connected, ip " IPSTR, IP2STR(&got_ip->ip_info.ip));
        set_state(NETWORK_CONNECTED, NETWORK_FAIL_NONE);
        reset_retry();
        atomic_store(&s_got_ip, true);
        esp_netif_sntp_start(); /* (re)starts SNTP, so a reconnect also resyncs */
    }
}

/** @brief SNTP sync notification: the system clock has just been set. */
static void on_time_sync(struct timeval* time_value)
{
    (void)time_value;
    atomic_store(&s_time_synced, true);
}

/** @brief Zeroes a buffer. Unlike a memset of a local about to go out of scope, the compiler cannot drop it. */
static void wipe(void* buffer, size_t length)
{
    volatile unsigned char* byte = (volatile unsigned char*)buffer;
    while (length--)
    {
        *byte++ = 0;
    }
}

/** @brief Reads a string key, leaving @p out alone when it is missing or does not fit. */
static void read_key(nvs_handle_t handle, const char* key, char* out, size_t capacity)
{
    size_t length = capacity;
    char value[NETWORK_PASSWORD_BYTES];
    if (capacity <= sizeof(value) && nvs_get_str(handle, key, value, &length) == ESP_OK)
    {
        memcpy(out, value, length);
    }
    wipe(value, sizeof(value));
}

/** @brief Writes string keys and commits, returning whether they were saved; @p value NULL erases @p key. */
static bool write_keys(const char* const* keys, const char* const* values, int count)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    const bool opened = (error == ESP_OK);
    for (int index = 0; error == ESP_OK && index < count; index++)
    {
        error = values[index] ? nvs_set_str(handle, keys[index], values[index]) : nvs_erase_key(handle, keys[index]);
        if (error == ESP_ERR_NVS_NOT_FOUND)
        {
            error = ESP_OK; /* erasing a key that was never set */
        }
    }
    if (error == ESP_OK)
    {
        error = nvs_commit(handle);
    }
    if (opened)
    {
        nvs_close(handle);
    }
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "saving settings failed: %s", esp_err_to_name(error));
        return false;
    }
    return true;
}

/** @brief Applies s_time_zone. */
static void apply_time_zone(void)
{
    setenv("TZ", s_time_zone, 1);
    tzset();
}

/** @brief Whether @p text is all hex digits: a 64-character password is the raw key, in hex. */
static bool is_hex(const char* text)
{
    for (; *text; text++)
    {
        if (!isxdigit((unsigned char)*text))
        {
            return false;
        }
    }
    return true;
}

/** @brief Builds a station config; a full-length SSID or PSK has no terminator. */
static void make_config(wifi_config_t* config, const char* ssid, const char* password)
{
    memset(config, 0, sizeof(*config));
    const size_t ssid_length = strlen(ssid);
    const size_t password_length = strlen(password);
    memcpy(config->sta.ssid, ssid, (ssid_length < sizeof(config->sta.ssid)) ? ssid_length : sizeof(config->sta.ssid));
    memcpy(config->sta.password,
           password,
           (password_length < sizeof(config->sta.password)) ? password_length : sizeof(config->sta.password));
    config->sta.threshold.authmode = (password_length > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config->sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
}

#if CONFIG_NVS_ENCRYPTION
/** @brief Erases the settings partition on the first encrypted boot. A password an unencrypted build saved
    would otherwise stay readable in flash until NVS reused its page. */
static esp_err_t erase_plaintext_once(void)
{
    nvs_handle_t handle;
    uint8_t encrypted = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK)
    {
        nvs_get_u8(handle, "encrypted", &encrypted);
        nvs_close(handle);
    }
    if (encrypted)
    {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "first encrypted boot: erasing the saved settings");
    esp_err_t error = nvs_flash_erase();
    if (error == ESP_OK)
    {
        error = nvs_flash_init();
    }
    if (error == ESP_OK)
    {
        error = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    }
    if (error == ESP_OK)
    {
        error = nvs_set_u8(handle, "encrypted", 1);
        if (error == ESP_OK)
        {
            error = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    return error;
}
#endif

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
#if CONFIG_NVS_ENCRYPTION
    CHECK(erase_plaintext_once());
#endif
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK)
    {
        read_key(handle, "tz", s_time_zone, sizeof(s_time_zone));
        read_key(handle, "ssid", s_status.ssid, sizeof(s_status.ssid)); /* no other task yet: no lock */
        nvs_close(handle);
    }
    apply_time_zone();

    s_records = heap_caps_malloc(SCAN_RECORDS * sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM);
    s_scan_buffer = heap_caps_calloc(NETWORK_SCAN_MAX, sizeof(NetworkAp), MALLOC_CAP_SPIRAM);
    s_scan_results = heap_caps_calloc(NETWORK_SCAN_MAX, sizeof(NetworkAp), MALLOC_CAP_SPIRAM);
    if (!s_records || !s_scan_buffer || !s_scan_results)
    {
        ESP_LOGE(TAG, "no PSRAM for scan results");
        return false;
    }

    CHECK(esp_netif_init());
    CHECK(esp_event_loop_create_default());
    /* With assertions compiled out (NDEBUG) this returns NULL instead of aborting inside. */
    if (!esp_netif_create_default_wifi_sta())
    {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return false;
    }
    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    CHECK(esp_wifi_init(&init_cfg));

    const esp_timer_create_args_t retry_args = {.callback = retry_join, .name = "wifi_retry"};
    CHECK(esp_timer_create(&retry_args, &s_retry_timer));
    CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_net_event, NULL, &s_wifi_handler));
    CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_net_event, NULL, &s_ip_handler));
    CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (s_status.ssid[0] != '\0')
    {
        /* The saved password is read only here, and wiped before anything can return. */
        char password[NETWORK_PASSWORD_BYTES] = "";
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK)
        {
            read_key(handle, "pass", password, sizeof(password));
            nvs_close(handle);
        }
        wifi_config_t config;
        make_config(&config, s_status.ssid, password);
        wipe(password, sizeof(password));
        err = esp_wifi_set_config(WIFI_IF_STA, &config);
        wipe(&config, sizeof(config));
        CHECK(err);
        s_status.state = NETWORK_CONNECTING;
    }

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
    NetworkStatus status;
    network_status(&status);
    if (status.state == NETWORK_CONNECTING || status.state == NETWORK_FAILED)
    {
        /* A connect attempt holds the radio: pause it until the scan is done. */
        esp_timer_stop(s_retry_timer);
        atomic_store(&s_resume_after_scan, true);
        esp_wifi_disconnect();
    }
    atomic_store(&s_scanning, true);
    const wifi_scan_config_t config = {.show_hidden = false};
    const esp_err_t error = esp_wifi_scan_start(&config, false);
    if (error != ESP_OK)
    {
        atomic_store(&s_scanning, false);
        ESP_LOGW(TAG, "scan not started: %s", esp_err_to_name(error));
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
    const int count = (s_ap_count < max) ? s_ap_count : max;
    if (count > 0)
    {
        memcpy(out, s_scan_results, (size_t)count * sizeof(NetworkAp));
    }
    portEXIT_CRITICAL(&s_lock);
    return count;
}

bool network_connect(const char* ssid, const char* password)
{
    const size_t ssid_length = strlen(ssid);
    const size_t password_length = strlen(password);
    if (!s_started || ssid_length == 0 || ssid_length > NETWORK_SSID_MAX_LENGTH
        || password_length > NETWORK_PASSWORD_MAX_LENGTH
        || (password_length > 0 && password_length < NETWORK_PASSWORD_MIN_LENGTH)
        || (password_length == NETWORK_PASSWORD_MAX_LENGTH && !is_hex(password)))
    {
        return false;
    }
    wifi_config_t config;
    make_config(&config, ssid, password);
    esp_timer_stop(s_retry_timer);
    reset_retry();
    esp_wifi_disconnect(); /* leave the current network; reported as ASSOC_LEAVE, which is ignored */
    const esp_err_t error = esp_wifi_set_config(WIFI_IF_STA, &config);
    wipe(&config, sizeof(config));
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(error));
        return false;
    }
    memcpy(s_pending_ssid, ssid, ssid_length + 1);
    memcpy(s_pending_password, password, password_length + 1);
    portENTER_CRITICAL(&s_lock);
    memcpy(s_status.ssid, ssid, ssid_length + 1);
    s_status.state = NETWORK_CONNECTING;
    s_status.fail = NETWORK_FAIL_NONE;
    portEXIT_CRITICAL(&s_lock);
    atomic_store(&s_got_ip, false);
    ESP_LOGI(TAG, "joining \"%s\"", ssid);
    join();
    return true;
}

bool network_forget(void)
{
    if (!s_started)
    {
        return false;
    }
    esp_timer_stop(s_retry_timer);
    atomic_store(&s_resume_after_scan, false);
    memset(s_pending_ssid, 0, sizeof(s_pending_ssid));
    memset(s_pending_password, 0, sizeof(s_pending_password));
    portENTER_CRITICAL(&s_lock);
    memset(s_status.ssid, 0, sizeof(s_status.ssid));
    s_status.state = NETWORK_IDLE;
    s_status.fail = NETWORK_FAIL_NONE;
    portEXIT_CRITICAL(&s_lock);
    esp_wifi_disconnect();
    wifi_config_t config;
    memset(&config, 0, sizeof(config));
    esp_wifi_set_config(WIFI_IF_STA, &config);
    static const char* const keys[] = {"ssid", "pass"};
    static const char* const none[] = {NULL, NULL};
    if (!write_keys(keys, none, ARRAY_COUNT(keys)))
    {
        return false; /* still saved, so the board rejoins it at the next boot */
    }
    ESP_LOGI(TAG, "network forgotten");
    return true;
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
        const char* const values[] = {s_pending_ssid, s_pending_password};
        /* A failed save stays pending and is tried again at the next connection. */
        if (write_keys(keys, values, ARRAY_COUNT(keys)))
        {
            ESP_LOGI(TAG, "saved \"%s\"", s_pending_ssid);
            memset(s_pending_ssid, 0, sizeof(s_pending_ssid));
            memset(s_pending_password, 0, sizeof(s_pending_password));
        }
    }
    if (!atomic_exchange(&s_time_synced, false))
    {
        return false;
    }
    struct timeval now;
    gettimeofday(&now, NULL);
    *epoch_ms = (int64_t)now.tv_sec * MS_PER_SECOND + now.tv_usec / US_PER_MS;
    s_time_known = true;

    struct tm local;
    char stamp[TIME_STAMP_BYTES];
    localtime_r(&now.tv_sec, &local);
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
    return ((days * HOURS_PER_DAY + (local.tm_hour - utc.tm_hour)) * MINUTES_PER_HOUR + (local.tm_min - utc.tm_min))
               * SECONDS_PER_MINUTE
           + (local.tm_sec - utc.tm_sec);
}

void network_time_zone(char* out, size_t capacity)
{
    strlcpy(out, s_time_zone, capacity);
}

bool network_set_time_zone(const char* time_zone)
{
    const size_t length = strlen(time_zone);
    if (length == 0 || length >= sizeof(s_time_zone))
    {
        return false;
    }
    memcpy(s_time_zone, time_zone, length + 1);
    apply_time_zone();
    static const char* const keys[] = {"tz"};
    const char* const values[] = {s_time_zone};
    ESP_LOGI(TAG, "time zone %s", s_time_zone);
    return write_keys(keys, values, ARRAY_COUNT(keys));
}
