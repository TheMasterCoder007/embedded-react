// Logic: `switch`, early `return` and helpers that return a value. A switch on a tag narrows each case, a case can
// fall through to the next, a `return` in a helper leaves only the helper, and helpers work out values in render.
import {useState} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

type Mode =
  {kind: 'off'} | {kind: 'run'; speed: number} | {kind: 'jam'; code: number};

const LIMIT = 3;

function clamp(value: number, low: number, high: number) {
  if (value < low) return low;
  if (value > high) return high;
  return value;
}

function sizeName(count: number) {
  switch (count) {
    case 0:
      return 'none';
    case 1:
    case 2:
      return 'few';
    default:
      return 'many';
  }
}

const percent = (part: number, whole: number) =>
  `${Math.round((part * 100) / whole)}%`;

function modeValue(mode: Mode) {
  switch (mode.kind) {
    case 'run':
      return mode.speed;
    case 'jam':
      return mode.code;
    default:
      return 0;
  }
}

function speedOf(mode: Mode) {
  if (mode.kind !== 'run') return 0;
  return mode.speed;
}

export function App() {
  const [count, setCount] = useState(0);
  const [taps, setTaps] = useState(0);
  const [log, setLog] = useState('-');
  const [mode, setMode] = useState<Mode>({kind: 'off'});

  const bump = (step: number) => {
    if (count + step > LIMIT) {
      setLog('full');
      return;
    }
    setCount(count + step);
  };

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() => {
            bump(1);
            setTaps(taps + 1);
          }}>
          <Text style={styles.buttonText}>add</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            if (count === 0) return;
            setCount(count - 1);
            setLog('drop');
          }}>
          <Text style={styles.buttonText}>drop</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            switch (count) {
              case 3:
                setLog('three');
              case 2:
                setCount(clamp(count - 2, 0, LIMIT));
                break;
              case 0:
                break;
              default:
                setCount(0);
            }
          }}>
          <Text style={styles.buttonText}>fold</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            switch (mode.kind) {
              case 'off':
                setMode({kind: 'run', speed: 1});
                break;
              case 'run':
                if (mode.speed >= 3) {
                  return setMode({kind: 'jam', code: 30});
                }
                setMode({...mode, speed: mode.speed + 1});
                break;
              default:
                setMode({kind: 'off'});
            }
          }}>
          <Text style={styles.buttonText}>mode</Text>
        </Pressable>
      </View>
      <Text style={styles.text}>
        {count} {sizeName(count)}
      </Text>
      <Text style={styles.text}>{percent(count, LIMIT)}</Text>
      <View style={styles.track}>
        <View
          style={{
            width: clamp(count * 50, 10, 120),
            height: 8,
            backgroundColor: '#30a0e0',
          }}
        />
      </View>
      <Text style={styles.text}>
        {log} {taps}
      </Text>
      <Text style={styles.text}>
        {mode.kind} {modeValue(mode)} {speedOf(mode)}
      </Text>
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
  track: {width: 200, height: 8, backgroundColor: '#202838'},
});
