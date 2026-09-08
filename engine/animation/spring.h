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

#ifndef EMBEDDED_REACT_SPRING_H
#define EMBEDDED_REACT_SPRING_H

#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - The fixed-step spring integrator, shared by the two animation systems.
 *
 * animation.c (Animated.spring / Animated.decay) and layout_anim.c (LayoutAnimation) both integrate a spring
 * toward the normalised target 1.0, and a user sees them side by side — a value-driven fade running against a
 * layout transition. The timestep and the step budget live here because they are coupled to each other and to
 * both callers: the budget only means "200 ms of physics per frame" BECAUSE the step is 1 ms, so changing one
 * in one file silently gives the other module a different wall-clock cap and desyncs two springs that were
 * configured identically.
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Integration timestep in SECONDS — one step per elapsed millisecond of the frame. */
#define ER_SPRING_DT_SECONDS 0.001f

/**
 * @brief Maximum integration steps per tick, bounding the cost of a long frame.
 *
 * At one step per millisecond this is 200 ms of physics. An animation caught by the cap coasts for a few more ticks
 * rather than jumping to where it would have landed. Also bounds animation.c's decay integrator, which steps
 * on the same one-per-millisecond schedule.
 */
#define ER_SPRING_MAX_STEPS 200u

/**
 * @brief Integrates one ER_SPRING_DT_SECONDS step of spring physics toward the normalised target 1.0.
 *
 * @param[in,out] pos        Current normalised spring position.
 * @param[in,out] vel        Current normalised velocity.
 * @param[in]     stiffness  Spring constant k.
 * @param[in]     damping    Damping coefficient c.
 * @param[in]     mass       Spring mass m.
 */
static inline void er_spring_step(float* pos, float* vel, float stiffness, float damping, float mass)
{
    const float disp = *pos - 1.0f;
    const float accel = (-stiffness * disp - damping * (*vel)) / mass;
    *vel += accel * ER_SPRING_DT_SECONDS;
    *pos += (*vel) * ER_SPRING_DT_SECONDS;
}

#endif
