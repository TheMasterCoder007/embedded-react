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

import {describe, it, expect} from 'vitest';
import {compilePackage, packPackage} from '../compile.mts';
import {STYLE_KEYS} from '../style-map.mts';
import {
  ELEMENT_TAGS,
  SVG_SHAPE_TAGS,
  elementSpec,
  svgShapeSpec,
} from '../compiler/elements/props.mts';

// Every element declares its props, style keys and children. Each declared prop must change the package it
// compiles to (or be listed here as lowering to nothing, with why); anything undeclared must be an AOT: error.

const PRE = `import {useState, useRef} from 'react';
import logo from './logo.png';
import knob from './knob.png';
import {View, Text, Svg, Circle, Ellipse, Rect, Line, Arc, G, Path, Pressable, TouchableOpacity, Image,
  ScrollView, Switch, TextInput, ActivityIndicator, Modal, Dial, FlatList, Animated, useAnimatedValue}
  from 'embedded-react';
`;

/** An app whose App returns `jsx`, with state `n`, an animated value `a` and a node ref `r` in reach. */
const app = jsx =>
  `${PRE}export function App() {
  const [n, setN] = useState(0);
  const a = useAnimatedValue(0);
  const r = useRef(null);
  return ${jsx};
}`;

/** The package an app compiles to, as text to compare. */
const gen = jsx =>
  Buffer.from(
    packPackage(
      compilePackage(app(jsx), 'App.jsx', {
        screen: {width: 240, height: 320},
        release: true,
      }),
    ).bytes,
  ).toString('hex');

const HANDLER = '{() => setN(1)}';
const GRADIENT =
  "{{type: 1, stops: [{offset: 0, color: '#f00'}, {offset: 1, color: '#00f'}]}}";

/** The host-node props, as attributes that each change the package. */
const HOST = {
  style: 'style={{marginLeft: 3}}',
  visible: 'visible={false}',
  delayLongPress: 'delayLongPress={900}',
  onPress: `onPress=${HANDLER}`,
  onLongPress: `onLongPress=${HANDLER}`,
  onPressIn: `onPressIn=${HANDLER}`,
  onPressOut: `onPressOut=${HANDLER}`,
  onTouchStart: `onTouchStart=${HANDLER}`,
  onTouchMove: `onTouchMove=${HANDLER}`,
  onTouchEnd: `onTouchEnd=${HANDLER}`,
  onLayout: `onLayout=${HANDLER}`,
};

/** The paint every shape takes. */
const PAINT = {
  fill: 'fill="#f00"',
  stroke: 'stroke="#f00"',
  strokeWidth: 'strokeWidth={3}',
  strokeLinecap: 'strokeLinecap="round"',
  strokeLinejoin: 'strokeLinejoin="round"',
  strokeMiterlimit: 'strokeMiterlimit={9}',
  fillRule: 'fillRule="evenodd"',
  fillGrad: `fillGrad=${GRADIENT}`,
  strokeGrad: `strokeGrad=${GRADIENT}`,
};

/**
 * Per element: the attributes every compile carries, its children, and each declared prop as the attribute
 * that sets it, or a complete [without, with] pair of attribute strings when it needs other attributes to show.
 */
const SAMPLES = {
  View: {props: HOST},
  ScrollView: {props: HOST},
  Pressable: {
    props: {
      ...HOST,
      disabled: [`onPress=${HANDLER}`, `disabled onPress=${HANDLER}`],
    },
  },
  TouchableOpacity: {
    props: {
      ...HOST,
      disabled: 'disabled',
      activeOpacity: 'activeOpacity={0.5}',
    },
  },
  Text: {
    children: 'hi',
    props: {
      ...HOST,
      numberOfLines: 'numberOfLines={2}',
      ellipsizeMode: 'ellipsizeMode="clip"',
    },
  },
  Image: {
    props: {
      ...HOST,
      source: 'source="logo"',
      imageName: 'imageName="logo"',
      resizeMode: 'resizeMode="contain"',
      tintColor: 'tintColor="#f00"',
    },
  },
  Switch: {
    props: {
      style: 'style={{marginLeft: 3}}',
      value: 'value={true}',
      onValueChange: [
        'value={n > 0}',
        'value={n > 0} onValueChange={v => setN(v ? 1 : 0)}',
      ],
      trackColor: "trackColor={{true: '#f00'}}",
      thumbColor: 'thumbColor="#f00"',
    },
  },
  TextInput: {
    props: {
      style: 'style={{marginLeft: 3}}',
      value: 'value="x"',
      defaultValue: 'defaultValue="x"',
      onChangeText: 'onChangeText={text => setN(1)}',
      onSubmitEditing: `onSubmitEditing=${HANDLER}`,
      onFocus: `onFocus=${HANDLER}`,
      onBlur: `onBlur=${HANDLER}`,
      placeholder: 'placeholder="x"',
      placeholderTextColor: 'placeholderTextColor="#f00"',
      cursorColor: 'cursorColor="#f00"',
      editable: 'editable={false}',
      secureTextEntry: 'secureTextEntry',
    },
  },
  ActivityIndicator: {
    props: {
      style: 'style={{marginLeft: 3}}',
      color: 'color="#f00"',
      size: 'size="small"',
      animating: 'animating={false}',
    },
  },
  Modal: {
    base: 'visible',
    props: {
      style: 'style={{marginLeft: 3}}',
      visible: ['visible', 'visible={n > 0}'],
      backdropColor: 'backdropColor="#f00"',
    },
  },
  Dial: {
    props: {
      style: 'style={{marginLeft: 3}}',
      value: 'value={5}',
      min: 'min={1}',
      max: 'max={50}',
      startAngle: 'startAngle={10}',
      sweepAngle: 'sweepAngle={200}',
      step: 'step={2}',
      thickness: 'thickness={9}',
      bandThickness: 'bandThickness={3}',
      trackColor: 'trackColor="#f00"',
      indicatorColor: 'indicatorColor="#f00"',
      indicatorGradient:
        "indicatorGradient={{type: 'conic', stops: [{color: '#f00'}, {color: '#00f'}]}}",
      bandColor: 'bandColor="#f00"',
      cap: 'cap="round"',
      segments: 'segments={4}',
      gapAngle: 'gapAngle={5}',
      knob: 'knob="circle"',
      knobSize: 'knobSize={9}',
      minSpan: 'minSpan={3}',
      knobColor: 'knobColor="#f00"',
      knobBorderColor: 'knobBorderColor="#f00"',
      knobBorderWidth: 'knobBorderWidth={2}',
      knobImage: 'knobImage="knob"',
      adjustable: 'adjustable',
      range: 'range',
      valueStart: 'valueStart={2}',
      onChange: 'onChange={value => setN(value)}',
    },
  },
  FlatList: {
    base: 'data={[1]} renderItem={({item}) => <Text>{item}</Text>}',
    props: {
      data: [
        'data={[]} renderItem={({item}) => <Text>{item}</Text>}',
        'data={[1]} renderItem={({item}) => <Text>{item}</Text>}',
      ],
      renderItem: [
        'data={[1]} renderItem={({item}) => <Text>{item}</Text>}',
        'data={[1]} renderItem={({item}) => <View />}',
      ],
      style: 'style={{marginLeft: 3}}',
    },
  },
  Svg: {
    children: '<Circle r={4} />',
    props: {
      style: 'style={{marginLeft: 3}}',
      width: 'width={10}',
      height: 'height={10}',
      viewBox: [
        'width={10} height={10}',
        'width={10} height={10} viewBox="0 0 5 5"',
      ],
      ...PAINT,
    },
  },
};

/** Declared props that lower to nothing on purpose, with why. */
const LOWERED_TO_NOTHING = {
  'Switch.disabled': 'only disabled={false} lowers, which is the default',
  'Modal.animationType':
    "only 'none' lowers, which is what the engine does anyway",
  'Modal.onRequestClose': 'nothing on a panel asks a Modal to close',
  'Modal.statusBarTranslucent': 'there is no status bar',
  'FlatList.keyExtractor':
    'Flow B unrolls the rows at build time; there is no reconciler to key',
  'Svg.source': 'needs a baked .svg artifact; the <Svg source> tests cover it',
  ...Object.fromEntries(
    [
      'View',
      'ScrollView',
      'Pressable',
      'TouchableOpacity',
      'Text',
      'Image',
      'Switch',
      'TextInput',
      'ActivityIndicator',
      'Modal',
      'Dial',
      'Svg',
    ].map(tag => [
      `${tag}.ref`,
      'a node ref takes no room until updateVector reads it',
    ]),
  ),
};

/** Per shape: the geometry every compile carries, and each declared prop as an attribute or a pair. */
const SHAPES = {
  Path: {base: '', props: {d: ['d="M0 0 L5 5"', 'd="M0 0 L6 6"']}},
  Circle: {
    base: 'r={4}',
    props: {cx: 'cx={6}', cy: 'cy={6}', r: ['', 'r={4}']},
  },
  Ellipse: {
    base: 'rx={4} ry={3}',
    props: {
      cx: 'cx={6}',
      cy: 'cy={6}',
      rx: ['ry={3}', 'rx={4} ry={3}'],
      ry: ['rx={4}', 'rx={4} ry={3}'],
    },
  },
  Rect: {
    base: 'width={5} height={5}',
    props: {
      x: 'x={1}',
      y: 'y={1}',
      width: ['height={5}', 'width={5} height={5}'],
      height: ['width={5}', 'width={5} height={5}'],
      rx: 'rx={1}',
      ry: 'ry={1}',
    },
  },
  Line: {
    base: 'x2={5} y2={5}',
    props: {
      x1: 'x1={1}',
      y1: 'y1={1}',
      x2: ['y2={5}', 'x2={5} y2={5}'],
      y2: ['x2={5}', 'x2={5} y2={5}'],
    },
  },
  Arc: {
    base: 'r={4} endAngle={90}',
    props: {
      cx: 'cx={6}',
      cy: 'cy={6}',
      startAngle: 'startAngle={10}',
      r: ['endAngle={90}', 'r={4} endAngle={90}'],
      endAngle: ['r={4}', 'r={4} endAngle={90}'],
    },
  },
  G: {
    base: '',
    children: '<Circle r={4} />',
    props: {
      x: 'x={1}',
      y: 'y={1}',
      translateX: 'translateX={1}',
      translateY: 'translateY={1}',
      scale: 'scale={2}',
    },
  },
};

/**
 * The [without, with] attribute strings for one sample.
 *
 * @param sample  An attribute, added to `base`, or a complete pair.
 * @param base  The attributes every compile carries.
 */
const pairOf = (sample, base = '') =>
  Array.isArray(sample) ? sample : [base, `${base} ${sample}`];

describe('declared element props', () => {
  it('has a sample for every declared prop, and nothing undeclared', () => {
    expect(Object.keys(SAMPLES).sort()).toEqual([...ELEMENT_TAGS].sort());
    for (const tag of ELEMENT_TAGS) {
      const sampled = [
        ...Object.keys(SAMPLES[tag].props),
        ...Object.keys(LOWERED_TO_NOTHING)
          .filter(key => key.startsWith(`${tag}.`))
          .map(key => key.slice(tag.length + 1)),
      ];
      expect(sampled.sort(), tag).toEqual([...elementSpec(tag).props].sort());
    }

    expect(Object.keys(SHAPES).sort()).toEqual([...SVG_SHAPE_TAGS].sort());
    for (const tag of SVG_SHAPE_TAGS) {
      expect(
        [...Object.keys(SHAPES[tag].props), ...Object.keys(PAINT)].sort(),
        tag,
      ).toEqual([...svgShapeSpec(tag).props].sort());
    }
  });

  for (const tag of ELEMENT_TAGS) {
    const {base, children = '', props} = SAMPLES[tag];
    const jsx = attrs => `<${tag} ${attrs}>${children}</${tag}>`;
    it.each(Object.entries(props))(`<${tag}> lowers %s`, (name, sample) => {
      const [without, withProp] = pairOf(sample, base);
      expect(gen(jsx(withProp))).not.toBe(gen(jsx(without)));
    });
  }

  for (const tag of SVG_SHAPE_TAGS) {
    const {base, children = '', props} = SHAPES[tag];
    const jsx = attrs =>
      `<Svg width={20} height={20}><${tag} ${attrs}>${children}</${tag}></Svg>`;
    it.each(Object.entries({...props, ...PAINT}))(
      `<${tag}> lowers %s`,
      (name, sample) => {
        const [without, withProp] = pairOf(
          sample,
          tag === 'Path' && !Array.isArray(sample) ? 'd="M0 0 L5 5"' : base,
        );
        expect(gen(jsx(withProp))).not.toBe(gen(jsx(without)));
      },
    );
  }

  it('refuses the props Flow A or React Native know, with what to do instead', () => {
    expect(() => gen('<TextInput autoFocus />')).toThrow(
      /prop "autoFocus" is not supported[\s\S]*hint: the engine's TextInput has no such setting/,
    );
    expect(() => gen('<Modal visible transparent />')).toThrow(
      /prop "transparent" is not supported[\s\S]*hint: the engine always draws a backdrop/,
    );
    expect(() => gen('<ScrollView onScroll={() => setN(1)} />')).toThrow(
      /prop "onScroll" is not supported/,
    );
    expect(() =>
      gen('<Svg width={9} height={9} onPress={() => setN(1)} />'),
    ).toThrow(
      /prop "onPress" is not supported[\s\S]*hint: wrap it in a <Pressable>/,
    );
  });
});

describe('declared style keys', () => {
  it('gives every style key the AOT lowers to at least one element', () => {
    const taken = new Set(
      ELEMENT_TAGS.flatMap(tag => [...elementSpec(tag).styleKeys]),
    );
    expect([...taken].sort()).toEqual([...STYLE_KEYS, 'transform'].sort());
  });
});

describe('a baked <Svg source>', () => {
  const artifact = {
    ops: [0, 0, 1, 0, 0, 2, 10, 0, 2, 10, 10, 6],
    paints: [0, 0, 4, 4, 0, 0, 0, 0, 0],
    width: 10,
    height: 10,
  };
  const baked = attrs =>
    compilePackage(
      `${PRE}import art from './art.svg';
export function App() { return <Svg source={art} ${attrs} />; }`,
      'App.jsx',
      {svgArtifacts: {art: artifact}},
    ).c;

  it('refuses a percentage style size, which it cannot bake', () => {
    expect(() =>
      baked("width={20} height={20} style={{width: '50%'}}"),
    ).toThrow(/percentage style width on an <Svg source> is not supported/);
  });
});
