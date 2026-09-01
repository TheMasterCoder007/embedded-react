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

// The same two-page pager as App.jsx, with the swipe driven by <PanResponder> instead of hand-rolled
// touch math. Side-by-side, this is what the module is for: App.jsx records the touch-down x in a ref
// and subtracts it on every move; here the gesture arrives with the travel (g.dx) and the speed (g.vx)
// already worked out.
//
// FLOW A ONLY. PanResponder is a JS module, so this file cannot be compiled by the AOT — the spread
// `{...pan.panHandlers}` fails with "AOT: a spread {...} on <View> is not supported". App.jsx stays
// hand-rolled precisely so it still compiles to C for the RP2040, which has no room for QuickJS. Build
// this one with `npm run build:pan` (a .erpkg for a PSRAM-class board) or run it in the simulator.
//
// The pages, styles, and page geometry are imported from App.jsx — only the pager shell differs.
import {useState, useEffect, useRef} from 'react';
import {View, PanResponder, useHostValue} from 'embedded-react';
import {WatchFace, LevelPage, styles, PAGE_W, START_TIME} from './App.jsx';

/** Past this much of a page dragged, release commits to the neighbour (directional, so both ways are easy). */
const LATCH_PX = 58;
/** …or past this speed, in px/ms, however short the drag was. A flick, rather than a haul. */
const LATCH_VELOCITY = 0.4;

export function App() {
  const [t, setT] = useState(START_TIME); // seconds since midnight
  const [hr, setHr] = useState(66); // heart rate — a gentle random walk in [61, 68]
  const steps = useHostValue(0); // real step count (IMU pedometer, via er_app_set_steps)
  const dotx = useHostValue(0); // level dot offset in px, from the accel tilt (via er_app_set_dotx/doty)
  const doty = useHostValue(0);

  const [page, setPage] = useState(0); // settled page (0 = watch, 1 = level)
  const [slide, setSlide] = useState(0.0); // px the track is shifted left (0..240); float for smooth ease
  const [dragging, setDragging] = useState(0); // 1 while a finger is down

  // PanResponder.create runs ONCE (it owns the gesture state, so re-creating it per render would throw
  // the drag away). Its callbacks therefore close over the FIRST render's `page` — the standard RN
  // discipline applies: mirror into a ref anything the handlers read.
  const pageRef = useRef(0);
  useEffect(() => {
    pageRef.current = page;
  }, [page]);

  // Where the track sat when the finger landed. Everything the gesture does is that plus g.dx, so the
  // handlers never need to read `slide` back out of state.
  const slideAtGrant = useRef(0);
  const clamp = px => Math.max(0, Math.min(PAGE_W, px));

  const pan = useRef(
    PanResponder.create({
      onStartShouldSetPanResponder: () => true,
      onPanResponderGrant: () => {
        slideAtGrant.current = pageRef.current * PAGE_W;
        setDragging(1);
      },
      // Drag right-to-left (negative dx) to pull the next page over.
      onPanResponderMove: (e, g) =>
        setSlide(clamp(slideAtGrant.current - g.dx)),
      onPanResponderRelease: (e, g) => {
        setDragging(0);
        const settled = clamp(slideAtGrant.current - g.dx);
        // Commit on either a long enough drag OR a fast enough flick — the flick is what the raw
        // touch handlers in App.jsx can't do, since a plain onTouchMove carries no speed.
        const flick = Math.abs(g.vx) >= LATCH_VELOCITY;
        if (pageRef.current === 0) {
          if (settled > LATCH_PX || (flick && g.vx < 0)) setPage(1);
        } else {
          if (settled < PAGE_W - LATCH_PX || (flick && g.vx > 0)) setPage(0);
        }
      },
      // The engine can abandon the sequence (a cancelled touch from the panel driver, or a fresh
      // touch-down on a finger that never lifted). Without this the pager would stay stuck mid-drag
      // with `dragging` latched at 1 and the settle interval frozen — the raw-handler version in
      // App.jsx has no onTouchCancel and does exactly that.
      onPanResponderTerminate: () => setDragging(0),
    }),
  ).current;

  useEffect(() => {
    const clockId = setInterval(() => {
      setT(v => v + 1);
      setHr(p =>
        Math.max(
          61,
          Math.min(
            68,
            p +
              (Math.floor(
                (p * 0.6180339887 - Math.floor(p * 0.6180339887)) * 3,
              ) -
                1),
          ),
        ),
      );
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

      {/* Transparent overlay captures the swipe (the pages have no handlers). */}
      <View style={styles.overlay} {...pan.panHandlers} />
    </View>
  );
}
