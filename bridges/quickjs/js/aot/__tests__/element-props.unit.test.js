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
import {compileSource} from '../compile.mts';
import {STYLE_KEYS} from '../style-map.mts';
import {
  ELEMENT_TAGS,
  SVG_SHAPE_TAGS,
  elementSpec,
  svgShapeSpec,
} from '../compiler/elements/props.mts';

// Every element declares its props, style keys and children. Each declared prop must change the C it lowers
// to (or be listed here as lowering to nothing, with why); anything undeclared must be an AOT: error.

const PRE = `import {useState, useRef} from 'react';
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

/** The C an app compiles to. */
const gen = jsx => compileSource(app(jsx), 'App.jsx').c;

/** The message of what compiling an app throws. */
function errorOf(jsx) {
  try {
    gen(jsx);
  } catch (error) {
    return error.message.split('\n')[0];
  }
  throw new Error('expected an AOT error');
}

const HANDLER = '{() => setN(1)}';
const GRADIENT =
  "{{type: 1, stops: [{offset: 0, color: '#f00'}, {offset: 1, color: '#00f'}]}}";

/** The host-node props, as attributes that each change the C. */
const HOST = {
  style: 'style={{marginLeft: 3}}',
  visible: 'visible={false}',
  ref: 'ref={r}',
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
      ref: 'ref={r}',
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
      ref: 'ref={r}',
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
      ref: 'ref={r}',
      color: 'color="#f00"',
      size: 'size="small"',
      animating: 'animating={false}',
    },
  },
  Modal: {
    base: 'visible',
    props: {
      style: 'style={{marginLeft: 3}}',
      ref: 'ref={r}',
      visible: ['visible', 'visible={n > 0}'],
      backdropColor: 'backdropColor="#f00"',
    },
  },
  Dial: {
    props: {
      style: 'style={{marginLeft: 3}}',
      ref: 'ref={r}',
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
      ref: 'ref={r}',
      width: 'width={10}',
      height: 'height={10}',
      viewBox: [
        'width={10} height={10}',
        'width={10} height={10} viewBox="0 0 5 5"',
      ],
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

  it('refuses an undeclared prop whose value is undefined', () => {
    expect(errorOf('<View bogus={undefined} />')).toBe(
      'AOT: <View> prop "bogus" is not supported',
    );
    expect(errorOf('<TextInput autoFocus={undefined} />')).toBe(
      'AOT: <TextInput> prop "autoFocus" is not supported',
    );
  });

  it('refuses an undeclared prop on every element, at the prop', () => {
    for (const tag of ELEMENT_TAGS) {
      expect(errorOf(`<View><${tag} bogus={1} /></View>`), tag).toBe(
        `AOT: <${tag}> prop "bogus" is not supported`,
      );
    }

    for (const tag of SVG_SHAPE_TAGS) {
      expect(
        errorOf(`<Svg width={9} height={9}><${tag} bogus={1} /></Svg>`),
        tag,
      ).toBe(`AOT: <${tag}> prop "bogus" is not supported`);
    }

    expect(() => gen('<View\n  bogus={1} />')).toThrow(/App\.jsx:10:3/);
  });

  it('takes the RN props that wrap a missing OS service, except on <FlatList>', () => {
    for (const tag of ELEMENT_TAGS.filter(tag => tag !== 'FlatList')) {
      const {base = '', children = ''} = SAMPLES[tag];
      const jsx = attrs => `<${tag} ${base} ${attrs}>${children}</${tag}>`;
      expect(gen(jsx('testID="x" accessibilityLabel="y"')), tag).toBe(
        gen(jsx('')),
      );
    }

    expect(gen('<Text>a<Text testID="x">b</Text></Text>')).toBe(
      gen('<Text>a<Text>b</Text></Text>'),
    );
    expect(
      errorOf(
        '<FlatList data={[1]} renderItem={({item}) => <Text>{item}</Text>} testID="list" />',
      ),
    ).toBe('AOT: <FlatList> prop "testID" is not supported');
  });

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
    expect(() => gen('<Svg width={9} height={9} fill="red" />')).toThrow(
      /prop "fill" is not supported[\s\S]*hint: paint goes on the shapes/,
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

  it.each([
    ['opacity', '<Text style={{opacity: 0.5}}>x</Text>', 'Text'],
    [
      'an animated opacity',
      '<Animated.Text style={{opacity: a}}>x</Animated.Text>',
      'Text',
    ],
    [
      'backgroundColor',
      "<Text style={{backgroundColor: '#fff'}}>x</Text>",
      'Text',
    ],
    ['color', "<View style={{color: '#fff'}} />", 'View'],
    [
      'borderRadius',
      '<Image source="logo" style={{borderRadius: 4}} />',
      'Image',
    ],
    ['fontSize', '<Switch style={{fontSize: 4}} />', 'Switch'],
    [
      'a state-driven color',
      "<View style={{color: n ? '#fff' : '#000'}} />",
      'View',
    ],
  ])(
    'refuses %s on an element the engine does not draw it on',
    (what, jsx, tag) => {
      expect(errorOf(jsx)).toMatch(new RegExp(`has no effect on <${tag}>$`));
    },
  );

  it('checks a style reference key by key, and lets a null value through', () => {
    expect(() =>
      compileSource(
        `${PRE}const S = {color: '#fff'};\nexport function App() { return <View style={S} />; }`,
        'App.jsx',
      ),
    ).toThrow(/style "color" has no effect on <View>/);
    expect(gen('<Text style={{opacity: null}}>x</Text>')).toBe(
      gen('<Text>x</Text>'),
    );
  });

  it('takes only the span keys on a nested <Text>, and no other props', () => {
    expect(
      errorOf('<Text>a<Text style={{marginLeft: 4}}>b</Text></Text>'),
    ).toBe('AOT: style "marginLeft" has no effect on <Text>');
    expect(
      errorOf('<Text>a<Text onPress={() => setN(1)}>b</Text></Text>'),
    ).toBe('AOT: <Text> prop "onPress" is not supported');
  });
});

describe('declared children', () => {
  it.each(['Image', 'Switch', 'TextInput', 'ActivityIndicator'])(
    'refuses children on <%s>',
    tag => {
      expect(errorOf(`<${tag}><View /></${tag}>`)).toBe(
        `AOT: <${tag}> takes no children`,
      );
    },
  );

  it('refuses text outside a <Text>, and skips the whitespace between tags', () => {
    expect(errorOf('<View>hello</View>')).toBe(
      'AOT: text "hello" must be inside a <Text>',
    );
    expect(gen('<View>\n  <View />\n</View>')).toBe(
      gen('<View><View /></View>'),
    );
  });

  it('inlines a fragment child, in a container and in a <Text>', () => {
    expect(gen('<View><><Text>a</Text><Text>b</Text></></View>')).toBe(
      gen('<View><Text>a</Text><Text>b</Text></View>'),
    );
    expect(gen('<Text>a<>b</></Text>')).toBe(gen('<Text>ab</Text>'));
  });

  it('builds every branch of a nested ternary, each shown only while chosen', () => {
    const c = gen(
      '<View>{n === 0 ? <Text>A</Text> : n === 1 ? <Text>B</Text> : <Text>C</Text>}</View>',
    );
    expect(c.match(/er_node_create\(ER_NODE_TEXT\)/g)).toHaveLength(3);
    expect(c).toContain(
      'p.display = ((((!((s_state.n == 0))) && ((s_state.n == 1)))) ? ER_DISPLAY_FLEX : ER_DISPLAY_NONE);',
    );
    expect(c).toContain(
      'p.display = ((((!((s_state.n == 0))) && (!((s_state.n == 1))))) ? ER_DISPLAY_FLEX : ER_DISPLAY_NONE);',
    );
  });

  it("passes a fragment and text in a component's children through to where it renders them", () => {
    const withCard = jsx =>
      compileSource(
        `${PRE}function Card({children}) { return <View>{children}</View>; }\n` +
          `export function App() { return ${jsx}; }`,
        'App.jsx',
      ).c;
    expect(withCard('<Card><><Text>a</Text><Text>b</Text></></Card>')).toBe(
      withCard('<Card><Text>a</Text><Text>b</Text></Card>'),
    );
    expect(withCard('<Card />')).toBe(withCard('<Card></Card>'));
    expect(() => withCard('<Card>hello</Card>')).toThrow(
      /text "hello" must be inside a <Text>/,
    );
  });

  it('builds nothing for a constant branch that renders nothing', () => {
    expect(gen('<View>{false ? <Text>A</Text> : null}</View>')).toBe(
      gen('<View />'),
    );
  });

  it('refuses a branch that is neither an element nor nothing', () => {
    expect(errorOf("<View>{n ? <Text>A</Text> : 'oops'}</View>")).toBe(
      'AOT: a non-element expression child ("oops") cannot render here',
    );
  });

  it('refuses a state-driven number on the left of `&&`, which renders 0 as text', () => {
    expect(errorOf('<View>{n && <Text>A</Text>}</View>')).toBe(
      'AOT: a number on the left of `&&` renders 0 as text when it is 0',
    );
    expect(gen('<View>{n > 0 && <Text>A</Text>}</View>')).toContain(
      'p.display = (((s_state.n > 0)) ? ER_DISPLAY_FLEX : ER_DISPLAY_NONE);',
    );
  });

  it('refuses a constant `&&` whose falsy value React would draw as text', () => {
    expect(errorOf('<View>{0 && <Text>A</Text>}</View>')).toBe(
      'AOT: a non-element expression child (0) cannot render here',
    );
    for (const left of ['false', 'null', "''"]) {
      expect(gen(`<View>{${left} && <Text>A</Text>}</View>`), left).toBe(
        gen('<View />'),
      );
    }
  });
});

describe('values a declared prop cannot lower', () => {
  it.each([
    [
      'a static transform beside an animated one',
      "<View style={{transform: [{scale: a}, {rotate: '45deg'}]}} />",
      /not an animated value cannot sit beside an animated one/,
    ],
    [
      'an animated rotateX',
      '<View style={{transform: [{rotateX: a}]}} />',
      /transform "rotateX" cannot be animated/,
    ],
    [
      'a percentage <Svg width> with a viewBox',
      '<Svg width="50%" height={9} viewBox="0 0 9 9" />',
      /<Svg width> must be a number of pixels/,
    ],
    [
      'a state-driven <Svg width>',
      '<Svg width={n} height={9}><Circle r={n} /></Svg>',
      /state-driven <Svg width> is not supported/,
    ],
    [
      'a state-driven style on an <Svg>',
      '<Svg width={9} height={9} style={{marginLeft: n}} />',
      /state-driven style on an <Svg>/,
    ],
    [
      'ellipsizeMode="head"',
      '<Text numberOfLines={1} ellipsizeMode="head">x</Text>',
      /"head" is not drawn by the engine/,
    ],
    [
      'a fractional numberOfLines',
      '<Text numberOfLines={1.5}>x</Text>',
      /whole number from 0 to 255/,
    ],
    [
      'five <Dial> gradient stops',
      "<Dial indicatorGradient={{type: 'conic', stops: [{color: '#f00'}, {color: '#0f0'}, {color: '#00f'}, {color: '#ff0'}, {color: '#0ff'}]}} />",
      /takes at most 4 stops \(got 5\)/,
    ],
    [
      'a non-string <Dial> color',
      '<Dial trackColor={5} />',
      /<Dial trackColor> must be a color string/,
    ],
    [
      'a non-number <Dial> number',
      '<Dial min="0" />',
      /<Dial min> must be a number/,
    ],
    [
      'an infinite whole <Dial> number',
      '<Dial segments={1 / 0} />',
      /folded to Infinity/,
    ],
    [
      'a disabled <Switch>',
      '<Switch value={false} disabled />',
      /disabled <Switch> is not supported/,
    ],
    [
      'a malformed trackColor',
      "<Switch trackColor={{on: '#fff'}} />",
      /must be an object of two colors/,
    ],
    [
      "size='medium'",
      '<ActivityIndicator size="medium" />',
      /'small', 'large' or a number of pixels/,
    ],
    [
      "animationType='slide'",
      '<Modal visible animationType="slide" />',
      /animationType> "slide" is not supported/,
    ],
    [
      'a state-driven tintColor',
      "<Image source=\"logo\" tintColor={n ? '#fff' : '#000'} />",
      /tintColor> must be a constant color/,
    ],
    [
      'a one-stop static shape gradient',
      "<Svg width={9} height={9}><Circle r={4} fillGrad={{type: 1, stops: [{offset: 0, color: '#f00'}]}} /></Svg>",
      /"fillGrad.stops" needs 2\.\.8 entries \(got 1\)/,
    ],
    [
      'a shape an <Svg> cannot draw',
      '<Svg width={9} height={9}><Polygon /></Svg>',
      /<Polygon> is not a shape an <Svg> draws/,
    ],
  ])('refuses %s', (what, jsx, message) => {
    expect(errorOf(jsx)).toMatch(message);
  });

  it('lets a style size win over the <Svg> width and height, in either form', () => {
    const c = gen(
      '<Svg width="50%" height={40} style={{width: 30, height: \'25%\'}}><Circle r={4} /></Svg>',
    );
    expect(c).not.toContain('p.width_pct = 50.0f;');
    expect(c).not.toContain('p.height = (int16_t)40;');
    expect(c).toContain('p.width = 30;');
    expect(c).toContain('p.height_pct = 25.0f;');
  });

  it('lowers a percentage <Svg> size, and refuses a numeric string', () => {
    expect(
      gen('<Svg width="50%" height={40}><Circle r={4} /></Svg>'),
    ).toContain('p.width_pct = 50.0f;\n    p.height = (int16_t)40;');
    expect(errorOf('<Svg width={9} height="40" />')).toBe(
      'AOT: <Svg height> must be a number of pixels or a percentage (got "40")',
    );
  });

  it('keeps the gradients of a constant <Svg>', () => {
    expect(
      gen(
        `<Svg width={20} height={20}><Circle r={4} fillGrad=${GRADIENT} /></Svg>`,
      ),
    ).toContain('s_svg0_paints, 1, s_svg0_grads, 1);');
  });

  it('wires the <TextInput> events that carry no value', () => {
    const c = gen(
      `<TextInput onSubmitEditing=${HANDLER} onFocus=${HANDLER} onBlur=${HANDLER} />`,
    );
    for (const event of ['SUBMIT_EDITING', 'FOCUS', 'BLUR']) {
      expect(c).toMatch(
        new RegExp(
          `er_event_set\\(n0, ER_EVENT_${event}, er_handler_\\d, NULL\\);`,
        ),
      );
    }
  });

  it('drops the press handlers of a disabled <Pressable>', () => {
    expect(gen(`<Pressable disabled onPress=${HANDLER} />`)).not.toContain(
      'ER_EVENT_PRESS',
    );
  });

  it('tints an <ActivityIndicator> from style.color, as Flow A does', () => {
    expect(gen("<ActivityIndicator style={{color: '#f00'}} />")).toContain(
      'p.indicator_color = 0xFFFF0000u;',
    );
  });
});

describe('typed elements under a state-driven conditional', () => {
  it('shows a Switch, Dial, TextInput or ActivityIndicator only while it is chosen', () => {
    for (const element of [
      '<Switch />',
      '<Dial />',
      '<TextInput />',
      '<ActivityIndicator />',
    ]) {
      expect(gen(`<View>{n > 0 && ${element}}</View>`), element).toContain(
        'p.display = (((s_state.n > 0)) ? ER_DISPLAY_FLEX : ER_DISPLAY_NONE);',
      );
    }
  });

  it("gates a Modal's own visible flag, which the engine derives its display from", () => {
    expect(gen('<View>{n > 0 && <Modal visible />}</View>')).toContain(
      'p.modal_visible = (((s_state.n > 0)) ? 1 : 0);',
    );
    expect(
      gen('<View>{n > 0 ? <Text>a</Text> : <Modal visible={n < -1} />}</View>'),
    ).toContain(
      'p.modal_visible = ((!((s_state.n > 0))) ? (uint8_t)(((s_state.n < -1)) ? 1 : 0) : 0);',
    );
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
    compileSource(
      `${PRE}import art from './art.svg';
export function App() { return <Svg source={art} ${attrs} />; }`,
      'App.jsx',
      {svgArtifacts: {art: artifact}},
    ).c;

  it('scales the drawing to the style size, which wins over the attribute', () => {
    const tape = c => c.match(/s_svg0_ops\[\] = \{[^}]*\}/)[0];
    const styled = baked(
      'width={20} height={20} style={{width: 30, height: 30}}',
    );
    expect(tape(styled)).toBe(tape(baked('width={30} height={30}')));
    expect(styled).toContain('p.width = 30;');
    expect(styled).not.toContain('(int16_t)20');
  });

  it('refuses a percentage style size, which it cannot bake', () => {
    expect(() =>
      baked("width={20} height={20} style={{width: '50%'}}"),
    ).toThrow(/percentage style width on an <Svg source> is not supported/);
  });

  it('takes the last style size, a percentage overridden by pixels or the reverse', () => {
    const tape = c => c.match(/s_svg0_ops\[\] = \{[^}]*\}/)[0];
    const overridden = baked(
      "style={[{width: '50%', height: 30}, {width: 30}]}",
    );
    expect(tape(overridden)).toBe(
      tape(baked('style={{width: 30, height: 30}}')),
    );
    expect(overridden).not.toContain('width_pct');
    expect(() =>
      baked("style={[{width: 30, height: 30}, {width: '50%'}]}"),
    ).toThrow(/percentage style width on an <Svg source> is not supported/);
  });
});

describe('a pixel size and its percentage twin', () => {
  it('are one style key, so the later one replaces the other', () => {
    expect(gen("<View style={[{width: '50%'}, {width: 30}]} />")).toBe(
      gen('<View style={{width: 30}} />'),
    );
    expect(gen("<View style={[{left: 30}, {left: '50%'}]} />")).toBe(
      gen("<View style={{left: '50%'}} />"),
    );
  });
});

describe('animated style keys', () => {
  it('binds an animated transform on a typed element and an <Svg>', () => {
    expect(gen('<Switch style={{transform: [{scale: a}]}} />')).toContain(
      'er_anim_value_bind(s_av_a, n0, ER_PROP_SCALE_X);',
    );
    expect(
      gen(
        '<Svg width={9} height={9} style={{transform: [{scale: a}]}}><Circle r={4} /></Svg>',
      ),
    ).toContain('er_anim_value_bind(s_av_a, n0, ER_PROP_SCALE_X);');
  });

  it.each([
    ['opacity', '<TextInput style={{opacity: a}} />', 'TextInput'],
    [
      'backgroundColor',
      '<TextInput style={{backgroundColor: a}} />',
      'TextInput',
    ],
    ['color', '<ActivityIndicator style={{color: a}} />', 'ActivityIndicator'],
    ['color', '<Text>a<Text style={{color: a}}>b</Text></Text>', 'Text'],
  ])(
    'refuses an animated %s the engine does not animate there',
    (key, jsx, tag) => {
      expect(errorOf(jsx)).toBe(
        `AOT: an animated "${key}" has no effect on <${tag}>`,
      );
    },
  );
});
