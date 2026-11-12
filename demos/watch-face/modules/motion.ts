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

import type {Int} from 'embedded-react';

/** The level's dot, as its offset from the center in pixels. */
export interface Offset {
  x: Int;
  y: Int;
}

/** The RP2040 board's accelerometer: a pedometer and the level's tilt. */
export interface Motion {
  events: {
    /** Steps counted since boot. */
    steps(count: Int): void;
    tilt(offset: Offset): void;
  };
}
