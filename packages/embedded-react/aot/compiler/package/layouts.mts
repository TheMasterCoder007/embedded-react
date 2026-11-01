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
 * Finds the layouts of an app that names no sizes: compile one, ask which size its check rejects, compile again, until
 * every size has a layout. The search is over ranges of sizes, which is what makes "every size" a proof instead of a
 * sample: a range is settled once one check passes for all of it, and a range every check fails for holds a size with
 * no layout. Anything else is split in two.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {checkOverSizes, runCheck} from './checks.mts';
import type {LayoutCheck, SizeRange} from './checks.mts';
import type {Screen} from '../options.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The largest panel side the search covers, as options.mts allows. */
export const MAX_PANEL_SIDE = 32767;

/** The most ranges one search looks at before it gives up: a few seconds at worst. */
const MAX_RANGES = 8000000;

/** A range holding several sizes starts its answer here: a small panel, which is what most compile best for. */
const PREFERRED: Screen = {width: 320, height: 240};

/** Panels tried first, so a layout is compiled at, and named by, a size someone has. */
const COMMON_PANELS: Screen[] = [
  {width: 240, height: 320},
  {width: 320, height: 240},
  {width: 240, height: 240},
  {width: 240, height: 280},
  {width: 320, height: 480},
  {width: 480, height: 320},
  {width: 480, height: 272},
  {width: 480, height: 800},
  {width: 800, height: 480},
  {width: 1024, height: 600},
  {width: 1280, height: 720},
];

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Finds sizes that have no layout yet, as the layouts grow.
 *
 * @class SizeSearch
 */
export class SizeSearch {
  /** The ranges not settled yet. Ranges already settled stay settled, whatever layouts come later. */
  private readonly open: SizeRange[] = [
    {
      minWidth: 1,
      maxWidth: MAX_PANEL_SIDE,
      minHeight: 1,
      maxHeight: MAX_PANEL_SIDE,
    },
  ];

  private looked = 0;

  /**
   * The next size none of the checks takes.
   *
   * @param checks  The checks of the layouts so far.
   *
   * @returns The size, null when every size has a layout, or 'gave-up' when the checks are too intricate to tell.
   */
  next(checks: LayoutCheck[]): Screen | null | 'gave-up' {
    const common = COMMON_PANELS.find(
      ({width, height}) =>
        !checks.some(check => runCheck(check, width, height)),
    );
    if (common) return common;

    for (;;) {
      const sizes = this.open.pop();
      if (!sizes) return null;
      if (++this.looked > MAX_RANGES) return 'gave-up';

      const verdicts = checks.map(check => checkOverSizes(check, sizes));
      if (verdicts.includes('pass')) continue;

      if (verdicts.every(verdict => verdict === 'fail')) {
        // The rest of this range may be left for later layouts to settle.
        const size = {
          width: Math.min(
            sizes.maxWidth,
            Math.max(sizes.minWidth, PREFERRED.width),
          ),
          height: Math.min(
            sizes.maxHeight,
            Math.max(sizes.minHeight, PREFERRED.height),
          ),
        };
        this.open.push(sizes);
        return size;
      }

      this.split(
        sizes,
        checks.filter((_, index) => verdicts[index] === 'unsure'),
      );
    }
  }

  /**
   * Splits a range across the side that decides it: the one whose pinning settles the most checks. Otherwise a
   * breakpoint on the width alone would split the height all along its edge too.
   *
   * @param sizes  The range.
   * @param unsure  The checks that can't tell yet.
   */
  private split(sizes: SizeRange, unsure: LayoutCheck[]): void {
    const midWidth = Math.floor((sizes.minWidth + sizes.maxWidth) / 2);
    const midHeight = Math.floor((sizes.minHeight + sizes.maxHeight) / 2);
    const wide = sizes.maxWidth > sizes.minWidth;
    const tall = sizes.maxHeight > sizes.minHeight;
    const stillUnsure = (range: SizeRange): number =>
      unsure.filter(check => checkOverSizes(check, range) === 'unsure').length;
    const pinnedWidth = stillUnsure({
      ...sizes,
      minWidth: midWidth,
      maxWidth: midWidth,
    });
    const pinnedHeight = stillUnsure({
      ...sizes,
      minHeight: midHeight,
      maxHeight: midHeight,
    });
    const byWidth =
      wide &&
      (!tall ||
        pinnedWidth < pinnedHeight ||
        (pinnedWidth === pinnedHeight &&
          sizes.maxWidth - sizes.minWidth >=
            sizes.maxHeight - sizes.minHeight));

    if (byWidth) {
      this.open.push(
        {...sizes, minWidth: midWidth + 1},
        {...sizes, maxWidth: midWidth},
      );
    } else {
      this.open.push(
        {...sizes, minHeight: midHeight + 1},
        {...sizes, maxHeight: midHeight},
      );
    }
  }
}
