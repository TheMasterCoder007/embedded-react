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
import type {JSX} from 'react';
import {
  View,
  Text,
  StyleSheet,
  PanResponder,
  useNativeEvent,
} from 'embedded-react';
import {Chip, Thermometer, Motion} from 'example-device';

// Watch face — a two-page swipe pager, sized for the Waveshare RP2040-Touch-LCD-1.69 (240×280):
//   • page 0: a digital watch face (clock, weekday/date, the chip, its temperature, a live step counter)
//   • page 1: a bubble level (a dot that rolls with the board's tilt)
// Swipe right-to-left to drag the level page over; release, and it eases to settle as the active page.
//
// It runs in both flows. The pager rides only AOT-supported primitives: a dynamic `marginLeft` on a two-page
// track follows the finger during the drag and is eased toward the settled page by a ~30 fps interval; a
// transparent full-screen overlay captures the swipe with a `PanResponder`, which the AOT lowers onto the engine's
// own C gesture-responder system (so the gesture arrives with its travel and speed already worked out).
//
// What the board knows comes from native modules, imported from the boards' device package (examples/device). Each
// board's firmware provides them in C, and the simulator runs on the package's mocks. Every board has the chip,
// whose once-a-second event drives the clock. The thermometer and the motion sensor are optional: a board without
// one shows "--" in its place.
//
// There is no RTC, so the clock shows the time since boot, from 12:00:00 AM.

// ----------------------------------------------------------------------------------------------------
// Model
// ----------------------------------------------------------------------------------------------------
const PAGE_WIDTH = screen.width;
const PAGE_HEIGHT = screen.height;

/** Past this much of a page dragged, release commits to the neighbor (directional, so both ways are easy). */
const LATCH_PX = 58;
/** …or past this speed, in px/ms, however short the drag was. A flick, rather than a haul. */
const LATCH_VELOCITY = 0.4;

const SECONDS_PER_MINUTE = 60;
const SECONDS_PER_HOUR = 3600;
const SECONDS_PER_HALF_DAY = 43200;
const MS_PER_SECOND = 1000;
const MEASURE_EVERY_SECONDS = 5;
const BYTES_PER_KILOBYTE = 1024;
const SETTLE_TICK_MS = 33; // ~30 fps
const SETTLE_EASE = 0.35; // the share of the remaining distance each settle tick covers
const SETTLE_SNAP_PX = 0.5;
const LEVEL_PX = 8;

// ----------------------------------------------------------------------------------------------------
// Design tokens
// ----------------------------------------------------------------------------------------------------
const theme = {
  background: '#0d0d0f',
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

interface StatCardProps {
  dot: string;
  label: string;
  value: string;
}

/** One bottom stat card: a colored dot + label, then a big value. */
function StatCard({dot, label, value}: StatCardProps): JSX.Element {
  return (
    <View style={styles.card}>
      <View style={styles.cardHeader}>
        <View style={[styles.dot, {backgroundColor: dot}]} />
        <Text style={styles.cardLabel}>{label}</Text>
      </View>
      <Text style={styles.cardValue}>{value}</Text>
    </View>
  );
}

// ----------------------------------------------------------------------------------------------------
// Page 0 — the watch face
// ----------------------------------------------------------------------------------------------------
interface WatchFaceProps {
  seconds: number;
  chipName: string;
  freeKilobytes: number;
  temperature: string;
  steps: string;
}

function WatchFace({
  seconds,
  chipName,
  freeKilobytes,
  temperature,
  steps,
}: WatchFaceProps): JSX.Element {
  return (
    <View style={styles.page}>
      <View style={styles.topBar}>
        <View>
          <Text style={styles.weekday}>WEDNESDAY</Text>
          <Text style={styles.date}>Jan 1, 2020</Text>
        </View>
        <View style={styles.chip}>
          <Text style={styles.chipName}>{chipName}</Text>
          <Text style={styles.chipMemory}>{freeKilobytes} KB free</Text>
        </View>
      </View>

      <View style={styles.middle}>
        <View style={styles.timeRow}>
          <Text style={styles.time}>
            {((Math.floor(seconds / SECONDS_PER_HOUR) + 11) % 12) + 1}
          </Text>
          <Text style={styles.timeColon}>:</Text>
          <Text style={styles.time}>
            {Math.floor(
              (Math.floor(seconds / SECONDS_PER_MINUTE) % SECONDS_PER_MINUTE) /
                10,
            )}
            {(Math.floor(seconds / SECONDS_PER_MINUTE) % SECONDS_PER_MINUTE) %
              10}
          </Text>
        </View>
        <View style={styles.metaRow}>
          <Text style={styles.ampm}>
            {Math.floor(seconds / SECONDS_PER_HALF_DAY) % 2 === 0 ? 'AM' : 'PM'}
          </Text>
          <View style={styles.pill}>
            <Text style={styles.pillText}>
              {Math.floor((seconds % SECONDS_PER_MINUTE) / 10)}
              {(seconds % SECONDS_PER_MINUTE) % 10}
            </Text>
          </View>
        </View>
      </View>

      <View style={styles.cardsRow}>
        <StatCard dot={theme.red} label="TEMP °C" value={temperature} />
        <StatCard dot={theme.green} label="STEPS" value={steps} />
      </View>
    </View>
  );
}

// ----------------------------------------------------------------------------------------------------
// Page 1 — a bubble level (the dot rolls with the board's tilt)
// ----------------------------------------------------------------------------------------------------
interface LevelPageProps {
  dotX: number;
  dotY: number;
}

function LevelPage({dotX, dotY}: LevelPageProps): JSX.Element {
  return (
    <View style={styles.page}>
      <Text style={styles.levelTitle}>LEVEL</Text>
      <View style={styles.levelArea}>
        {Motion.available ? (
          <View style={styles.target}>
            <View style={styles.crossHorizontal} />
            <View style={styles.crossVertical} />
            <View style={styles.centerRing} />
            {/* The dot: parked at the target center, then nudged by dynamic margins = the tilt offset. Turns
                green when it settles inside the center ring (the board is level). */}
            <View
              style={[
                styles.levelDot,
                {
                  marginLeft: dotX,
                  marginTop: dotY,
                  backgroundColor:
                    Math.abs(dotX) < LEVEL_PX && Math.abs(dotY) < LEVEL_PX
                      ? theme.green
                      : theme.amber,
                },
              ]}
            />
          </View>
        ) : (
          <Text style={styles.levelMissing}>
            No motion sensor on this board
          </Text>
        )}
      </View>
    </View>
  );
}

// ----------------------------------------------------------------------------------------------------
// App — the swipe pager
// ----------------------------------------------------------------------------------------------------
export function App(): JSX.Element {
  const [seconds, setSeconds] = useState(0); // seconds since boot
  const [chipName, setChipName] = useState('');
  const [freeKilobytes, setFreeKilobytes] = useState(0);
  const [temperature, setTemperature] = useState('--');
  const [steps, setSteps] = useState('--');
  const [dotX, setDotX] = useState(0); // the level dot's offset in px
  const [dotY, setDotY] = useState(0);

  const [page, setPage] = useState(0); // settled page (0 = watch, 1 = level)
  const [slide, setSlide] = useState(0.0); // px the track is shifted left (0..PAGE_WIDTH); float for smooth ease
  const [dragging, setDragging] = useState(0); // 1 while a finger is down

  // A call is quick: the name and free memory come straight back. A reading takes a while, so measure() starts
  // one and the thermometer posts `reading` when it is done.
  useEffect(() => {
    setChipName(Chip.name());
    setFreeKilobytes(Math.floor(Chip.freeMemory() / BYTES_PER_KILOBYTE));
    if (Thermometer.available) Thermometer.measure();
  }, []);

  useNativeEvent(Chip, 'second', uptimeMs => {
    const uptime = Math.floor(uptimeMs / MS_PER_SECOND);
    setSeconds(uptime);
    if (uptime % MEASURE_EVERY_SECONDS === 0) {
      setFreeKilobytes(Math.floor(Chip.freeMemory() / BYTES_PER_KILOBYTE));
      if (Thermometer.available) Thermometer.measure();
    }
  });
  useNativeEvent(Thermometer, 'reading', celsius =>
    setTemperature(`${Math.round(celsius)}`),
  );
  useNativeEvent(Motion, 'steps', count => setSteps(`${count}`));
  useNativeEvent(Motion, 'tilt', ({x, y}) => {
    setDotX(x);
    setDotY(y);
  });

  // PanResponder.create runs ONCE (it owns the live gesture, so re-creating it per render would throw
  // the drag away), so its callbacks close over the FIRST render's `page`. The standard RN discipline
  // applies: mirror into a ref anything the handlers read. (Flow B reads state live and needs no ref,
  // but one source has to be right in both flows.)
  const pageRef = useRef(0);
  useEffect(() => {
    pageRef.current = page;
  }, [page]);

  // Where the track sat when the finger landed. Everything the gesture does is that plus gestureState.dx, so the
  // handlers never need to read `slide` back out of state.
  const slideAtGrant = useRef(0);

  const pan = useRef(
    PanResponder.create({
      onStartShouldSetPanResponder: () => true,
      onPanResponderGrant: () => {
        slideAtGrant.current = pageRef.current * PAGE_WIDTH;
        setDragging(1);
      },
      // Drag right-to-left (negative dx) to pull the next page over.
      onPanResponderMove: (event, gestureState) =>
        setSlide(
          Math.max(
            0,
            Math.min(PAGE_WIDTH, slideAtGrant.current - gestureState.dx),
          ),
        ),
      onPanResponderRelease: (event, gestureState) => {
        setDragging(0);
        const settled = Math.max(
          0,
          Math.min(PAGE_WIDTH, slideAtGrant.current - gestureState.dx),
        );
        // Commit on a long enough drag (directional, so it's easy both ways) OR on a fast enough flick, however
        // short. Otherwise, the ease springs back to where you started.
        if (pageRef.current === 0) {
          if (settled > LATCH_PX || gestureState.vx <= -LATCH_VELOCITY)
            setPage(1);
        } else {
          if (
            settled < PAGE_WIDTH - LATCH_PX ||
            gestureState.vx >= LATCH_VELOCITY
          )
            setPage(0);
        }
      },
      // The engine can abandon the sequence (a canceled touch from the panel driver, or a fresh
      // touch-down on a finger that never lifted). Without this the pager would stay stuck mid-drag,
      // `dragging` latched at 1 and the settle interval frozen.
      onPanResponderTerminate: () => setDragging(0),
    }),
  ).current;

  // Settle animation: while not dragging, ease `slide` toward the settled page (snap when very close so
  // it comes to rest exactly and stops nudging). The finger owns `slide` during a drag.
  useEffect(() => {
    const settleId = setInterval(() => {
      setSlide(current =>
        dragging
          ? current
          : Math.abs(page * PAGE_WIDTH - current) < SETTLE_SNAP_PX
            ? page * PAGE_WIDTH
            : current + (page * PAGE_WIDTH - current) * SETTLE_EASE,
      );
    }, SETTLE_TICK_MS);
    return () => clearInterval(settleId);
  }, [page, dragging]);

  return (
    <View style={styles.root}>
      {/* The track holds both pages side by side; marginLeft slides it (0 → page 0, −PAGE_WIDTH → page 1). */}
      <View style={[styles.track, {marginLeft: -slide}]}>
        <WatchFace
          seconds={seconds}
          chipName={chipName}
          freeKilobytes={freeKilobytes}
          temperature={temperature}
          steps={steps}
        />
        <LevelPage dotX={dotX} dotY={dotY} />
      </View>

      {/* Transparent overlay captures the swipe (the pages have no handlers). */}
      <View style={styles.overlay} {...pan.panHandlers} />
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: theme.background},
  track: {flexDirection: 'row', width: 2 * PAGE_WIDTH, height: PAGE_HEIGHT},
  page: {
    width: PAGE_WIDTH,
    height: PAGE_HEIGHT,
    paddingHorizontal: 16,
    paddingTop: 16,
    paddingBottom: 14,
  },
  overlay: {position: 'absolute', left: 0, top: 0, right: 0, bottom: 0},

  // --- Watch face ---
  topBar: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'flex-start',
  },
  weekday: {
    color: theme.white,
    fontSize: 16,
    fontWeight: 'bold',
    letterSpacing: 1,
  },
  date: {color: theme.amber, fontSize: 12, fontWeight: '500', marginTop: 2},
  chip: {alignItems: 'flex-end'},
  chipName: {color: theme.green, fontSize: 16, fontWeight: '600'},
  chipMemory: {
    color: theme.muted,
    fontSize: 12,
    fontWeight: '500',
    marginTop: 2,
  },

  middle: {flex: 1, alignItems: 'center', justifyContent: 'center'},
  timeRow: {flexDirection: 'row', alignItems: 'center'},
  time: {
    color: theme.white,
    fontSize: 48,
    fontWeight: 'bold',
    letterSpacing: 1,
  },
  timeColon: {
    color: theme.colon,
    fontSize: 48,
    fontWeight: 'bold',
    marginHorizontal: 4,
  },
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
  cardHeader: {flexDirection: 'row', alignItems: 'center', gap: 6},
  dot: {width: 7, height: 7, borderRadius: 4},
  cardLabel: {
    color: theme.muted,
    fontSize: 10,
    fontWeight: '600',
    letterSpacing: 1,
  },
  cardValue: {color: theme.white, fontSize: 24, fontWeight: 'bold'},

  // --- Level page ---
  levelTitle: {
    color: theme.white,
    fontSize: 20,
    fontWeight: 'bold',
    letterSpacing: 2,
    marginTop: 6,
  },
  levelArea: {flex: 1, alignItems: 'center', justifyContent: 'center'},
  target: {
    width: 168,
    height: 168,
    borderRadius: 84,
    borderWidth: 2,
    borderColor: theme.cardBorder,
    backgroundColor: theme.card,
  },
  crossHorizontal: {
    position: 'absolute',
    left: 16,
    right: 16,
    top: 81,
    height: 1,
    backgroundColor: theme.cardBorder,
  },
  crossVertical: {
    position: 'absolute',
    top: 16,
    bottom: 16,
    left: 81,
    width: 1,
    backgroundColor: theme.cardBorder,
  },
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
  levelMissing: {color: theme.muted, fontSize: 12, fontWeight: '500'},
  levelDot: {
    position: 'absolute',
    left: 74,
    top: 74,
    width: 16,
    height: 16,
    borderRadius: 8,
  },
});
