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
 * The app's side of network.c (see network_js.h).
 */

#include "network_js.h"

#include "network.h"

#include <stdbool.h>

#define MS_PER_SECOND 1000

static const char* const k_states[] = {"idle", "connecting", "connected", "failed"};
static const char* const k_fails[] = {NULL, "auth", "notFound", "other"};

/** @brief `__erWifi.scan()`. */
static JSValue js_scan(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewBool(ctx, network_scan_start());
}

/** @brief `__erWifi.networks()`. */
static JSValue js_networks(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    NetworkAp networks[NETWORK_SCAN_MAX];
    const int count = network_scan_results(networks, NETWORK_SCAN_MAX);
    if (count < 0)
    {
        return JS_NULL;
    }
    JSValue list = JS_NewArray(ctx);
    for (int index = 0; index < count; index++)
    {
        JSValue network = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, network, "ssid", JS_NewString(ctx, networks[index].ssid));
        JS_SetPropertyStr(ctx, network, "rssi", JS_NewInt32(ctx, networks[index].rssi));
        JS_SetPropertyStr(ctx, network, "secure", JS_NewBool(ctx, networks[index].secure));
        JS_SetPropertyUint32(ctx, list, (uint32_t)index, network);
    }
    return list;
}

/** @brief `__erWifi.connect(ssid, password)`. */
static JSValue js_connect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
    {
        return JS_FALSE;
    }
    const char* ssid = JS_ToCString(ctx, argv[0]);
    if (!ssid)
    {
        return JS_EXCEPTION;
    }
    const bool has_password = argc > 1 && !JS_IsUndefined(argv[1]);
    const char* password = has_password ? JS_ToCString(ctx, argv[1]) : NULL;
    if (has_password && !password)
    {
        JS_FreeCString(ctx, ssid);
        return JS_EXCEPTION;
    }
    const bool ok = network_connect(ssid, password ? password : "");
    JS_FreeCString(ctx, ssid);
    if (password)
    {
        JS_FreeCString(ctx, password);
    }
    return JS_NewBool(ctx, ok);
}

/** @brief `__erWifi.forget()`. */
static JSValue js_forget(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewBool(ctx, network_forget());
}

/** @brief `__erWifi.status()`. */
static JSValue js_status(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    NetworkStatus status;
    network_status(&status);
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "state", JS_NewString(ctx, k_states[status.state]));
    JS_SetPropertyStr(ctx, out, "ssid", JS_NewString(ctx, status.ssid));
    JS_SetPropertyStr(ctx, out, "reason", k_fails[status.fail] ? JS_NewString(ctx, k_fails[status.fail]) : JS_NULL);
    return out;
}

/** @brief `__erClock.utcOffsetMs()`. */
static JSValue js_utc_offset_ms(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return network_time_known() ? JS_NewInt32(ctx, network_utc_offset_s() * MS_PER_SECOND) : JS_NULL;
}

/** @brief `__erClock.timeZone()`. */
static JSValue js_time_zone(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    char time_zone[NETWORK_TIME_ZONE_BYTES];
    network_time_zone(time_zone, sizeof(time_zone));
    return JS_NewString(ctx, time_zone);
}

/** @brief `__erClock.setTimeZone(tz)`. */
static JSValue js_set_time_zone(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
    {
        return JS_FALSE;
    }
    const char* time_zone = JS_ToCString(ctx, argv[0]);
    if (!time_zone)
    {
        return JS_EXCEPTION;
    }
    const bool ok = network_set_time_zone(time_zone);
    JS_FreeCString(ctx, time_zone);
    return JS_NewBool(ctx, ok);
}

/** @brief Adds a native method to @p object. */
static void add_fn(JSContext* ctx, JSValue object, const char* name, JSCFunction* function, int length)
{
    JS_SetPropertyStr(ctx, object, name, JS_NewCFunction(ctx, function, name, length));
}

void network_js_install(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);

    JSValue wifi = JS_NewObject(ctx);
    add_fn(ctx, wifi, "scan", js_scan, 0);
    add_fn(ctx, wifi, "networks", js_networks, 0);
    add_fn(ctx, wifi, "connect", js_connect, 2);
    add_fn(ctx, wifi, "forget", js_forget, 0);
    add_fn(ctx, wifi, "status", js_status, 0);
    JS_SetPropertyStr(ctx, global, "__erWifi", wifi);

    JSValue clock = JS_NewObject(ctx);
    add_fn(ctx, clock, "utcOffsetMs", js_utc_offset_ms, 0);
    add_fn(ctx, clock, "timeZone", js_time_zone, 0);
    add_fn(ctx, clock, "setTimeZone", js_set_time_zone, 1);
    JS_SetPropertyStr(ctx, global, "__erClock", clock);

    JS_FreeValue(ctx, global);
}
