// Records: object state, with a nested object, written whole by spreads, an updater, a ternary of objects, handler
// objects and destructuring. Render destructures it, spreads it into an object, keeps a memo of one, and passes it to
// a child. Each counter holds an object state of its own, written from its old value.
import {useMemo, useState} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

interface Box {
  pos: {x: number; y: number};
  label: string;
  on: boolean;
  scale: number;
}

function Readout({box}: {box: Box}) {
  return (
    <Text style={styles.text}>
      {box.label} at {box.pos.x},{box.pos.y} {box.on ? 'on' : 'off'} x
      {box.scale}
    </Text>
  );
}

function Counter({step}: {step: number}) {
  const [count, setCount] = useState({n: 0, last: '-'});
  return (
    <Pressable
      style={styles.button}
      onPress={() => setCount({n: count.n + step, last: `was ${count.n}`})}>
      <Text style={styles.buttonText}>
        {count.n} {count.last}
      </Text>
    </Pressable>
  );
}

export function App() {
  const [box, setBox] = useState<Box>({
    pos: {x: 0, y: 0},
    label: 'box',
    on: false,
    scale: 1.0,
  });
  const {pos, label} = box;
  const shown = {...pos, size: 16 * box.scale};
  const area = useMemo(
    () => ({...box.pos, sum: box.pos.x + box.pos.y}),
    [box.pos.x, box.pos.y],
  );

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() =>
            setBox({...box, pos: {...box.pos, x: box.pos.x + 12}})
          }>
          <Text style={styles.buttonText}>right</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            setBox(previous => ({
              ...previous,
              pos: {x: previous.pos.x, y: previous.pos.y + 8},
            }))
          }>
          <Text style={styles.buttonText}>down</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setBox({...box, pos: {x: box.pos.y, y: box.pos.x}})}>
          <Text style={styles.buttonText}>swap</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            const next = box.on
              ? {label: 'box', on: false}
              : {label: 'BOX', on: true};
            setBox({...box, ...next});
          }}>
          <Text style={styles.buttonText}>flip</Text>
        </Pressable>
      </View>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() => {
            const {scale, ...rest} = box;
            setBox({...rest, scale: scale + 0.5});
          }}>
          <Text style={styles.buttonText}>grow</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            setBox(
              box.scale > 1
                ? {pos: {x: 0, y: 0}, label: 'box', on: false, scale: 1}
                : {...box, pos: {x: 4, y: 4}},
            )
          }>
          <Text style={styles.buttonText}>reset</Text>
        </Pressable>
        <Counter step={1} />
        <Counter step={5} />
      </View>
      <Readout box={box} />
      <Text style={styles.text}>
        {label} {area.sum} {shown.x}+{shown.size}
      </Text>
      <View style={styles.field}>
        <View
          style={{
            marginLeft: pos.x,
            marginTop: pos.y,
            width: shown.size,
            height: shown.size,
            backgroundColor: box.on ? '#e0a030' : '#30a0e0',
          }}
        />
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#182030', padding: 8, gap: 4},
  bar: {flexDirection: 'row', gap: 4, height: 24},
  button: {
    width: 48,
    height: 24,
    borderRadius: 4,
    backgroundColor: '#3060c0',
    alignItems: 'center',
    justifyContent: 'center',
  },
  buttonText: {fontSize: 10, color: '#ffffff'},
  text: {fontSize: 10, color: '#f0f0f0'},
  field: {width: 120, height: 80, backgroundColor: '#202838'},
});
