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

#ifndef NETWORK_JS_H
#define NETWORK_JS_H

/*
 * network_js — the app's side of network.c, as two JS globals:
 *
 *   __erWifi.scan()                  starts a scan; false if the radio is busy (try again shortly)
 *   __erWifi.networks()              null while scanning, else [{ssid, rssi, secure}], strongest first
 *   __erWifi.connect(ssid, password) joins; the network is saved once it connects
 *   __erWifi.forget()                leaves and deletes the saved network
 *   __erWifi.status()                {state: 'idle'|'connecting'|'connected'|'failed', ssid,
 *                                     reason: null|'auth'|'notFound'|'other'}
 *   __erClock.utcOffsetMs()          local time minus UTC in ms, or null until the clock has synced
 *   __erClock.timeZone()             the saved POSIX TZ string
 *   __erClock.setTimeZone(tz)        applies and saves one
 *
 * All of them run on the frame-loop task, like the rest of the app.
 */

#include "quickjs.h"

/**
 * @brief ErRuntimeConfig.install_host_globals: gives a context `__erWifi` and `__erClock`.
 *
 * @param[in] ctx  The context the app is about to run in.
 */
void network_js_install(JSContext* ctx);

#endif
