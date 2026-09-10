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

// The clock is an offset onto Date.now(): the local time you set, minus Date.now() at that moment. The
// target boards have no RTC, so Date.now() is the engine clock counting from boot, and the offset turns it
// into local time. It lives outside React state, so stepping it re-renders the two clock views rather
// than the whole thermostat. Nothing saves it: after a power cycle the clock has to be set again.
let offset = null; // null until the clock has been set
const listeners = new Set();
const subscribe = fn => {
  listeners.add(fn);
  return () => listeners.delete(fn);
};
const getOffset = () => offset;
const setOffset = v => {
  offset = v;
  listeners.forEach(fn => fn());
};
const useOffset = () => useSyncExternalStore(subscribe, getOffset);

/** Re-renders at each local minute boundary while the clock is set — once a minute, not every frame. */
function useMinuteTick(off) {
  const [, setTick] = useState(0);
  useEffect(() => {
    if (off === null) return undefined;
    let id = 0;
    const arm = () => {
      id = setTimeout(
        () => {
          setTick(n => n + 1);
          arm();
        },
        MINUTE - ((Date.now() + off) % MINUTE),
      );
    };
    arm();
    return () => clearTimeout(id);
  }, [off]);
}

/** The header readout. Tapping it opens the settings sheet, where the clock is set. */
export const HeaderClock = memo(function HeaderClock({theme, onPress}) {
  const off = useOffset();
  useMinuteTick(off);
  const p = off === null ? null : clockParts(Date.now() + off);
  return (
    <Pressable onPress={onPress} style={{alignItems: 'flex-end', gap: 3}}>
      <Text
        style={{
          fontSize: 20,
          fontWeight: '500',
          color: p ? theme.fg : theme.dim,
        }}>
        {p ? fmtTime(p) : '--:--'}
      </Text>
      <Text style={{fontSize: 10, letterSpacing: 1, color: theme.dim}}>
        {p ? fmtDate(p) : 'SET CLOCK'}
      </Text>
    </Pressable>
  );
});

// Press-and-hold: one step on touch-down, then a repeat every REPEAT_MS once the finger has stayed down
// for HOLD_MS. REPEAT_MAX bounds a hold whose release never arrives.
const HOLD_MS = 400;
const REPEAT_MS = 110;
const REPEAT_MAX = 300;

function RepeatStepper({dir, onStep, theme}) {
  const stepRef = useRef(onStep);
  stepRef.current = onStep;
  const timer = useRef(0);
  const stop = () => {
    clearTimeout(timer.current);
    timer.current = 0;
  };
  const start = () => {
    stop();
    let n = 0;
    const tick = delay => {
      stepRef.current(dir);
      if (++n < REPEAT_MAX) {
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
      {dir < 0 ? (
        <View style={{width: 16, height: 2, backgroundColor: theme.fg}} />
      ) : (
        <Text style={{fontSize: 20, color: theme.fg}}>+</Text>
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
        backgroundColor: on ? theme.trackBg : theme.surface,
      }}>
      <Text
        style={{fontSize: big ? 24 : 16, color: on ? theme.fg : theme.dim2}}>
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
  const off = useOffset();
  useMinuteTick(off);
  const [field, setField] = useState('hour');
  const p = clockParts(off === null ? UNSET_START : Date.now() + off);
  const step = dir => {
    const cur = getOffset();
    const now = Date.now();
    setOffset(
      stepField(cur === null ? UNSET_START : now + cur, field, dir) - now,
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
        {off === null ? 'CLOCK • NOT SET' : 'CLOCK'}
      </Text>
      <View style={{flexDirection: 'row', alignItems: 'center', gap: 8}}>
        <RepeatStepper dir={-1} onStep={step} theme={theme} />
        <View style={{flex: 1, alignItems: 'center', gap: 4}}>
          <View style={{flexDirection: 'row', alignItems: 'center', gap: 2}}>
            {f('hour', fmtHour(p), 44, true)}
            <Text style={{fontSize: 24, color: theme.dim}}>:</Text>
            {f('min', fmtMin(p), 44, true)}
            {f('ampm', fmtAmPm(p), 52, true)}
          </View>
          <View style={{flexDirection: 'row', alignItems: 'center', gap: 4}}>
            {f('month', MONTHS[p.m - 1], 52)}
            {f('day', String(p.d), 40)}
            {f('year', String(p.y), 60)}
          </View>
        </View>
        <RepeatStepper dir={1} onStep={step} theme={theme} />
      </View>
    </View>
  );
}
