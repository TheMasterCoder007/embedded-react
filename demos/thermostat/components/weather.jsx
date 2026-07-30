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

import {memo} from 'react';
import {View, Text, Image, ScrollView} from 'embedded-react';
// Weather icons. The bundler's asset plugin turns each import into its baked asset name (the PNG's
// basename), so <Image source={wxSun}> resolves to the "wx_sun" buffer registered at boot; `npm run build`
// decodes the PNGs into dist/assets.generated.c. They are a raster, not vector, so one set serves both
// themes — the cloud grey is the midpoint of the design's dark and light cloud colors.
import wxSun from '../assets/wx_sun.png';
import wxPartly from '../assets/wx_partly.png';
import wxCloud from '../assets/wx_cloud.png';
import wxRain from '../assets/wx_rain.png';

const ICONS = {sun: wxSun, partly: wxPartly, cloud: wxCloud, rain: wxRain};

// The 14-day outlook. `p` is precipitation probability; the bar spans lo→hi within WMIN..WMAX.
export const FORECAST = [
  {day: 'TODAY', c: 'sun', hi: 82, lo: 61, p: 0},
  {day: 'TUE', c: 'sun', hi: 84, lo: 63, p: 0},
  {day: 'WED', c: 'partly', hi: 79, lo: 62, p: 10},
  {day: 'THU', c: 'cloud', hi: 74, lo: 60, p: 20},
  {day: 'FRI', c: 'rain', hi: 70, lo: 58, p: 70},
  {day: 'SAT', c: 'rain', hi: 68, lo: 57, p: 55},
  {day: 'SUN', c: 'partly', hi: 75, lo: 59, p: 15},
  {day: 'MON', c: 'sun', hi: 80, lo: 60, p: 0},
  {day: 'TUE', c: 'sun', hi: 85, lo: 64, p: 0},
  {day: 'WED', c: 'partly', hi: 83, lo: 65, p: 5},
  {day: 'THU', c: 'cloud', hi: 76, lo: 62, p: 25},
  {day: 'FRI', c: 'rain', hi: 71, lo: 59, p: 60},
  {day: 'SAT', c: 'partly', hi: 77, lo: 60, p: 10},
  {day: 'SUN', c: 'sun', hi: 81, lo: 62, p: 0},
];
const WMIN = 57;
const WMAX = 85;

const OUTDOOR_F = 78;

// ----------------------------------------------------------------------------------------------------
// Weather panel — the right column when the layout is `split`, the lower card when it is `stack`. Not
// rendered at all in `solo`. Static content, so it never re-renders during a dial drag.
//
// MEMOISED, and it takes `unit` rather than a formatter closure. This panel is ~100 of the app's ~160
// nodes, and reconciling it cost ~770 ms of the ~1250 ms mode-switch stall measured on the ESP32-S3. Its
// content depends only on the palette and the unit, so every prop here must stay referentially stable
// across a mode or setpoint change — hence module-level style objects in App.jsx and no inline closures.
//
//   theme   the resolved palette (a module-level object, stable per theme)
//   unit    'F' | 'C'
//   barW    pixel width of the hi/lo range bar's track (the row's flexible column)
// ----------------------------------------------------------------------------------------------------
export const WeatherPanel = memo(function WeatherPanel({
  theme,
  unit,
  barW,
  style,
}) {
  const fmt = f =>
    unit === 'C' ? Math.round(((f - 32) * 5) / 9) + '°' : Math.round(f) + '°';
  const fmtBare = f =>
    unit === 'C'
      ? String(Math.round(((f - 32) * 5) / 9))
      : String(Math.round(f));
  return (
    <View style={style}>
      <View
        style={{
          flexDirection: 'row',
          justifyContent: 'space-between',
          alignItems: 'flex-start',
        }}>
        <View style={{gap: 6}}>
          <Text
            style={{
              fontSize: 12,
              letterSpacing: 1,
              fontWeight: '600',
              color: theme.fg,
            }}>
            CUPERTINO, CA
          </Text>
          <View style={{flexDirection: 'row', alignItems: 'flex-end', gap: 10}}>
            <Text style={{fontSize: 32, fontWeight: '500', color: theme.fg}}>
              {fmt(OUTDOOR_F)}
            </Text>
            <Text
              style={{
                fontSize: 10,
                letterSpacing: 2,
                color: theme.dim,
                marginBottom: 4,
              }}>
              CLEAR
            </Text>
          </View>
        </View>
        <Image
          source={wxSun}
          resizeMode="contain"
          style={{width: 34, height: 34}}
        />
      </View>

      <View style={{height: 1, backgroundColor: theme.trackBg}} />

      <ScrollView style={{flex: 1}}>
        {FORECAST.map((d, i) => {
          const left = ((d.lo - WMIN) / (WMAX - WMIN)) * barW;
          const width = ((d.hi - d.lo) / (WMAX - WMIN)) * barW;
          return (
            <View
              key={i}
              style={{
                height: 44,
                flexDirection: 'row',
                alignItems: 'center',
                gap: 12,
              }}>
              <Text
                style={{
                  width: 46,
                  fontSize: 10,
                  letterSpacing: 1,
                  color: theme.fg,
                }}>
                {d.day}
              </Text>
              <Image
                source={ICONS[d.c]}
                resizeMode="contain"
                style={{width: 24, height: 24}}
              />
              <Text
                style={{
                  width: 36,
                  fontSize: 10,
                  color: d.p >= 40 ? theme.rainText : theme.dim2,
                }}>
                {d.p > 0 ? d.p + '%' : '—'}
              </Text>
              <Text style={{width: 30, fontSize: 12, color: theme.dim}}>
                {fmtBare(d.lo)}
              </Text>
              {/* The hi/lo range bar: a track with the day's span laid over it at a pixel offset. Percent
                  offsets aren't available here, and the column width is known, so both are in px. */}
              <View
                style={{
                  width: barW,
                  height: 3,
                  borderRadius: 2,
                  backgroundColor: theme.trackBg,
                }}>
                <View
                  style={{
                    position: 'absolute',
                    left: left,
                    top: 0,
                    width: width,
                    height: 3,
                    borderRadius: 2,
                    backgroundColor: theme.barFill,
                  }}
                />
              </View>
              <Text style={{width: 30, fontSize: 12, color: theme.fg}}>
                {fmtBare(d.hi)}
              </Text>
            </View>
          );
        })}
      </ScrollView>
    </View>
  );
});
