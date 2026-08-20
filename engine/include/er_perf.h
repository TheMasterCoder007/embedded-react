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

#ifndef EMBEDDED_REACT_ER_PERF_H
#define EMBEDDED_REACT_ER_PERF_H

/*
 * Per-frame instrumentation: where a frame's time went, and how much of each fixed-size resource pool
 * is in use.
 *
 * The problem this solves: a frame that occasionally takes 2 seconds is invisible to an FPS counter —
 * the average stays fine and the spike is gone before anyone can look. So each frame is split into the
 * four phases that can independently blow up, the split of the WORST frame seen so far is retained
 * (er_perf_get_worst), and the resource counters that usually explain such a spike are sampled
 * alongside it:
 *
 *   ER_PERF_PHASE_JS       host-marked: JS pump + React's commit into the scene graph
 *   ER_PERF_PHASE_LAYOUT   engine-marked: the flex solve + text measurement inside er_commit()
 *   ER_PERF_PHASE_RASTER   engine-marked: the rest of er_commit() — damage pre-pass, composite, blits
 *   ER_PERF_PHASE_PRESENT  host-marked: the backend flush / panel transfer
 *
 * RASTER gets one more level of detail, because "raster is slow" alone does not say WHICH of its very
 * different jobs is slow — the damage pre-pass walks the whole node pool whether one node changed or
 * none did, the composite cost scales with the damage area, and the backend blits scale with memory
 * bandwidth (on a PSRAM framebuffer they can dwarf the compositing that produced the pixels). So the
 * raster phase reports its own split (ERPerfRasterSub, disjoint by construction) plus the pixel count
 * actually pushed through the backend — blit_px against dirty_px is the write-amplification ratio.
 *
 * Anything in the frame outside those four (input polling, animation tick, the host's own work) lands
 * in `other_us`, so the four phases plus `other_us` always account for the whole frame.
 *
 * Usage — the host owns the frame boundary and its own two phases; the engine marks its own:
 *
 *     er_perf_set_clock(now_us);                   // once, at startup
 *     ...
 *     er_perf_frame_begin();
 *     er_perf_phase_begin(ER_PERF_PHASE_JS);
 *     er_runtime_pump();
 *     er_perf_phase_end(ER_PERF_PHASE_JS);
 *     er_commit();                                 // times LAYOUT + RASTER itself
 *     er_perf_phase_begin(ER_PERF_PHASE_PRESENT);
 *     er_display_present();
 *     er_perf_phase_end(ER_PERF_PHASE_PRESENT);
 *     er_perf_frame_end();
 *
 * er_perf_overlay_lines() formats the whole thing into short text lines ready to hand to
 * er_perf_overlay_draw(), so a host gets the panel without writing any snprintf of its own.
 *
 * Toggle with ER_PERF_STATS: 0 = compiled out (every entry point becomes a no-op, the getters report
 * false, and no state is linked in), 1 = enabled. It defaults to ER_PERF_OVERLAY, so turning the
 * overlay on turns the instrumentation on with it; set it explicitly to collect the numbers without
 * drawing a panel (e.g. to log them, or to ship them over a debug link).
 */

#include "perf_overlay.h" /* ER_PERF_OVERLAY — the default for ER_PERF_STATS below */

#include <stdbool.h>
#include <stdint.h>

#ifndef ER_PERF_STATS
#define ER_PERF_STATS ER_PERF_OVERLAY
#endif

/**
 * @brief Longest formatted overlay line, including the null terminator.
 *
 * Sized so no line can be truncated even at the widest values every field can hold (a 32-bit
 * microsecond total is 7 digits of milliseconds; the raster-split lines carry five such fields).
 * Typical lines are a third of this; the panel sizes itself to the text, so the headroom costs
 * nothing on screen.
 */
#define ER_PERF_LINE_MAX 64U

/** @brief Number of lines er_perf_overlay_lines() can produce. */
#define ER_PERF_OVERLAY_LINES 7

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief The four phases a frame is split into.
     *
     * JS and PRESENT are marked by the host (only it knows where its pump and its panel transfer
     * begin); LAYOUT and RASTER are marked by er_commit() itself.
     */
    typedef enum
    {
        ER_PERF_PHASE_JS = 0,  /**< JS pump + React commit into the scene graph (host-marked). */
        ER_PERF_PHASE_LAYOUT,  /**< Flex solve + text measurement inside er_commit(). */
        ER_PERF_PHASE_RASTER,  /**< Damage pre-pass + composite + backend blits inside er_commit(). */
        ER_PERF_PHASE_PRESENT, /**< Backend flush / panel transfer (host-marked). */
        ER_PERF_PHASE_COUNT
    } ERPerfPhase;

    /**
     * @brief The sub-steps the RASTER phase is split into (all engine-marked).
     *
     * The buckets are disjoint: BLIT is measured inside the composite passes but subtracted back out
     * of RENDER when the frame closes, so BLIT and RENDER do not double-count time. Any gap is
     * unattributed raster bookkeeping, kept out rather than guessed at.
     *
     * With parallel render workers BLIT is CPU time SUMMED across workers while RENDER is wall time,
     * so the subtraction can clamp RENDER to 0 on a heavily parallel frame — the split stays honest
     * about where the cycles went even when it can no longer mirror the wall clock.
     */
    typedef enum
    {
        ER_PERF_RASTER_PREPASS = 0, /**< Damage pre-pass: the full node-pool walk that decides what to
                                         repaint, plus the multi-buffer damage-debt fold and replay. Runs
                                         every commit, even when nothing changed — a constant floor that
                                         scales with ERUI_MAX_NODES, not with the damage. */
        ER_PERF_RASTER_RENDER,      /**< Compositing: the render_tree passes over the damage rects —
                                         tree walk, software rasterisation, scratch composites — minus
                                         the backend blits they emitted. */
        ER_PERF_RASTER_BLIT,        /**< Backend pixel emission: the fill/copy/blend(_fmt) callbacks
                                         (the actual framebuffer writes) plus banded band_begin/flush.
                                         Scales with blit_px and the framebuffer's write bandwidth. */
        ER_PERF_RASTER_SWEEP,       /**< Post-paint bookkeeping: the dirty-flag sweep over the node pool
                                         and the per-worker dirty-rect merge. Like PREPASS, an
                                         ERUI_MAX_NODES-proportional floor. */
        ER_PERF_RASTER_COUNT
    } ERPerfRasterSub;

    /**
     * @brief One frame's timing split and resource counters.
     *
     * All times are microseconds. The clock is only sampled between er_perf_frame_begin() and
     * er_perf_frame_end(), so a phase that did not run that frame (e.g. layout on a static frame)
     * reads exactly 0 — which is itself the useful signal.
     */
    typedef struct
    {
        uint32_t frame_us;                        /**< Whole frame: frame_begin -> frame_end. */
        uint32_t phase_us[ER_PERF_PHASE_COUNT];   /**< Per-phase totals (a phase may be entered more than once). */
        uint32_t other_us;                        /**< frame_us minus the four phases: input, tick, host work. */
        uint32_t raster_us[ER_PERF_RASTER_COUNT]; /**< RASTER's own split (see ERPerfRasterSub): disjoint; may exceed
                                                       the wall-time phase_us[ER_PERF_PHASE_RASTER] when BLIT is summed across workers. */
        uint32_t blit_px;                         /**< Pixels pushed through the backend blit callbacks this
                                                       frame (sum of each call's w*h, post-clip). Against
                                                       dirty_px this is the write amplification: overlapping
                                                       layers, erase-then-repaint, and multi-buffer debt
                                                       replay all raise it above the damage area. */
        int32_t dirty_x;                          /**< Bounding box of this frame's repaint rects (screen px). */
        int32_t dirty_y;                          /**< @see dirty_x */
        int32_t dirty_w;                          /**< Repaint bbox width; 0 when the frame painted nothing. */
        int32_t dirty_h;                          /**< Repaint bbox height; 0 when the frame painted nothing. */
        uint32_t dirty_px;                        /**< SUM of the disjoint repaint rects' areas (<= bbox area) — the
                                                       pixels actually repainted, which raster cost scales with. */
        uint16_t vector_slots_used;               /**< Vector (Svg) storage slots holding geometry. */
        uint16_t vector_slots_total;              /**< ERUI_MAX_VECTOR_NODES. */
        uint16_t image_slots_used;                /**< Image registry slots holding a registered asset. */
        uint16_t image_slots_total;               /**< ERUI_IMAGE_REGISTRY_MAX. */
        bool vector_slots_overflow;               /**< A vector node was denied a slot since the last er_reset(): it
                                                       holds no geometry and draws NOTHING. Sticky, not per-frame —
                                                       the store fails at commit time but the blank node persists. */
        uint32_t index;                           /**< Monotonic frame number since the last er_perf_reset(). */
    } ERPerfFrame;

    /**
     * @brief Installs the microsecond clock the instrumentation samples.
     *
     * The engine has no platform clock of its own (er_now_ms is a logical clock the host advances), so
     * timing is opt-in: pass something like esp_timer_get_time() or SDL_GetPerformanceCounter() scaled
     * to microseconds. It must be monotonic; a 32-bit wrap (~71 minutes) is handled correctly as long
     * as no single frame spans one.
     *
     * Without a clock the phase times all read 0 while the resource counters still update, so a host
     * that only wants slot usage need not provide one.
     *
     * @param[in] now_us  Monotonic microsecond clock, or NULL to disable timing.
     */
    void er_perf_set_clock(uint32_t (*now_us)(void));

    /**
     * @brief Opens a frame: resets the accumulators and stamps the frame's start.
     *
     * Every er_perf_phase_begin/end outside an open frame is ignored, so an app that never calls this
     * pays nothing but the (no-op) calls the engine makes from er_commit().
     */
    void er_perf_frame_begin(void);

    /**
     * @brief Starts timing @p phase within the open frame.
     *
     * Re-entering a phase is fine — the durations accumulate — which is how a banded backend's
     * per-strip render still reports one RASTER total.
     *
     * @param[in] phase  Phase to start.
     */
    void er_perf_phase_begin(ERPerfPhase phase);

    /**
     * @brief Stops timing @p phase and adds the elapsed time to its total for this frame.
     *
     * Ignored when the phase was never begun, so an early return between begin and end cannot corrupt
     * the accounting (the phase simply contributes nothing).
     *
     * @param[in] phase  Phase to stop.
     */
    void er_perf_phase_end(ERPerfPhase phase);

    /**
     * @brief Closes the frame: finalises the split, samples the resource counters, updates the peak.
     *
     * Any phase left open is closed here rather than dropped. The completed frame becomes
     * er_perf_get_last(), and replaces er_perf_get_worst() when its frame_us is the largest seen.
     */
    void er_perf_frame_end(void);

    /**
     * @brief Starts timing raster sub-step @p sub within the open frame (called by the engine).
     *
     * Same accumulate-on-re-entry semantics as er_perf_phase_begin(); meaningful only inside the
     * RASTER phase, which is where the engine marks them.
     *
     * @param[in] sub  Raster sub-step to start.
     */
    void er_perf_raster_begin(ERPerfRasterSub sub);

    /**
     * @brief Stops timing raster sub-step @p sub (called by the engine).
     *
     * Ignored when the sub-step was never begun, like er_perf_phase_end().
     *
     * @param[in] sub  Raster sub-step to stop.
     */
    void er_perf_raster_end(ERPerfRasterSub sub);

    /**
     * @brief Adds already-measured backend-blit time and pixels to this frame (called by the engine).
     *
     * The blit layer accumulates per-worker (its callbacks run on render-worker threads, where the
     * shared frame state must not be touched); the compositor collects the per-worker sums on the
     * commit thread when the composite passes finish and reports them here in one call.
     *
     * @param[in] us  Microseconds spent inside backend blit callbacks (summed across workers).
     * @param[in] px  Pixels pushed through those callbacks (sum of each call's w*h, post-clip).
     */
    void er_perf_note_blit(uint32_t us, uint32_t px);

    /**
     * @brief Samples the clock installed with er_perf_set_clock().
     *
     * For engine code that must accumulate durations somewhere other than the phase state (the
     * per-worker blit accounting). Reads 0 when no clock is installed or the instrumentation is
     * compiled out — durations formed from it then collapse to 0 rather than garbage.
     *
     * @return Current microsecond timestamp, or 0 without a clock.
     */
    uint32_t er_perf_now_us(void);

    /**
     * @brief Notes one repainted rect for this frame (called by the engine, not the host).
     *
     * The compositor calls this once per disjoint repaint rect — its own scissors, which also bound
     * the backend's flush. The frame accumulates: dirty_x/y/w/h grow to the union bounding box and
     * dirty_px sums the (disjoint) areas, so it reports the pixels actually repainted rather than the
     * span between far-apart changes. Empty rects and calls outside an open frame are ignored; a
     * frame with no notes reports all-zero.
     *
     * @param[in] x  Rect left edge in screen pixels.
     * @param[in] y  Rect top edge in screen pixels.
     * @param[in] w  Rect width in pixels.
     * @param[in] h  Rect height in pixels.
     */
    void er_perf_note_repaint(int x, int y, int w, int h);

    /**
     * @brief Retrieves the most recently completed frame.
     *
     * @param[out] out  Receives the frame (zeroed when there is none). May be NULL.
     *
     * @return true when a frame has completed since the last er_perf_reset().
     */
    bool er_perf_get_last(ERPerfFrame* out);

    /**
     * @brief Retrieves the longest frame seen since the last er_perf_reset().
     *
     * This is the point of the whole module: the spike is retained with its full split and counters, so
     * a 2-second frame that happened minutes ago can still be attributed to a subsystem.
     *
     * @param[out] out  Receives the frame (zeroed when there is none). May be NULL.
     *
     * @return true when at least one frame has completed since the last er_perf_reset().
     */
    bool er_perf_get_worst(ERPerfFrame* out);

    /**
     * @brief Clears the retained last/worst frames and the frame counter.
     *
     * Call after reading a peak (or when entering a new screen) so the next spike is not hidden behind
     * an old one — mount frames are legitimately slow and would otherwise stay the reported worst
     * forever.
     */
    void er_perf_reset(void);

    /**
     * @brief Formats the current metrics into short lines for er_perf_overlay_draw().
     *
     * The lines point into an internal static buffer and stay valid until the next call. They are
     * deliberately terse so the panel fits a 240 px-wide display; all times are milliseconds:
     *
     *     FRM 18.4 PK 2013.1     last frame / worst frame
     *     J6.2 L0.3 R9.1 P2.4    last frame: JS, layout, raster, present
     *     PK J1900 L12 R80 P9    the WORST frame's split — what to blame the spike on
     *     PKDRT 800x40 32k       the WORST frame's repainted region (pairs with PK above)
     *     VEC 3/8 IMG 5/32       vector + image slots in use, out of the compiled-in pool size
     *     RST P0.4 C7.2 B22.1 S0.9 W96k   last frame's raster split: pre-pass, composite, blit,
     *                            sweep (ERPerfRasterSub) + pixels pushed through the backend — the
     *                            line to watch during a steady drag, where the peak lines are stuck
     *                            on the mount frame
     *     PKR P2 C11 B16 S1 W96k the WORST frame's raster split + blit pixels (pairs with PK)
     *
     * The VEC field gains a `!FULL` marker (`VEC 8/8!FULL`) once a vector node has been denied a
     * storage slot, because a full pool and a pool that has already turned a node away look the same
     * on the counter alone — and only the second one explains missing shapes.
     *
     * @param[out] lines      Receives pointers to the formatted lines.
     * @param[in]  max_lines  Capacity of @p lines; at most ER_PERF_OVERLAY_LINES are written.
     *
     * @return Number of lines written (0 when instrumentation is compiled out).
     */
    int er_perf_overlay_lines(const char** lines, int max_lines);

#ifdef __cplusplus
}
#endif

#endif
