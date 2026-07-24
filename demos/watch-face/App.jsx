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

import {useState, useEffect, useRef} from 'react';
import {View, Text, StyleSheet, useHostValue} from 'embedded-react';

// Watch face — a two-page swipe pager for the Waveshare RP2040-Touch-LCD-1.69 (240×280):
//   • page 0: a digital watch face (clock, weekday/date, battery, heart rate, live step counter)
//   • page 1: a bubble level (a dot that rolls with the board's tilt, from the QMI8658 accelerometer)
// Swipe right-to-left to drag the level page over; release, and it eases to settle as the active page.
//
// Built for Flow B (AOT) — compiles to C, no JS on the device. The pager rides only AOT-supported
// primitives: a dynamic `marginLeft` on a 480-wide track follows the finger during the drag and is
// eased toward the settled page by a ~30 fps interval; a transparent full-screen overlay captures the
// swipe (onTouchStart/Move/End; plain touch handlers get e.x/e.y, so the drag delta is derived from
// e.x and a start ref). The real sensor values come from the host via useHostValue (the step count and
// the level dot's x/y offset), fed by the IMU's accelerometer in main.c.
//
// There is no RTC, so time is a second counter seeded at 12:00:00 AM and advanced by a 1 Hz interval.

// ----------------------------------------------------------------------------------------------------
// Model
// ----------------------------------------------------------------------------------------------------
// Seconds since midnight. Starts at 0 (12:00 AM) — the factory default of a device whose clock has
// never been set. The weekday/date below (Wed, Jan 1 2020) is the matching unset default.
const START_TIME = 0;
const PAGE_W = 240; // page width == screen width

// ----------------------------------------------------------------------------------------------------
// Design tokens
// ----------------------------------------------------------------------------------------------------
const theme = {
  bg: '#0d0d0f',
  card: '#1b1b1e',
  cardBorder: '#2a2a2e',
  white: '#f5f5f7',
  muted: '#8a8a8e',
  colon: '#5a5a5e',
  amber: '#ff9f0a',
  green: '#34c759',
  red: '#ff453a',
  cyan: '#38bdf8',
  pillText: '#1b1300',
};

// ----------------------------------------------------------------------------------------------------
// Shared components
// ----------------------------------------------------------------------------------------------------

/** One bottom stat card: a colored dot + label, then a big value with a small unit. */
function StatCard({dot, label, value, unit}) {
  return (
    <View style={styles.card}>
      <View style={styles.cardHead}>
        <View style={[styles.dot, {backgroundColor: dot}]} />
        <Text style={styles.cardLabel}>{label}</Text>
      </View>
      <View style={styles.cardValueRow}>
        <Text style={styles.cardValue}>{value}</Text>
        <Text style={styles.cardUnit}>{unit}</Text>
      </View>
    </View>
  );
}

// ----------------------------------------------------------------------------------------------------
// Page 0 — the watch face
// ----------------------------------------------------------------------------------------------------
function WatchFace({t, hr, steps}) {
  return (
    <View style={styles.page}>
      <View style={styles.topBar}>
        <View>
          <Text style={styles.weekday}>WEDNESDAY</Text>
          <Text style={styles.date}>Jan 1, 2020</Text>
        </View>
        <Text style={styles.batteryPct}>84%</Text>
      </View>

      <View style={styles.middle}>
        <View style={styles.timeRow}>
          <Text style={styles.time}>{((Math.floor(t / 3600) + 11) % 12) + 1}</Text>
          <Text style={styles.timeColon}>:</Text>
          <Text style={styles.time}>
            {Math.floor((Math.floor(t / 60) % 60) / 10)}
            {(Math.floor(t / 60) % 60) % 10}
          </Text>
        </View>
        <View style={styles.metaRow}>
          <Text style={styles.ampm}>{Math.floor(t / 43200) % 2 === 0 ? 'AM' : 'PM'}</Text>
          <View style={styles.pill}>
            <Text style={styles.pillText}>
              {Math.floor((t % 60) / 10)}
              {(t % 60) % 10}
            </Text>
          </View>
        </View>
      </View>

      <View style={styles.cardsRow}>
        <StatCard dot={theme.red} label="HEART" value={hr} unit="BPM" />
        <StatCard dot={theme.green} label="STEPS" value={steps} unit="" />
      </View>
    </View>
  );
}

// ----------------------------------------------------------------------------------------------------
// Page 1 — a bubble level (dot rolls with the board's tilt; from the accelerometer's gravity vector)
// ----------------------------------------------------------------------------------------------------
function LevelPage({dotx, doty}) {
  return (
    <View style={styles.page}>
      <Text style={styles.levelTitle}>LEVEL</Text>
      <View style={styles.levelArea}>
        <View style={styles.target}>
          <View style={styles.crossH} />
          <View style={styles.crossV} />
          <View style={styles.centerRing} />
          {/* The dot: parked at the target center, then nudged by dynamic margins = the tilt offset. Turns
              green when it settles inside the center ring (the board is level). */}
          <View
            style={[
              styles.levelDot,
              {
                marginLeft: dotx,
                marginTop: doty,
                backgroundColor: Math.abs(dotx) < 8 && Math.abs(doty) < 8 ? theme.green : theme.amber,
              },
            ]}
          />
        </View>
      </View>
    </View>
  );
}

// ----------------------------------------------------------------------------------------------------
// App — the swipe pager
// ----------------------------------------------------------------------------------------------------
export function App() {
  const [t, setT] = useState(START_TIME); // seconds since midnight
  const [hr, setHr] = useState(66); // heart rate — a gentle random walk-in [61, 68]
  const steps = useHostValue(0); // real step count (IMU pedometer, via er_app_set_steps)
  const dotx = useHostValue(0); // level dot offset in px, from the accel tilt (via er_app_set_dotx/doty)
  const doty = useHostValue(0);

  const [page, setPage] = useState(0); // settled page (0 = watch, 1 = level)
  const [slide, setSlide] = useState(0.0); // px the track is shifted left (0..240); float for smooth ease
  const [dragging, setDragging] = useState(0); // 1 while a finger is down
  const startX = useRef(0); // absolute touch x at drag start (onTouchMove has e.x/e.y but not e.dx)

  useEffect(() => {
    const clockId = setInterval(() => {
      setT(v => {
        const next = v + 1; // 1 Hz clock
        setHr(p =>
          Math.max(
            61,
            Math.min(
              68,
              p +
                (Math.floor(
                  (next * 0.6180339887 - Math.floor(next * 0.6180339887)) * 3,
                ) -
                  1),
            ),
          ),
        );
        return next;
      });
    }, 1000);
    return () => clearInterval(clockId);
  }, []);

  // Settle animation: while not dragging, ease `slide` toward the settled page (snap when very close so
  // it comes to rest exactly and stops nudging). The finger owns `slide` during a drag.
  useEffect(() => {
    const settleId = setInterval(() => {
      setSlide(s =>
        dragging
          ? s
          : Math.abs(page * PAGE_W - s) < 0.5
            ? page * PAGE_W
            : s + (page * PAGE_W - s) * 0.35,
      );
    }, 33);
    return () => clearInterval(settleId);
  }, [page, dragging]);

  return (
    <View style={styles.root}>
      {/* The 480-wide track holds both pages; marginLeft slides it (0 → page 0, −240 → page 1). */}
      <View style={[styles.track, {marginLeft: -slide}]}>
        <WatchFace t={t} hr={hr} steps={steps} />
        <LevelPage dotx={dotx} doty={doty} />
      </View>

      {/* Transparent overlay captures the swipe (the pages have no handlers). Plain touch handlers get
          e.x/e.y but NOT a delta, so we record the start x in a ref and derive the drag from e.x. On
          release, we settle to whichever page is more than half revealed. */}
      <View
        style={styles.overlay}
        onTouchStart={e => {
          startX.current = e.x;
          setDragging(1);
        }}
        onTouchMove={e =>
          setSlide(Math.max(0, Math.min(PAGE_W, page * PAGE_W - (e.x - startX.current))))
        }
        onTouchEnd={() => {
          setDragging(0);
          // Easy latch: commit to the other page after dragging only ~25% toward it (directional, so it's
          // easy in both directions); otherwise the ease springs back to where you started.
          if (page === 0) {
            if (slide > 58) setPage(1);
          } else {
            if (slide < PAGE_W - 58) setPage(0);
          }
        }}
      />
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: theme.bg},
  track: {flexDirection: 'row', width: 480, height: 280},
  page: {width: PAGE_W, height: 280, paddingHorizontal: 16, paddingTop: 16, paddingBottom: 14},
  overlay: {position: 'absolute', left: 0, top: 0, right: 0, bottom: 0},

  // --- Watch face ---
  topBar: {flexDirection: 'row', justifyContent: 'space-between', alignItems: 'flex-start'},
  weekday: {color: theme.white, fontSize: 16, fontWeight: 'bold', letterSpacing: 1},
  date: {color: theme.amber, fontSize: 12, fontWeight: '500', marginTop: 2},
  batteryPct: {color: theme.green, fontSize: 14, fontWeight: '600'},

  middle: {flex: 1, alignItems: 'center', justifyContent: 'center'},
  timeRow: {flexDirection: 'row', alignItems: 'center'},
  time: {color: theme.white, fontSize: 48, fontWeight: 'bold', letterSpacing: 1},
  timeColon: {color: theme.colon, fontSize: 48, fontWeight: 'bold', marginHorizontal: 4},
  metaRow: {flexDirection: 'row', alignItems: 'center', gap: 10, marginTop: 8},
  ampm: {color: theme.muted, fontSize: 12, fontWeight: '600', letterSpacing: 1},
  pill: {
    backgroundColor: theme.amber,
    borderRadius: 10,
    paddingHorizontal: 9,
    paddingVertical: 2,
    minWidth: 30,
    alignItems: 'center',
  },
  pillText: {color: theme.pillText, fontSize: 12, fontWeight: 'bold'},

  cardsRow: {flexDirection: 'row', gap: 10},
  card: {
    flex: 1,
    backgroundColor: theme.card,
    borderWidth: 1,
    borderColor: theme.cardBorder,
    borderRadius: 14,
    paddingHorizontal: 12,
    paddingVertical: 10,
    gap: 6,
  },
  cardHead: {flexDirection: 'row', alignItems: 'center', gap: 6},
  dot: {width: 7, height: 7, borderRadius: 4},
  cardLabel: {color: theme.muted, fontSize: 10, fontWeight: '600', letterSpacing: 1},
  cardValueRow: {flexDirection: 'row', alignItems: 'flex-end', gap: 4},
  cardValue: {color: theme.white, fontSize: 24, fontWeight: 'bold'},
  cardUnit: {color: theme.muted, fontSize: 10, fontWeight: '500', marginBottom: 3},

  // --- Level page ---
  levelTitle: {color: theme.white, fontSize: 20, fontWeight: 'bold', letterSpacing: 2, marginTop: 6},
  levelArea: {flex: 1, alignItems: 'center', justifyContent: 'center'},
  target: {
    width: 168,
    height: 168,
    borderRadius: 84,
    borderWidth: 2,
    borderColor: theme.cardBorder,
    backgroundColor: theme.card,
  },
  crossH: {position: 'absolute', left: 16, right: 16, top: 81, height: 1, backgroundColor: theme.cardBorder},
  crossV: {position: 'absolute', top: 16, bottom: 16, left: 81, width: 1, backgroundColor: theme.cardBorder},
  centerRing: {
    position: 'absolute',
    left: 70,
    top: 70,
    width: 24,
    height: 24,
    borderRadius: 12,
    borderWidth: 1,
    borderColor: theme.muted,
  },
  levelDot: {position: 'absolute', left: 74, top: 74, width: 16, height: 16, borderRadius: 8},
});
