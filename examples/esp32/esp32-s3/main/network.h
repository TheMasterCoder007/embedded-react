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

#ifndef NETWORK_H
#define NETWORK_H

/*
 * network — WiFi for this board, driven by the app: a station the app scans with and points at a network,
 * the network and the time zone saved in NVS, and SNTP on top so the app's Date.now() is the real time.
 * Compiled only with CONFIG_ER_WIFI; network_js.c is the app's side of it.
 *
 * Concurrency (keeps er_runtime single-threaded): WiFi, the event loop and SNTP run on their own tasks and
 * only update state here. The app's calls and network_poll() run on the frame-loop task.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Most networks a scan reports: the strongest, one entry per name. */
#define NETWORK_SCAN_MAX 20

/** @brief Where the station is. */
typedef enum
{
    NETWORK_IDLE,       /**< no network chosen */
    NETWORK_CONNECTING, /**< joining the chosen network */
    NETWORK_CONNECTED,  /**< joined, with an IP address */
    NETWORK_FAILED,     /**< the last attempt failed; it keeps retrying */
} NetworkState;

/** @brief Why the last attempt failed. */
typedef enum
{
    NETWORK_FAIL_NONE,
    NETWORK_FAIL_AUTH,      /**< the handshake failed, most often a wrong password */
    NETWORK_FAIL_NOT_FOUND, /**< no network by that name in range */
    NETWORK_FAIL_OTHER,
} NetworkFail;

/** @brief One network a scan found. */
typedef struct
{
    char ssid[33];
    int8_t rssi; /**< dBm */
    bool secure; /**< needs a password */
} NetworkAp;

/** @brief The station's state and the network it is on or joining. */
typedef struct
{
    NetworkState state;
    NetworkFail fail;
    char ssid[33]; /**< "" when idle */
} NetworkStatus;

/**
 * @brief Loads the saved network and time zone, brings up the WiFi station and SNTP, and starts joining the
 *        saved network if there is one.
 *
 * Returns straight away; joining, retrying and the NTP syncs happen in the background. Returns false, with
 * WiFi left off and the UI still running, when a setup call fails, logging which.
 *
 * @return true if WiFi is up.
 */
bool network_start(void);

/**
 * @brief Starts a scan in the background. A connect attempt in progress is paused until it finishes.
 *
 * @return true if a scan is running; false if WiFi is off or the driver refused (try again shortly).
 */
bool network_scan_start(void);

/**
 * @brief The last scan's networks, strongest first, one entry per name, hidden networks left out.
 *
 * @param[out] out  Receives up to @p max entries.
 * @param[in]  max  Capacity of @p out.
 *
 * @return How many were written, or -1 while a scan is running or before the first one finishes.
 */
int network_scan_results(NetworkAp* out, int max);

/**
 * @brief Joins a network, leaving the current one. It is saved once it connects, so a mistyped password
 *        never replaces the network the board joins at boot.
 *
 * @param[in] ssid      Network name, 1-32 bytes.
 * @param[in] password  "" for an open network, else 8-63 characters (or 64 hex digits).
 *
 * @return false if WiFi is off or either argument is out of range.
 */
bool network_connect(const char* ssid, const char* password);

/** @brief Leaves the current network and deletes the saved one. */
void network_forget(void);

/** @brief Reads the station's state. */
void network_status(NetworkStatus* out);

/**
 * @brief Frame-loop hook: saves a network that just connected and reports an NTP sync.
 *
 * @param[out] epoch_ms  Receives the current time, in ms since the Unix epoch, when this returns true.
 *
 * @return true once per sync (the first, then SNTP's periodic resyncs); false otherwise.
 */
bool network_poll(int64_t* epoch_ms);

/** @brief Whether network_poll() has reported a sync this boot, so the system clock is the real time. */
bool network_time_known(void);

/**
 * @brief The time zone's current UTC offset (local time minus UTC), daylight saving included.
 *
 * @return Offset in seconds, e.g. -25200 for Vancouver in summer.
 */
int32_t network_utc_offset_s(void);

/**
 * @brief The saved time zone, as a POSIX TZ string ("UTC0" until one is set).
 *
 * @param[out] out  Receives the string.
 * @param[in]  cap  Capacity of @p out.
 */
void network_time_zone(char* out, size_t cap);

/**
 * @brief Applies a POSIX TZ string (e.g. "PST8PDT,M3.2.0,M11.1.0") and saves it.
 *
 * @return false if it is empty or too long.
 */
bool network_set_time_zone(const char* tz);

#endif
