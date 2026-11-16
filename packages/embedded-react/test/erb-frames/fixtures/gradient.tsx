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

// A backgroundImage in each form the engine draws.
import {Pressable, StyleSheet, View} from 'embedded-react';

export function App() {
  return (
    <View style={styles.root}>
      <View style={[styles.box, styles.down]} />
      <View style={[styles.box, styles.angle]} />
      <View style={[styles.box, styles.corner]} />
      <View style={[styles.box, styles.bands]} />
      <View style={[styles.box, styles.fade]} />
      <Pressable style={[styles.box, styles.radial]} />
      <View style={[styles.box, styles.shallow]} />
    </View>
  );
}

const styles = StyleSheet.create({
  root: {
    flex: 1,
    backgroundColor: '#101820',
    alignItems: 'center',
    justifyContent: 'space-evenly',
  },
  box: {
    width: 200,
    height: 34,
    backgroundColor: '#ff00ff',
  },
  down: {
    backgroundImage: 'linear-gradient(#0f172a, #38bdf8)',
  },
  angle: {
    borderRadius: 12,
    backgroundImage: 'linear-gradient(60deg, #f97316, #facc15 40%, #22c55e)',
  },
  corner: {
    backgroundImage: [
      {
        type: 'linear-gradient',
        direction: 'to bottom right',
        colorStops: [{color: '#7c3aed'}, {color: '#f43f5e'}],
      },
    ],
  },
  bands: {
    backgroundImage:
      'linear-gradient(to right, #ef4444 0% 50%, #3b82f6 50% 100%)',
  },
  fade: {
    backgroundImage: 'linear-gradient(to left, #ffffff, #ffffff00)',
  },
  radial: {
    height: 60,
    borderRadius: 30,
    backgroundImage: 'radial-gradient(circle, #fde047, #b45309 70%, #451a03)',
  },
  shallow: {
    backgroundImage: 'linear-gradient(to right, #000000, #101010)',
  },
});
