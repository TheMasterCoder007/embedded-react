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

// The native modules the board examples offer apps. An app imports the ones it uses, and each board's
// native_modules.c builds its table from the headers generated from this file (include/).
//
// Every board has the chip. The thermometer and the motion sensor are optional: the S3 has no motion sensor, and the
// CYD has neither.

import {nativeModule} from 'embedded-react';
import type {Int, Int64} from 'embedded-react';

/** The board's chip. */
export interface Chip {
  /** The chip's name, such as "RP2040". */
  name(): string;
  /** Free heap, in bytes. */
  freeMemory(): Int;
  events: {
    /** Posted once a second, with the milliseconds since boot. */
    second(uptimeMs: Int64): void;
  };
}

/** The chip's own temperature sensor. */
export interface Thermometer {
  /** Starts a reading, which `reading` brings. */
  measure(): void;
  events: {
    reading(celsius: number): void;
  };
}

/** A level's dot, as its offset from the center in pixels. */
export interface Offset {
  x: Int;
  y: Int;
}

/** The accelerometer: a pedometer and a level's tilt. */
export interface Motion {
  events: {
    /** Steps counted since boot. */
    steps(count: Int): void;
    tilt(offset: Offset): void;
  };
}

export const Chip = nativeModule<Chip>('chip', 1);
export const Thermometer = nativeModule.optional<Thermometer>('thermometer', 1);
export const Motion = nativeModule.optional<Motion>('motion', 1);
