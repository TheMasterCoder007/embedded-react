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

import type {NativeMock} from 'embedded-react';
import type {Motion} from './motion';

const STEP_MS = 600;
const TILT_MS = 50;
const TILT_RADIUS = 40;
const TILT_PERIOD_MS = 6000;

// A walk, and a board that rocks in a slow circle.
const motion: NativeMock<Motion> = emit => {
  let steps = 0;
  setInterval(() => emit('steps', ++steps), STEP_MS);
  setInterval(() => {
    const angle =
      ((Date.now() % TILT_PERIOD_MS) / TILT_PERIOD_MS) * 2 * Math.PI;
    emit('tilt', {
      x: Math.round(Math.cos(angle) * TILT_RADIUS),
      y: Math.round(Math.sin(angle) * TILT_RADIUS),
    });
  }, TILT_MS);
  return {};
};

export default motion;
