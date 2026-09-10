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

// Calendar math on "local milliseconds": the local wall time, counted from 1970 as if it were UTC. The
// runtime has Date.now() but no Date objects, and the clock is set by hand in local time, so there is no
// time zone to handle — the offset the user sets absorbs it.

export const MINUTE = 60000;
const HOUR = 60 * MINUTE;
const DAY = 24 * HOUR;

export const MONTHS = [
  'JAN',
  'FEB',
  'MAR',
  'APR',
  'MAY',
  'JUN',
  'JUL',
  'AUG',
  'SEP',
  'OCT',
  'NOV',
  'DEC',
];
const WEEKDAYS = ['SUN', 'MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT'];

const isLeap = y => (y % 4 === 0 && y % 100 !== 0) || y % 400 === 0;
const daysInMonth = (y, m) =>
  m === 2
    ? isLeap(y)
      ? 29
      : 28
    : m === 4 || m === 6 || m === 9 || m === 11
      ? 30
      : 31;

// Days since 1970-01-01 to and from a Gregorian y/m/d (Howard Hinnant's civil-date algorithms).
function daysFromCivil(y, m, d) {
  const yy = m <= 2 ? y - 1 : y;
  const era = Math.floor(yy / 400);
  const yoe = yy - era * 400;
  const doy = Math.floor((153 * (m > 2 ? m - 3 : m + 9) + 2) / 5) + d - 1;
  const doe = yoe * 365 + Math.floor(yoe / 4) - Math.floor(yoe / 100) + doy;
  return era * 146097 + doe - 719468;
}

function civilFromDays(days) {
  const z = days + 719468;
  const era = Math.floor(z / 146097);
  const doe = z - era * 146097;
  const yoe = Math.floor(
    (doe -
      Math.floor(doe / 1460) +
      Math.floor(doe / 36524) -
      Math.floor(doe / 146096)) /
      365,
  );
  const doy = doe - (365 * yoe + Math.floor(yoe / 4) - Math.floor(yoe / 100));
  const mp = Math.floor((5 * doy + 2) / 153);
  const m = mp < 10 ? mp + 3 : mp - 9;
  return {
    y: yoe + era * 400 + (m <= 2 ? 1 : 0),
    m,
    d: doy - Math.floor((153 * mp + 2) / 5) + 1,
  };
}

/** Local ms → {y, m (1-12), d, wd (0 = Sunday), hh (0-23), mm}. */
export function clockParts(ms) {
  const days = Math.floor(ms / DAY);
  const mins = Math.floor((ms - days * DAY) / MINUTE);
  return {
    ...civilFromDays(days),
    wd: (((days + 4) % 7) + 7) % 7, // 1970-01-01 was a Thursday
    hh: Math.floor(mins / 60),
    mm: mins % 60,
  };
}

const toMs = p =>
  daysFromCivil(p.y, p.m, p.d) * DAY + p.hh * HOUR + p.mm * MINUTE;

/** Where a clock that has never been set starts when you open the setter. */
export const UNSET_START = toMs({y: 2026, m: 1, d: 1, hh: 12, mm: 0});

/**
 * Moves one field of a local time by `dir` (±1), wrapping within its own range the way a clock's set mode
 * does — minutes roll over without touching the hour. The result lands on :00 seconds.
 */
export function stepField(ms, field, dir) {
  const p = clockParts(ms);
  const wrap = (v, lo, n) => ((((v - lo + dir) % n) + n) % n) + lo;
  if (field === 'hour') p.hh = wrap(p.hh, 0, 24);
  else if (field === 'min') p.mm = wrap(p.mm, 0, 60);
  else if (field === 'ampm') p.hh = (p.hh + 12) % 24;
  else if (field === 'month') p.m = wrap(p.m, 1, 12);
  else if (field === 'day') p.d = wrap(p.d, 1, daysInMonth(p.y, p.m));
  else if (field === 'year') p.y = Math.min(2099, Math.max(2000, p.y + dir));
  p.d = Math.min(p.d, daysInMonth(p.y, p.m)); // Jan 31 → Feb lands on the last day of February
  return toMs(p);
}

export const fmtHour = p => String(p.hh % 12 || 12);
export const fmtMin = p => (p.mm < 10 ? '0' : '') + p.mm;
export const fmtAmPm = p => (p.hh < 12 ? 'AM' : 'PM');
export const fmtTime = p => fmtHour(p) + ':' + fmtMin(p) + ' ' + fmtAmPm(p);
export const fmtDate = p => WEEKDAYS[p.wd] + ', ' + MONTHS[p.m - 1] + ' ' + p.d;
