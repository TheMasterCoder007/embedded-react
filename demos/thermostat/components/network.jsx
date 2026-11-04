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

const zoneName = timeZone => {
  const zone = ZONES.find(candidate => candidate.tz === timeZone);
  return zone ? zone.name : timeZone;
};

// A page is a larger sheet in the settings sheet's place; the list fills what the header leaves.
const PAGE_WIDTH = 600;
const PAGE_HEIGHT = 420;
const PAGE_PADDING = 22;
const HEADER_HEIGHT = 36;
const GAP = 14;

const STATUS_POLL_MS = 1000;
const SCAN_POLL_MS = 500;
const MIN_PASSPHRASE_LENGTH = 8;
const MAX_PASSPHRASE_LENGTH = 63;
const HEX_KEY_LENGTH = 64;
const LIST_HEIGHT = PAGE_HEIGHT - 2 * PAGE_PADDING - HEADER_HEIGHT - GAP;

const pageStyle = (theme, height) => ({
  width: PAGE_WIDTH,
  height,
  backgroundColor: theme.surface,
  borderWidth: 1,
  borderColor: theme.line,
  borderRadius: 16,
  padding: PAGE_PADDING,
  gap: GAP,
});

/** The station's status, re-read once a second; `refresh` re-reads it now. */
function useWifiStatus() {
  const [status, setStatus] = useState(() => wifi.status());
  const refresh = useCallback(() => setStatus(wifi.status()), []);
  useEffect(() => {
    const intervalId = setInterval(() => {
      const next = wifi.status();
      setStatus(previous =>
        previous.state === next.state &&
        previous.ssid === next.ssid &&
        previous.reason === next.reason
          ? previous
          : next,
      );
    }, STATUS_POLL_MS);
    return () => clearInterval(intervalId);
  }, []);
  return [status, refresh];
}

function statusText(status) {
  if (status.state === 'idle') return 'NOT CONNECTED';
  const what =
    status.state === 'connected'
      ? 'CONNECTED'
      : status.state === 'connecting'
        ? 'CONNECTING...'
        : status.reason === 'auth'
          ? 'WRONG PASSWORD?'
          : status.reason === 'notFound'
            ? 'NOT FOUND'
            : 'NOT CONNECTED';
  return status.ssid + ' • ' + what;
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
        height: HEADER_HEIGHT,
        paddingLeft: 16,
        paddingRight: 16,
        borderRadius: 9,
        alignItems: 'center',
        justifyContent: 'center',
        backgroundColor: strong ? theme.foreground : theme.trackBackground,
      }}>
      <Text
        style={{
          fontSize: 12,
          letterSpacing: 1,
          color: strong ? theme.background : theme.foreground,
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
        height: HEADER_HEIGHT,
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
          color: theme.foreground,
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
          backgroundColor: theme.trackBackground,
        }}>
        <Text style={{flex: 1, fontSize: 12, color: theme.foreground}}>
          {value}
        </Text>
        <Text style={{fontSize: 16, color: theme.dim}}>{'>'}</Text>
      </Pressable>
    </View>
  );
}

/** The settings sheet's network column. `onOpen('wifi' | 'zone')` swaps in that page. */
export function NetworkRows({theme, onOpen}) {
  const [status] = useWifiStatus();
  return (
    <View style={{gap: 20}}>
      <SettingRow
        theme={theme}
        label="WI-FI"
        value={statusText(status)}
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

const BAR_HEIGHTS = [5, 8, 11, 14];

/** Signal strength as four bars. */
function Bars({theme, rssi}) {
  const litBars = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
  return (
    <View
      style={{
        flexDirection: 'row',
        alignItems: 'flex-end',
        gap: 2,
        height: 14,
      }}>
      {BAR_HEIGHTS.map((barHeight, barIndex) => (
        <View
          key={barIndex}
          style={{
            width: 4,
            height: barHeight,
            backgroundColor: barIndex < litBars ? theme.foreground : theme.dim,
          }}
        />
      ))}
    </View>
  );
}

function NetworkRow({theme, accessPoint, tag, onPress}) {
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
        backgroundColor: theme.trackBackground,
      }}>
      <Text style={{flex: 1, fontSize: 16, color: theme.foreground}}>
        {accessPoint.ssid}
      </Text>
      {tag ? (
        <Text
          style={{fontSize: 10, letterSpacing: 1, color: theme.accents.auto}}>
          {tag}
        </Text>
      ) : null}
      {accessPoint.secure ? (
        <Text style={{fontSize: 10, letterSpacing: 1, color: theme.dim}}>
          SECURED
        </Text>
      ) : null}
      <Bars theme={theme} rssi={accessPoint.rssi} />
    </Pressable>
  );
}

/**
 * Picks a network. It scans on open (and on RESCAN), lists what it found, and asks for a password when the
 * network needs one. The host saves the network once it connects.
 */
export function WifiPage({theme, onDone}) {
  const [status, refresh] = useWifiStatus();
  const [networks, setNetworks] = useState(null); // null while scanning
  const [scanId, setScanId] = useState(0);
  const [pendingSsid, setPendingSsid] = useState(null); // a secured network waiting for its password
  const [error, setError] = useState('');

  // The radio can be busy for a moment (a connect attempt), so keep asking until the scan starts.
  useEffect(() => {
    setNetworks(null);
    let started = wifi.scan();
    const intervalId = setInterval(() => {
      if (!started) {
        started = wifi.scan();
        return;
      }
      const found = wifi.networks();
      if (found) {
        setNetworks(found);
        clearInterval(intervalId);
      }
    }, SCAN_POLL_MS);
    return () => clearInterval(intervalId);
  }, [scanId]);

  const join = (ssid, password) => {
    const ok = wifi.connect(ssid, password);
    refresh();
    return ok;
  };

  if (pendingSsid !== null) {
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
          ssid={pendingSsid}
          onCancel={() => setPendingSsid(null)}
          onJoin={password => {
            const ok = join(pendingSsid, password);
            if (ok) setPendingSsid(null);
            return ok;
          }}
        />
      </View>
    );
  }

  return (
    <View style={pageStyle(theme, PAGE_HEIGHT)}>
      <Header theme={theme} title={'WI-FI  •  ' + statusText(status)}>
        <Button
          theme={theme}
          label="RESCAN"
          onPress={() => setScanId(previous => previous + 1)}
        />
        {status.state !== 'idle' ? (
          <Button
            theme={theme}
            label="FORGET"
            onPress={() => {
              setError(wifi.forget() ? '' : "COULDN'T FORGET THE NETWORK");
              refresh();
            }}
          />
        ) : null}
        <Button theme={theme} label="DONE" strong onPress={onDone} />
      </Header>
      <ScrollView style={{height: LIST_HEIGHT}}>
        <View style={{gap: 6}}>
          {error ? (
            <Text
              style={{
                fontSize: 10,
                letterSpacing: 1,
                color: theme.accents.heat,
              }}>
              {error}
            </Text>
          ) : null}
          {networks === null ? (
            <Label theme={theme}>SCANNING...</Label>
          ) : networks.length === 0 ? (
            <Label theme={theme}>NO NETWORKS FOUND</Label>
          ) : (
            networks.map(accessPoint => (
              <NetworkRow
                key={accessPoint.ssid}
                theme={theme}
                accessPoint={accessPoint}
                tag={
                  accessPoint.ssid === status.ssid &&
                  status.state === 'connected'
                    ? 'CONNECTED'
                    : null
                }
                onPress={() =>
                  accessPoint.secure
                    ? setPendingSsid(accessPoint.ssid)
                    : join(accessPoint.ssid, '')
                }
              />
            ))
          )}
        </View>
      </ScrollView>
    </View>
  );
}

/** Whether every character is a hex digit: a 64-character password is the raw WPA2 key, in hex. */
function isHex(text) {
  for (let index = 0; index < text.length; index++) {
    const code = text.charCodeAt(index);
    const hex =
      (code >= 48 && code <= 57) ||
      (code >= 65 && code <= 70) ||
      (code >= 97 && code <= 102);
    if (!hex) return false;
  }
  return true;
}

/**
 * The password for one network. It sits at the top of the screen so its buttons stay clear of the
 * on-screen keyboard. `onJoin(password)` returns false when the host would not start connecting.
 */
function PasswordPage({theme, ssid, onCancel, onJoin}) {
  const [password, setPassword] = useState('');
  const [show, setShow] = useState(false);
  const [error, setError] = useState('');
  const join = () => {
    const ok =
      (password.length >= MIN_PASSPHRASE_LENGTH &&
        password.length <= MAX_PASSPHRASE_LENGTH) ||
      (password.length === HEX_KEY_LENGTH && isHex(password));
    if (!ok) {
      setError('USE 8-63 CHARACTERS OR 64 HEX DIGITS');
      return;
    }
    if (!onJoin(password)) setError("COULDN'T START CONNECTING");
  };
  return (
    <View
      style={pageStyle(
        theme,
        2 * PAGE_PADDING + HEADER_HEIGHT + 44 + 12 + 2 * GAP,
      )}>
      <Header theme={theme} title={'PASSWORD FOR ' + ssid}>
        <Button theme={theme} label="CANCEL" onPress={onCancel} />
        <Button theme={theme} label="JOIN" strong onPress={join} />
      </Header>
      <View style={{flexDirection: 'row', alignItems: 'center', gap: 10}}>
        <TextInput
          value={password}
          secureTextEntry={!show}
          onChangeText={text => {
            setPassword(text);
            setError('');
          }}
          onSubmitEditing={join}
          placeholder="Tap to type the password"
          placeholderTextColor={theme.dim}
          style={{
            flex: 1,
            height: 44,
            borderRadius: 9,
            paddingLeft: 12,
            paddingRight: 12,
            fontSize: 16,
            color: theme.foreground,
            backgroundColor: theme.trackBackground,
          }}
        />
        <Button
          theme={theme}
          label={show ? 'HIDE' : 'SHOW'}
          onPress={() => setShow(!show)}
        />
      </View>
      <Text
        style={{
          fontSize: 10,
          letterSpacing: 1,
          color: error ? theme.accents.heat : theme.dim,
        }}>
        {error || 'SAVED ONCE IT CONNECTS'}
      </Text>
    </View>
  );
}

/** Picks the time zone the clock shows. The host saves it, and the clock follows within a second. */
export function ZonePage({theme, onDone}) {
  const currentZone = clock.timeZone();
  const [error, setError] = useState('');
  return (
    <View style={pageStyle(theme, PAGE_HEIGHT)}>
      <Header theme={theme} title="TIME ZONE">
        <Button theme={theme} label="DONE" strong onPress={onDone} />
      </Header>
      <ScrollView style={{height: LIST_HEIGHT}}>
        <View style={{gap: 6}}>
          {error ? (
            <Text
              style={{
                fontSize: 10,
                letterSpacing: 1,
                color: theme.accents.heat,
              }}>
              {error}
            </Text>
          ) : null}
          {ZONES.map(zone => {
            const on = zone.tz === currentZone;
            return (
              <Pressable
                key={zone.tz}
                onPress={() => {
                  if (clock.setTimeZone(zone.tz)) onDone();
                  else setError('TIME ZONE SET, BUT NOT SAVED');
                }}
                style={{
                  height: 44,
                  borderRadius: 9,
                  flexDirection: 'row',
                  alignItems: 'center',
                  paddingLeft: 14,
                  paddingRight: 14,
                  backgroundColor: on
                    ? theme.foreground
                    : theme.trackBackground,
                }}>
                <Text
                  style={{
                    flex: 1,
                    fontSize: 16,
                    color: on ? theme.background : theme.foreground,
                  }}>
                  {zone.name}
                </Text>
                <Text
                  style={{
                    fontSize: 12,
                    color: on ? theme.background : theme.dim,
                  }}>
                  {'UTC' + zone.utc}
                </Text>
              </Pressable>
            );
          })}
        </View>
      </ScrollView>
    </View>
  );
}
