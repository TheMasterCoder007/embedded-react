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

import {useCallback, useEffect, useState} from 'react';
import {View, Text, Pressable, ScrollView, TextInput} from 'embedded-react';

// The settings sheet's Wi-Fi and time-zone pages (Flow A only). They drive the host's __erWifi and
// __erClock, which the ESP32-S3 built with Wi-Fi installs before the app runs (see its main/network_js.h),
// so one look at load is enough. Where they are absent (the simulator, other boards), the sheet leaves
// these pages out.
const wifi = globalThis.__erWifi;
const clock = globalThis.__erClock;
export const HAS_NETWORK = !!wifi && !!clock;

// Time zones to pick from, as POSIX TZ strings with their daylight-saving rules, for the host to apply.
export const ZONES = [
  {name: 'Honolulu', utc: '-10', tz: 'HST10'},
  {name: 'Anchorage', utc: '-9', tz: 'AKST9AKDT,M3.2.0,M11.1.0'},
  {name: 'Vancouver / Los Angeles', utc: '-8', tz: 'PST8PDT,M3.2.0,M11.1.0'},
  {name: 'Edmonton / Denver', utc: '-7', tz: 'MST7MDT,M3.2.0,M11.1.0'},
  {name: 'Phoenix', utc: '-7', tz: 'MST7'},
  {name: 'Winnipeg / Chicago', utc: '-6', tz: 'CST6CDT,M3.2.0,M11.1.0'},
  {name: 'Regina', utc: '-6', tz: 'CST6'},
  {name: 'Toronto / New York', utc: '-5', tz: 'EST5EDT,M3.2.0,M11.1.0'},
  {name: 'Halifax', utc: '-4', tz: 'AST4ADT,M3.2.0,M11.1.0'},
  {name: "St. John's", utc: '-3:30', tz: 'NST3:30NDT,M3.2.0,M11.1.0'},
  {name: 'Sao Paulo', utc: '-3', tz: 'BRT3'},
  {name: 'UTC', utc: '+0', tz: 'UTC0'},
  {name: 'London', utc: '+0', tz: 'GMT0BST,M3.5.0/1,M10.5.0'},
  {name: 'Paris / Berlin', utc: '+1', tz: 'CET-1CEST,M3.5.0,M10.5.0/3'},
  {name: 'Athens / Helsinki', utc: '+2', tz: 'EET-2EEST,M3.5.0/3,M10.5.0/4'},
  {name: 'Moscow', utc: '+3', tz: 'MSK-3'},
  {name: 'Dubai', utc: '+4', tz: 'GST-4'},
  {name: 'India', utc: '+5:30', tz: 'IST-5:30'},
  {name: 'Beijing / Singapore', utc: '+8', tz: 'CST-8'},
  {name: 'Tokyo', utc: '+9', tz: 'JST-9'},
  {name: 'Sydney', utc: '+10', tz: 'AEST-10AEDT,M10.1.0,M4.1.0/3'},
  {name: 'Auckland', utc: '+12', tz: 'NZST-12NZDT,M9.5.0,M4.1.0/3'},
];

const zoneName = tz => {
  const z = ZONES.find(z => z.tz === tz);
  return z ? z.name : tz;
};

// A page is a larger sheet in the settings sheet's place; the list fills what the header leaves.
const PAGE_W = 600;
const PAGE_H = 420;
const PAGE_PAD = 22;
const HEADER_H = 36;
const GAP = 14;
const LIST_H = PAGE_H - 2 * PAGE_PAD - HEADER_H - GAP;

const pageStyle = (theme, height) => ({
  width: PAGE_W,
  height,
  backgroundColor: theme.surface,
  borderWidth: 1,
  borderColor: theme.line,
  borderRadius: 16,
  padding: PAGE_PAD,
  gap: GAP,
});

/** The station's status, re-read once a second; `refresh` re-reads it now. */
function useWifiStatus() {
  const [st, setSt] = useState(() => wifi.status());
  const refresh = useCallback(() => setSt(wifi.status()), []);
  useEffect(() => {
    const id = setInterval(() => {
      const next = wifi.status();
      setSt(prev =>
        prev.state === next.state &&
        prev.ssid === next.ssid &&
        prev.reason === next.reason
          ? prev
          : next,
      );
    }, 1000);
    return () => clearInterval(id);
  }, []);
  return [st, refresh];
}

function statusText(st) {
  if (st.state === 'idle') return 'NOT CONNECTED';
  const what =
    st.state === 'connected'
      ? 'CONNECTED'
      : st.state === 'connecting'
        ? 'CONNECTING...'
        : st.reason === 'auth'
          ? 'WRONG PASSWORD?'
          : st.reason === 'notFound'
            ? 'NOT FOUND'
            : 'NOT CONNECTED';
  return st.ssid + ' • ' + what;
}

function Label({theme, children}) {
  return (
    <Text style={{fontSize: 10, letterSpacing: 2, color: theme.dim}}>
      {children}
    </Text>
  );
}

function Button({theme, label, strong, onPress}) {
  return (
    <Pressable
      onPress={onPress}
      style={{
        height: HEADER_H,
        paddingLeft: 16,
        paddingRight: 16,
        borderRadius: 9,
        alignItems: 'center',
        justifyContent: 'center',
        backgroundColor: strong ? theme.fg : theme.trackBg,
      }}>
      <Text
        style={{
          fontSize: 12,
          letterSpacing: 1,
          color: strong ? theme.bg : theme.fg,
        }}>
        {label}
      </Text>
    </Pressable>
  );
}

function Header({theme, title, children}) {
  return (
    <View
      style={{
        height: HEADER_H,
        flexDirection: 'row',
        alignItems: 'center',
        gap: 10,
      }}>
      <Text
        style={{
          flex: 1,
          fontSize: 12,
          fontWeight: '600',
          letterSpacing: 1,
          color: theme.fg,
        }}>
        {title}
      </Text>
      {children}
    </View>
  );
}

/** A tappable row: what is set, and a chevron to the page that changes it. */
function SettingRow({theme, label, value, onPress}) {
  return (
    <View style={{gap: 9}}>
      <Label theme={theme}>{label}</Label>
      <Pressable
        onPress={onPress}
        style={{
          height: 44,
          borderRadius: 9,
          flexDirection: 'row',
          alignItems: 'center',
          paddingLeft: 14,
          paddingRight: 14,
          backgroundColor: theme.trackBg,
        }}>
        <Text style={{flex: 1, fontSize: 12, color: theme.fg}}>{value}</Text>
        <Text style={{fontSize: 16, color: theme.dim}}>{'>'}</Text>
      </Pressable>
    </View>
  );
}

/** The settings sheet's network column. `onOpen('wifi' | 'zone')` swaps in that page. */
export function NetworkRows({theme, onOpen}) {
  const [st] = useWifiStatus();
  return (
    <View style={{gap: 20}}>
      <SettingRow
        theme={theme}
        label="WI-FI"
        value={statusText(st)}
        onPress={() => onOpen('wifi')}
      />
      <SettingRow
        theme={theme}
        label="TIME ZONE"
        value={zoneName(clock.timeZone())}
        onPress={() => onOpen('zone')}
      />
    </View>
  );
}

const BAR_H = [5, 8, 11, 14];

/** Signal strength as four bars. */
function Bars({theme, rssi}) {
  const n = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
  return (
    <View
      style={{
        flexDirection: 'row',
        alignItems: 'flex-end',
        gap: 2,
        height: 14,
      }}>
      {BAR_H.map((h, i) => (
        <View
          key={i}
          style={{
            width: 4,
            height: h,
            backgroundColor: i < n ? theme.fg : theme.dim,
          }}
        />
      ))}
    </View>
  );
}

function NetworkRow({theme, ap, tag, onPress}) {
  return (
    <Pressable
      onPress={onPress}
      style={{
        height: 46,
        borderRadius: 9,
        flexDirection: 'row',
        alignItems: 'center',
        gap: 12,
        paddingLeft: 14,
        paddingRight: 14,
        backgroundColor: theme.trackBg,
      }}>
      <Text style={{flex: 1, fontSize: 16, color: theme.fg}}>{ap.ssid}</Text>
      {tag ? (
        <Text
          style={{fontSize: 10, letterSpacing: 1, color: theme.accents.auto}}>
          {tag}
        </Text>
      ) : null}
      {ap.secure ? (
        <Text style={{fontSize: 10, letterSpacing: 1, color: theme.dim}}>
          SECURED
        </Text>
      ) : null}
      <Bars theme={theme} rssi={ap.rssi} />
    </Pressable>
  );
}

/**
 * Picks a network. It scans on open (and on RESCAN), lists what it found, and asks for a password when the
 * network needs one. The host saves the network once it connects.
 */
export function WifiPage({theme, onDone}) {
  const [st, refresh] = useWifiStatus();
  const [nets, setNets] = useState(null); // null while scanning
  const [scanId, setScanId] = useState(0);
  const [pick, setPick] = useState(null); // a secured network waiting for its password

  // The radio can be busy for a moment (a connect attempt), so keep asking until the scan starts.
  useEffect(() => {
    setNets(null);
    let started = wifi.scan();
    const id = setInterval(() => {
      if (!started) {
        started = wifi.scan();
        return;
      }
      const found = wifi.networks();
      if (found) {
        setNets(found);
        clearInterval(id);
      }
    }, 500);
    return () => clearInterval(id);
  }, [scanId]);

  const join = (ssid, password) => {
    const ok = wifi.connect(ssid, password);
    refresh();
    return ok;
  };

  if (pick !== null) {
    return (
      <View
        style={{
          position: 'absolute',
          left: 0,
          top: 0,
          width: screen.width,
          height: screen.height,
          alignItems: 'center',
          paddingTop: 36,
        }}>
        <PasswordPage
          theme={theme}
          ssid={pick}
          onCancel={() => setPick(null)}
          onJoin={pw => {
            const ok = join(pick, pw);
            if (ok) setPick(null);
            return ok;
          }}
        />
      </View>
    );
  }

  return (
    <View style={pageStyle(theme, PAGE_H)}>
      <Header theme={theme} title={'WI-FI  •  ' + statusText(st)}>
        <Button
          theme={theme}
          label="RESCAN"
          onPress={() => setScanId(n => n + 1)}
        />
        {st.state !== 'idle' ? (
          <Button
            theme={theme}
            label="FORGET"
            onPress={() => {
              wifi.forget();
              refresh();
            }}
          />
        ) : null}
        <Button theme={theme} label="DONE" strong onPress={onDone} />
      </Header>
      <ScrollView style={{height: LIST_H}}>
        <View style={{gap: 6}}>
          {nets === null ? (
            <Label theme={theme}>SCANNING...</Label>
          ) : nets.length === 0 ? (
            <Label theme={theme}>NO NETWORKS FOUND</Label>
          ) : (
            nets.map(ap => (
              <NetworkRow
                key={ap.ssid}
                theme={theme}
                ap={ap}
                tag={
                  ap.ssid === st.ssid && st.state === 'connected'
                    ? 'CONNECTED'
                    : null
                }
                onPress={() =>
                  ap.secure ? setPick(ap.ssid) : join(ap.ssid, '')
                }
              />
            ))
          )}
        </View>
      </ScrollView>
    </View>
  );
}

/**
 * The password for one network. It sits at the top of the screen so its buttons stay clear of the
 * on-screen keyboard. `onJoin(password)` returns false when the host would not start connecting.
 */
function PasswordPage({theme, ssid, onCancel, onJoin}) {
  const [pw, setPw] = useState('');
  const [err, setErr] = useState('');
  const join = () => {
    if (pw.length < 8 || pw.length > 64) {
      setErr('A WI-FI PASSWORD IS 8 TO 63 CHARACTERS');
      return;
    }
    if (!onJoin(pw)) setErr("COULDN'T START CONNECTING");
  };
  return (
    <View style={pageStyle(theme, 2 * PAGE_PAD + HEADER_H + 44 + 12 + 2 * GAP)}>
      <Header theme={theme} title={'PASSWORD FOR ' + ssid}>
        <Button theme={theme} label="CANCEL" onPress={onCancel} />
        <Button theme={theme} label="JOIN" strong onPress={join} />
      </Header>
      <TextInput
        value={pw}
        onChangeText={t => {
          setPw(t);
          setErr('');
        }}
        onSubmitEditing={join}
        placeholder="Tap to type the password"
        placeholderTextColor={theme.dim}
        style={{
          height: 44,
          borderRadius: 9,
          paddingLeft: 12,
          paddingRight: 12,
          fontSize: 16,
          color: theme.fg,
          backgroundColor: theme.trackBg,
        }}
      />
      <Text
        style={{
          fontSize: 10,
          letterSpacing: 1,
          color: err ? theme.accents.heat : theme.dim,
        }}>
        {err || 'SAVED ONCE IT CONNECTS'}
      </Text>
    </View>
  );
}

/** Picks the time zone the clock shows. The host saves it, and the clock follows within a second. */
export function ZonePage({theme, onDone}) {
  const cur = clock.timeZone();
  return (
    <View style={pageStyle(theme, PAGE_H)}>
      <Header theme={theme} title="TIME ZONE">
        <Button theme={theme} label="DONE" strong onPress={onDone} />
      </Header>
      <ScrollView style={{height: LIST_H}}>
        <View style={{gap: 6}}>
          {ZONES.map(z => {
            const on = z.tz === cur;
            return (
              <Pressable
                key={z.tz}
                onPress={() => {
                  clock.setTimeZone(z.tz);
                  onDone();
                }}
                style={{
                  height: 44,
                  borderRadius: 9,
                  flexDirection: 'row',
                  alignItems: 'center',
                  paddingLeft: 14,
                  paddingRight: 14,
                  backgroundColor: on ? theme.fg : theme.trackBg,
                }}>
                <Text
                  style={{
                    flex: 1,
                    fontSize: 16,
                    color: on ? theme.bg : theme.fg,
                  }}>
                  {z.name}
                </Text>
                <Text style={{fontSize: 12, color: on ? theme.bg : theme.dim}}>
                  {'UTC' + z.utc}
                </Text>
              </Pressable>
            );
          })}
        </View>
      </ScrollView>
    </View>
  );
}
