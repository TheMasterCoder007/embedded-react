// A screen size that changes while the app runs: what reads the size follows it, and state is kept. Props set once,
// props that read state too, a branch, rows, a drawing and an effect that depends on the width.
import {useEffect, useState} from 'react';
import {
  View,
  Text,
  Pressable,
  StyleSheet,
  Svg,
  Path,
  useWindowDimensions,
} from 'embedded-react';

const PADDING = 8;
const SHARES = [{share: 2}, {share: 3}, {share: 4}];

function Bar({share}: {share: number}) {
  const {width} = useWindowDimensions();
  return (
    <View
      style={{width: width / share, height: 6, backgroundColor: '#60e080'}}
    />
  );
}

export function App() {
  const {width, height} = useWindowDimensions();
  const [count, setCount] = useState(0);
  const [shown, setShown] = useState(false);
  const [seen, setSeen] = useState(0);
  useEffect(() => {
    setSeen(width);
  }, [width]);
  const landscape = width > height;
  const inner = width - 2 * PADDING;

  return (
    <View
      style={[
        styles.root,
        {backgroundColor: landscape ? '#101820' : '#181010'},
      ]}>
      <View style={styles.bar}>
        <Pressable style={styles.button} onPress={() => setCount(count + 1)}>
          <Text style={styles.buttonText}>count</Text>
        </Pressable>
        <Pressable style={styles.button} onPress={() => setShown(!shown)}>
          <Text style={styles.buttonText}>show</Text>
        </Pressable>
      </View>
      <Text style={styles.text}>
        {width}x{height} {landscape ? 'wide' : 'tall'}
      </Text>
      <Text style={styles.text}>
        count {count} of {width} seen {seen}
      </Text>
      <View style={{width: inner, height: 8, backgroundColor: '#f4a261'}} />
      <View
        style={{
          width: (inner * count) / 4,
          height: 8,
          backgroundColor: '#3060c0',
        }}
      />
      {shown ? (
        <View
          style={{
            width: width / 2,
            height: height / 8,
            backgroundColor: '#c03060',
          }}
        />
      ) : null}
      {SHARES.map(row => (
        <Bar share={row.share} />
      ))}
      <Svg width={width / 2} height={height / 8} viewBox="0 0 100 50">
        <Path d="M10 5 L90 5 L90 45 C 80 30 60 30 50 45 Z" fill="#60e080" />
      </Svg>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, padding: PADDING, gap: 4},
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
});
