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

import {describe, it, expect, vi} from 'vitest';
import {readFileSync} from 'node:fs';
import {compilePackage} from '../compile.mts';
import {ENGINE_STRING_MAX} from '../compiler/diagnostics.mts';

// The Flow B AOT compiler turns an App.jsx source string into a package. These tests check what it refuses
// (compilePackage is pure — no file I/O), so each fixture is a complete minimal app. `PREAMBLE` is the usual import
// preamble (the compiler pattern-matches names, it does not resolve imports, so the import line is cosmetic but
// kept for realism).
const PREAMBLE = `import { useState } from 'react';
import { View, Text, Pressable, StyleSheet, Animated, useAnimatedValue } from 'embedded-react';
`;
const gen = source => compilePackage(source, 'test');

describe('AOT baseline (regression)', () => {
  it('rejects `visible` on an <Svg> instead of silently dropping it', () => {
    expect(() =>
      gen(`${PREAMBLE}
      export function App() {
        return (<View><Svg visible={false} width={10} height={10}><Path d="M0 0 L1 1" fill="none" stroke="#fff" /></Svg></View>);
      }`),
    ).toThrow(/<Svg> prop "visible" is not supported[\s\S]*hint: wrap it/);
  });

  it('throws a clear error when <Svg source> is not an imported .svg', () => {
    expect(() =>
      compilePackage(
        `import { View, Svg } from 'embedded-react';\nexport function App() { return <View><Svg source={{}} width={10} height={10} /></View>; }`,
        'test',
        {},
      ),
    ).toThrow(/<Svg source> must reference an imported \.svg/);
  });
});

describe('AOT animation completeness', () => {
  const IMPORTS = `import { View, Animated, Easing, useAnimatedValue } from 'embedded-react';`;
  // Wraps a handler body in an App with two animated values a and b, on an onPress View.
  const app = (body, style = '') =>
    gen(`${IMPORTS}
      export function App() {
        const a = useAnimatedValue(0);
        const b = useAnimatedValue(0);
        return (<View onPress={() => { ${body} }} ${style ? `style={${style}}` : ''}><Text>x</Text></View>);
      }`);
  it('rejects the same value driven twice in a parallel (they would cancel)', () => {
    expect(() =>
      app(`Animated.parallel([
      Animated.timing(a, { toValue: 1, duration: 100 }),
      Animated.timing(a, { toValue: 0, duration: 100 }),
    ]).start();`),
    ).toThrow(/same animated value/);
  });

  it('rejects a loop whose steps all finish instantly', () => {
    expect(() =>
      app(`Animated.loop(Animated.sequence([
      Animated.timing(a, { toValue: 1, duration: 0 }),
      Animated.timing(a, { toValue: 0, duration: 0 }),
    ])).start();`),
    ).toThrow(/finishes instantly/);
  });

  it('rejects a looped sequence nested inside another composition', () => {
    expect(() =>
      app(`Animated.parallel([
      Animated.loop(Animated.sequence([
        Animated.timing(a, { toValue: 1, duration: 100 }),
        Animated.timing(b, { toValue: 1, duration: 100 }),
      ])),
    ]).start();`),
    ).toThrow(/inside another composition/);
  });

  it('rejects looping a parallel', () => {
    expect(() =>
      app(`Animated.loop(Animated.parallel([
      Animated.timing(a, { toValue: 1, duration: 100 }),
      Animated.timing(b, { toValue: 1, duration: 100 }),
    ])).start();`),
    ).toThrow(/looping a parallel\/stagger is not yet supported/);
  });

  it('rejects mismatched interpolate ranges', () => {
    expect(() =>
      app(
        `Animated.timing(a, { toValue: 1, duration: 200 }).start();`,
        `{ opacity: a.interpolate({ inputRange: [0, 1], outputRange: [0.2, 0.5, 1] }) }`,
      ),
    ).toThrow(/same length/);
  });
});

describe('AOT responsive layout', () => {
  it('throws on a top-level if whose test is not compile-time constant', () => {
    const source = `${PREAMBLE}
      export function App() {
        const [n, setN] = useState(0);
        if (n > 5) return (<View><Text>a</Text></View>);
        return (<View><Text>b</Text></View>);
      }`;
    expect(() => compilePackage(source, 'test')).toThrow(
      /compile-time-constant test/,
    );
  });
});

describe('AOT inline-Svg gradients', () => {
  // endAngle is state-driven, so the <Svg> takes the dynamic path — the only one that supports
  // gradients on inline shapes. A fully static <Svg> ignores them.
  const arc = extra => `${PREAMBLE}
    export function App() {
      const [lo, setLo] = useState(68);
      return (<View><Svg width={200} height={200}>
        <Arc cx={100} cy={100} r={80} startAngle={240} endAngle={240 + lo} fill="none"
             strokeWidth={12} stroke="#F2A64B" ${extra} />
      </Svg></View>);
    }`;

  it('rejects a conditional whose other branch is not null', () => {
    expect(() =>
      gen(
        arc(`strokeGradient={lo > 60
          ? {type: 3, ax: 0, ay: 0, r: 0, stops: [{color: '#F2A64B', offset: 0}, {color: '#4FA9F5', offset: 1}]}
          : {type: 1, ax: 0, ay: 0, bx: 1, by: 0, stops: [{color: '#F2A64B', offset: 0}, {color: '#4FA9F5', offset: 1}]}}`),
      ),
    ).toThrow(/conditional "strokeGradient"/);
  });

  it('rejects a non-constant gradient type — it decides the emitted table shape', () => {
    expect(() =>
      gen(
        arc(`strokeGradient={{type: lo, ax: 0, ay: 0,
          stops: [{color: '#F2A64B', offset: 0}, {color: '#4FA9F5', offset: 1}]}}`),
      ),
    ).toThrow(/type.*compile-time constant/);
  });

  it('rejects a stop count the engine table cannot hold', () => {
    const many = Array.from(
      {length: 9},
      (_, index) => `{color: '#F2A64B', offset: ${index / 9}}`,
    ).join(', ');
    expect(() =>
      gen(
        arc(
          `strokeGradient={{type: 1, ax: 0, ay: 0, bx: 1, by: 0, stops: [${many}]}}`,
        ),
      ),
    ).toThrow(/stops.*2\.\.8/);
  });
});

// <Svg> children are shape descriptions, not nodes, so the compiler walks the JSX itself — and used to
// see only direct JSXElements. A fragment around a shape matched nothing and was dropped from the
// package without a word, on both the static and the state-driven path.
describe('AOT <Svg> child unwrapping', () => {
  const svg = body => `${PREAMBLE}
    import { Svg, Circle, Line, Arc, G } from 'embedded-react';
    export function App() {
      const [t, setT] = useState(0);
      return (
        <Pressable onPress={() => setT(t + 1)}>
          <Svg width={100} height={100}>${body}</Svg>
        </Pressable>
      );
    }`;

  it('throws on a child it cannot lower instead of dropping it', () => {
    const dynamic = /dynamic <Svg> children/;
    // static path
    expect(() =>
      gen(svg(`{[1, 2].map(i => <Circle cx={i} cy={i} r={1} fill="#fff" />)}`)),
    ).toThrow(dynamic);
    // state-driven path (a `t`-driven sibling forces it) — used to compile, silently short a shape
    expect(() =>
      gen(
        svg(`
          <Arc cx={50} cy={50} r={40} startAngle={-135} endAngle={t * 2} stroke="#f4a261" strokeWidth={8} />
          {[1, 2].map(i => <Circle cx={i} cy={i} r={1} fill="#fff" />)}
        `),
      ),
    ).toThrow(dynamic);
    expect(() =>
      gen(svg(`hello<Circle cx={1} cy={1} r={1} fill="#fff" />`)),
    ).toThrow(/cannot draw text/);
  });
});

describe('AOT arithmetic semantics', () => {
  it('refuses a constant that folds to NaN or Infinity, which a number slot cannot hold', () => {
    const app = (declaration, handler) => `${PREAMBLE}
      ${declaration}
      export function App() {
        const [n, setN] = useState(0);
        const [f, setF] = useState(0.5);
        return (<Pressable onPress={() => ${handler}}><Text>{n}</Text><Text>{f}</Text></Pressable>);
      }`;
    expect(() => gen(app('const X = -1 / 0;', 'setF(f + X)'))).toThrow(
      /folded to -?Infinity/,
    );
    expect(() =>
      gen(app('const X = 0 / 0;', 'setF(f > X ? 1.5 : 2.5)')),
    ).toThrow(/folded to NaN/);
    expect(() => gen(app('const X = 1 / 0;', 'setN(Math.floor(X))'))).toThrow(
      /folded to -?Infinity/,
    );
  });
});

describe('AOT touch drag', () => {
  it('refuses a layout read outside an onLayout handler', () => {
    expect(() =>
      gen(`${PREAMBLE}
        import { useRef } from 'react';
        export function App() {
          const top = useRef(0);
          return <View onTouchStart={(e) => { top.current = e.nativeEvent.layout.y; }} />;
        }`),
    ).toThrow(/only an onLayout event carries a layout rect/);
  });
});

describe('AOT PanResponder (lowered onto the engine responder system)', () => {
  const PAN_PRE = `import { useState, useRef } from 'react';
import { View, Pressable, ScrollView, Text, PanResponder } from 'embedded-react';
`;

  it('still rejects a spread that is not a PanResponder', () => {
    expect(() =>
      gen(`${PAN_PRE}
      const extra = {};
      export function App() { return (<View {...extra} />); }`),
    ).toThrow(/a spread \{\.\.\.\} on <View> is not supported/);
  });

  it('rejects a PanResponder that is not kept in a useRef', () => {
    expect(() =>
      gen(`${PAN_PRE}
      export function App() {
        const [x, setX] = useState(0);
        const pan = PanResponder.create({ onPanResponderMove: (e, g) => setX(g.dx) });
        return (<View {...pan.panHandlers}><Text>{x}</Text></View>);
      }`),
    ).toThrow(/must be kept in a useRef/);
  });

  it('rejects an unknown config key instead of silently dropping it', () => {
    expect(() =>
      gen(`${PAN_PRE}
      export function App() {
        const pan = useRef(PanResponder.create({ onPanResponderWobble: () => {} })).current;
        return (<View {...pan.panHandlers} />);
      }`),
    ).toThrow(/unknown PanResponder config key "onPanResponderWobble"/);
  });

  it('names the Flow-A-only multi-finger callbacks rather than ignoring them', () => {
    expect(() =>
      gen(`${PAN_PRE}
      export function App() {
        const pan = useRef(PanResponder.create({ onPanResponderStart: () => {} })).current;
        return (<View {...pan.panHandlers} />);
      }`),
    ).toThrow(/"onPanResponderStart" is not supported in Flow B/);
  });

  it('rejects a should-set predicate that is not a single expression', () => {
    expect(() =>
      gen(`${PAN_PRE}
      export function App() {
        const [x, setX] = useState(0);
        const pan = useRef(PanResponder.create({
          onStartShouldSetPanResponder: (e, g) => { setX(1); return true; },
        })).current;
        return (<View {...pan.panHandlers}><Text>{x}</Text></View>);
      }`),
    ).toThrow(/must be a single boolean expression/);
  });

  it('rejects an unknown gestureState field', () => {
    expect(() =>
      gen(`${PAN_PRE}
      export function App() {
        const [x, setX] = useState(0);
        const pan = useRef(PanResponder.create({
          onPanResponderMove: (e, g) => setX(g.dz),
        })).current;
        return (<View {...pan.panHandlers}><Text>{x}</Text></View>);
      }`),
    ).toThrow(/unknown gestureState field "dz"/);
  });

  it('rejects a spread whose shape disagrees with how the responder was declared', () => {
    expect(() =>
      gen(`${PAN_PRE}
      export function App() {
        const [x, setX] = useState(0);
        const pan = useRef(PanResponder.create({
          onPanResponderMove: (e, g) => setX(g.dx),
        }));
        return (<View {...pan.panHandlers}><Text>{x}</Text></View>);
      }`),
    ).toThrow(/does not match how it was declared/);
  });

  it('rejects a PanResponder spread onto a component instance', () => {
    expect(() =>
      gen(`${PAN_PRE}
      function Box() { return (<View style={{ width: 10, height: 10 }} />); }
      export function App() {
        const [x, setX] = useState(0);
        const pan = useRef(PanResponder.create({
          onPanResponderMove: (e, g) => setX(g.dx),
        })).current;
        return (<View><Box {...pan.panHandlers} /><Text>{x}</Text></View>);
      }`),
    ).toThrow(/can only be spread onto a host element/);
  });
});

describe('AOT diagnostics', () => {
  // A responsive app folds its layout from `screen`, so the size picks the branch the compiler walks, and every
  // located error names it.
  const errAtScreen = options => {
    try {
      compilePackage(
        `${PREAMBLE}\nexport function App() { const [n, setN] = useState(0); return (<Pressable onPress={() => setN(window.x)}><Text>{n}</Text></Pressable>); }`,
        'demo',
        options,
      );
    } catch (error) {
      return error;
    }
  };

  it('names the size of a layout it found itself, and how to drop the layout', () => {
    expect(errAtScreen({}).message).toMatch(
      /screen: this is the layout a 240×320 panel picks[\s\S]*--screen/,
    );
  });

  it('names a size it was told', () => {
    expect(errAtScreen({screen: {width: 320, height: 480}}).message).toContain(
      'screen: 320×480.',
    );
  });

  it('takes the size from ER_AOT_SCREEN_W/H', () => {
    vi.stubEnv('ER_AOT_SCREEN_W', '240');
    vi.stubEnv('ER_AOT_SCREEN_H', '320');
    try {
      expect(errAtScreen({}).message).toContain('screen: 240×320.');
    } finally {
      vi.unstubAllEnvs();
    }
  });

  // A spread on a HOST element used to be silently dropped (the style/event loops only read named
  // attributes) — it must now error clearly, like the typed components already do.
  it('rejects a spread on a host element instead of silently dropping it', () => {
    expect(() =>
      gen(
        `${PREAMBLE}\nexport function App() { const o = window.x; return (<View {...o} />); }`,
      ),
    ).toThrow(/spread \{\.\.\.\} on <View> is not supported/);
  });
});

describe('AOT Switch', () => {
  it('throws when a Switch has onValueChange but no value prop', () => {
    expect(() =>
      gen(`${PREAMBLE}
        import { Switch } from 'embedded-react';
        export function App() { const [on, setOn] = useState(false); return (<Switch onValueChange={(v) => setOn(v)} />); }`),
    ).toThrow(/needs a value prop/);
  });
});

describe('AOT Dial', () => {
  it('rejects an unsupported prop and a bad enum token', () => {
    expect(() =>
      gen(`${PREAMBLE}
        import { Dial } from 'embedded-react';
        export function App() { return (<Dial value={1} bogus={2} />); }`),
    ).toThrow(/prop "bogus" is not supported/);
    expect(() =>
      gen(`${PREAMBLE}
        import { Dial } from 'embedded-react';
        export function App() { return (<Dial value={1} knob="triangle" />); }`),
    ).toThrow(/unsupported <Dial knob>/);
  });
});

describe('AOT Modal', () => {
  it('throws when a Modal has no visible prop', () => {
    expect(() =>
      gen(`${PREAMBLE}
        import { Modal } from 'embedded-react';
        export function App() { return (<Modal><Text>x</Text></Modal>); }`),
    ).toThrow(/needs a visible prop/);
  });
});

describe('AOT dynamic enum styles', () => {
  it('throws on an unknown enum value in a dynamic enum style', () => {
    expect(() =>
      gen(`${PREAMBLE}
        export function App() {
          const [r, setR] = useState(false);
          return (<View style={{ flexDirection: r ? 'sideways' : 'row' }}><Text>x</Text></View>);
        }`),
    ).toThrow(/unsupported enum value "sideways"/);
  });
});

describe('AOT nested Text spans', () => {
  it('throws when a <Text> exceeds the engine span limit (4)', () => {
    expect(() =>
      gen(`${PREAMBLE}
        export function App() {
          return (<Text>a <Text style={{ fontWeight: 'bold' }}>b</Text> c <Text style={{ fontWeight: 'bold' }}>d</Text> e</Text>);
        }`),
    ).toThrow(/renders at most 4/);
  });
});

describe('AOT FlatList (thin rewrite → ScrollView + .map)', () => {
  it('throws when renderItem is not a ({ item }) destructuring function', () => {
    expect(() =>
      gen(`${PREAMBLE}
        import { FlatList } from 'embedded-react';
        export function App() { const d = []; return (<FlatList data={d} renderItem={(x) => <Text>x</Text>} />); }`),
    ).toThrow(/must destructure/);
  });
});

// The package exports three RN wrappers the AOT cannot lower yet (issue #114 shipped them Flow-A only).
// The failure mode that matters is the MESSAGE: the import resolves, the simulator renders them, and the
// spelling is right, so a bare "unknown element" sends people hunting for a typo that isn't there.
describe('AOT Flow-A-only components', () => {
  // The catch-all must still fire for a name that really is a typo / a missing import.
  it('still reports a genuinely unknown element as unknown', () => {
    expect(() =>
      gen(`${PREAMBLE}
        export function App() { return (<Vieww />); }`),
    ).toThrow(/unknown element <Vieww>/);
  });
});

describe('AOT TouchableOpacity (press-to-dim on the native driver)', () => {
  const IMPORTS = `import { useState, useCallback } from 'react';
import { View, Text, Pressable, TouchableOpacity, useAnimatedValue } from 'embedded-react';
`;
  /** The body of the handler wired to `event` on the (single) node, so a test can read its animate call. */
  it('rejects a state-driven opacity, which the feedback would overwrite', () => {
    expect(() =>
      gen(`${IMPORTS}
        export function App() {
          const [o, setO] = useState(1);
          return (<TouchableOpacity style={{opacity: o}} onPress={() => setO(0.5)}><Text>x</Text></TouchableOpacity>);
        }`),
    ).toThrow(/cannot take a state-driven opacity/);
  });

  it('rejects a state-driven disabled, which it cannot compile away', () => {
    expect(() =>
      gen(`${IMPORTS}
        export function App() {
          const [d, setD] = useState(true);
          return (<TouchableOpacity disabled={d} onPress={() => setD(false)}><Text>x</Text></TouchableOpacity>);
        }`),
    ).toThrow(/disabled> must be a compile-time constant/);
  });

  // An Animated.Value opacity is collected as a `binds` entry, NOT a dynAssign — a different list, so
  // the state-driven check above misses it entirely. Left alone it emits a SECOND
  // er_animation_value_bind on the same node+prop: the engine's duplicate guard is per-value, so both
  // register and whichever changed last wins the frame.
  it.each([
    ['an Animated.Value', 'style={{opacity: fade}}'],
    [
      'an interpolation',
      'style={{opacity: fade.interpolate({inputRange: [0, 1], outputRange: [0.3, 1]})}}',
    ],
  ])('rejects %s driving a TouchableOpacity opacity', (_what, attribute) => {
    expect(() =>
      gen(`${IMPORTS}
        export function App() {
          const fade = useAnimatedValue(1);
          return (<TouchableOpacity ${attribute}><Text>x</Text></TouchableOpacity>);
        }`),
    ).toThrow(/cannot take an Animated opacity/);
  });
});

describe('AOT Pressable disabled', () => {
  const IMPORTS = `import { useState } from 'react';
import { View, Text, Pressable } from 'embedded-react';
`;

  it('rejects a state-driven disabled on Pressable', () => {
    expect(() =>
      gen(`${IMPORTS}
        export function App() {
          const [d, setD] = useState(true);
          return (<Pressable disabled={d} onPress={() => setD(false)}><Text>x</Text></Pressable>);
        }`),
    ).toThrow(/disabled> must be a compile-time constant/);
  });
});

describe('AOT TextInput', () => {
  const IMPORTS = `import { useState } from 'react';
import { View, Text, TextInput } from 'embedded-react';
`;

  it('rejects a non-function onChangeText', () => {
    expect(() =>
      gen(`${IMPORTS}
        export function App() { const [n, setN] = useState(''); return (<TextInput value={n} onChangeText={n} />); }`),
    ).toThrow(/onChangeText must be an inline function/);
  });

  it('rejects an unsupported TextInput prop', () => {
    expect(() =>
      gen(`${IMPORTS}
        export function App() { return (<TextInput selectionColor="#fff" />); }`),
    ).toThrow(/not supported/);
  });
});

describe('AOT images', () => {
  // `import x from './x.png'` + <Image source={x}> → the node's image_name is the file's basename, and
  // compilePackage returns the import so the CLI can bake it (er_register_assets).
  const IMPORTS = `import { View, Image } from 'embedded-react';
import logo from './assets/logo.png';
`;

  it('rejects an <Image source> that is not a string (e.g. a number)', () => {
    expect(() =>
      compilePackage(
        `import { Image } from 'embedded-react';\nimport { useState } from 'react';\nexport function App() { const [n] = useState(5); return (<Image source={n} />); }`,
        'demo',
      ),
    ).toThrow(/must resolve to an asset NAME/);
  });

  it('rejects an unsupported resizeMode', () => {
    expect(() =>
      compilePackage(
        `${IMPORTS}\nexport function App() { return (<Image source={logo} resizeMode="squish" />); }`,
        'demo',
      ),
    ).toThrow(/unsupported <Image resizeMode>/);
  });

  it('rejects two image files of the same name, at the second import', () => {
    expect(() =>
      compilePackage(
        `import { View, Image } from 'embedded-react';\nimport tall from './portrait/icon.png';\nimport wide from './landscape/icon.png';\nexport function App() { return (<View><Image source={tall} /><Image source={wide} /></View>); }`,
        'demo',
      ),
    ).toThrow(
      /two images are named "icon" \(\.\/portrait\/icon\.png and \.\/landscape\/icon\.png\)[\s\S]*App\.jsx:3:1/,
    );
  });
});

describe('AOT handler follow-ons', () => {
  it('detects a recursive helper and errors instead of looping forever', () => {
    expect(() =>
      compilePackage(
        `import { Text, Pressable } from 'embedded-react';
function App() { const loop = () => { loop(); }; return (<Pressable onPress={() => { loop(); }}><Text>x</Text></Pressable>); }
export { App };`,
        'demo',
      ),
    ).toThrow(/recursive/);
  });

  it('rejects a completion callback on a parallel animation (not yet supported)', () => {
    expect(() =>
      compilePackage(
        `import { Text, Pressable, Animated, useAnimatedValue } from 'embedded-react';
function App() {
  const a = useAnimatedValue(0); const b = useAnimatedValue(0);
  return (<Pressable onPress={() => Animated.parallel([Animated.timing(a, { toValue: 1 }), Animated.timing(b, { toValue: 1 })]).start(() => {})}><Text>x</Text></Pressable>);
}
export { App };`,
        'demo',
      ),
    ).toThrow(/parallel\/stagger animation is not yet supported/);
  });
});

describe('AOT useHostValue (host-fed input)', () => {
  const HOST_PRE = `import { useHostValue } from 'embedded-react';
`;

  it('rejects a string host value', () => {
    expect(() =>
      gen(`${HOST_PRE}import { Text } from 'embedded-react';
      export function App() { const name = useHostValue("hi"); return (<Text>{name}</Text>); }`),
    ).toThrow(/useHostValue.*must be a number/);
  });

  it('rejects a boolean host value (its slot would otherwise be an int)', () => {
    expect(() =>
      gen(`${HOST_PRE}import { Text } from 'embedded-react';
      export function App() { const on = useHostValue(true); return (<Text>{on}</Text>); }`),
    ).toThrow(/useHostValue.*must be a number/);
  });

  it('rejects a non-finite host value (NaN)', () => {
    expect(() =>
      gen(`${HOST_PRE}import { Text } from 'embedded-react';
      export function App() { const x = useHostValue(0 / 0); return (<Text>{x}</Text>); }`),
    ).toThrow(/useHostValue.*must be a number/);
  });
});

// Name resolution has been a recurring source of silently-wrong text: the constant fold reaching past a
// runtime binding, App's locals leaking into a child, a child's own const not shadowing. These pin the
// rule rather than one instance of it — a child component is declared at module level, so its body sees
// MODULE scope plus its own props and consts, never the caller's locals.
describe('AOT const scoping', () => {
  const IMPORTS = `import {useState} from 'react';
import {View, Text} from 'embedded-react';
`;
  it('a prop default cannot read its own or a later binding', () => {
    for (const params of ['{ size = size }', '{ size = SIZE, SIZE = 5 }']) {
      expect(() =>
        gen(`${IMPORTS}const size = 30; const SIZE = 30;
          function Box(${params}) { return (<View style={{width: size}} />); }
          export function App() { return (<View><Box /></View>); }`),
      ).toThrow(/cannot statically resolve identifier/);
    }
  });
});

describe('AOT strings where a number belongs', () => {
  const IMPORTS = `import {useState} from 'react';
import {View, Text, Pressable, Switch} from 'embedded-react';
`;
  it('rejects arithmetic other than + on a string', () => {
    for (const operator of ['-', '*', '/', '%']) {
      expect(() =>
        gen(
          `${IMPORTS}export function App() { const [n] = useState(1); return (<Text>{'5' ${operator} n}</Text>); }`,
        ),
      ).toThrow(new RegExp(`"\\${operator}" on a string is not supported`));
    }
  });
});

// The global `undefined` folds like any constant — text needs it (`s + undefined` is "…undefined") — so
// every TYPED slot has to refuse it (and null / NaN) itself, with a location, or it reaches the package as `NaN`.
describe('AOT typed slots refuse nothing-values', () => {
  const IMPORTS = `import {useState, useRef} from 'react';
import {View, Text, useAnimatedValue} from 'embedded-react';
`;
  const err = source => {
    try {
      gen(source);
    } catch (error) {
      return error;
    }
    throw new Error('expected a compile error');
  };
  it.each([
    [
      'useState(undefined)',
      'const [n] = useState(undefined);',
      /initial value of state "n" must be a compile-time constant/,
    ],
    [
      'useState(null)',
      'const [n] = useState(null);',
      /initial value of state "n" is null/,
    ],
    [
      'useState(NaN)',
      'const [n] = useState(0 / 0);',
      /initial value of state "n" is NaN/,
    ],
    [
      'useAnimatedValue(undefined)',
      'const v = useAnimatedValue(undefined);',
      /initial value of useAnimatedValue "v" must be a compile-time constant/,
    ],
    [
      'useRef(undefined)',
      'const r = useRef(undefined);',
      /useRef initial for "r"/,
    ],
    ['useRef(NaN)', 'const r = useRef(0 / 0);', /useRef initial for "r"/],
  ])('%s is a located error', (_name, declaration, pattern) => {
    const error = err(
      `${IMPORTS}export function App() { ${declaration} return (<Text>x</Text>); }`,
    );
    expect(error.message).toMatch(pattern);
    expect(error.aotLoc).toBeTruthy();
  });
});

// State, refs and animated values are runtime bindings. They follow the rule every other binding already
// did — beat a same-named module const in EVERY fold, not only the ones that consult env — which went
// unenforced for them: a style, prop, conditional or svg attribute silently used the module value.
describe('AOT hook bindings beat module consts', () => {
  const IMPORTS = `import {useState, useRef} from 'react';
import {View, Text, Svg, Circle} from 'embedded-react';
`;
  // A dynamic child const is unsupported, as it is in App — so it must fail loudly, never quietly fall
  // back to the module const it shadows.
  it("a child's dynamic const is a located error, never the module value", () => {
    const child = body =>
      `${IMPORTS}const k = 99;
      function C({n}) { const k = n * 2; ${body} }
      export function App() { const [n] = useState(3); return (<View><C n={n} /></View>); }`;
    expect(() => gen(child(`return (<Text>{'k=' + k}</Text>);`))).toThrow(
      /cannot resolve identifier "k"/,
    );
    expect(() =>
      gen(child(`const [v] = useState(k); return (<Text>{v}</Text>);`)),
    ).toThrow(/initial value of state "v" must be a compile-time constant/);
  });
});

describe('AOT a string where a style needs a number', () => {
  it('a string state cannot drive a numeric style', () => {
    expect(() =>
      gen(`${PREAMBLE}export function App() {
        const [radius] = useState('10');
        return (<View style={{borderTopLeftRadius: radius}} />);
      }`),
    ).toThrow(/style "borderTopLeftRadius" needs a number/);
  });
});

describe('AOT name shadowing', () => {
  const IMPORTS = `import {useState, useCallback} from 'react';
import {View, Text, Pressable} from 'embedded-react';
`;

  it('a .map parameter shadows a callback of the same name', () => {
    expect(() =>
      gen(`${IMPORTS}export function App() {
        const [n, setN] = useState(0);
        const onTap = useCallback(() => setN(n + 1), [n]);
        return (<View>{['a'].map(onTap => <Pressable key={onTap} onPress={onTap}><Text>x</Text></Pressable>)}</View>);
      }`),
    ).toThrow(/onPress must be an inline function/);
  });

  it('a hook binding shadows a module const before the local consts fold', () => {
    expect(() =>
      gen(`${IMPORTS}const r = 10;
        export function App() { const [r] = useState(4); const copy = r; return (<Text>{copy}</Text>); }`),
    ).toThrow(/cannot resolve identifier "copy"/);
    expect(() =>
      gen(`${IMPORTS}const r = 10;
        function C() { const [r] = useState(4); const copy = r; return (<Text>{copy}</Text>); }
        export function App() { return (<View><C /></View>); }`),
    ).toThrow(/cannot resolve identifier "copy"/);
  });

  it("App's dynamic const is a located error, never the module value it shadows", () => {
    const source = use =>
      `${IMPORTS}const L = 'module';
      export function App() { const [n] = useState(3); const L = n; return (${use}); }`;
    for (const use of [
      `<Text>{'L=' + L}</Text>`,
      `<View style={{width: L}} />`,
    ])
      expect(() => gen(source(use))).toThrow(/cannot resolve identifier "L"/);
  });
});

describe('AOT unary arithmetic on a string', () => {
  it.each(['+', '-'])('rejects unary %s on a string', operator => {
    expect(() =>
      gen(
        `${PREAMBLE}export function App() { const [label] = useState('5'); return (<Text>{'v=' + (${operator}label)}</Text>); }`,
      ),
    ).toThrow(new RegExp(`unary "\\${operator}" on a string is not supported`));
  });
});

// An attribute that FOLDS to undefined — a prop a child was never given — is omitted like a literal
// {undefined}, as in Flow A. Read as a value, `visible` hid the node and `placeholder` printed the word
// "undefined"; both did so on master too.
describe('AOT props that fold to undefined are omitted', () => {
  const IMPORTS = `import {useState} from 'react';
import {View, Text, Pressable, TextInput} from 'embedded-react';
`;
  const withChild = child =>
    gen(`${IMPORTS}${child}
      export function App() { return (<View><C /></View>); }`);

  it('placeholder from an absent prop is not the word "undefined"', () => {
    const compiled = withChild(
      `function C({x}) { const [v, setV] = useState(''); return (<TextInput value={v} placeholder={x} onChangeText={t => setV(t)} />); }`,
    );
    expect(compiled).not.toContain('"undefined"');
  });

  it('a handler from an absent prop is simply not attached', () => {
    const compiled = withChild(
      `function C({x}) { return (<Pressable onPress={x}><Text>t</Text></Pressable>); }`,
    );
    expect(compiled).not.toContain('ER_EVENT_PRESS,');
  });
});

describe('AOT delayLongPress', () => {
  const IMPORTS = `import { View, Text, Pressable, TouchableOpacity } from 'embedded-react';
`;

  it('rejects a value that does not fold', () => {
    expect(() =>
      gen(`${IMPORTS}import { useState } from 'react';
      export function App() {
        const [ms] = useState(700);
        return (<Pressable delayLongPress={ms} onLongPress={() => {}}><Text>x</Text></Pressable>);
      }`),
    ).toThrow(/delayLongPress> must fold to a number/);
  });
});

describe('AOT string concatenation in text', () => {
  const IMPORTS = `import { useState } from 'react';
import { View, Text, TextInput } from 'embedded-react';
`;

  // `on && 'yes'` is 'yes' or false in JS, and a package's && only has 0/1 — neither value survives. Printing
  // "1" (or relabelling it "true") is a wrong answer either way, so text refuses the shape instead.
  const textErr = body => {
    try {
      gen(`${IMPORTS}
      export function App() {
        const [n, setN] = useState(0);
        const [m, setM] = useState(1);
        const [on, setOn] = useState(false);
        return (${body});
      }`);
    } catch (error) {
      return error;
    }
    throw new Error('expected a compile error');
  };

  it.each([
    ['&& yielding a string', `<Text>{'x: ' + (on && 'yes')}</Text>`],
    ['&& as a bare child', `<Text>{on && 'yes'}</Text>`],
    ['|| yielding a string', `<Text>{'x: ' + (on || 'no')}</Text>`],
    ['&& of two numbers', `<Text>{'x: ' + (n && m)}</Text>`],
  ])('refuses %s in text, with a location', (_name, body) => {
    const error = textErr(body);
    expect(error.message).toMatch(/evaluates to one of its operands/);
    expect(error.aotLoc).toBeTruthy();
    expect(error.message).toContain('^');
  });

  // `(ok ? 1 : "none")` has no single type.
  it.each([
    ['numeric then string', `on ? 1 : 'none'`],
    ['string then numeric', `on ? 'none' : 1`],
  ])('refuses a ternary mixing %s', (_name, expression) => {
    const error = textErr(`<Text>{'x: ' + (${expression})}</Text>`);
    expect(error.message).toMatch(
      /cannot mix a string branch with a numeric one/,
    );
    expect(error.aotLoc).toBeTruthy();
  });

  // A branch that concatenates would need a format of its own, and a text has one. The generic
  // "not supported in this position" used to fire first, with a hint that text buffers are fine — which
  // is exactly wrong inside a text buffer.
  it('names a ternary whose branch concatenates, instead of the generic position error', () => {
    const error = textErr(`<Text>{'x: ' + (on ? 'a' + n : 'b')}</Text>`);
    expect(error.message).toMatch(
      /ternary in text cannot concatenate inside its test or a branch/,
    );
    expect(error.message).not.toMatch(/not supported in this position/);
    expect(error.aotLoc).toBeTruthy();
  });

  it('names a logical whose operand concatenates, and a ternary whose test does', () => {
    const logicalError = textErr(`<Text>{on && ('n=' + n)}</Text>`);
    expect(logicalError.message).toMatch(
      /"&&" in text cannot concatenate inside an operand/,
    );
    const ternaryError = textErr(`<Text>{('a' + n) ? 'x' : 'y'}</Text>`);
    expect(ternaryError.message).toMatch(
      /cannot concatenate inside its test or a branch/,
    );
  });

  it('refuses a ternary mixing a boolean branch with a non-boolean one', () => {
    const error = textErr(`<Text>{'x: ' + (on ? on : 5)}</Text>`);
    expect(error.message).toMatch(/mixes a boolean branch/);
    expect(error.aotLoc).toBeTruthy();
  });
});

describe('AOT string helper limits on constants', () => {
  // A constant folds with JS itself, but takes the same limits as a runtime value, so changing one to the other
  // never changes whether the app compiles.
  const app = ({module = '', body = '', text}) => `${PREAMBLE}
${module}
export function App() {
  ${body}
  return <Text>{${text}}</Text>;
}`;

  it.each([
    ['in text', {text: '(1.5).toFixed(21)'}, /toFixed\(21\) is out of range/],
    [
      'in a module const',
      {module: "const LABEL = 'a'.padStart(3, 'xy');", text: 'LABEL'},
      /fill must be one printable ASCII character/,
    ],
    [
      "in a component's const",
      {body: "const label = 'a'.padStart(300);", text: 'label'},
      /padStart\(300\) is longer than any text/,
    ],
    [
      "in a state's initial value",
      {body: 'const [label] = useState((2).toFixed(1.5));', text: 'label'},
      /toFixed's digits must be a compile-time whole number/,
    ],
  ])(
    'refuses one past the limits %s, with a location',
    (_name, parts, message) => {
      let error;
      try {
        gen(app(parts));
      } catch (thrown) {
        error = thrown;
      }
      expect(error?.message).toMatch(message);
      expect(error.aotLoc).toBeTruthy();
    },
  );

  it('still folds one within the limits', () => {
    expect(() =>
      gen(
        app({
          module:
            "const LABEL = `${(1.25).toFixed(1)} ${'7'.padStart(3, '0')}`;",
          text: 'LABEL',
        }),
      ),
    ).not.toThrow();
  });
});

describe('AOT style diagnostics', () => {
  const IMPORTS = `import { useState } from 'react';
import { View, StyleSheet } from 'embedded-react';
`;
  const err = source => {
    try {
      gen(source);
    } catch (error) {
      return error;
    }
    throw new Error('expected a compile error');
  };

  // A key the static lowering does not know used to fall through to the dynamic branch and be reported
  // as state-driven, which sent the author looking for state that isn't there.
  it('names an unknown style key as unknown, not as state-driven', () => {
    const error = err(`${IMPORTS}
      export function App() {
        return (<View style={{transform: [{scale: 2}]}} />);
      }`);
    expect(error.message).toMatch(/style "transform" is not supported/);
    expect(error.message).not.toMatch(/state-driven/);
    expect(error.aotHint).toMatch(/Flow B lowers these:/);
  });

  it('says the same for an unknown key given a state-driven value', () => {
    const error = err(`${IMPORTS}
      export function App() {
        const [n, setN] = useState(0);
        return (<View style={{shadowRadius: n}} />);
      }`);
    expect(error.message).toMatch(/style "shadowRadius" is not supported/);
    expect(error.message).not.toMatch(/state-driven/);
  });

  it('still reports a state-driven value on a known key as state-driven', () => {
    const error = err(`${IMPORTS}
      export function App() {
        const [n, setN] = useState(0);
        return (<View style={{flex: n}} />);
      }`);
    expect(error.message).toMatch(/state-driven value for style "flex"/);
  });

  it('reports a bad value on a known key as a bad value, with a location', () => {
    const error = err(`${IMPORTS}
      export function App() {
        return (<View style={{alignItems: 'baseline'}} />);
      }`);
    expect(error.message).toMatch(/unsupported value for style "alignItems"/);
    expect(error.message).toMatch(/one of auto, flex-start/);
    expect(error.aotLoc).toBeTruthy();
  });

  // DYN_FIELDS/KEYS are plain objects, so `style={{toString: n}}` used to find Object.prototype's method,
  // look like a known key, and emit a prop named `undefined`, from the compiler that is supposed to
  // reject unsupported styles up front.
  it.each(['toString', 'constructor', 'valueOf', 'hasOwnProperty'])(
    'rejects the inherited Object.prototype key "%s" as an unknown style',
    key => {
      const error = err(`${IMPORTS}
      export function App() {
        const [n, setN] = useState(0);
        return (<View style={{${key}: n}} />);
      }`);
      expect(error.message).toMatch(
        new RegExp(`style "${key}" is not supported`),
      );
    },
  );

  it('does not emit an undefined ERProps field for a prototype key', () => {
    expect(() =>
      gen(`${IMPORTS}
      export function App() {
        const [n, setN] = useState(0);
        return (<View style={{toString: n}} />);
      }`),
    ).toThrow();
    // And the static path too, which goes through lowerStyle's own key lookup.
    expect(() =>
      gen(`${IMPORTS}
      export function App() {
        return (<View style={{valueOf: 4}} />);
      }`),
    ).toThrow(/style "valueOf" is not supported/);
  });

  it('rejects an Object.prototype name as an enum value', () => {
    const error = err(`${IMPORTS}
      export function App() {
        return (<View style={{alignItems: 'toString'}} />);
      }`);
    expect(error.message).toMatch(/unsupported value for style "alignItems"/);
  });

  it('rejects an Object.prototype name as a color', () => {
    const error = err(`${IMPORTS}
      export function App() {
        return (<View style={{backgroundColor: 'constructor'}} />);
      }`);
    expect(error.message).toMatch(/unsupported color "constructor"/);
  });

  it('does not resolve an undeclared Object.prototype name as a constant', () => {
    expect(() =>
      gen(`${IMPORTS}
      export function App() {
        return (<View style={{width: toString ? 10 : 20}} />);
      }`),
    ).toThrow(/resolve identifier "toString"/);
  });

  it('rejects an Object.prototype name in a state-driven enum style', () => {
    const error = err(`${IMPORTS}
      export function App() {
        const [on, setOn] = useState(false);
        return (<View style={{alignItems: on ? 'toString' : 'center'}} />);
      }`);
    expect(error.message).toMatch(/unsupported enum value "toString"/);
  });

  it('locates an unknown key that arrived through a StyleSheet', () => {
    const error = err(`${IMPORTS}
      const styles = StyleSheet.create({ card: { shadowOpacity: 0.5 } });
      export function App() {
        return (<View style={styles.card} />);
      }`);
    expect(error.message).toMatch(/style "shadowOpacity" is not supported/);
    expect(error.aotLoc).toBeTruthy();
  });
});

describe('AOT list setters', () => {
  const app = body =>
    gen(`${PREAMBLE}
      export function App() {
        const [k, setK] = useState(-2);
        const [f, setF] = useState(0.5);
        const [items, setItems] = useState([{w: 1}, {w: 2}, {w: 3}]);
        return (<Pressable onPress={() => { ${body} }}><Text>x</Text></Pressable>);
      }`);

  it('refuses a slice that drops items from the front', () => {
    expect(() => app('setItems(items.slice(1, 3));')).toThrow(
      /only items\.slice\(0, end\) is supported/,
    );
    expect(() => app('setItems(items.slice(k));')).toThrow(
      /only items\.slice\(0, end\) is supported/,
    );
  });
});

describe('AOT fixed limits', () => {
  const IMPORTS = `import {useState} from 'react';\nimport {Text, Pressable, View} from 'embedded-react';\n`;
  const small = source =>
    compilePackage(IMPORTS + source, 'test', {listCap: 2, listStrCap: 8});

  it('refuses an initial list with more rows than a list holds', () => {
    expect(() =>
      small(`export function App() {
        const [items] = useState([{v: 1}, {v: 2}, {v: 3}]);
        return (<Text>{items.length}</Text>);
      }`),
    ).toThrow(/list state "items" starts with 3 rows, but a list holds 2/);
  });

  it('refuses an initial value too long for a string built from itself', () => {
    expect(() =>
      small(`export function App() {
        const [label, setLabel] = useState('12345678');
        return (<Pressable onPress={() => setLabel(label + '!')}><Text>{label}</Text></Pressable>);
      }`),
    ).toThrow(
      /state "label" starts 8 bytes long, but a string built from itself holds 7 bytes/,
    );
    expect(() =>
      small(`export function App() {
        const [items, setItems] = useState([{t: 'ok'}, {t: 'much too long'}]);
        return (<View>{items.map((item, index) => <Pressable key={index} onPress={() => setItems([...items, {t: item.t + '!'}])}><Text>{item.t}</Text></Pressable>)}</View>);
      }`),
    ).toThrow(/field "t" of list state "items" starts 13 bytes long/);
  });

  it('sizes a string that is not built from itself to fit its values', () => {
    expect(() =>
      small(`export function App() {
        const [label, setLabel] = useState('12345678');
        return (<Pressable onPress={() => setLabel('much too long')}><Text>{label}</Text></Pressable>);
      }`),
    ).not.toThrow();
  });

  it('refuses a constant string write too long for its buffer', () => {
    expect(() =>
      small(`export function App() {
        const [label, setLabel] = useState('');
        return (<Pressable onPress={() => { setLabel(label + '!'); setLabel('much too long'); }}><Text>{label}</Text></Pressable>);
      }`),
    ).toThrow(
      /state "label" is set to a 13-byte string, but a string built from itself holds 7 bytes/,
    );

    const long = 'x'.repeat(256);
    expect(() =>
      small(`export function App() {
        const [label, setLabel] = useState('');
        return (<Pressable onPress={() => setLabel('${long}')}><Text>{label}</Text></Pressable>);
      }`),
    ).toThrow(
      /state "label" is set to a 256-byte string, but a write holds 255 bytes \(ER_TEXT_MAX\)/,
    );
    expect(() =>
      small(`export function App() {
        const [items, setItems] = useState([{t: ''}]);
        return (<Pressable onPress={() => setItems([...items, {t: '${long}'}])}><Text>x</Text></Pressable>);
      }`),
    ).toThrow(/field "t" of list state "items" is set to a 256-byte string/);
    expect(() =>
      small(`export function App() {
        const [label, setLabel] = useState('');
        return (<Pressable onPress={() => { const t = '${long}'; setLabel(t); }}><Text>{label}</Text></Pressable>);
      }`),
    ).toThrow(/local "t" is set to a 256-byte string/);
  });

  it('keeps its copy of the engine string limits in step with er_scene.h', () => {
    const header = readFileSync(
      new URL('../../../../engine/include/er_scene.h', import.meta.url),
      'utf8',
    );
    for (const [name, max] of Object.entries(ENGINE_STRING_MAX)) {
      expect(header).toMatch(new RegExp(`#define ${name} ${max}\\b`));
    }
  });
});
