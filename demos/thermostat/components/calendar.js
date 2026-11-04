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
// runtime has Date.now() but no Date objects, so there is no time zone to handle here: the clock's offset
// onto Date.now() absorbs it, whether it was set by hand or comes from the host's network time.

export const MINUTE = 60000;
const HOUR = 60 * MINUTE;
const DAY = 24 * HOUR;

// The civil-date algorithms count in 400-year eras, from 0000-03-01.
const YEARS_PER_ERA = 400;
const DAYS_PER_ERA = 146097;
const EPOCH_DAYS = 719468; // 0000-03-01 to 1970-01-01
const FIRST_YEAR = 2000;
const LAST_YEAR = 2099;

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

const isLeap = year => (year % 4 === 0 && year % 100 !== 0) || year % 400 === 0;
const daysInMonth = (year, month) =>
  month === 2
    ? isLeap(year)
      ? 29
      : 28
    : month === 4 || month === 6 || month === 9 || month === 11
      ? 30
      : 31;

// Days since 1970-01-01 to and from a Gregorian y/m/d (Howard Hinnant's civil-date algorithms).
function daysFromCivil(year, month, day) {
  const shiftedYear = month <= 2 ? year - 1 : year;
  const era = Math.floor(shiftedYear / YEARS_PER_ERA);
  const yearOfEra = shiftedYear - era * YEARS_PER_ERA;
  const dayOfYear =
    Math.floor((153 * (month > 2 ? month - 3 : month + 9) + 2) / 5) + day - 1;
  const dayOfEra =
    yearOfEra * 365 +
    Math.floor(yearOfEra / 4) -
    Math.floor(yearOfEra / 100) +
    dayOfYear;
  return era * DAYS_PER_ERA + dayOfEra - EPOCH_DAYS;
}

function civilFromDays(days) {
  const shiftedDays = days + EPOCH_DAYS;
  const era = Math.floor(shiftedDays / DAYS_PER_ERA);
  const dayOfEra = shiftedDays - era * DAYS_PER_ERA;
  const yearOfEra = Math.floor(
    (dayOfEra -
      Math.floor(dayOfEra / 1460) +
      Math.floor(dayOfEra / 36524) -
      Math.floor(dayOfEra / 146096)) /
      365,
  );
  const dayOfYear =
    dayOfEra -
    (365 * yearOfEra + Math.floor(yearOfEra / 4) - Math.floor(yearOfEra / 100));
  const shiftedMonth = Math.floor((5 * dayOfYear + 2) / 153);
  const month = shiftedMonth < 10 ? shiftedMonth + 3 : shiftedMonth - 9;
  return {
    year: yearOfEra + era * YEARS_PER_ERA + (month <= 2 ? 1 : 0),
    month,
    day: dayOfYear - Math.floor((153 * shiftedMonth + 2) / 5) + 1,
  };
}

/** Local ms → {year, month (1-12), day, weekday (0 = Sunday), hour (0-23), minute}. */
export function clockParts(localMs) {
  const days = Math.floor(localMs / DAY);
  const minutes = Math.floor((localMs - days * DAY) / MINUTE);
  return {
    ...civilFromDays(days),
    weekday: (((days + 4) % 7) + 7) % 7, // 1970-01-01 was a Thursday
    hour: Math.floor(minutes / 60),
    minute: minutes % 60,
  };
}

const toMs = parts =>
  daysFromCivil(parts.year, parts.month, parts.day) * DAY +
  parts.hour * HOUR +
  parts.minute * MINUTE;

/** Where a clock that has never been set starts when you open the setter. */
export const UNSET_START = toMs({
  year: 2026,
  month: 1,
  day: 1,
  hour: 12,
  minute: 0,
});

/**
 * Moves one field of a local time by `dir` (±1), wrapping within its own range the way a clock's set mode
 * does — minutes roll over without touching the hour. The result lands on :00 seconds.
 */
export function stepField(localMs, field, direction) {
  const parts = clockParts(localMs);
  const wrap = (value, first, count) =>
    ((((value - first + direction) % count) + count) % count) + first;
  if (field === 'hour') parts.hour = wrap(parts.hour, 0, 24);
  else if (field === 'min') parts.minute = wrap(parts.minute, 0, 60);
  else if (field === 'ampm') parts.hour = (parts.hour + 12) % 24;
  else if (field === 'month') parts.month = wrap(parts.month, 1, 12);
  else if (field === 'day')
    parts.day = wrap(parts.day, 1, daysInMonth(parts.year, parts.month));
  else if (field === 'year')
    parts.year = Math.min(
      LAST_YEAR,
      Math.max(FIRST_YEAR, parts.year + direction),
    );
  parts.day = Math.min(parts.day, daysInMonth(parts.year, parts.month)); // Jan 31 → Feb lands on the last day of February
  return toMs(parts);
}

export const fmtHour = parts => String(parts.hour % 12 || 12);
export const fmtMin = parts => (parts.minute < 10 ? '0' : '') + parts.minute;
export const fmtAmPm = parts => (parts.hour < 12 ? 'AM' : 'PM');
export const fmtTime = parts =>
  fmtHour(parts) + ':' + fmtMin(parts) + ' ' + fmtAmPm(parts);
export const fmtDate = parts =>
  WEEKDAYS[parts.weekday] + ', ' + MONTHS[parts.month - 1] + ' ' + parts.day;
