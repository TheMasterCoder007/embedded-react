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

/*
 * embedded-react version.
 *
 * The repo ships LOCKSTEP: the engine, the `embedded-react` npm package, the
 * ESP-IDF component, and the PlatformIO library all share ONE version. The SINGLE SOURCE OF TRUTH is the
 * repo-root `VERSION` file; this header is regenerated from it by `tools/sync-version.mjs`
 * (`npm run version:sync`). DO NOT EDIT BY HAND — bump VERSION and re-sync instead.
 *
 * The Flow B AOT compiler stamps the version it targeted into the generated app and `_Static_assert`s it
 * against these macros, so an app built against a different engine fails at COMPILE time, not on-device.
 */

#ifndef ER_VERSION_H
#define ER_VERSION_H

#define ER_VERSION_MAJOR 0
#define ER_VERSION_MINOR 5
#define ER_VERSION_PATCH 3
#define ER_VERSION_STRING "0.5.3"

/** @brief Packed integer version for easy comparisons: major*10000 + minor*100 + patch. */
#define ER_VERSION_NUMBER (ER_VERSION_MAJOR * 10000 + ER_VERSION_MINOR * 100 + ER_VERSION_PATCH)

#endif /* ER_VERSION_H */
