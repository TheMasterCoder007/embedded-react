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

// The 14-day outlook. `precipitation` is its probability; the bar spans low→high within SCALE_MIN..SCALE_MAX.
export const FORECAST = [
  {day: 'TODAY', condition: 'sun', high: 82, low: 61, precipitation: 0},
  {day: 'TUE', condition: 'sun', high: 84, low: 63, precipitation: 0},
  {day: 'WED', condition: 'partly', high: 79, low: 62, precipitation: 10},
  {day: 'THU', condition: 'cloud', high: 74, low: 60, precipitation: 20},
  {day: 'FRI', condition: 'rain', high: 70, low: 58, precipitation: 70},
  {day: 'SAT', condition: 'rain', high: 68, low: 57, precipitation: 55},
  {day: 'SUN', condition: 'partly', high: 75, low: 59, precipitation: 15},
  {day: 'MON', condition: 'sun', high: 80, low: 60, precipitation: 0},
  {day: 'TUE', condition: 'sun', high: 85, low: 64, precipitation: 0},
  {day: 'WED', condition: 'partly', high: 83, low: 65, precipitation: 5},
  {day: 'THU', condition: 'cloud', high: 76, low: 62, precipitation: 25},
  {day: 'FRI', condition: 'rain', high: 71, low: 59, precipitation: 60},
  {day: 'SAT', condition: 'partly', high: 77, low: 60, precipitation: 10},
  {day: 'SUN', condition: 'sun', high: 81, low: 62, precipitation: 0},
];
const SCALE_MIN = 57;
const SCALE_MAX = 85;

const OUTDOOR_FAHRENHEIT = 78;
const RAINY_PERCENT = 40; // a chance of rain from here up is shown in the rain color

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
//   barWidth  pixel width of the high/low range bar's track (the row's flexible column)
// ----------------------------------------------------------------------------------------------------
export const WeatherPanel = memo(function WeatherPanel({
  theme,
  unit,
  barWidth,
  style,
}) {
  const fmt = fahrenheit =>
    unit === 'C'
      ? Math.round(((fahrenheit - 32) * 5) / 9) + '°'
      : Math.round(fahrenheit) + '°';
  const fmtBare = fahrenheit =>
    unit === 'C'
      ? String(Math.round(((fahrenheit - 32) * 5) / 9))
      : String(Math.round(fahrenheit));
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
              color: theme.foreground,
            }}>
            CUPERTINO, CA
          </Text>
          <View style={{flexDirection: 'row', alignItems: 'flex-end', gap: 10}}>
            <Text
              style={{
                fontSize: 32,
                fontWeight: '500',
                color: theme.foreground,
              }}>
              {fmt(OUTDOOR_FAHRENHEIT)}
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

      <View style={{height: 1, backgroundColor: theme.trackBackground}} />

      <ScrollView style={{flex: 1}}>
        {FORECAST.map((forecast, dayIndex) => {
          const left =
            ((forecast.low - SCALE_MIN) / (SCALE_MAX - SCALE_MIN)) * barWidth;
          const width =
            ((forecast.high - forecast.low) / (SCALE_MAX - SCALE_MIN)) *
            barWidth;
          return (
            <View
              key={dayIndex}
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
                  color: theme.foreground,
                }}>
                {forecast.day}
              </Text>
              <Image
                source={ICONS[forecast.condition]}
                resizeMode="contain"
                style={{width: 24, height: 24}}
              />
              <Text
                style={{
                  width: 36,
                  fontSize: 10,
                  color:
                    forecast.precipitation >= RAINY_PERCENT
                      ? theme.rainText
                      : theme.secondary,
                }}>
                {forecast.precipitation > 0
                  ? forecast.precipitation + '%'
                  : '—'}
              </Text>
              <Text style={{width: 30, fontSize: 12, color: theme.dim}}>
                {fmtBare(forecast.low)}
              </Text>
              {/* The high/low range bar: a track with the day's span laid over it at a pixel offset. Percent
                  offsets aren't available here, and the column width is known, so both are in px. */}
              <View
                style={{
                  width: barWidth,
                  height: 3,
                  borderRadius: 2,
                  backgroundColor: theme.trackBackground,
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
              <Text style={{width: 30, fontSize: 12, color: theme.foreground}}>
                {fmtBare(forecast.high)}
              </Text>
            </View>
          );
        })}
      </ScrollView>
    </View>
  );
});
