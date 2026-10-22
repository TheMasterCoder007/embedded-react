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

// A type-level FIXTURE, not a runtime test: `npm run typecheck` compiles it alongside index.d.ts.
// It uses Animated, Easing and StyleSheet the way an app does, and the `@ts-expect-error` lines pin
// what the runtime does NOT accept, so a type that drifts looser than the bridge fails here too.
import {
  Animated,
  Dial,
  Easing,
  Pressable,
  StyleSheet,
  View,
  useAnimatedValue,
} from '../../src/embedded-react/index';

const AnimatedPressable = Animated.createAnimatedComponent(Pressable);

const styles = StyleSheet.create({
  fill: {...StyleSheet.absoluteFill},
  rule: {height: StyleSheet.hairlineWidth, flexBasis: '50%', left: '10%'},
});

export function AnimatedUsage() {
  const v = useAnimatedValue(0);
  const spin = v.interpolate({
    inputRange: [0, 1],
    outputRange: [0, 360],
    extrapolate: 'clamp',
    extrapolateRight: 'extend',
  });

  // @ts-expect-error — the bridge reads outputRange as numbers; a string becomes NaN.
  v.interpolate({inputRange: [0, 1], outputRange: ['0deg', '360deg']});
  // @ts-expect-error — an interpolation is not a settable value.
  spin.setValue(1);
  // @ts-expect-error — nor does it own an engine slot to free.
  spin.destroy();

  const flat = StyleSheet.flatten([styles.fill, false, [styles.rule]]);
  void flat.flexBasis;

  return (
    <View style={styles.fill}>
      <AnimatedPressable
        onPress={() => {
          Animated.timing(v, {
            toValue: 1,
            easing: Easing.bezier(0.2, 0, 0, 1),
          }).start();
          Animated.timing(v, {toValue: 0, easing: Easing.easeInOut}).start();
          Animated.spring(v, {
            toValue: 1,
            stiffness: 180,
            damping: 12,
            mass: 1,
            velocity: 0.5,
          }).start();
          Animated.decay(v, {velocity: 0.8, deceleration: 0.997}).start();
          // @ts-expect-error — RN's spelling; the engine reads stiffness/damping.
          Animated.spring(v, {toValue: 1, tension: 40});
          // @ts-expect-error — easing is an engine token, not a JS function.
          Animated.timing(v, {toValue: 1, easing: (t: number) => t});

          const scratch = new Animated.Value(0);
          scratch.destroy();
        }}
        style={{opacity: v, transform: [{rotate: spin}]}}
      />
      <Dial value={spin} />
    </View>
  );
}
