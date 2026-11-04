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

import {memo, useEffect, useRef, useState, useSyncExternalStore} from 'react';
import {View, Text, Pressable} from 'embedded-react';
import {
  MINUTE,
  MONTHS,
  UNSET_START,
  clockParts,
  stepField,
  fmtHour,
  fmtMin,
  fmtAmPm,
  fmtTime,
  fmtDate,
} from './calendar.js';

// The clock is an offset onto Date.now(): local time minus Date.now(). Set by hand, Date.now() is the
// engine clock counting from boot and nothing saves the offset, so a power cycle loses it. A host with
// network time (the ESP32-S3 built with WiFi) anchors Date.now() to UTC and exposes
// __erClock.utcOffsetMs(), so there the offset is just the time zone. It lives outside React state, so a
// change re-renders the two clock views rather than the whole thermostat.
let offset = null; // null until the clock has been set
let networkOffset = null; // the host's offset as last applied; null until it has synced
const listeners = new Set();
const subscribe = listener => {
  listeners.add(listener);
  return () => listeners.delete(listener);
};
const getOffset = () => offset;
const setOffset = nextOffset => {
  offset = nextOffset;
  listeners.forEach(listener => listener());
};
const useOffset = () => useSyncExternalStore(subscribe, getOffset);
const NETWORK_TIME_POLL_MS = 1000;

/** Re-renders at each local minute boundary while the clock is set — once a minute, not every frame. */
function useMinuteTick(clockOffset) {
  const [, setTick] = useState(0);
  useEffect(() => {
    if (clockOffset === null) return undefined;
    let timerId = 0;
    const arm = () => {
      timerId = setTimeout(
        () => {
          setTick(count => count + 1);
          arm();
        },
        MINUTE - ((Date.now() + clockOffset) % MINUTE),
      );
    };
    arm();
    return () => clearTimeout(timerId);
  }, [clockOffset]);
}

/**
 * Follows the host's network time when there is one: takes its offset on the first sync and whenever it
 * changes (daylight saving). Checked once a second; a hand-set nudge in between stands until then.
 */
function useNetworkTime() {
  useEffect(() => {
    const host = globalThis.__erClock;
    if (!host) return undefined;
    const timerId = setInterval(() => {
      const hostOffset = host.utcOffsetMs();
      if (hostOffset !== null && hostOffset !== networkOffset) {
        networkOffset = hostOffset;
        setOffset(hostOffset);
      }
    }, NETWORK_TIME_POLL_MS);
    return () => clearInterval(timerId);
  }, []);
}

/** The header readout. Tapping it opens the settings sheet, where the clock is set. */
export const HeaderClock = memo(function HeaderClock({theme, onPress}) {
  const clockOffset = useOffset();
  useNetworkTime();
  useMinuteTick(clockOffset);
  const parts =
    clockOffset === null ? null : clockParts(Date.now() + clockOffset);
  return (
    <Pressable onPress={onPress} style={{alignItems: 'flex-end', gap: 3}}>
      <Text
        style={{
          fontSize: 20,
          fontWeight: '500',
          color: parts ? theme.foreground : theme.dim,
        }}>
        {parts ? fmtTime(parts) : '--:--'}
      </Text>
      <Text style={{fontSize: 10, letterSpacing: 1, color: theme.dim}}>
        {parts ? fmtDate(parts) : 'SET CLOCK'}
      </Text>
    </Pressable>
  );
});

// Press-and-hold: one step on touch-down, then a repeat every REPEAT_MS once the finger has stayed down
// for HOLD_MS. REPEAT_MAX bounds a hold whose release never arrives.
const HOLD_MS = 400;
const REPEAT_MS = 110;
const REPEAT_MAX = 300;

function RepeatStepper({direction, onStep, theme}) {
  const stepRef = useRef(onStep);
  stepRef.current = onStep;
  const timer = useRef(0);
  const stop = () => {
    clearTimeout(timer.current);
    timer.current = 0;
  };
  const start = () => {
    stop();
    let steps = 0;
    const tick = delay => {
      stepRef.current(direction);
      if (++steps < REPEAT_MAX) {
        timer.current = setTimeout(() => tick(REPEAT_MS), delay);
      }
    };
    tick(HOLD_MS);
  };
  useEffect(() => stop, []);
  return (
    <Pressable
      onPressIn={start}
      onPressOut={stop}
      style={{
        width: 52,
        height: 52,
        borderRadius: 26,
        borderWidth: 1,
        borderColor: theme.line,
        backgroundColor: theme.surface,
        alignItems: 'center',
        justifyContent: 'center',
      }}>
      {direction < 0 ? (
        <View
          style={{width: 16, height: 2, backgroundColor: theme.foreground}}
        />
      ) : (
        <Text style={{fontSize: 20, color: theme.foreground}}>+</Text>
      )}
    </Pressable>
  );
}

/** One tappable field. Fixed widths keep the row still as digits change. */
function Field({id, label, width, big, selected, onSelect, theme}) {
  const on = id === selected;
  return (
    <Pressable
      onPress={() => onSelect(id)}
      style={{
        width,
        height: big ? 40 : 32,
        borderRadius: 8,
        alignItems: 'center',
        justifyContent: 'center',
        backgroundColor: on ? theme.trackBackground : theme.surface,
      }}>
      <Text
        style={{
          fontSize: big ? 24 : 16,
          color: on ? theme.foreground : theme.secondary,
        }}>
        {label}
      </Text>
    </Pressable>
  );
}

/**
 * The settings-sheet setter: tap a field (hour, minute, AM/PM, month, day or year), then step it with −/+.
 * Each step applies at once, and the weekday follows from the date.
 */
export function ClockSetter({theme}) {
  const clockOffset = useOffset();
  useMinuteTick(clockOffset);
  const [field, setField] = useState('hour');
  const parts = clockParts(
    clockOffset === null ? UNSET_START : Date.now() + clockOffset,
  );
  const step = direction => {
    const current = getOffset();
    const now = Date.now();
    setOffset(
      stepField(
        current === null ? UNSET_START : now + current,
        field,
        direction,
      ) - now,
    );
  };
  const f = (id, label, width, big) => (
    <Field
      id={id}
      label={label}
      width={width}
      big={big}
      selected={field}
      onSelect={setField}
      theme={theme}
    />
  );
  return (
    <View style={{gap: 9}}>
      <Text style={{fontSize: 10, letterSpacing: 2, color: theme.dim}}>
        {clockOffset === null
          ? 'CLOCK • NOT SET'
          : clockOffset === networkOffset
            ? 'CLOCK • NETWORK'
            : 'CLOCK'}
      </Text>
      <View style={{flexDirection: 'row', alignItems: 'center', gap: 8}}>
        <RepeatStepper direction={-1} onStep={step} theme={theme} />
        <View style={{flex: 1, alignItems: 'center', gap: 4}}>
          <View style={{flexDirection: 'row', alignItems: 'center', gap: 2}}>
            {f('hour', fmtHour(parts), 44, true)}
            <Text style={{fontSize: 24, color: theme.dim}}>:</Text>
            {f('min', fmtMin(parts), 44, true)}
            {f('ampm', fmtAmPm(parts), 52, true)}
          </View>
          <View style={{flexDirection: 'row', alignItems: 'center', gap: 4}}>
            {f('month', MONTHS[parts.month - 1], 52)}
            {f('day', String(parts.day), 40)}
            {f('year', String(parts.year), 60)}
          </View>
        </View>
        <RepeatStepper direction={1} onStep={step} theme={theme} />
      </View>
    </View>
  );
}
