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
import type {Thermometer} from './index';

const READING_MS = 50;
const ROOM_CELSIUS = 31.5;

const thermometer: NativeMock<Thermometer> = emit => {
  let readings = 0;
  return {
    measure: () => {
      readings++;
      setTimeout(
        () => emit('reading', ROOM_CELSIUS + (readings % 3) * 0.5),
        READING_MS,
      );
    },
  };
};

export default thermometer;
