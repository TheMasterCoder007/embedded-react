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

// Platform — minimal RN analog. OS is "embedded"; select() picks the embedded entry (falling back
// to default) so apps can write Platform.select({ embedded: ..., default: ... }).
export const Platform = {
  OS: 'embedded',
  select(specifics) {
    if (specifics == null) return undefined;
    if ('embedded' in specifics) return specifics.embedded;
    return specifics.default;
  },
};
