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
    NetworkAp aps[NETWORK_SCAN_MAX];
    const int n = network_scan_results(aps, NETWORK_SCAN_MAX);
    if (n < 0)
    {
        return JS_NULL;
    }
    JSValue list = JS_NewArray(ctx);
    for (int i = 0; i < n; i++)
    {
        JSValue ap = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ap, "ssid", JS_NewString(ctx, aps[i].ssid));
        JS_SetPropertyStr(ctx, ap, "rssi", JS_NewInt32(ctx, aps[i].rssi));
        JS_SetPropertyStr(ctx, ap, "secure", JS_NewBool(ctx, aps[i].secure));
        JS_SetPropertyUint32(ctx, list, (uint32_t)i, ap);
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
    const char* password = (argc > 1 && !JS_IsUndefined(argv[1])) ? JS_ToCString(ctx, argv[1]) : NULL;
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
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    network_forget();
    return JS_UNDEFINED;
}

/** @brief `__erWifi.status()`. */
static JSValue js_status(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    NetworkStatus st;
    network_status(&st);
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "state", JS_NewString(ctx, k_states[st.state]));
    JS_SetPropertyStr(ctx, out, "ssid", JS_NewString(ctx, st.ssid));
    JS_SetPropertyStr(ctx, out, "reason", k_fails[st.fail] ? JS_NewString(ctx, k_fails[st.fail]) : JS_NULL);
    return out;
}

/** @brief `__erClock.utcOffsetMs()`. */
static JSValue js_utc_offset_ms(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return network_time_known() ? JS_NewInt32(ctx, network_utc_offset_s() * 1000) : JS_NULL;
}

/** @brief `__erClock.timeZone()`. */
static JSValue js_time_zone(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    char tz[64];
    network_time_zone(tz, sizeof(tz));
    return JS_NewString(ctx, tz);
}

/** @brief `__erClock.setTimeZone(tz)`. */
static JSValue js_set_time_zone(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
    {
        return JS_FALSE;
    }
    const char* tz = JS_ToCString(ctx, argv[0]);
    if (!tz)
    {
        return JS_EXCEPTION;
    }
    const bool ok = network_set_time_zone(tz);
    JS_FreeCString(ctx, tz);
    return JS_NewBool(ctx, ok);
}

/** @brief Adds a native method to @p obj. */
static void add_fn(JSContext* ctx, JSValue obj, const char* name, JSCFunction* fn, int length)
{
    JS_SetPropertyStr(ctx, obj, name, JS_NewCFunction(ctx, fn, name, length));
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
